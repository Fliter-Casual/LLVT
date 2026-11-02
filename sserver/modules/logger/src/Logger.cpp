// ============================================================
// 文件：Logger.cpp
// 模块：sserver::logger
// 功能：基于 spdlog 的日志后端实现。控制台按级别分流（error 走 stderr，
//       其余走 stdout），另有轮转文件输出；把 component/event/session_id
//       与自定义字段组装成 logfmt 行，关停时把日志文件按起止时间归档。
// 知识点：Meyers 单例、std::atomic 运行时开关、std::filesystem、
//         spdlog 自定义 formatter 标志、logfmt 转义、轮转文件 sink
// ============================================================

#include "sserver/logger/Logger.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#else
#include <limits.h>
#include <unistd.h>
#endif

#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace sserver {
namespace logger {

namespace {

// ==================== 常量 ====================

constexpr std::size_t kMaximumLogFileSize = 10U * 1024U * 1024U;
constexpr std::size_t kRotatedLogFileCount = 5U;

// %J 是本文件注册的自定义级别标志：默认的 %l 在 spdlog 各版本间会输出
// "error" 或 "critical"，两端日志按级别 grep 时不稳定，所以自己渲染。
constexpr const char *kConsolePattern =
        "%Y-%m-%dT%H:%M:%S.%e%z [%^%J%$] app=sserver pid=%P tid=%t %v";
constexpr const char *kFilePattern =
        "%Y-%m-%dT%H:%M:%S.%e%z [%J] app=sserver pid=%P tid=%t %v";

// ==================== 环境变量开关 ====================

bool environmentFlag(const char *name, bool defaultValue) {
    const char *value = std::getenv(name);
    if (value == nullptr) {
        return defaultValue;
    }
    const std::string text(value);
    return text != "0" && text != "false" && text != "off";
}

// 未设置 MEDIA_LOG_FILE 时默认开启文件日志；显式 0/false/off 才关闭。
bool fileLoggingEnabled() {
    return environmentFlag("MEDIA_LOG_FILE", true);
}

// Meyers 单例 + atomic：C++11 起局部 static 初始化自带线程安全，
// 包一层函数避免全局静态对象的初始化顺序问题。
std::atomic<bool> &verboseFlag() {
    static std::atomic<bool> verbose{environmentFlag("MEDIA_LOG_VERBOSE", false)};
    return verbose;
}

// ==================== 级别映射 ====================

spdlog::level::level_enum toSpdlogLevel(LogLevel level) {
    switch (level) {
        case LogLevel::Debug:
            return spdlog::level::debug;
        case LogLevel::Info:
            return spdlog::level::info;
        case LogLevel::Warn:
            return spdlog::level::warn;
        case LogLevel::Error:
            return spdlog::level::err;
    }
    return spdlog::level::info;
}

// ==================== 日志目录定位 ====================

std::string executablePath() {
#if defined(_WIN32)
    char buffer[4096] = {0};
    const DWORD size = GetModuleFileNameA(nullptr, buffer, sizeof(buffer));
    if (size == 0 || size >= sizeof(buffer)) {
        return std::string();
    }
    return std::string(buffer, size);
#else
    // /proc/self/exe 是指向当前进程可执行文件的符号链接；-1 给结尾空字符留位置。
    char buffer[PATH_MAX] = {0};
    const ssize_t size = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (size <= 0) {
        return std::string();
    }
    buffer[size] = '\0';
    return std::string(buffer);
#endif
}

// 从给定目录向上最多找 12 层，定位名为 sserver 的工程目录。
std::filesystem::path findServerRoot(std::filesystem::path directory) {
    for (int depth = 0; depth < 12 && !directory.empty(); ++depth) {
        if (directory.filename() == "sserver") {
            return directory;
        }
        const std::filesystem::path parent = directory.parent_path();
        if (parent == directory) {
            break;
        }
        directory = parent;
    }
    return std::filesystem::path();
}

// 优先级：MEDIA_LOG_DIRECTORY 显式指定 > MEDIA_LOG_ROOT/logs >
// 从可执行文件位置向上找工程根下的 logs > 当前目录的 "logs"。
// 显式变量排第一，测试和容器部署都靠它把日志固定到可预期的位置。
std::string resolveLogDirectory() {
    const char *overridePath = std::getenv("MEDIA_LOG_DIRECTORY");
    if (overridePath != nullptr && overridePath[0] != '\0') {
        return overridePath;
    }
    const char *rootOverride = std::getenv("MEDIA_LOG_ROOT");
    if (rootOverride != nullptr && rootOverride[0] != '\0') {
        return (std::filesystem::path(rootOverride) / "logs").string();
    }

    std::filesystem::path serverRoot;
    const std::string path = executablePath();
    if (!path.empty()) {
        serverRoot = findServerRoot(std::filesystem::path(path).parent_path());
    }
    if (serverRoot.empty()) {
        std::error_code error;
        serverRoot = findServerRoot(std::filesystem::current_path(error));
    }
    if (!serverRoot.empty()) {
        return (serverRoot / "logs").string();
    }
    return "logs";
}

bool ensureLogDirectory(const std::string &logDirectory) {
    if (logDirectory.empty()) {
        return false;
    }
    std::error_code error;
    const std::filesystem::path directory(logDirectory);
    if (std::filesystem::create_directories(directory, error)) {
        return true;
    }
    if (error) {
        return false;
    }
    return std::filesystem::is_directory(directory, error) && !error;
}

// ==================== 时间戳与文件名 ====================

// localtime 的非 _r 版本返回静态缓冲，多线程并发格式化会互相踩踏，
// 因此用平台的安全变体把结果写进调用方的结构体。
std::tm localTimeOf(std::time_t rawTime) {
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &rawTime);
#else
    localtime_r(&rawTime, &local);
#endif
    return local;
}

std::string formatTimestamp(std::chrono::system_clock::time_point timePoint) {
    const std::time_t rawTime = std::chrono::system_clock::to_time_t(timePoint);
    std::tm local = localTimeOf(rawTime);
    char timestamp[32] = {0};
    if (std::strftime(timestamp, sizeof(timestamp), "%Y.%m.%d.%H.%M", &local) == 0) {
        return "unknown";
    }
    return timestamp;
}

std::string buildActiveLogFilePath(const std::string &logDirectory,
                                   const std::string &startTimestamp) {
    return (std::filesystem::path(logDirectory)
            / ("server_" + startTimestamp + "_running.log")).string();
}

std::string buildFinalLogFilePath(const std::string &logDirectory,
                                  const std::string &startTimestamp,
                                  const std::string &endTimestamp) {
    return (std::filesystem::path(logDirectory)
            / ("server_" + startTimestamp + "_" + endTimestamp + ".log")).string();
}

std::filesystem::path rotatedPath(const std::filesystem::path &basePath, std::size_t index) {
    return basePath.parent_path()
            / (basePath.stem().string() + "." + std::to_string(index)
               + basePath.extension().string());
}

// 同一起止时间重名时追加 _2、_3 后缀，绝不覆盖既有日志。
std::filesystem::path uniqueFinalPath(const std::filesystem::path &requestedPath) {
    std::error_code error;
    if (!std::filesystem::exists(requestedPath, error)) {
        return requestedPath;
    }
    for (int duplicate = 2; duplicate < 10000; ++duplicate) {
        const std::filesystem::path candidate = requestedPath.parent_path()
                / (requestedPath.stem().string() + "_" + std::to_string(duplicate)
                   + requestedPath.extension().string());
        error.clear();
        if (!std::filesystem::exists(candidate, error)) {
            return candidate;
        }
    }
    return requestedPath;
}

// 把 running.log 重命名为带结束时间的归档名；已经轮转出去的 .1~.5 分片
// 一并改名，否则一次运行的日志会被拆成"归档文件 + 一堆带 running 后缀的
// 分片"，事后无法判断哪些文件属于同一次启动。
std::string finalizeLogFiles(const std::string &activePath,
                             const std::string &requestedFinalPath) {
    if (activePath.empty() || requestedFinalPath.empty()) {
        return activePath;
    }
    const std::filesystem::path active(activePath);
    std::error_code error;
    if (!std::filesystem::exists(active, error)) {
        return activePath;
    }
    const std::filesystem::path finalPath = uniqueFinalPath(requestedFinalPath);
    error.clear();
    // 同一文件系统内 rename 是原子操作，不会留下写了一半的日志。
    std::filesystem::rename(active, finalPath, error);
    if (error) {
        std::cerr << "failed to finalize log file '" << activePath
                  << "': " << error.message() << std::endl;
        return activePath;
    }
    for (std::size_t index = 1; index <= kRotatedLogFileCount; ++index) {
        const std::filesystem::path activeRotated = rotatedPath(active, index);
        error.clear();
        if (!std::filesystem::exists(activeRotated, error)) {
            continue;
        }
        error.clear();
        std::filesystem::rename(activeRotated, rotatedPath(finalPath, index), error);
        if (error) {
            std::cerr << "failed to finalize rotated log file '"
                      << activeRotated.string() << "': " << error.message() << std::endl;
        }
    }
    return finalPath.string();
}

// ==================== 全局状态 ====================

struct LoggerState {
    std::shared_ptr<spdlog::logger> m_stdoutLogger;
    std::shared_ptr<spdlog::logger> m_stderrLogger;
    std::shared_ptr<spdlog::logger> m_fileLogger;
    std::string m_filePath;
    std::string m_startTimestamp;
    bool m_shutdown = false;
};

std::mutex &loggerStateMutex() {
    static std::mutex mutex;
    return mutex;
}

// flush_on(warn)：warn 及以上立刻落盘，info/debug 留在缓冲里凑批。
// 注意这是 flush 不是 fsync，进程崩溃或断电时不承诺零丢失。
void configureLogger(const std::shared_ptr<spdlog::logger> &logger) {
    logger->set_level(verboseFlag().load() ? spdlog::level::debug : spdlog::level::info);
    logger->flush_on(spdlog::level::warn);
    logger->set_error_handler([](const std::string &message) {
        std::cerr << "spdlog error: " << message << std::endl;
    });
}

// ==================== 自定义级别标志 %J ====================

// 把级别渲染成固定小写别名（debug/info/warn/error），critical 归到 error，
// 保证输出格式不随 spdlog 版本变化。
class StableLevelFlag final : public spdlog::custom_flag_formatter {
public:
    void format(const spdlog::details::log_msg &message,
                const std::tm &,
                spdlog::memory_buf_t &destination) override {
        const char *level = "info";
        switch (message.level) {
            case spdlog::level::debug:
                level = "debug";
                break;
            case spdlog::level::info:
                level = "info";
                break;
            case spdlog::level::warn:
                level = "warn";
                break;
            case spdlog::level::err:
            case spdlog::level::critical:
                level = "error";
                break;
            default:
                break;
        }
        destination.append(level, level + std::strlen(level));
    }

