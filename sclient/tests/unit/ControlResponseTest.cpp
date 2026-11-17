// ControlResponse：响应解析的单元测试。
//
// 纯函数，不需要网络：只要服务端改了响应形状，这里就会先失败。
#include <string>

#include "sclient/control/ControlResponse.h"
#include "support/Check.h"

namespace {

using sclient::control::ControlResponse;
using sclient::control::parseControlResponse;

void testOkResponse() {
    const ControlResponse response = parseControlResponse(
            "OK state=Running transport=udp port=9001 session=s1 codec=h264");
    CHECK(response.m_ok);
    CHECK(response.field("state") == "Running");
    CHECK(response.field("transport") == "udp");
    CHECK(response.field("session") == "s1");
    CHECK(response.intField("port") == 9001);
}

void testErrorResponse() {
    const ControlResponse response =
            parseControlResponse("ERR code=start_failed message=no_such_device state=Idle");
    CHECK(!response.m_ok);
    CHECK(response.m_errorCode == "start_failed");
    CHECK(response.m_errorMessage == "no_such_device");
    // 错误响应同样带全套状态字段。
    CHECK(response.field("state") == "Idle");
    // code/message 不进通用字段表，避免和状态字段混淆。
    CHECK(response.field("code").empty());
}

void testBareOk() {
    const ControlResponse response = parseControlResponse("OK");
    CHECK(response.m_ok);
    CHECK(response.m_fields.empty());
}

void testUnrecognizedLine() {
    // 首词既不是 OK 也不是 ERR：这是协议错误。不设置 m_errorCode，
    // 调用方据此区分"服务端拒绝了"和"对端根本不是我们的服务端"。
    const ControlResponse empty = parseControlResponse("");
    CHECK(!empty.m_ok);
    CHECK(empty.m_errorCode.empty());

    const ControlResponse foreign = parseControlResponse("HTTP/1.1 200 OK");
    CHECK(!foreign.m_ok);
    CHECK(foreign.m_errorCode.empty());
}

void testMalformedFieldsIgnored() {
    // 残缺 token 不影响其它字段的可信度。
    const ControlResponse response = parseControlResponse("OK broken port=1234 =nokey");
    CHECK(response.m_ok);
    CHECK(response.intField("port") == 1234);
    CHECK(response.field("broken").empty());
}

void testIntFieldFallback() {
    const ControlResponse response = parseControlResponse("OK port=abc session=s1");
    CHECK(response.intField("port", 7) == 7);        // 非数字
    CHECK(response.intField("missing", 9) == 9);     // 不存在
    CHECK(response.intField("session", 3) == 3);     // 不是数字
}

void testExtraWhitespaceTolerated() {
    const ControlResponse response = parseControlResponse("OK   port=42    session=s2");
    CHECK(response.m_ok);
    CHECK(response.intField("port") == 42);
    CHECK(response.field("session") == "s2");
}

}  // namespace

int main() {
    testOkResponse();
    testErrorResponse();
    testBareOk();
    testUnrecognizedLine();
    testMalformedFieldsIgnored();
    testIntFieldFallback();
    testExtraWhitespaceTolerated();
    return sclient::test::finish("control_response_test");
}
