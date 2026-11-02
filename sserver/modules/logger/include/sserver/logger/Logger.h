// ============================================================
// 文件：Logger.h
// 模块：sserver::logger
// 功能：声明全局静态日志工具 Logger：四级别纯文本日志与 logfmt 结构化事件
//       日志，同时输出到控制台与轮转文件，供全部模块记录运行状态与故障
//       诊断信息。
// 知识点：静态工具类、enum class 日志级别、logfmt 键值对、spdlog 轮转 sink
// ============================================================

#ifndef SSERVER_LOGGER_LOGGER_H
#define SSERVER_LOGGER_LOGGER_H

#include <string>
#include <vector>

namespace sserver {
namespace logger {

enum class LogLevel {
    Debug,
    Info,
    Warn,
    Error,
};

struct LogField {
    std::string m_key;
    std::string m_value;
};

using LogFields = std::vector<LogField>;

// ==================== 纯格式化函数（可独立单测） ====================

// 把一次事件拼成单行 logfmt 文本：
//   component=<name> event=<name> session_id=<id> [key=value ...]
//
// component / event / session_id 是保留键，fields 里同名的条目会被丢弃，
// 否则一条日志里出现两个 session_id，按字段解析的工具就不知道信哪个。
//
// 键名做白名单清洗（非法字符换成 '_'），值做 logfmt 转义（含空白、引号、
// '=' 或控制字符时整体加引号并转义）。暴露成独立函数是为了让格式本身可以
// 脱离 spdlog 单测——日志格式是两端约定的一部分，不能只靠肉眼看输出。
std::string formatEventMessage(const std::string &component,
                               const std::string &event,
                               const std::string &sessionId,
                               const LogFields &fields);

// warn/error 事件缺少 error_code / error 时补默认值：这两个字段是排障时
// 第一眼要找的东西，缺了会让"失败"看起来像"没有原因"。
LogFields completeFailureFields(const std::string &event,
                                const LogFields &fields,
                                const char *fallbackError);

// ==================== 日志器 ====================

// 进程内共享一份配置的静态工具类：不持有实例，等价于受命名空间约束的
// 自由函数集合。
//
// 输出分三路：info/debug/warn 走 stdout，error 走 stderr，另有一份轮转文件。
// 运行中文件名是 server_<开始时间>_running.log；调用 shutdown() 关闭文件
// sink 后，按 server_<开始时间>_<结束时间>.log 定稿，轮转分片一并改名，
// 一次运行的日志整组归档、不会和别的运行混在一起。
class Logger {
public:
    // 打开后 Debug 级别也会输出；未调用时由环境变量 MEDIA_LOG_VERBOSE 决定。
    static void setVerbose(bool verbose);

    // 当前日志文件路径：未启用文件日志时为空，shutdown 后为定稿路径。
    static std::string currentLogFilePath();

    static void flush();

    // 关闭文件 sink 并按起止时间定稿。幂等，重复调用返回同一路径。
    static std::string shutdown();

    // ==================== 纯文本日志 ====================
    // 旧式接口只是结构化事件的薄包装，最终走同一出口，格式不会分叉。

    static void debug(const std::string &message);
    static void info(const std::string &message);
    static void warn(const std::string &message);
    static void error(const std::string &message);

    // ==================== 结构化事件日志（logfmt） ====================

    static void debugEvent(const std::string &component, const std::string &event,
                           const std::string &sessionId, const LogFields &fields = LogFields());
    static void infoEvent(const std::string &component, const std::string &event,
                          const std::string &sessionId, const LogFields &fields = LogFields());
    static void warnEvent(const std::string &component, const std::string &event,
                          const std::string &sessionId, const LogFields &fields = LogFields());
    static void errorEvent(const std::string &component, const std::string &event,
                           const std::string &sessionId, const LogFields &fields = LogFields());

private:
    static void log(LogLevel level, const std::string &message);
};

} // namespace logger
} // namespace sserver

#endif // SSERVER_LOGGER_LOGGER_H