    // formatter 会被多个 sink 复制使用，框架通过 clone() 多态拷贝派生对象。
    std::unique_ptr<spdlog::custom_flag_formatter> clone() const override {
        return std::unique_ptr<spdlog::custom_flag_formatter>(new StableLevelFlag());
    }
};

template <typename Sink>
void setLocalPattern(const std::shared_ptr<Sink> &sink, const char *pattern) {
    std::unique_ptr<spdlog::pattern_formatter> formatter(
            new spdlog::pattern_formatter(spdlog::pattern_time_type::local));
    formatter->add_flag<StableLevelFlag>('J');
    formatter->set_pattern(pattern);
    sink->set_formatter(std::move(formatter));
}

// ==================== logfmt 转义 ====================

bool isNameCharacter(char character) {
    return (character >= 'a' && character <= 'z')
            || (character >= 'A' && character <= 'Z')
            || (character >= '0' && character <= '9')
            || character == '_' || character == '.' || character == '-';
}

// 键名只允许白名单字符，其余换成 '_'：键名要能被 grep 和日志系统索引，
// 带空格或引号的键会让整行失去可解析性。空值回退到 fallback，避免
// "component= event=..." 这种带空值的畸形行。
std::string safeName(const std::string &value, const char *fallback) {
    if (value.empty()) {
        return fallback;
    }
    std::string safe;
    safe.reserve(value.size());
    for (char character : value) {
        safe.push_back(isNameCharacter(character) ? character : '_');
    }
    return safe;
}

// logfmt 值转义：普通值原样输出；含空白、引号、'='、反斜杠或非可打印字符
// 时整体加引号并转义，否则下游按 "key=value" 切分时会错位。
std::string escapeValue(const std::string &value) {
    // 按 unsigned char 遍历：char 若为有符号类型，>= 0x7f 的比较会先被
    // 整型提升成负数，把中文等高位字节误判成控制字符。
    bool quote = value.empty();
    for (unsigned char character : value) {
        if (character <= 0x20 || character >= 0x7f || character == '"'
            || character == '\\' || character == '=') {
            quote = true;
            break;
        }
    }
    if (!quote) {
        return value;
    }

    static const char kHex[] = "0123456789abcdef";
    std::string escaped;
    escaped.reserve(value.size() + 2);
    escaped.push_back('"');
    for (unsigned char character : value) {
        switch (character) {
            case '"':
                escaped += "\\\"";
                break;
            case '\\':
                escaped += "\\\\";
                break;
            case '\n':
                escaped += "\\n";
                break;
            case '\r':
                escaped += "\\r";
                break;
            case '\t':
                escaped += "\\t";
                break;
            default:
                if (character < 0x20 || character == 0x7f) {
                    escaped += "\\u00";
                    escaped.push_back(kHex[(character >> 4U) & 0x0fU]);
                    escaped.push_back(kHex[character & 0x0fU]);
                } else {
                    escaped.push_back(static_cast<char>(character));
                }
                break;
        }
    }
    escaped.push_back('"');
    return escaped;
}

bool hasField(const LogFields &fields, const std::string &key) {
    for (const LogField &field : fields) {
        if (field.m_key == key) {
            return true;
        }
    }
    return false;
}

// ==================== 初始化 ====================

LoggerState createLoggerState() {
    auto stdoutSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    setLocalPattern(stdoutSink, kConsolePattern);
    auto stderrSink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
    setLocalPattern(stderrSink, kConsolePattern);
    auto stdoutLogger = std::make_shared<spdlog::logger>("sserver.stdout", stdoutSink);
    auto stderrLogger = std::make_shared<spdlog::logger>("sserver.stderr", stderrSink);

    std::shared_ptr<spdlog::logger> fileLogger;
    std::string filePath;
    const std::string logDirectory = resolveLogDirectory();
    const std::string startTimestamp = formatTimestamp(std::chrono::system_clock::now());

    if (!fileLoggingEnabled()) {
        // 测试进程可以只留控制台输出。
    } else if (!ensureLogDirectory(logDirectory)) {
        std::cerr << "failed to create logs directory '" << logDirectory
                  << "'; file logging disabled" << std::endl;
    } else {
        filePath = buildActiveLogFilePath(logDirectory, startTimestamp);
        try {
            auto fileSink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                    filePath, kMaximumLogFileSize, kRotatedLogFileCount);
            setLocalPattern(fileSink, kFilePattern);
            fileLogger = std::make_shared<spdlog::logger>("sserver.file", fileSink);
        } catch (const spdlog::spdlog_ex &exception) {
            // 文件打不开只禁用文件日志：日志初始化问题不应该让整个进程起不来。
            std::cerr << "failed to initialize log file '" << filePath
                      << "': " << exception.what() << "; file logging disabled" << std::endl;
            filePath.clear();
        }
    }

