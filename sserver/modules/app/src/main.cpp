// ============================================================
// 文件：main.cpp
// 模块：sserver::app
// 功能：服务端入口：解析参数、装载并校验配置、构造 ServerApp 与控制服务、
//       等退出信号后按"先停控制面、再停会话"的顺序收尾。
// 知识点：建线程前安装信号处理、配置错误即拒绝启动、按顺序停机
// ============================================================

#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "sserver/app/ServerApp.h"
#include "sserver/appconfig/AppConfig.h"
#include "sserver/control/ControlServer.h"
#include "sserver/logger/Logger.h"

namespace {

volatile std::sig_atomic_t g_stopRequested = 0;

void handleSignal(int) {
    // 信号处理器里只置标志：线程里做任何别的事都不安全。
    g_stopRequested = 1;
}

void printUsage(const char *program) {
    std::fprintf(stderr,
                 "usage: %s [--config <path>] [--control-port <port>]\n"
                 "  --config        配置路径，默认 config/control.toml\n"
                 "  --control-port  覆盖配置文件里的控制端口（0 表示内核分配）\n",
                 program);
}

}  // namespace

int main(int argc, char **argv) {
    // 必须在创建任何线程之前安装：否则信号可能落到某个工作线程上，
    // 处理函数就得考虑线程安全，而它本来只需要动一个标志。
    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    std::string configPath = "config/control.toml";
    int controlPortOverride = -1;
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--config") == 0 && index + 1 < argc) {
            configPath = argv[++index];
        } else if (std::strcmp(argv[index], "--control-port") == 0 && index + 1 < argc) {
            controlPortOverride = std::atoi(argv[++index]);
            if (controlPortOverride < 0 || controlPortOverride > 65535) {
                std::fprintf(stderr, "invalid control port\n");
                return 2;
            }
        } else {
            printUsage(argv[0]);
            return 2;
        }
    }

    sserver::appconfig::AppConfig config;
    std::string error;
    if (!sserver::appconfig::ConfigLoader::loadFromFile(configPath, &config, &error)) {
        // 配置错误必须在启动时报告：带着错误配置跑起来比直接退出更糟。
        std::fprintf(stderr, "failed to load config: %s\n", error.c_str());
        return 1;
    }

    sserver::logger::Logger::setVerbose(config.m_runtime.m_logVerbose);

    sserver::app::ServerApp server(config);

    sserver::control::ControlServerConfig controlConfig;
    controlConfig.m_bindAddress = config.m_control.m_bindAddress;
    controlConfig.m_port = controlPortOverride >= 0 ? controlPortOverride
                                                    : config.m_control.m_port;
    sserver::control::ControlServer controlServer(server, controlConfig);
    if (!controlServer.start(&error)) {
        std::fprintf(stderr, "failed to start control server: %s\n", error.c_str());
        return 1;
    }

    sserver::logger::Logger::infoEvent(
            "app", "server_started", "none",
            {{"control_port", std::to_string(controlServer.boundPort())},
             {"config", configPath},
             {"synthetic", "true"}});

    while (g_stopRequested == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // 先停控制面（不再接受新命令），再回收会话（停管线与线程）。
    controlServer.stop();
    server.shutdown();
    sserver::logger::Logger::infoEvent("app", "server_stopped", "none", {});
    // 关停文件 sink 并按起止时间归档本次运行日志。
    sserver::logger::Logger::shutdown();
    return 0;
}
