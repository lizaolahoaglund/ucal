#include "ucal/FrontendPlugin.h"
#include <windows.h>
#include <cstring>
#include <string>
#include <vector>

namespace {
std::string environment(const wchar_t *name) {
    const auto size = GetEnvironmentVariableW(name, nullptr, 0);
    if (!size)
        return {};
    std::vector<wchar_t> value(size);
    const auto length = GetEnvironmentVariableW(name, value.data(), size);
    if (!length || length >= size)
        return {};
    const auto bytes =
        WideCharToMultiByte(CP_UTF8, 0, value.data(), int(length), nullptr, 0, nullptr, nullptr);
    std::string result(size_t(bytes), '\0');
    if (bytes)
        WideCharToMultiByte(CP_UTF8, 0, value.data(), int(length), &result[0], bytes, nullptr, nullptr);
    return result;
}
std::string quoted(const std::string &value) {
    std::string result = "\"";
    const char hex[] = "0123456789abcdef";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') {
            result += '\\';
            result += char(c);
        } else if (c < 32) {
            result += "\\u00";
            result += hex[c >> 4];
            result += hex[c & 15];
        } else
            result += char(c);
    }
    return result + '"';
}
int32_t UCAL_FRONTEND_CALL invoke(uint32_t operation, const char *request, uint32_t bytes, char *output,
                                  uint32_t capacity, uint32_t *written) {
    if (!request || !output || !written || capacity < UCAL_FRONTEND_MAX_JSON ||
        bytes > UCAL_FRONTEND_MAX_JSON)
        return 2;
    *written = 0;
    try {
        const std::string context(request, bytes);
        const auto executable = environment(L"UCAL_FRONTEND_TEST_EXE");
        std::string result = "{}";
        int code = 0;
        if (context.find("\"fail\":true") != std::string::npos) {
            result = "{\"error\":\"Fixture operation rejected\",\"vendorCode\":87}";
            code = 3;
        } else if (operation == UCAL_FRONTEND_SCAN) {
            result = "{\"installations\":[";
            bool comma = false;
            for (const auto &path : {executable, environment(L"UCAL_FRONTEND_TEST_EXE_2")}) {
                if (path.empty())
                    continue;
                if (comma)
                    result += ',';
                result += "{\"executablePath\":" + quoted(path) +
                          ",\"displayName\":\"Fixture software\",\"version\":\"1.0.0\","
                          "\"integrationAdapters\":[\"fixture-route\",\"native\"]}";
                comma = true;
            }
            result += "]}";
        } else if (operation == UCAL_FRONTEND_INSPECT) {
            if (!executable.empty() &&
                context.find("\"executablePath\":" + quoted(executable)) != std::string::npos)
                result = "{\"executablePath\":" + quoted(executable) +
                         ",\"integrationAdapters\":[\"fixture-route\",\"native\"]}";
        } else if (operation == UCAL_FRONTEND_CHECK_COMPATIBILITY) {
            result = "{\"frontend\":\"Fixture frontend\",\"blockers\":[]}";
        } else if (operation == UCAL_FRONTEND_PREPARE_LAUNCH) {
            result = "{\"program\":" + quoted(executable) +
                     ",\"workingDirectory\":" + quoted(executable.substr(0, executable.find_last_of("/\\"))) +
                     ",\"arguments\":[\"--fixture\",\"a b\",\"literal;$()\"],"
                     "\"environment\":{\"UCAL_FRONTEND_FIXTURE\":\"ready\"}}";
        } else if (operation == UCAL_FRONTEND_DEPLOY || operation == UCAL_FRONTEND_RESTORE_DEPLOYMENT) {
            result = "{\"message\":\"Fixture operation completed\"}";
        } else {
            result = "{\"error\":\"Unsupported fixture operation\"}";
            code = 1;
        }
        if (result.size() > capacity)
            return 3;
        std::memcpy(output, result.data(), result.size());
        *written = uint32_t(result.size());
        return code;
    } catch (...) {
        return 3;
    }
}
} // namespace
extern "C" int32_t UCAL_FRONTEND_CALL UcalFrontendGetApi(uint32_t abi, UcalFrontendApiV1 *api) {
    if (!api || api->size != sizeof(*api) || abi != UCAL_FRONTEND_ABI_V1)
        return 2;
    *api = {sizeof(*api), UCAL_FRONTEND_ABI_V1, 0, invoke};
    return 0;
}
