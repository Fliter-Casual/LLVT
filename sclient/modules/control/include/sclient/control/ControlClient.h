// ============================================================
// 文件：ControlClient.h
// 模块：sclient::control
// 功能：同步控制客户端：建立 TCP 连接、发一行命令、读一行响应并解析。
//       上层只需关心"命令 → 结构化结果"。
// 知识点：同步请求响应模型、连接超时与读取超时、平台套接字封装
// ============================================================

#ifndef SCLIENT_CONTROL_CONTROL_CLIENT_H
#define SCLIENT_CONTROL_CONTROL_CLIENT_H

#include <cstdint>
#include <string>

#include "sclient/control/ControlResponse.h"

namespace sclient {
namespace control {

// 单连接、串行请求响应：一个连接上一次只允许有一个未完成命令。
// 命令之间有依赖（先连接才能启动、启动成功才能要关键帧），串行处理省掉了
// 一堆交错状态；需要并发时由上层把命令排进队列，而不是在这里加锁。
class ControlClient {
public:
    ControlClient() = default;
    ~ControlClient();

    ControlClient(const ControlClient &) = delete;
    ControlClient &operator=(const ControlClient &) = delete;

    bool connect(const std::string &host, int port, int timeoutMs, std::string *errorMessage);
    void close();
    bool isConnected() const { return m_handle >= 0; }

    // 发送一行命令（自动补换行）并等待一行响应。
    // @return 成功收发并解析出 OK/ERR 时返回 true；网络或协议错误返回 false
    bool execute(const std::string &command, ControlResponse *response,
                 int timeoutMs, std::string *errorMessage);

private:
    std::intptr_t m_handle = -1;
};

}  // namespace control
}  // namespace sclient

#endif  // SCLIENT_CONTROL_CONTROL_CLIENT_H