// ============================================================
// 文件：ControlResponse.cpp
// 模块：sclient::control
// 功能：控制响应解析的实现。
// 知识点：按空白分词、key=value 拆分、非法数值的安全回退
// ============================================================

#include "sclient/control/ControlResponse.h"

#include <cstdlib>
#include <sstream>
#include <vector>

namespace sclient {
namespace control {

namespace {

std::vector<std::string> splitFields(const std::string &line) {
    std::vector<std::string> fields;
    std::istringstream input(line);
    std::string field;
    while (input >> field) {
        fields.push_back(field);
    }
    return fields;
}

}  // namespace

std::string ControlResponse::field(const std::string &name) const {
    const auto iterator = m_fields.find(name);
    return iterator == m_fields.end() ? std::string() : iterator->second;
}

int ControlResponse::intField(const std::string &name, int fallback) const {
    const std::string text = field(name);
    if (text.empty()) {
        return fallback;
    }
    char *end = nullptr;
    const long value = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0') {
        return fallback;
    }
    return static_cast<int>(value);
}

ControlResponse parseControlResponse(const std::string &line) {
    ControlResponse response;
    const std::vector<std::string> fields = splitFields(line);
    if (fields.empty()) {
        return response;
    }
    if (fields[0] == "OK") {
        response.m_ok = true;
    } else if (fields[0] == "ERR") {
        response.m_ok = false;
    } else {
        // 首词不认识：这是协议层面的错误，不是业务失败，所以不设置
        // m_errorCode，调用方据此区分"服务端拒绝了"和"对方不是我们的服务端"。
        return response;
    }

    for (std::size_t index = 1; index < fields.size(); ++index) {
        const std::string &field = fields[index];
        const std::size_t equal = field.find('=');
        if (equal == std::string::npos || equal == 0) {
            continue;  // 残缺字段忽略，不影响其它字段的可信度
        }
        const std::string key = field.substr(0, equal);
        const std::string value = field.substr(equal + 1);
        if (key == "code") {
            response.m_errorCode = value;
        } else if (key == "message") {
            response.m_errorMessage = value;
        } else {
            // code/message 不是状态字段，只放在专有成员里；
            // 其余字段（含 session/port/state 等）都进 m_fields。
            response.m_fields[key] = value;
        }
    }
    return response;
}

}  // namespace control
}  // namespace sclient
