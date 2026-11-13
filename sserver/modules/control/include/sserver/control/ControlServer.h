// ============================================================
// 文件：ControlServer.h
// 模块：sserver::control
// 功能：TCP 文本控制服务：监听、逐行解析、调用 StreamingController、
//       回写单行响应。
// 知识点：监听套接字生命周期、select 限时等待与优雅停止、
//         接口注入（只依赖 contracts）
// ============================================================

#ifndef SSERVER_CONTROL_CONTROL_SERVER_H
#define SSERVER_CONTROL_CONTROL_SERVER_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "sserver/contracts/StreamingControl.h"

namespace sserver {
namespace control {

struct ControlServerConfig {
    std::string m_bindAddress = "0.0.0.0";
    int m_port = 9000;
    std::size_t m_maxLineLength = 1024;
    // CAPS 返回的档位白名单；现阶段只有合成链路，给一个占位名。
    std::vector<std::string> m_availableMediaProfiles{"default"};
};

// 当前实现是单线程串行处理：接受一个连接，逐行处理到客户端关闭，再接受
// 下一个。这样够用于联调与手工 nc 验证，但一个慢客户端会挡住其他客户端。
// 反应器 + 命令线程池（文档里的设计）等内容多点再换，接口不变。
class ControlServer {
public:
    ControlServer(contracts::StreamingController &controller, ControlServerConfig config);
    ~ControlServer();

    ControlServer(const ControlServer &) = delete;
    ControlServer &operator=(const ControlServer &) = delete;

    bool start(std::string *errorMessage);
    void stop();

    bool running() const { return m_running.load(std::memory_order_acquire); }
    // start 之后是内核实际分配的端口（配置为 0 时有用）。
    int boundPort() const { return m_boundPort; }

private:
    void acceptLoop();
    void handleConnection(std::intptr_t handle, const std::string &peerAddress);
    std::string handleLine(const std::string &line, const std::string &peerAddress);

    contracts::StreamingController &m_controller;
    ControlServerConfig m_config;
    std::intptr_t m_listenHandle = -1;
    int m_boundPort = 0;
    std::atomic<bool> m_running{false};
    std::thread m_thread;
};

}  // namespace control
}  // namespace sserver

#endif  // SSERVER_CONTROL_CONTROL_SERVER_H
