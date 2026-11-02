// Logger 文件通道：落盘、轮转命名与关停归档的集成测试。
//
// 用 MEDIA_LOG_DIRECTORY 把日志固定到临时目录，测完整体删除；不碰仓库里的
// logs/ 目录，也不依赖运行目录。
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "sserver/logger/Logger.h"
#include "support/Check.h"

namespace {

using sserver::logger::Logger;

void setEnvironmentVariable(const char *name, const char *value) {
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

std::string readFile(const std::string &path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

bool contains(const std::string &text, const std::string &needle) {
    return text.find(needle) != std::string::npos;
}

}  // namespace

int main() {
    // 单例在首次使用时按环境变量初始化，所以必须在任何 Logger 调用之前设置。
    const auto now = std::chrono::system_clock::now().time_since_epoch().count();
    const std::filesystem::path logDirectory =
            std::filesystem::temp_directory_path()
            / ("sserver_logger_test_" + std::to_string(now));
    setEnvironmentVariable("MEDIA_LOG_DIRECTORY", logDirectory.string().c_str());
    setEnvironmentVariable("MEDIA_LOG_FILE", "1");

    Logger::infoEvent("transport", "udp_started", "session-1",
                      {{"port", "9001"}, {"reason", "manual start"}});
    Logger::warnEvent("transport", "retransmit_throttled", "session-1",
                      {{"count", "3"}});
    Logger::flush();

    const std::string activePath = Logger::currentLogFilePath();
    CHECK(!activePath.empty());
    CHECK(contains(activePath, "server_"));
    CHECK(contains(activePath, "_running.log"));
    CHECK(std::filesystem::exists(activePath));

    const std::string content = readFile(activePath);
    CHECK(contains(content, "component=transport"));
    CHECK(contains(content, "event=udp_started"));
    CHECK(contains(content, "session_id=session-1"));
    CHECK(contains(content, "port=9001"));
    // 含空格的值必须带引号落盘，否则下游按空格切分就会把一条日志拆成两半。
    CHECK(contains(content, "reason=\"manual start\""));

    // warn 级事件自动补齐 error_code / error，排障时不会出现"失败但没原因"。
    CHECK(contains(content, "event=retransmit_throttled"));
    CHECK(contains(content, "error_code=retransmit_throttled"));
    CHECK(contains(content, "error=\"recoverable failure\""));

    const std::string finalPath = Logger::shutdown();
    CHECK(finalPath != activePath);
    CHECK(!contains(finalPath, "_running.log"));
    CHECK(contains(finalPath, "server_"));
    CHECK(std::filesystem::exists(finalPath));
    // 归档后 running 文件应当消失，否则同一时段会留下两份内容重叠的日志。
    CHECK(!std::filesystem::exists(activePath));

    // 幂等：重复调用返回同一路径，不重复改名。
    CHECK(Logger::shutdown() == finalPath);
    CHECK(std::filesystem::exists(finalPath));

    const std::string finalContent = readFile(finalPath);
    CHECK(contains(finalContent, "event=udp_started"));

    std::error_code cleanupError;
    std::filesystem::remove_all(logDirectory, cleanupError);

    return sserver::test::finish("logger_file_test");
}
