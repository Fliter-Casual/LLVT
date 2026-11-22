// ============================================================
// 文件：GuiRunnerStub.cpp
// 模块：sclient::app
// 功能：未链接 Qt6 时的占位实现：明确告知"这个构建没有图形界面"。
// 知识点：同一接口的两套实现、明确的失败信息优于链接错误
// ============================================================

#include "sclient/app/GuiRunner.h"

#include <cstdio>

namespace sclient {
namespace app {

bool guiAvailable() {
    return false;
}

int runGuiClient(const GuiOptions &) {
    std::fprintf(stderr,
                 "this build has no Qt6 support: install the Qt6 development packages\n"
                 "and reconfigure to get the window client. The command-line client works\n"
                 "without it (omit --gui).\n");
    return 1;
}

}  // namespace app
}  // namespace sclient
