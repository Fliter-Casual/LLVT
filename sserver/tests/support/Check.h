// ============================================================
// 文件：Check.h
// 模块：tests/support
// 功能：无外部依赖的断言宏。不用 assert，因为 NDEBUG 构建下 assert 会被
//       编译掉，测试会静默通过；这里无论构建类型都保留检查与失败计数。
// ============================================================

#ifndef SSERVER_TESTS_SUPPORT_CHECK_H
#define SSERVER_TESTS_SUPPORT_CHECK_H

#include <cstdio>

namespace sserver {
namespace test {

inline int &failureCount() {
    static int count = 0;
    return count;
}

inline void reportFailure(const char *file, int line, const char *expression) {
    std::fprintf(stderr, "  %s:%d: CHECK failed: %s\n", file, line, expression);
    ++failureCount();
}

// 每个测试可执行文件在 main 末尾调用一次：打印结果并返回进程退出码。
inline int finish(const char *name) {
    if (failureCount() == 0) {
        std::printf("[PASS] %s\n", name);
        return 0;
    }
    std::printf("[FAIL] %s (%d check(s) failed)\n", name, failureCount());
    return 1;
}

}  // namespace test
}  // namespace sserver

#define CHECK(expression)                                                        \
    do {                                                                         \
        if (!(expression)) {                                                     \
            ::sserver::test::reportFailure(__FILE__, __LINE__, #expression);      \
        }                                                                        \
    } while (0)

#endif  // SSERVER_TESTS_SUPPORT_CHECK_H