#pragma once

#include "ucal/Model.hpp"

#include <windows.h>
#include <bcrypt.h>

#include <array>
#include <vector>

namespace Ucal::Backend {

// Capture this once immediately after LoadLibrary, while the image is pinned.
// Diagnostics report the loaded module, never a search candidate or preference.
inline BackendRuntimeIdentity loadedRuntimeIdentity(const HMODULE module) {
    BackendRuntimeIdentity result;
    std::array<wchar_t, 32768> path{};
    const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) return result;
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, path.data(), static_cast<int>(length), nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return result;
    result.path.resize(static_cast<std::size_t>(bytes));
    WideCharToMultiByte(CP_UTF8, 0, path.data(), static_cast<int>(length), result.path.data(), bytes, nullptr, nullptr);
    const auto file = CreateFileW(path.data(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return result;
    BCRYPT_ALG_HANDLE algorithm{};
    BCRYPT_HASH_HANDLE hash{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0) {
        if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
            std::array<std::uint8_t, 65536> block{};
            DWORD count{};
            bool valid = true;
            std::uint64_t total = 0;
            for (;;) {
                if (!ReadFile(file, block.data(), static_cast<DWORD>(block.size()), &count, nullptr)) {
                    valid = false; break;
                }
                if (count == 0) break;
                total += count;
                if (total > 128u * 1024u * 1024u || BCryptHashData(hash, block.data(), count, 0) != 0) {
                    valid = false; break;
                }
            }
            std::array<std::uint8_t, 32> digest{};
            if (valid && BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) == 0) {
                constexpr char hex[] = "0123456789abcdef";
                for (const auto value : digest) {
                    result.sha256 += hex[value >> 4u]; result.sha256 += hex[value & 15u];
                }
            }
            BCryptDestroyHash(hash);
        }
        BCryptCloseAlgorithmProvider(algorithm, 0);
    }
    DWORD ignored{};
    const DWORD size = GetFileVersionInfoSizeW(path.data(), &ignored);
    if (size != 0 && size <= 1024u * 1024u) {
        std::vector<std::uint8_t> resource(size);
        VS_FIXEDFILEINFO* version{};
        UINT versionSize{};
        if (GetFileVersionInfoW(path.data(), 0, size, resource.data()) &&
            VerQueryValueW(resource.data(), L"\\", reinterpret_cast<void**>(&version), &versionSize) &&
            versionSize >= sizeof(*version) && version->dwSignature == 0xfeef04bdu) {
            result.fileVersion = std::to_string(HIWORD(version->dwFileVersionMS)) + "." +
                std::to_string(LOWORD(version->dwFileVersionMS)) + "." +
                std::to_string(HIWORD(version->dwFileVersionLS)) + "." +
                std::to_string(LOWORD(version->dwFileVersionLS));
        }
    }
    CloseHandle(file);
    return result;
}

}  // namespace Ucal::backend
