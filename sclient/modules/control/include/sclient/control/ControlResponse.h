// ============================================================
// 文件：ControlResponse.h
// 模块：sclient::control
// 功能：控制协议响应的结构化表示与解析：OK/ERR 成败、错误码与描述、
//       以及服务端回带的全部 key=value 状态字段。
// 知识点：单行响应解析、值语义结果、纯函数便于单测
// ============================================================

#ifndef SCLIENT_CONTROL_CONTROL_RESPONSE_H
#define SCLIENT_CONTROL_CONTROL_RESPONSE_H

#include <map>
#include <string>

namespace sclient {
namespace control {

// 服务端每条响应都带全套状态字段，客户端的原则是"只认回传值、不信自己的
// 预期"：媒体端口由服务端分配、档位可能被规范化，所以这里必须能取任意字段。
struct ControlResponse {
    bool m_ok = false;
    std::string m_errorCode;
    std::string m_errorMessage;
    std::map<std::string, std::string> m_fields;

    // 未命中返回空串。
    std::string field(const std::string &name) const;
    // 数值字段：未命中或无法解析时返回 fallback。
    int intField(const std::string &name, int fallback = 0) const;
};

// 解析一行响应（不含换行符）。格式：
//   OK key=value key=value ...
//   ERR code=xxx message=yyy key=value ...
// 无法识别的形状（首词既不是 OK 也不是 ERR）返回 m_ok=false 且 code 为空，
// 由调用方当成协议错误处理。
ControlResponse parseControlResponse(const std::string &line);

}  // namespace control
}  // namespace sclient

#endif  // SCLIENT_CONTROL_CONTROL_RESPONSE_H