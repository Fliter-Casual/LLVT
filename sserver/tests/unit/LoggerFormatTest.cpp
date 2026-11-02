// Logger 格式化：logfmt 转义、保留键过滤、失败字段补全的单元测试。
//
// 只测纯函数，不初始化日志器、不写文件——日志格式是两端约定的一部分，
// 必须能脱离 I/O 精确断言。
#include <cstdlib>
#include <string>

#include "sserver/logger/Logger.h"
#include "support/Check.h"

namespace {

using sserver::logger::completeFailureFields;
using sserver::logger::formatEventMessage;
using sserver::logger::LogFields;

bool contains(const std::string &text, const std::string &needle) {
    return text.find(needle) != std::string::npos;
}

void testBasicLine() {
    const std::string line = formatEventMessage(
            "transport", "udp_started", "session-7",
            {{"port", "9001"}, {"profile", "auto"}});
    CHECK(line == "component=transport event=udp_started session_id=session-7"
                   " port=9001 profile=auto");
}

void testReservedKeysAreDropped() {
    // fields 里混入保留键时必须被丢弃，否则一行里出现两个 session_id，
    // 按字段解析的工具无法判断以哪个为准。
    const std::string line = formatEventMessage(
            "control", "start", "s1",
            {{"component", "evil"}, {"event", "evil"}, {"session_id", "evil"},
             {"action", "start"}});
    CHECK(line == "component=control event=start session_id=s1 action=start");
    CHECK(line.find("evil") == std::string::npos);
}

void testEmptyNamesFallBack() {
    const std::string line = formatEventMessage("", "", "", {});
    CHECK(line == "component=unknown event=unknown session_id=none");
}

void testKeySanitization() {
    // 键名只允许字母数字与 _ . -，其余替换成下划线，保证可被 grep 与索引。
    const std::string line = formatEventMessage(
            "comp", "evt", "s", {{"bad key", "1"}, {"ok.key-1", "2"}});
    CHECK(line == "component=comp event=evt session_id=s bad_key=1 ok.key-1=2");
}

void testPlainValuesAreNotQuoted() {
    const std::string line = formatEventMessage("c", "e", "s", {{"path", "/dev/video0"}});
    CHECK(contains(line, "path=/dev/video0"));
}

void testValuesWithSeparatorsAreQuoted() {
    // 含空白或 '=' 的值不加引号，下游按空格/等号切分时就会错位。
    CHECK(contains(formatEventMessage("c", "e", "s", {{"reason", "no device"}}),
                   "reason=\"no device\""));
    CHECK(contains(formatEventMessage("c", "e", "s", {{"expr", "a=b"}}),
                   "expr=\"a=b\""));
    CHECK(contains(formatEventMessage("c", "e", "s", {{"empty", ""}}),
                   "empty=\"\""));
}

void testControlCharactersAreEscaped() {
    CHECK(contains(formatEventMessage("c", "e", "s", {{"k", "line\nbreak"}}),
                   "k=\"line\\nbreak\""));
    CHECK(contains(formatEventMessage("c", "e", "s", {{"k", "quote\"inside"}}),
                   "k=\"quote\\\"inside\""));
    CHECK(contains(formatEventMessage("c", "e", "s", {{"k", "back\\slash"}}),
                   "k=\"back\\\\slash\""));
    CHECK(contains(formatEventMessage("c", "e", "s", {{"k", std::string("bell\x07")}}),
                   "k=\"bell\\u0007\""));
}

void testUtf8ValuesPassThrough() {
    // 高位字节（UTF-8 多字节）不算控制字符，加引号但内容原样保留。
    const std::string line = formatEventMessage("c", "e", "s", {{"reason", "设备离线"}});
    CHECK(contains(line, "reason=\"设备离线\""));
}

void testFailureFieldsAreCompleted() {
    const LogFields completed = completeFailureFields("start_failed", {}, "operation failed");
    CHECK(completed.size() == 2);
    CHECK(completed[0].m_key == "error_code");
    CHECK(completed[0].m_value == "start_failed");
    CHECK(completed[1].m_key == "error");
    CHECK(completed[1].m_value == "operation failed");
}

void testFailureFieldsKeepExplicitValues() {
    // 调用方给了具体原因就不能被默认值覆盖。
    const LogFields completed = completeFailureFields(
            "start_failed", {{"error_code", "ENODEV"}, {"error", "no such device"}},
            "operation failed");
    CHECK(completed.size() == 2);
    CHECK(completed[0].m_value == "ENODEV");
    CHECK(completed[1].m_value == "no such device");
}

void testFailureFieldsAddOnlyMissing() {
    const LogFields completed = completeFailureFields(
            "start_failed", {{"error", "no such device"}}, "operation failed");
    CHECK(completed.size() == 2);
    CHECK(completed[0].m_key == "error");
    CHECK(completed[1].m_key == "error_code");
    CHECK(completed[1].m_value == "start_failed");
}

void testFileLoggingCanBeDisabled() {
    // MEDIA_LOG_FILE=0 时只留控制台输出、不创建文件。必须在任何日志调用
    // 之前设置：单例在首次使用时按当时的环境变量初始化。
#if defined(_WIN32)
    _putenv_s("MEDIA_LOG_FILE", "0");
#else
    setenv("MEDIA_LOG_FILE", "0", 1);
#endif
    CHECK(sserver::logger::Logger::currentLogFilePath().empty());
}

}  // namespace

int main() {
    testBasicLine();
    testReservedKeysAreDropped();
    testEmptyNamesFallBack();
    testKeySanitization();
    testPlainValuesAreNotQuoted();
    testValuesWithSeparatorsAreQuoted();
    testControlCharactersAreEscaped();
    testUtf8ValuesPassThrough();
    testFailureFieldsAreCompleted();
    testFailureFieldsKeepExplicitValues();
    testFailureFieldsAddOnlyMissing();
    testFileLoggingCanBeDisabled();
    return sserver::test::finish("logger_format_test");
}
