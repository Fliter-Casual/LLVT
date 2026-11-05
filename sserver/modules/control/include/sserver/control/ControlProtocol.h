// ============================================================
// 文件：ControlProtocol.h
// 模块：sserver::control
// 功能：单行文本控制协议的解析与应答格式化：命令类型枚举、命令解析，
//       以及 OK / ERR / CAPS 三类响应的拼装。纯函数，不含 socket 与线程。
// 知识点：行分帧协议、枚举命令分发、字段白名单与重复检测、
//         输出参数回传细粒度错误码
// ============================================================

#ifndef SSERVER_CONTROL_CONTROL_PROTOCOL_H
#define SSERVER_CONTROL_CONTROL_PROTOCOL_H

#include <cstddef>
#include <string>
#include <vector>

#include "sserver/contracts/StreamingControl.h"

namespace sserver {
namespace control {

// 协议层的绝对上限，与运行期可配置的 max_line_length 构成两级防护：
// 连接层先用配置值挡住绝大多数超长输入，这里再兜一道，防止读取端
// 在校验之前就为一个畸形连接分配巨量内存。
constexpr std::size_t kAbsoluteMaxControlLineLength = 4096;

enum class CommandType {
    Status,
    Start,
    Stop,
    Keyframe,
    Caps,
};

// 一个结构体承载全部命令的参数，靠 m_type 决定哪些字段有效。
struct ControlCommand {
    CommandType m_type = CommandType::Status;
    contracts::StreamingRequest m_request; /**< 仅 Start 填充 */
    std::string m_targetSession;            /**< STATUS / STOP / KEYFRAME 的目标会话 */
    int m_mediaPort = 0;                    /**< 仅 STATUS：客户端媒体源端口，0 表示未携带 */
};

/**
 * 解析一行控制命令。
 *
 * 输入必须是可见 ASCII、无首尾空格、无连续空格，长度不超过
 * kAbsoluteMaxControlLineLength。START 的字段走白名单 + 重复键检测，
 * transport / port / session / profile / quality / enc 必填，
 * media_profile 与 device 可选，host 禁止出现非空值。
 *
 * @param command 仅解析成功时写入；失败时保持原值（不留半解析结果）
 * @param errorCode 非空时写入细粒度错误码，如 invalid_line / unknown_field
 */
bool parseControlCommand(const std::string &line,
                         ControlCommand *command,
                         std::string *errorCode);

// ==================== 应答格式化 ====================

// 只有成功 START 才需要带上密钥材料；状态查询和错误响应一律不带。
std::string formatOkResponse(const contracts::StreamingStatus &status,
                             bool includeMediaKey = false);

std::string formatErrorResponse(const std::string &code,
                                const std::string &message,
                                const contracts::StreamingStatus &status);

// CAPS：逗号分隔的可用档位 + 常规状态字段；空列表输出 none（保证客户端
// 按空格分词时字段数恒定）。
std::string formatCapsResponse(const std::vector<std::string> &availableMediaProfiles,
                               const contracts::StreamingStatus &status);

}  // namespace control
}  // namespace sserver

#endif  // SSERVER_CONTROL_CONTROL_PROTOCOL_H