    configureLogger(stdoutLogger);
    configureLogger(stderrLogger);
    if (fileLogger != nullptr) {
        configureLogger(fileLogger);
    }

    return {
            std::move(stdoutLogger),
            std::move(stderrLogger),
            std::move(fileLogger),
            std::move(filePath),
            startTimestamp,
            false};
}

LoggerState &loggerState() {
    static LoggerState state = createLoggerState();
    return state;
}

}  // namespace

// ==================== 纯格式化函数 ====================

std::string formatEventMessage(const std::string &component,
                               const std::string &event,
                               const std::string &sessionId,
                               const LogFields &fields) {
    std::string message = "component=" + safeName(component, "unknown")
            + " event=" + safeName(event, "unknown")
            + " session_id=" + escapeValue(sessionId.empty() ? std::string("none") : sessionId);
    for (const LogField &field : fields) {
        if (field.m_key == "component" || field.m_key == "event"
            || field.m_key == "session_id") {
            continue;
        }
        message += " " + safeName(field.m_key, "field") + "=" + escapeValue(field.m_value);
    }
    return message;
}

LogFields completeFailureFields(const std::string &event,
                                const LogFields &fields,
                                const char *fallbackError) {
    LogFields completed = fields;
    if (!hasField(completed, "error_code")) {
        completed.push_back({"error_code", safeName(event, "unspecified_error")});
    }
    if (!hasField(completed, "error")) {
        completed.push_back({"error", fallbackError});
    }
    return completed;
}

