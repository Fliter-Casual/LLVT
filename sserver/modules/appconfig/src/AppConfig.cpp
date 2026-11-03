// ============================================================
// 文件：AppConfig.cpp
// 模块：sserver::appconfig
// 功能：TOML 配置模型与加载器实现：逐表逐键识别未知项、按目标类型赋值、
//       加载后执行业务规则校验；所有错误统一带「文件:行:列」源位置。
// 知识点：toml++ 解析与源位置、白名单驱动的严格解析、函数重载分发、
//         整型边界检查、异常安全（失败不改配置）
// ============================================================

#include "sserver/appconfig/AppConfig.h"

#include <cstdint>
#include <exception>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include <toml++/toml.hpp>

namespace sserver {
namespace appconfig {

namespace {

// ==================== 错误定位 ====================

// toml++ 的行列号用 0 表示「未知」，统一归一成 1，避免输出 "file:0:0"
// 这种没法定位的错误。
std::string formatError(const std::string &filePath,
                        const toml::source_position &position,
                        const std::string &message) {
    const auto line = position.line == 0 ? 1 : position.line;
    const auto column = position.column == 0 ? 1 : position.column;
    std::ostringstream stream;
    stream << filePath << ':' << line << ':' << column << ": " << message;
    return stream.str();
}

// 设置错误信息：如果 errorMessage 非空，就赋值给它。
void setError(std::string *errorMessage, const std::string &text) {
    if (errorMessage != nullptr) {
        *errorMessage = text;
    }
}

// 节点类型名：错误信息要报告「实际拿到了什么」，只写「类型不对」
// 会让人还得回去翻文件确认自己写的是字符串还是整数。
std::string nodeTypeName(const toml::node &node) {
    if (node.is_table()) {
        return "table";
    }
    if (node.is_array()) {
        return "array";
    }
    if (node.is_string()) {
        return "string";
    }
    if (node.is_integer()) {
        return "integer";
    }
    if (node.is_floating_point()) {
        return "float";
    }
    if (node.is_boolean()) {
        return "boolean";
    }
    if (node.is_date_time()) {
        return "date-time";
    }
    return "unknown";
}

// ==================== 逐字段读取 ====================

// 统一的类型错误出口：scope 是 "control" 这种表名，key 是键名。
bool typeError(const toml::node &node, const std::string &filePath,
               const std::string &scope, const std::string &key,
               const char *expected, std::string *errorMessage) {
    setError(errorMessage,
             formatError(filePath, node.source().begin,
                         scope + "." + key + " must be " + expected
                         + " (got " + nodeTypeName(node) + ")"));
    return false;
}

// 键不存在不是错误：省略某个键就是「沿用默认值」，这是 TOML 配置的
// 常规用法，不该逼着使用者把每个键都写全。
const toml::node *findKey(const toml::table &table, const std::string &key) {
    return table.get(key);
}

// 安全整型配置读取函数
// 从 TOML 解析出来的数据表中，安全地提取一个整数，进行严格的类型和范围校验，最后赋值给目标变量
bool readInt(const toml::table &table, const std::string &filePath,
             const std::string &scope, const std::string &key,
             std::int64_t minimum, std::int64_t maximum,
             int *target, std::string *errorMessage) {
    const toml::node *node = findKey(table, key);
    if (node == nullptr) {
        return true;
    }
    if (!node->is_integer()) {
        return typeError(*node, filePath, scope, key, "an integer", errorMessage);
    }
    const std::int64_t value = node->value<std::int64_t>().value();
    if (value < minimum || value > maximum) {
        setError(errorMessage,
                 formatError(filePath, node->source().begin,
                             scope + "." + key + " must be in [" + std::to_string(minimum)
                             + ", " + std::to_string(maximum) + "] (got "
                             + std::to_string(value) + ")"));
        return false;
    }
    *target = static_cast<int>(value);
    return true;
}

// 安全大小型配置读取函数
// 从 TOML 解析出来的数据表中，安全地提取一个大小型整数，进行严格的类型和范围校验，最后赋值给目标变量
bool readSize(const toml::table &table, const std::string &filePath,
              const std::string &scope, const std::string &key,
              std::int64_t minimum, std::int64_t maximum,
              std::size_t *target, std::string *errorMessage) {
    const toml::node *node = findKey(table, key);
    if (node == nullptr) {
        return true;
    }
    if (!node->is_integer()) {
        return typeError(*node, filePath, scope, key, "an integer", errorMessage);
    }
    const std::int64_t value = node->value<std::int64_t>().value();
    if (value < minimum || value > maximum) {
        setError(errorMessage,
                 formatError(filePath, node->source().begin,
                             scope + "." + key + " must be in [" + std::to_string(minimum)
                             + ", " + std::to_string(maximum) + "] (got "
                             + std::to_string(value) + ")"));
        return false;
    }
    *target = static_cast<std::size_t>(value);
    return true;
}

// 安全布尔型配置读取函数
// 从 TOML 解析出来的数据表中，安全地提取一个布尔值，进行严格的类型校验，最后赋值给目标变量
bool readBool(const toml::table &table, const std::string &filePath,
              const std::string &scope, const std::string &key,
              bool *target, std::string *errorMessage) {
    const toml::node *node = findKey(table, key);
    if (node == nullptr) {
        return true;
    }
    if (!node->is_boolean()) {
        return typeError(*node, filePath, scope, key, "a boolean", errorMessage);
    }
    *target = node->value<bool>().value();
    return true;
}

// 安全字符串型读取函数
// 从 TOML 解析出来的数据表中，安全地提取一个字符串，进行严格的类型校验，最后赋值给目标变量
bool readString(const toml::table &table, const std::string &filePath,
                const std::string &scope, const std::string &key,
                bool allowEmpty, std::string *target, std::string *errorMessage) {
    const toml::node *node = findKey(table, key);
    if (node == nullptr) {
        return true;
    }
    if (!node->is_string()) {
        return typeError(*node, filePath, scope, key, "a string", errorMessage);
    }
    std::string value = node->value<std::string>().value();
    // 空设备路径没有任何含义，早报比等到打开 /dev 失败再猜要好。
    if (!allowEmpty && value.empty()) {
        setError(errorMessage,
                 formatError(filePath, node->source().begin,
                             scope + "." + key + " must not be empty"));
        return false;
    }
    *target = std::move(value);
    return true;
}

// ==================== 未知键检查 ====================

// 严格风格的核心：拼错的键必须报错。否则写错 "control.portt = 9000"，
// 程序会用默认端口 9000 悄悄跑起来，配置看起来"生效了"，排查时极难发现。
bool rejectUnknownKeys(const toml::table &table, const std::string &filePath,
                       const std::string &scope,
                       const std::vector<std::string> &knownKeys,
                       std::string *errorMessage) {
    for (const auto &[key, node] : table) {
        bool known = false;
        for (const std::string &candidate : knownKeys) {
            if (key == candidate) {
                known = true;
                break;
            }
        }
        if (!known) {
            setError(errorMessage,
                     formatError(filePath, node.source().begin,
                                 "unknown key '" + static_cast<std::string>(key.str())
                                 + "' in table [" + scope + "]"));
            return false;
        }
    }
    return true;
}

// ==================== 分段加载 ====================

bool loadControl(const toml::table &table, const std::string &filePath,
                 ControlConfig *config, std::string *errorMessage) {
    if (!rejectUnknownKeys(table, filePath, "control", {"port", "max_clients"},
                           errorMessage)) {
        return false;
    }
    // 端口 0 会让内核随机分配，控制通道的端口是客户端要提前知道的，
    // 随机端口等于没法连，所以下限取 1。
    return readInt(table, filePath, "control", "port", 1, 65535,
                   &config->m_port, errorMessage)
            && readSize(table, filePath, "control", "max_clients", 1, 64,
                        &config->m_maxClients, errorMessage);
}

bool loadCapture(const toml::table &table, const std::string &filePath,
                 CaptureConfig *config, std::string *errorMessage) {
    if (!rejectUnknownKeys(table, filePath, "capture", {"device"}, errorMessage)) {
        return false;
    }
    return readString(table, filePath, "capture", "device", false,
                      &config->m_device, errorMessage);
}

bool loadRuntime(const toml::table &table, const std::string &filePath,
                 RuntimeConfig *config, std::string *errorMessage) {
    if (!rejectUnknownKeys(table, filePath, "runtime", {"log_verbose"}, errorMessage)) {
        return false;
    }
    return readBool(table, filePath, "runtime", "log_verbose",
                    &config->m_logVerbose, errorMessage);
}

bool loadSections(const toml::table &root, const std::string &filePath,
                  AppConfig *config, std::string *errorMessage) {
    for (const auto &[key, node] : root) {
        const std::string name = static_cast<std::string>(key.str());
        if (name != "control" && name != "capture" && name != "runtime") {
            setError(errorMessage,
                     formatError(filePath, node.source().begin,
                                 "unknown table [" + name + "]"));
            return false;
        }
        if (!node.is_table()) {
            setError(errorMessage,
                     formatError(filePath, node.source().begin,
                                 "[" + name + "] must be a table (got "
                                 + nodeTypeName(node) + ")"));
            return false;
        }
    }

    if (const toml::table *control = root["control"].as_table()) {
        if (!loadControl(*control, filePath, &config->m_control, errorMessage)) {
            return false;
        }
    }
    if (const toml::table *capture = root["capture"].as_table()) {
        if (!loadCapture(*capture, filePath, &config->m_capture, errorMessage)) {
            return false;
        }
    }
    if (const toml::table *runtime = root["runtime"].as_table()) {
        if (!loadRuntime(*runtime, filePath, &config->m_runtime, errorMessage)) {
            return false;
        }
    }
    return true;
}

// 解析与加载的公共后半程：解析 → 分段加载 → 整体校验。
// 全程写进一份临时副本，只有全部成功才提交，失败时调用方的配置保持不变。
bool loadFromText(const std::string &tomlText, const std::string &sourceName,
                  AppConfig *config, std::string *errorMessage) {
    if (config == nullptr) {
        setError(errorMessage, "internal error: config output is null");
        return false;
    }

    toml::table root;
    try {
        root = toml::parse(tomlText, sourceName);
    } catch (const toml::parse_error &error) {
        // parse_error 自带源位置，直接用它的行列号，不要再从别处猜。
        // description() 返回 string_view，显式转成 string 再交给 formatError。
        setError(errorMessage,
                 formatError(sourceName, error.source().begin,
                             std::string(error.description())));
        return false;
    } catch (const std::exception &error) {
        setError(errorMessage, sourceName + ": parse error: " + error.what());
        return false;
    }

    AppConfig loaded = AppConfig::createDefault();
    if (!loadSections(root, sourceName, &loaded, errorMessage)) {
        return false;
    }
    if (!loaded.validate(errorMessage)) {
        return false;
    }
    *config = std::move(loaded);
    return true;
}

}  // namespace

// ==================== AppConfig ====================

AppConfig AppConfig::createDefault() {
    return AppConfig{};
}

// 这里放"加载器读不到"的组合约束。字段级的范围检查在读取时就做了
// （那样能直接引用 TOML 源位置），validate 只兜住跨字段的规则。
bool AppConfig::validate(std::string *errorMessage, std::string *errorField) const {
    const auto fail = [errorMessage, errorField](const std::string &field,
                                                 const std::string &message) {
        if (errorMessage != nullptr) {
            *errorMessage = message;
        }
        if (errorField != nullptr) {
            *errorField = field;
        }
        return false;
    };

    if (m_control.m_port < 1 || m_control.m_port > 65535) {
        return fail("control.port", "control.port must be in [1, 65535]");
    }
    if (m_control.m_maxClients < 1 || m_control.m_maxClients > 64) {
        return fail("control.max_clients", "control.max_clients must be in [1, 64]");
    }
    if (m_capture.m_device.empty()) {
        return fail("capture.device", "capture.device must not be empty");
    }
    return true;
}

// ==================== ConfigLoader ====================

bool ConfigLoader::loadFromFile(const std::string &filePath, AppConfig *config,
                                std::string *errorMessage) {
    std::ifstream input(filePath, std::ios::binary);
    if (!input) {
        setError(errorMessage, filePath + ": cannot open file for reading");
        return false;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return loadFromText(buffer.str(), filePath, config, errorMessage);
}

bool ConfigLoader::loadFromString(const std::string &tomlText, const std::string &sourceName,
                                  AppConfig *config, std::string *errorMessage) {
    return loadFromText(tomlText, sourceName, config, errorMessage);
}

}  // namespace appconfig
}  // namespace sserver
