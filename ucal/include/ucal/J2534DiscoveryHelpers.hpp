#pragma once
#include "ucal/J2534Discovery.h"
#include "ucal/Model.hpp"
#include "ucal/J2534Abi.hpp"
#include <windows.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <vector>

namespace Ucal::J2534Discovery {
inline int32_t code(Status value) { return static_cast<int32_t>(value); }
inline std::string bounded(const char *value, std::size_t size) {
    const auto end = static_cast<const char *>(std::memchr(value, 0, size));
    return end ? std::string(value, end) : std::string{};
}
template <std::size_t N> bool field(char (&output)[N], const std::string &value) {
    if (value.size() >= N || value.find_first_of("\r\n") != std::string::npos ||
        (!value.empty() && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                                static_cast<int>(value.size()), nullptr, 0)))
        return false;
    std::memset(output, 0, N);
    std::memcpy(output, value.data(), value.size());
    return true;
}
inline UcalJ2534PortV2 port(uint32_t physical) {
    UcalJ2534PortV2 result{};
    result.port.size = sizeof(result);
    result.port.version = 2;
    result.port.physicalPort = physical;
    result.port.capabilities = UCAL_J2534_DISCOVERY_FD | UCAL_J2534_DISCOVERY_BRS;
    result.connectionModel = UCAL_J2534_OPEN_PER_PORT;
    result.classicProtocol = J2534Backend::Abi::can;
    result.fdProtocol = J2534Backend::Abi::gmCanFd;
    return result;
}
inline int32_t deliver(const std::vector<UcalJ2534PortV2> &list, UcalJ2534PortV2 *output, uint32_t capacity,
                       uint32_t *count) {
    *count = static_cast<uint32_t>(list.size());
    if (list.size() > capacity)
        return code(Status::InvalidArgument);
    std::copy(list.begin(), list.end(), output);
    return 0;
}
struct Library {
    HMODULE handle{};
    // Explicit release runs outside DllMain, after all vendor sessions close.
    void release() {
        if (handle)
            FreeLibrary(handle);
        handle = nullptr;
    }
    bool load(const uint16_t *runtime, const wchar_t *companion, bool system = false) {
        if (handle)
            return true;
        const std::wstring path(reinterpret_cast<const wchar_t *>(runtime));
        const auto slash = path.find_last_of(L"\\/");
        if (slash == std::wstring::npos)
            return false;
        const auto adjacent = path.substr(0, slash + 1) + companion;
        if (GetFileAttributesW(adjacent.c_str()) != INVALID_FILE_ATTRIBUTES)
            handle = LoadLibraryExW(adjacent.c_str(), nullptr,
                                    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        else if (system)
            handle = LoadLibraryExW(companion, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return handle != nullptr;
    }
    template <class F> F function(const char *name) const {
        return reinterpret_cast<F>(GetProcAddress(handle, name));
    }
};
} // namespace Ucal::j2534_discovery
