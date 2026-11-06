// ControlProtocol：命令行解析与响应格式化的单元测试。
//
// 协议层只做语法判断，所有畸形输入都必须被明确拒绝，并且失败时不能污染
// 调用方对象——上层会把它当成"这条命令没发生过"。
#include <string>
#include <vector>

#include "sserver/contracts/StreamingControl.h"
#include "sserver/control/ControlProtocol.h"
#include "support/Check.h"

namespace {

using sserver::contracts::StreamingState;
using sserver::contracts::StreamingStatus;
using sserver::control::CommandType;
using sserver::control::ControlCommand;
using sserver::control::formatCapsResponse;
using sserver::control::formatErrorResponse;
using sserver::control::formatOkResponse;
using sserver::control::parseControlCommand;

bool contains(const std::string &text, const std::string &needle) {
    return text.find(needle) != std::string::npos;
}

// 解析"应该失败"的输入，并返回错误码，省掉每个用例重复三行。
std::string expectFailure(const std::string &line) {
    ControlCommand command;
    std::string error;
    const bool ok = parseControlCommand(line, &command, &error);
    CHECK(!ok);
    return error;
}

const char *kValidStart =
        "START transport=udp port=0 profile=auto session=s-1 quality=hd enc=1";

// ==================== STATUS / STOP / KEYFRAME / CAPS ====================

void testStatusWithSession() {
    ControlCommand command;
    std::string error;
    CHECK(parseControlCommand("STATUS session=abc-1", &command, &error));
    CHECK(command.m_type == CommandType::Status);
    CHECK(command.m_targetSession == "abc-1");
    CHECK(command.m_mediaPort == 0);
}

void testStatusWithMediaPort() {
    // 断流恢复后客户端换了 UDP 源端口，靠 media_port 上报；两键顺序任意。
    ControlCommand command;
    std::string error;
    CHECK(parseControlCommand("STATUS session=abc media_port=51234", &command, &error));
    CHECK(command.m_mediaPort == 51234);

    CHECK(parseControlCommand("STATUS media_port=51234 session=abc", &command, &error));
    CHECK(command.m_targetSession == "abc");
    CHECK(command.m_mediaPort == 51234);
}

void testStatusRejectsDuplicateAndUnknownFields() {
    CHECK(expectFailure("STATUS session=a session=b") == "duplicate_field");
    CHECK(expectFailure("STATUS session=a foo=1") == "unknown_field");
    CHECK(expectFailure("STATUS session=a media_port=0") == "invalid_media_port");
    // 裸 STATUS 只有一个词，参数数量没超限，落到"缺 session"分支。
    CHECK(expectFailure("STATUS") == "invalid_session");
    CHECK(expectFailure("STATUS session=a extra=1 more=2") == "unexpected_argument");
}

void testStatusRequiresValidSession() {
    CHECK(expectFailure("STATUS") == "invalid_session");
    CHECK(expectFailure("STATUS session=") == "unexpected_argument");
    // "none"/"legacy" 是状态字段的占位词，不能当会话号。
    CHECK(expectFailure("STATUS session=none") == "invalid_session");
    CHECK(expectFailure("STATUS session=legacy") == "invalid_session");
    CHECK(expectFailure("STATUS session=bad/id") == "invalid_session");
}

void testStopAndKeyframe() {
    ControlCommand command;
    std::string error;

    CHECK(parseControlCommand("STOP session=s1", &command, &error));
    CHECK(command.m_type == CommandType::Stop);
    CHECK(command.m_targetSession == "s1");

    CHECK(parseControlCommand("KEYFRAME session=s1", &command, &error));
    CHECK(command.m_type == CommandType::Keyframe);
    CHECK(command.m_targetSession == "s1");

    // 只接受恰好一个 session=<id> 参数。
    CHECK(expectFailure("STOP") == "unexpected_argument");
    CHECK(expectFailure("STOP session=s1 extra") == "unexpected_argument");
    CHECK(expectFailure("KEYFRAME session=") == "unexpected_argument");
}

void testCaps() {
    ControlCommand command;
    std::string error;
    CHECK(parseControlCommand("CAPS", &command, &error));
    CHECK(command.m_type == CommandType::Caps);
    CHECK(expectFailure("CAPS extra") == "unexpected_argument");
}

// ==================== START ====================

void testStartValid() {
    ControlCommand command;
    std::string error;
    CHECK(parseControlCommand(kValidStart, &command, &error));
    CHECK(command.m_type == CommandType::Start);
    CHECK(command.m_request.m_transport == "udp");
    CHECK(command.m_request.m_port == 0);
    CHECK(command.m_request.m_sessionId == "s-1");
    CHECK(command.m_request.m_profile == "auto");
    CHECK(command.m_request.m_quality == "hd");
    CHECK(command.m_request.m_encryptionRequested);
}

void testStartOptionalFields() {
    ControlCommand command;
    std::string error;
    const std::string line = std::string(kValidStart)
            + " media_profile=mjpeg_x264 device=/dev/video0";
    CHECK(parseControlCommand(line, &command, &error));
    CHECK(command.m_request.m_mediaProfile == "mjpeg_x264");
    CHECK(command.m_request.m_device == "/dev/video0");
}

void testStartRequiresMandatoryFields() {
    // 缺 enc：加密与否必须由客户端明说，服务端不替它决定。
    CHECK(expectFailure("START transport=udp port=0 profile=auto session=s1 quality=hd")
          == "invalid_start");
    // 缺 quality。
    CHECK(expectFailure("START transport=udp port=0 profile=auto session=s1 enc=0")
          == "invalid_start");
    // 缺 profile。
    CHECK(expectFailure("START transport=udp port=0 session=s1 quality=hd enc=0")
          == "invalid_start");
    // 缺 session。
    CHECK(expectFailure("START transport=udp port=0 profile=auto quality=hd enc=0")
          == "invalid_start");
}

void testStartRejectsHost() {
    // 媒体目的地由服务端决定，请求里带 host 说明客户端理解错了协议。
    CHECK(expectFailure(std::string(kValidStart) + " host=10.0.0.1") == "invalid_start");
}

void testStartFieldValidation() {
    CHECK(expectFailure("START transport=tcp port=0 profile=auto session=s1 quality=hd enc=0")
          == "invalid_transport");
    CHECK(expectFailure("START transport=udp port=70000 profile=auto session=s1 quality=hd enc=0")
          == "invalid_port");
    CHECK(expectFailure("START transport=udp port=-1 profile=auto session=s1 quality=hd enc=0")
          == "invalid_port");
    CHECK(expectFailure("START transport=udp port=abc profile=auto session=s1 quality=hd enc=0")
          == "invalid_port");
    CHECK(expectFailure("START transport=udp port=0 profile=fast session=s1 quality=hd enc=0")
          == "invalid_profile");
    CHECK(expectFailure("START transport=udp port=0 profile=auto session=s1 quality=4k enc=0")
          == "invalid_quality");
    CHECK(expectFailure("START transport=udp port=0 profile=auto session=s1 quality=hd enc=2")
          == "invalid_enc");
    CHECK(expectFailure("START transport=udp port=0 profile=auto session=s1 quality=hd enc=0 device=x$")
          == "invalid_device");
    CHECK(expectFailure("START transport=udp port=0 profile=auto session=s1 quality=hd enc=0 extra=1")
          == "unknown_field");
    CHECK(expectFailure("START transport=udp port=0 profile=auto session=s1 quality=hd enc=0 enc=1")
          == "duplicate_field");
    CHECK(expectFailure("START transport=udp port=0 profile=auto session=s1 quality=hd enc=0 =x")
          == "invalid_start_field");
}

void testStartRejectsPathTraversal() {
    // 协议层不做路径规范化，那就不放过可能的穿越写法。
    CHECK(expectFailure("START transport=udp port=0 profile=auto session=s1 quality=hd enc=0 device=../etc/passwd")
          == "invalid_device");
}

// ==================== 行格式与命令名 ====================

void testLineFormatRules() {
    CHECK(expectFailure("") == "invalid_line");
    CHECK(expectFailure(std::string(5000, 'A')) == "invalid_line");
    // 制表符与其他控制字符属不可见字符。
    CHECK(expectFailure("STATUS\tsession=s1") == "invalid_line");
    // 非 ASCII（UTF-8 中文）同样拒绝：协议只走可见 ASCII。
    CHECK(expectFailure("STATUS session=会话") == "invalid_line");
    // 规范形式：不允许首尾空格与连续空格。
    CHECK(expectFailure(" STATUS session=s1") == "invalid_spacing");
    CHECK(expectFailure("STATUS  session=s1") == "invalid_spacing");
    CHECK(expectFailure("STATUS session=s1 ") == "invalid_spacing");
}

void testUnknownCommand() {
    CHECK(expectFailure("status session=s1") == "unknown_command");
    CHECK(expectFailure("HELLO") == "unknown_command");
}

void testFailureLeavesCommandUntouched() {
    ControlCommand command;
    command.m_type = CommandType::Caps;
    command.m_targetSession = "keep";
    std::string error;
    CHECK(!parseControlCommand("STOP", &command, &error));
    CHECK(command.m_type == CommandType::Caps);
    CHECK(command.m_targetSession == "keep");
}

// ==================== 响应格式化 ====================

void testOkResponseFieldOrder() {
    StreamingStatus status;
    status.m_state = StreamingState::Running;
    status.m_sessionId = "s1";
    status.m_transport = "udp";
    status.m_host = "127.0.0.1";
    status.m_port = 9001;
    status.m_profile = "balanced";
    status.m_mediaProfile = "mjpeg_x264";
    status.m_codec = "h264";
    status.m_device = "/dev/video0";
    status.m_quality = "hd";

    // 字段顺序固定：客户端按固定键序断言时，顺序漂移会被当成协议不兼容。
    CHECK(formatOkResponse(status) ==
          "OK state=Running transport=udp host=127.0.0.1 port=9001 profile=balanced"
          " media_profile=mjpeg_x264 session=s1 codec=h264 device=/dev/video0 quality=hd");
}

void testAutoProfileExposesNetworkProfile() {
    StreamingStatus status;
    status.m_profile = "auto";
    status.m_activeNetworkProfile = "adaptive";
    CHECK(contains(formatOkResponse(status), "profile=auto network_profile=adaptive"));

    // 固定策略不回显 network_profile：老客户端会拒绝未知字段。
    status.m_profile = "balanced";
    CHECK(!contains(formatOkResponse(status), "network_profile"));
}

void testMediaKeyOnlyOnStart() {
    StreamingStatus status;
    status.m_mediaKeyHex = "0011223344556677";
    CHECK(!contains(formatOkResponse(status, false), "media_key"));
    CHECK(contains(formatOkResponse(status, true), "media_key=0011223344556677"));
}

void testErrorResponseSanitizesFields() {
    StreamingStatus status;
    const std::string line = formatErrorResponse("start_failed", "no such device", status);
    CHECK(line.rfind("ERR code=start_failed message=", 0) == 0);
    // 描述里的空格会破坏单行分词，必须被清洗掉。
    CHECK(!contains(line, "no such device"));
    CHECK(contains(line, "state=Idle"));
}

void testCapsResponse() {
    StreamingStatus status;
    CHECK(contains(formatCapsResponse({"mjpeg_x264", "yuyv_x264"}, status),
                   "OK media_profiles=mjpeg_x264,yuyv_x264"));
    // 空列表输出 none，保证客户端按空格分词时字段数恒定。
    CHECK(contains(formatCapsResponse({}, status), "media_profiles=none"));
}

}  // namespace

int main() {
    testStatusWithSession();
    testStatusWithMediaPort();
    testStatusRejectsDuplicateAndUnknownFields();
    testStatusRequiresValidSession();
    testStopAndKeyframe();
    testCaps();
    testStartValid();
    testStartOptionalFields();
    testStartRequiresMandatoryFields();
    testStartRejectsHost();
    testStartFieldValidation();
    testStartRejectsPathTraversal();
    testLineFormatRules();
    testUnknownCommand();
    testFailureLeavesCommandUntouched();
    testOkResponseFieldOrder();
    testAutoProfileExposesNetworkProfile();
    testMediaKeyOnlyOnStart();
    testErrorResponseSanitizesFields();
    testCapsResponse();
    return sserver::test::finish("control_protocol_test");
}