// ==================== 运行时配置与生命周期 ====================

void Logger::setVerbose(bool verbose) {
    // 先原子更新开关（供无锁读取方），再持锁统一切换已创建的 logger 级别。
    verboseFlag().store(verbose);
    std::lock_guard<std::mutex> lock(loggerStateMutex());
    const spdlog::level::level_enum level =
            verbose ? spdlog::level::debug : spdlog::level::info;
    LoggerState &state = loggerState();
    state.m_stdoutLogger->set_level(level);
    state.m_stderrLogger->set_level(level);
    if (state.m_fileLogger != nullptr) {
        state.m_fileLogger->set_level(level);
    }
}

std::string Logger::currentLogFilePath() {
    std::lock_guard<std::mutex> lock(loggerStateMutex());
    return loggerState().m_filePath;
}

void Logger::flush() {
    std::lock_guard<std::mutex> lock(loggerStateMutex());
    LoggerState &state = loggerState();
    state.m_stdoutLogger->flush();
    state.m_stderrLogger->flush();
    if (state.m_fileLogger != nullptr) {
        state.m_fileLogger->flush();
    }
}

std::string Logger::shutdown() {
    std::lock_guard<std::mutex> lock(loggerStateMutex());
    LoggerState &state = loggerState();
    if (state.m_shutdown) {
        return state.m_filePath;
    }
    state.m_stdoutLogger->flush();
    state.m_stderrLogger->flush();
    if (state.m_fileLogger != nullptr) {
        state.m_fileLogger->flush();
        // 先释放文件 sink 再改名：Windows 上文件被占用时 rename 会失败，
        // Linux 上虽然没有这个问题，但顺序对两端都是一样的。
        state.m_fileLogger.reset();
        state.m_filePath = finalizeLogFiles(
                state.m_filePath,
                buildFinalLogFilePath(resolveLogDirectory(),
                                      state.m_startTimestamp,
                                      formatTimestamp(std::chrono::system_clock::now())));
    }
    state.m_shutdown = true;
    return state.m_filePath;
}

