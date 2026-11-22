// ============================================================
// 文件：GuiRunner.h
// 模块：sclient::app
// 功能：图形界面模式的入口声明。实现分两份编译单元：链接 Qt6 的版本与
//       不链接时的占位版本，由 CMake 二选一。
// 知识点：同一接口的两套实现（有条件编译的替代做法）、接口隔离
// ============================================================

#ifndef SCLIENT_APP_GUI_RUNNER_H
#define SCLIENT_APP_GUI_RUNNER_H

#include <string>

namespace sclient {
namespace app {

// 图形界面模式的选项。
struct GuiOptions {
    std::string m_host = "127.0.0.1";
    int m_controlPort = 9000;
    std::string m_sessionId = "gui-1";
};

// 当前构建是否包含图形界面（取决于是否找到 Qt6）。
bool guiAvailable();

// 运行窗口版客户端：连接 → START → 注册 → 解码上屏 → 关窗时 STOP。
// 返回进程退出码。
int runGuiClient(const GuiOptions &options);

}  // namespace app
}  // namespace sclient

#endif  // SCLIENT_APP_GUI_RUNNER_H