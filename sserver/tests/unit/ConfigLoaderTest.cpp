// ConfigLoader：严格解析与错误定位的单元测试。
//
// 这批用例的目的不是覆盖字段，而是守住"严格"这件事：未知表、未知键、
// 类型不符、数值越界都必须失败，且错误里带「文件:行:列」。否则拼错的键
// 会静默回退到默认值，配置看起来生效了、实际没有。
#include <string>

#include "sserver/appconfig/AppConfig.h"
#include "support/Check.h"

namespace {

using sserver::appconfig::AppConfig;
using sserver::appconfig::ConfigLoader;

bool contains(const std::string &text, const std::string &needle) {
    return text.find(needle) != std::string::npos;
}

bool load(const std::string &text, AppConfig *config, std::string *error) {
    return ConfigLoader::loadFromString(text, "test.toml", config, error);
}

void testDefaultsWithoutContent() {
    AppConfig config;
    std::string error;
    CHECK(load("", &config, &error));
    CHECK(config.m_control.m_port == 9000);
    CHECK(config.m_control.m_maxClients == 16);
    CHECK(config.m_capture.m_device == "auto");
    CHECK(!config.m_runtime.m_logVerbose);
}

void testAllFieldsApplied() {
    AppConfig config;
    std::string error;
    const std::string text =
            "[control]\n"
            "port = 9100\n"
            "max_clients = 4\n"
            "[capture]\n"
            "device = \"/dev/video2\"\n"
            "[runtime]\n"
            "log_verbose = true\n";
    CHECK(load(text, &config, &error));
    CHECK(config.m_control.m_port == 9100);
    CHECK(config.m_control.m_maxClients == 4);
    CHECK(config.m_capture.m_device == "/dev/video2");
    CHECK(config.m_runtime.m_logVerbose);
}

void testPartialContentKeepsDefaults() {
    // 只写一个键时其余字段必须保持内置默认值，而不是被清零。
    AppConfig config;
    std::string error;
    CHECK(load("[control]\nport = 8080\n", &config, &error));
    CHECK(config.m_control.m_port == 8080);
    CHECK(config.m_control.m_maxClients == 16);
    CHECK(config.m_capture.m_device == "auto");
}

void testUnknownTableRejected() {
    AppConfig config;
    std::string error;
    CHECK(!load("[app]\nname = \"x\"\n", &config, &error));
    CHECK(contains(error, "test.toml:1:"));
    CHECK(contains(error, "unknown table [app]"));
}

void testUnknownKeyRejected() {
    AppConfig config;
    std::string error;
    CHECK(!load("[control]\nportt = 9000\n", &config, &error));
    CHECK(contains(error, "unknown key 'portt'"));
    CHECK(contains(error, "[control]"));
    // 位置指向出错的那一行，而不是文件开头。
    CHECK(contains(error, ":2:"));
}

void testTypeMismatchRejected() {
    AppConfig config;
    std::string error;

    // 字符串写成整数：报错要同时给出期望类型和实际类型。
    CHECK(!load("[control]\nport = \"9000\"\n", &config, &error));
    CHECK(contains(error, "control.port must be an integer (got string)"));

    CHECK(!load("[runtime]\nlog_verbose = \"true\"\n", &config, &error));
    CHECK(contains(error, "runtime.log_verbose must be a boolean (got string)"));

    CHECK(!load("[control]\nport = 9000.5\n", &config, &error));
    CHECK(contains(error, "must be an integer (got float)"));

    CHECK(!load("[capture]\ndevice = 2\n", &config, &error));
    CHECK(contains(error, "capture.device must be a string (got integer)"));
}

void testOutOfRangeRejected() {
    AppConfig config;
    std::string error;

    // 端口 0 会让内核随机分配，客户端连不上，所以下限是 1。
    CHECK(!load("[control]\nport = 0\n", &config, &error));
    CHECK(contains(error, "control.port must be in [1, 65535] (got 0)"));

    CHECK(!load("[control]\nport = 70000\n", &config, &error));
    CHECK(contains(error, "control.port must be in [1, 65535] (got 70000)"));

    CHECK(!load("[control]\nmax_clients = 0\n", &config, &error));
    CHECK(contains(error, "control.max_clients must be in [1, 64] (got 0)"));
}

void testEmptyDeviceRejected() {
    AppConfig config;
    std::string error;
    CHECK(!load("[capture]\ndevice = \"\"\n", &config, &error));
    CHECK(contains(error, "capture.device must not be empty"));
}

void testSectionMustBeTable() {
    AppConfig config;
    std::string error;
    CHECK(!load("control = 9000\n", &config, &error));
    CHECK(contains(error, "[control] must be a table"));
}

void testSyntaxErrorReportsPosition() {
    AppConfig config;
    std::string error;
    CHECK(!load("[control]\nport = \n", &config, &error));
    CHECK(contains(error, "test.toml:"));
    CHECK(error.size() > std::string("test.toml:2:1: ").size());
}

void testFailureLeavesConfigUntouched() {
    // 失败时不能留下半加载的配置：调用方可能选择"加载失败沿用默认值"，
    // 那时拿到一份被改了一半的配置比直接失败更危险。
    AppConfig config;
    config.m_control.m_port = 1234;
    config.m_capture.m_device = "/dev/video9";

    std::string error;
    CHECK(!load("[control]\nport = 8080\nmax_clients = 0\n", &config, &error));
    CHECK(config.m_control.m_port == 1234);
    CHECK(config.m_capture.m_device == "/dev/video9");
}

void testRepositoryConfigFileLoads() {
    // 仓库里的样板配置必须能通过校验：它是部署时的起点，改坏了要立刻发现。
    AppConfig config;
    std::string error;
    const std::string path = std::string(SSERVER_SOURCE_DIR) + "/config/control.toml";
    CHECK(ConfigLoader::loadFromFile(path, &config, &error));
    CHECK(config.m_control.m_port == 9000);
    CHECK(config.m_control.m_maxClients == 16);
    CHECK(config.m_capture.m_device == "auto");
    CHECK(!config.m_runtime.m_logVerbose);
}

void testMissingFileReportsPath() {
    AppConfig config;
    std::string error;
    CHECK(!ConfigLoader::loadFromFile("does-not-exist.toml", &config, &error));
    CHECK(contains(error, "does-not-exist.toml"));
    CHECK(contains(error, "cannot open file"));
}

void testValidateRejectsInvalidValues() {
    // 绕过加载器直接构造非法配置时，validate 仍要拦住。
    AppConfig config;
    config.m_control.m_port = 0;
    std::string error;
    std::string field;
    CHECK(!config.validate(&error, &field));
    CHECK(field == "control.port");

    config = AppConfig::createDefault();
    config.m_control.m_maxClients = 100;
    CHECK(!config.validate(&error, &field));
    CHECK(field == "control.max_clients");

    config = AppConfig::createDefault();
    config.m_capture.m_device.clear();
    CHECK(!config.validate(&error, &field));
    CHECK(field == "capture.device");

    config = AppConfig::createDefault();
    CHECK(config.validate(&error, &field));
}

}  // namespace

int main() {
    testDefaultsWithoutContent();
    testAllFieldsApplied();
    testPartialContentKeepsDefaults();
    testUnknownTableRejected();
    testUnknownKeyRejected();
    testTypeMismatchRejected();
    testOutOfRangeRejected();
    testEmptyDeviceRejected();
    testSectionMustBeTable();
    testSyntaxErrorReportsPosition();
    testFailureLeavesConfigUntouched();
    testRepositoryConfigFileLoads();
    testMissingFileReportsPath();
    testValidateRejectsInvalidValues();
    return sserver::test::finish("config_loader_test");
}