// ==================== 纯文本接口 ====================

void Logger::debug(const std::string &message) {
    debugEvent("legacy", "message", "none", {{"message", message}});
}

void Logger::info(const std::string &message) {
    infoEvent("legacy", "message", "none", {{"message", message}});
}

void Logger::warn(const std::string &message) {
    warnEvent("legacy", "warning", "none",
              {{"error_code", "legacy_warning"}, {"error", message}});
}

void Logger::error(const std::string &message) {
    errorEvent("legacy", "operation_failed", "none",
               {{"error_code", "legacy_error"}, {"error", message}});
}

// ==================== 结构化事件接口 ====================

void Logger::debugEvent(const std::string &component, const std::string &event,
                        const std::string &sessionId, const LogFields &fields) {
    log(LogLevel::Debug, formatEventMessage(component, event, sessionId, fields));
}

void Logger::infoEvent(const std::string &component, const std::string &event,
                       const std::string &sessionId, const LogFields &fields) {
    log(LogLevel::Info, formatEventMessage(component, event, sessionId, fields));
}

void Logger::warnEvent(const std::string &component, const std::string &event,
                       const std::string &sessionId, const LogFields &fields) {
    log(LogLevel::Warn,
        formatEventMessage(component, event, sessionId,
                           completeFailureFields(event, fields, "recoverable failure")));
}

void Logger::errorEvent(const std::string &component, const std::string &event,
                        const std::string &sessionId, const LogFields &fields) {
    log(LogLevel::Error,
        formatEventMessage(component, event, sessionId,
                           completeFailureFields(event, fields, "operation failed")));
}

// ==================== 输出路由 ====================

// 一把互斥量串行化所有写入。spdlog 的 mt sink 本身线程安全，这里加锁
// 是为了拦住 shutdown() 释放文件 sink 与并发写日志之间的竞争——那条路径
// 上 m_fileLogger 会变成 nullptr。
void Logger::log(LogLevel level, const std::string &message) {
    std::lock_guard<std::mutex> lock(loggerStateMutex());
    LoggerState &state = loggerState();
    const spdlog::level::level_enum spdlogLevel = toSpdlogLevel(level);
    // 消息体作为 "{}" 的实参而不是格式串本身：日志内容里带花括号时
    // 直接当格式串会被 spdlog 解析成占位符。
    if (level == LogLevel::Error) {
        state.m_stderrLogger->log(spdlogLevel, "{}", message);
    } else {
        state.m_stdoutLogger->log(spdlogLevel, "{}", message);
    }
    if (state.m_fileLogger != nullptr) {
        state.m_fileLogger->log(spdlogLevel, "{}", message);
    }
}

}  // namespace logger
}  // namespace sserver