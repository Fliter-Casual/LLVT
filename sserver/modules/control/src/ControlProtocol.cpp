// ============================================================
// 文件：ControlProtocol.cpp
// 模块：sserver::control
// 功能：控制平面文本协议的纯函数编解码：一行命令解析成 ControlCommand
//       （失败给细粒度错误码），StreamingStatus 序列化成 OK/ERR/CAPS 单行
//       响应。不含 socket、线程与业务判断。
// 知识点：行分帧文本协议、键值对解析、strtol 严格数字校验、
//         匿名命名空间、lambda 引用捕获、unordered_set 重复键检测
// ============================================================

#include "sserver/control/ControlProtocol.h"

#include <cerrno>
#include <cstdlib>
#include <sstream>
#include <unordered_set>
#include <vector>

namespace sserver {
namespace control {

namespace {

// char 是否带符号由平台决定：先转 unsigned char 再比范围，否则非 ASCII
// 字节会被当成负数，绕过 < 0x20 的判断。
bool isVisibleAscii(const std::string &value) {
    for (char character : value) {
        const unsigned char byte = static_cast<unsigned char>(character);
        if (byte < 0x20 || byte > 0x7e) {
            return false;
        }
    }
    return true;
}

// 入口已经拒绝连续空格，这里用流提取分词只是为了少写一遍循环。
std::vector<std::string> splitFields(const std::string &line) {
    std::vector<std::string> fields;
    std::istringstream input(line);
    std::string field;
    while (input >> field) {
        fields.push_back(field);
    }
    return fields;
}

// 严格端口解析：先限长度（超过 5 位不可能合法，也堵住大数溢出），
// 再用 errno + end 指针双重校验——溢出、空串、尾部残留字符都算失败。
bool parsePort(const std::string &value, int *port) {
    if (port == nullptr || value.empty() || value.size() > 5) {
        return false;
    }
    char *end = nullptr;
    // strtol 溢出时置 errno=ERANGE，不清零就分不清新旧错误。
    errno = 0;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (errno != 0 || end == value.c_str() || *end != '\0'
        || parsed < 0 || parsed > 65535) {
        return false;
    }
    *port = static_cast<int>(parsed);
    return true;
}

// 白名单清洗：任意输入收敛到字母数字与 _-. ，其余换成 '_'。响应文本直接
// 由客户端按空格分词，值里一旦混进空格或控制字符，整行帧结构就破了。
std::string safeWord(const std::string &value) {
    std::string safe;
    safe.reserve(value.size());
    for (char character : value) {
        const bool allowed = (character >= 'a' && character <= 'z')
                || (character >= 'A' && character <= 'Z')
                || (character >= '0' && character <= '9')
                || character == '_' || character == '-' || character == '.';
        safe.push_back(allowed ? character : '_');
    }
    return safe.empty() ? "unknown" : safe;
}

// 设备路径在 safeWord 字符集上额外允许 '/'（/dev/video0）。同时拒绝以 '.'
// 开头和含 ".." 的值：协议层不做路径规范化，那就别放过可能的路径穿越写法。
bool isValidDevicePath(const std::string &value) {
    if (value.empty() || value.size() > 64) {
        return false;
    }
    for (char character : value) {
        const bool allowed = (character >= 'a' && character <= 'z')
                || (character >= 'A' && character <= 'Z')
                || (character >= '0' && character <= '9')
                || character == '_' || character == '-' || character == '.'
                || character == '/';
        if (!allowed) {
            return false;
        }
    }
    if (value.find("..") != std::string::npos || value.front() == '.') {
        return false;
    }
    return true;
}

std::string formatDeviceField(const std::string &value) {
    return isValidDevicePath(value) ? value : safeWord(value);
}

// 状态字段按固定顺序输出。顺序固定不是风格问题：客户端按位置或按固定
// 键序做断言时，顺序漂移会被当成协议不兼容。
std::string formatStatusFields(const contracts::StreamingStatus &status,
                               bool includeMediaKey = false) {
    std::ostringstream output;
    output << "state=" << contracts::streamingStateName(status.m_state)
           << " transport=" << safeWord(status.m_transport)
           << " host=" << safeWord(status.m_host)
           << " port=" << status.m_port
           << " profile=" << safeWord(status.m_profile);
    // 只有 auto 策略才回显实际生效的网络档位：老客户端拒绝未知字段，
    // 无条件带上去会让它们连不上。
    if (status.m_profile == "auto") {
        output << " network_profile=" << safeWord(status.m_activeNetworkProfile);
    }
    output << " media_profile=" << safeWord(status.m_mediaProfile)
           << " session=" << safeWord(status.m_sessionId)
           << " codec=" << safeWord(status.m_codec)
           << " device=" << formatDeviceField(status.m_device);
    if (includeMediaKey && !status.m_mediaKeyHex.empty()) {
        output << " media_key=" << status.m_mediaKeyHex;
    }
    if (!status.m_colorimetry.empty()) {
        output << " colorimetry=" << status.m_colorimetry;
    }
    if (!status.m_quality.empty()) {
        output << " quality=" << status.m_quality;
    }
    return output.str();
}

void setError(std::string *errorCode, const char *code) {
    if (errorCode != nullptr) {
        *errorCode = code;
    }
}

}  // namespace

// ==================== 命令解析 ====================

bool parseControlCommand(const std::string &line,
                         ControlCommand *command,
                         std::string *errorCode) {
    if (command == nullptr) {
        setError(errorCode, "internal_error");
        return false;
    }
    if (line.empty() || line.size() > kAbsoluteMaxControlLineLength
        || !isVisibleAscii(line)) {
        setError(errorCode, "invalid_line");
        return false;
    }
    // 规范形式校验：一条命令只有唯一文本编码。否则 "STATUS  session=x" 和
    // "STATUS session=x" 在日志里是两行不同的记录，比对时永远对不上。
    if (line.front() == ' ' || line.back() == ' '
        || line.find("  ") != std::string::npos) {
        setError(errorCode, "invalid_spacing");
        return false;
    }

    const std::vector<std::string> fields = splitFields(line);
    if (fields.empty()) {
        setError(errorCode, "empty_command");
        return false;
    }

    ControlCommand parsed;

    // STATUS / STOP / KEYFRAME 共用显式 `session=<id>` 寻址：必须恰好两个
    // 字段，第二个必须是 session= 且值合法。
    const auto parseSessionAddressed = [&fields, &parsed, errorCode]() -> bool {
        if (fields.size() != 2) {
            setError(errorCode, "unexpected_argument");
            return false;
        }
        const std::string &field = fields[1];
        const std::size_t equal = field.find('=');
        if (equal == std::string::npos || field.substr(0, equal) != "session"
            || equal + 1 >= field.size()) {
            setError(errorCode, "unexpected_argument");
            return false;
        }
        const std::string value = field.substr(equal + 1);
        if (!contracts::isValidSessionId(value)) {
            setError(errorCode, "invalid_session");
            return false;
        }
        parsed.m_targetSession = value;
        return true;
    };

    if (fields[0] == "STATUS") {
        // STATUS 额外接受可选的 media_port=<port>：客户端断流恢复换了新的
        // UDP 源端口后借此上报，服务端据此改写发送目标。两键可任意顺序，
        // 重复或其他键一律拒绝。
        if (fields.size() > 3) {
            setError(errorCode, "unexpected_argument");
            return false;
        }
        bool hasSession = false;
        bool hasMediaPort = false;
        for (std::size_t index = 1; index < fields.size(); ++index) {
            const std::string &field = fields[index];
            const std::size_t equal = field.find('=');
            if (equal == std::string::npos || equal + 1 >= field.size()) {
                setError(errorCode, "unexpected_argument");
                return false;
            }
            const std::string key = field.substr(0, equal);
            const std::string value = field.substr(equal + 1);
            if (key == "session" && !hasSession) {
                if (!contracts::isValidSessionId(value)) {
                    setError(errorCode, "invalid_session");
                    return false;
                }
                parsed.m_targetSession = value;
                hasSession = true;
            } else if (key == "media_port" && !hasMediaPort) {
                int mediaPort = 0;
                if (!parsePort(value, &mediaPort) || mediaPort == 0) {
                    setError(errorCode, "invalid_media_port");
                    return false;
                }
                parsed.m_mediaPort = mediaPort;
                hasMediaPort = true;
            } else if (key == "session" || key == "media_port") {
                setError(errorCode, "duplicate_field");
                return false;
            } else {
                setError(errorCode, "unknown_field");
                return false;
            }
        }
        if (!hasSession) {
            setError(errorCode, "invalid_session");
            return false;
        }
        parsed.m_type = CommandType::Status;
        *command = parsed;
        return true;
    }

    if (fields[0] == "STOP") {
        if (!parseSessionAddressed()) {
            return false;
        }
        parsed.m_type = CommandType::Stop;
        *command = parsed;
        return true;
    }

    if (fields[0] == "KEYFRAME") {
        if (!parseSessionAddressed()) {
            return false;
        }
        parsed.m_type = CommandType::Keyframe;
        *command = parsed;
        return true;
    }

    if (fields[0] == "CAPS") {
        if (fields.size() != 1) {
            setError(errorCode, "unexpected_argument");
            return false;
        }
        parsed.m_type = CommandType::Caps;
        *command = parsed;
        return true;
    }

    if (fields[0] != "START") {
        setError(errorCode, "unknown_command");
        return false;
    }

    if (fields.size() < 3 || fields.size() > 9) {
        setError(errorCode, "invalid_start");
        return false;
    }

    parsed.m_type = CommandType::Start;
    parsed.m_request.m_profile = "balanced";
    bool hasTransport = false;
    bool hasPort = false;
    // insert() 的 second 为 false 表示键已存在：重复键一律拒绝，因为它
    // 意味着客户端有两种意图，服务端没法替它选一个。
    std::unordered_set<std::string> seenKeys;

    for (std::size_t index = 1; index < fields.size(); ++index) {
        const std::string &field = fields[index];
        const std::size_t equal = field.find('=');
        // 缺 '='、空键、空值都算畸形；未识别的键整体拒绝，不做静默忽略——
        // 忽略意味着客户端以为设了参数，实际没生效。
        if (equal == std::string::npos || equal == 0 || equal + 1 >= field.size()) {
            setError(errorCode, "invalid_start_field");
            return false;
        }
        const std::string key = field.substr(0, equal);
        const std::string value = field.substr(equal + 1);
        if (!seenKeys.insert(key).second) {
            setError(errorCode, "duplicate_field");
            return false;
        }

        if (key == "transport") {
            if (value != "udp") {
                setError(errorCode, "invalid_transport");
                return false;
            }
            parsed.m_request.m_transport = value;
            hasTransport = true;
        } else if (key == "port") {
            if (!parsePort(value, &parsed.m_request.m_port)) {
                setError(errorCode, "invalid_port");
                return false;
            }
            hasPort = true;
        } else if (key == "host") {
            // 媒体目的地由服务端决定（绑定自身地址），请求里的 host 没有
            // 语义；留出上限只是为了让错误信息具体，不是接受它。
            if (value.size() > 253) {
                setError(errorCode, "invalid_host");
                return false;
            }
            parsed.m_request.m_host = value;
        } else if (key == "profile") {
            if (value != "auto" && value != "balanced"
                && value != "adaptive" && value != "resilient") {
                setError(errorCode, "invalid_profile");
                return false;
            }
            parsed.m_request.m_profile = value;
        } else if (key == "media_profile") {
            if (value.size() > 64 || safeWord(value) != value) {
                setError(errorCode, "invalid_media_profile");
                return false;
            }
            parsed.m_request.m_mediaProfile = value;
        } else if (key == "session") {
            if (!contracts::isValidSessionId(value)) {
                setError(errorCode, "invalid_session");
                return false;
            }
            parsed.m_request.m_sessionId = value;
        } else if (key == "device") {
            if (!isValidDevicePath(value)) {
                setError(errorCode, "invalid_device");
                return false;
            }
            parsed.m_request.m_device = value;
        } else if (key == "enc") {
            if (value != "0" && value != "1") {
                setError(errorCode, "invalid_enc");
                return false;
            }
            parsed.m_request.m_encryptionRequested = value == "1";
        } else if (key == "quality") {
            if (value != "hd" && value != "sd" && value != "auto") {
                setError(errorCode, "invalid_quality");
                return false;
            }
            parsed.m_request.m_quality = value;
        } else {
            setError(errorCode, "unknown_field");
            return false;
        }
    }

    // 交叉校验：transport / port / session / profile / quality / enc 必填，
    // host 必须为空。单项格式都对、组合起来不可用的情况只能在这里发现。
    if (!hasTransport || !hasPort || !parsed.m_request.m_host.empty()
        || !contracts::isValidSessionId(parsed.m_request.m_sessionId)
        || seenKeys.count("profile") == 0
        || seenKeys.count("quality") == 0
        || seenKeys.count("enc") == 0) {
        setError(errorCode, "invalid_start");
        return false;
    }

    // 只有全部校验通过才写回，失败路径不碰调用方对象。
    *command = parsed;
    return true;
}

// ==================== 应答格式化 ====================

std::string formatOkResponse(const contracts::StreamingStatus &status,
                             bool includeMediaKey) {
    return "OK " + formatStatusFields(status, includeMediaKey);
}

std::string formatCapsResponse(const std::vector<std::string> &availableMediaProfiles,
                               const contracts::StreamingStatus &status) {
    std::string joined;
    for (const std::string &name : availableMediaProfiles) {
        if (!joined.empty()) {
            joined += ",";
        }
        joined += safeWord(name);
    }
    if (joined.empty()) {
        joined = "none";
    }
    return "OK media_profiles=" + joined + " " + formatStatusFields(status);
}

std::string formatErrorResponse(const std::string &code,
                                const std::string &message,
                                const contracts::StreamingStatus &status) {
    // 错误码和描述也过 safeWord：内部消息可能带空格，原样写进去会把
    // 单行响应拆成多个字段。
    return "ERR code=" + safeWord(code) + " message=" + safeWord(message)
            + " " + formatStatusFields(status);
}

}  // namespace control
}  // namespace sserver
