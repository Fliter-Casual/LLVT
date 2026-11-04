// ============================================================
// 文件：StreamingControl.h
// 模块：sserver::contracts
// 功能：定义控制面与应用层(数据面)之间的纯契约(contracts)：流媒体状态枚举 StreamingState、
//       会话请求/状态结构体，以及应用端口抽象 StreamingController。
//       只含数据类型与虚接口，被 control 与 app 两侧共同依赖。
// 知识点：抽象类划定模块边界、enum class 状态机、虚函数默认实现、
//         依赖倒置
// ============================================================

#ifndef SSERVER_CONTRACTS_STREAMING_CONTROL_H
#define SSERVER_CONTRACTS_STREAMING_CONTROL_H

#include <cstdint>
#include <string>

namespace sserver{
namespace contracts{

// 会话标识必须显式、可寻址：空值会让 STATUS/STOP(文本命令) 无法确定目标，
// "none"/"legacy" 是状态字段里的占位词，不能拿来当真实会话号。
// 白名单/严格校验,限定参数只能是特定字符
// 字符集限制在字母数字与 _-. ，保证日志里可直接 grep、协议里无需转义。
inline bool isValidSessionId(const std::string &value) {
    if (value.empty() || value.size() > 64 || value == "none" || value == "legacy") {
        return false;
    }
    for (char character : value) {
        const bool allowed = (character >= 'a' && character <= 'z')
                || (character >= 'A' && character <= 'Z')
                || (character >= '0' && character <= '9')
                || character == '_' || character == '-' || character == '.';
        if (!allowed) {
            return false;
        }
    }
    return true;
}

// ==================== 会话状态枚举 ====================

// 流媒体状态枚举
// 会话状态机，定义了会话的生命周期。
enum class StreamingState {
    Idle,       // 会话未开始
    Starting,   // 会话启动中
    Running,    // 会话运行中
    Stopping,   // 会话停止中
    Failed,     // 会话启动失败
};

// 会话请求结构体(接收客户端发来的信息，然后反序列化到这个结构体里)
struct StreamingRequest {
    std::string m_sessionId;        // 会话标识
    std::string m_transport;          // 传输协议,当前只有 udp
    std::string m_host;               // 主机地址,请求里禁止携带：服务端绑定自己的地址              
    int m_port = 0;              // 端口号,UDP 监听端口，0 表示动态分配
    std::string m_profile = "balanced"; // 流配置,网络策略：auto / balanced / adaptive / resilient
    std::string m_mediaProfile;    // 媒体配置,画质档位名，空表示沿用服务端默认配置档位
    std::string m_device;         // 设备标识,采集设备，空表示按档位/默认策略
    bool m_encryptionRequested = false; // 是否请求加密,START enc=1：请求媒体面 AES-GCM
    std::string m_quality = "hd";       // 视频质量,画质模式：hd：高画质 / sd：标清画质 / auto：自动选择
};

// 响应里回带的状态快照。每条响应都带全套字段，客户端只认服务端回传的
// 实际值，不需要再发一条查询确认——也就没有"请求成功但状态对不上"的窗口。
// 客户端发：START sessionId=123456 transport=udp host=192.168.1.100 port=5000 profile=balanced mediaProfile=1080p device=0 quality=hd...
// 服务端回：OK status=streaming profile=1080p fps=30 codec=h264 device=0...
// 会话状态结构体(服务端回传的状态快照)
struct StreamingStatus {
    StreamingState m_state = StreamingState::Idle; // 会话状态
    std::string m_sessionId = "none";
    std::string m_transport = "none";
    std::string m_host = "none";    
    int m_port = 0;
    std::string m_profile = "balanced";
    std::string m_mediaProfile = "none";
    std::string m_activeNetworkProfile = "none"; /**< 自动策略下实际生效的网络档位 */
    std::string m_codec = "none";
    std::string m_device = "none";
    std::string m_mediaKeyHex;   /**< 媒体密钥,媒体面 AES-GCM 加密密钥，仅成功 START 响应下发；空表示本会话未加密 */
    std::string m_colorimetry;   /**< 颜色空间,bt601 / bt709，客户端据此选 YUV→RGB 矩阵 */
    std::string m_quality = "hd";
    std::string m_lastError; // 最后错误信息,空表示无错误
   };

// 流媒体状态枚举转字符串(用于日志打印)
// 逐值 case 让 -Wswitch 能查出漏分支；末尾兜底 return 防未来新增枚举值未处理。
inline const char *streamingStateName(StreamingState state) {
    switch (state) {
        case StreamingState::Idle:
            return "Idle";
        case StreamingState::Starting:
            return "Starting";
        case StreamingState::Running:
            return "Running";
        case StreamingState::Stopping:
            return "Stopping";
        case StreamingState::Failed:
            return "Failed";
    }
    return "Failed";
}

// ==================== 应用端口抽象 ====================

// control 只依赖这个契约，不依赖 app 的实现类型；app 实现它，但不关心
// 命令是从 TCP 文本还是别的通道来的。两边因此可以各自替换、各自测试——
// 测协议层时注入一个假实现就能把解析到响应的流程跑全。
class StreamingController {
public:
    virtual ~StreamingController() = default;

    // 启动流媒体会话,参数: request 流媒体请求, errorMessage 错误信息指针：如果失败，将错误信息写入指针指向的字符串
    virtual bool startStreaming(const StreamingRequest &request,
                                std::string *errorMessage = nullptr) = 0;

    // 停止流媒体会话,参数: sessionId 会话标识, errorMessage 错误信息指针：如果失败，将错误信息写入指针指向的字符串
    virtual bool stopStreaming(const std::string &sessionId,
                               std::string *errorMessage = nullptr) = 0;

    // 获取流媒体会话状态,参数: sessionId 会话标识,返回值: 流媒体状态
    virtual StreamingStatus getStreamingStatus(const std::string &sessionId) const = 0;

    // 带默认空实现的虚函数：实现类只覆盖自己关心的能力。
    // (void) 转换用于消除"形参未使用"的告警。
    // 刷新流媒体会话租约,参数: sessionId 会话标识, peerIp 对方 IP 地址, mediaPort 媒体端口
    // 在分布式系统或网络编程中，租约(Lease) = 带有时间限制的授权
    virtual void refreshStreamingLease(const std::string &sessionId,
                                       const std::string &peerIp,
                                       int mediaPort) {
        (void)sessionId;
        (void)peerIp;
        (void)mediaPort;
    }

    // 由控制 worker 调用，绝不能跑在 I/O 反应器上——它可能去停管线。
    // 重新分配过期的流媒体会话
    virtual void reapExpiredSessions() {}

    // 请求关键帧,输入参数: sessionId 会话标识,输出参数: errorMessage 错误信息指针：如果失败，将错误信息写入指针指向的字符串
    // 输出参数: requestId 请求 ID指针：如果成功，将请求 ID写入指针指向的变量
    virtual bool requestKeyframe(const std::string &sessionId,
                                 std::string *errorMessage = nullptr,
                                 std::uint64_t *requestId = nullptr) = 0;
};

} // namespace contracts
} // namespace sserver


#endif // SSERVER_CONTRACTS_STREAMING_CONTROL_H
