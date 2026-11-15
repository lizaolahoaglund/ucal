#include "ucal/BackendDispatch.hpp"
#include "ucal/BackendPlugin.h"
#include <cstring>
#include <memory>
#include <windows.h>

// Supplied by each original plugin implementation; not an exported C++ ABI.
std::unique_ptr<Ucal::Backend::BackendInterface> makeUcalPlugin(const std::wstring &configuration);
namespace {
struct Instance {
    std::unique_ptr<Ucal::Backend::BackendInterface> backend;
    std::vector<Ucal::Backend::Wire::LegacyLease> leases;
    std::mutex mutex;
    std::atomic_bool shutdown{false};
};
int32_t UCAL_BACKEND_CALL create(const char *path, uint32_t size, void **result) {
    if (!result)
        return 1;
    *result = nullptr;
    if (size > 32768u || (!path && size) || (size && std::memchr(path, 0, size)))
        return 1;
    try {
        std::wstring configuration;
        if (size) {
            const auto count =
                MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, static_cast<int>(size), nullptr, 0);
            if (count <= 0)
                return 1;
            configuration.resize(count);
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, static_cast<int>(size),
                                configuration.data(), count);
        }
        auto instance = std::make_unique<Instance>();
        instance->backend = makeUcalPlugin(configuration);
        if (!instance->backend)
            return 8;
        *result = instance.release();
        return 0;
    } catch (...) {
        return 8;
    }
}
void UCAL_BACKEND_CALL destroy(void *value) {
    try {
        delete static_cast<Instance *>(value);
    } catch (...) {
    }
}
int32_t UCAL_BACKEND_CALL invoke(void *value, uint32_t owner, uint16_t operation, uint16_t minor,
                                 const void *input, uint32_t bytes, void *output, uint32_t capacity,
                                 uint32_t *written) {
    if (written)
        *written = 0;
    if (!value || !owner || !written || !output || capacity < UCAL_BACKEND_MAX_PAYLOAD ||
        bytes > UCAL_BACKEND_MAX_PAYLOAD || (!input && bytes))
        return 1;
    if (minor > Ucal::Protocol::minorVersion)
        return 2;
    try {
        std::vector<std::byte> request(bytes);
        if (bytes)
            std::memcpy(request.data(), input, bytes);
        auto header =
            Ucal::Protocol::makeRequest(static_cast<Ucal::Protocol::Operation>(operation), 1u, bytes);
        header.minor = minor;
        auto &instance = *static_cast<Instance *>(value);
        std::vector<std::byte> response;
        const auto status = Ucal::Backend::Wire::handleRequest(header, request, owner, instance.leases,
                                                                response, instance.backend.get(),
                                                                instance.mutex, false, instance.shutdown);
        if (response.size() > capacity)
            return 7;
        if (!response.empty())
            std::memcpy(output, response.data(), response.size());
        *written = static_cast<uint32_t>(response.size());
        return static_cast<int32_t>(status);
    } catch (...) {
        return 8;
    }
}
} // namespace
extern "C" int32_t UCAL_BACKEND_CALL UcalBackendGetApi(uint32_t abi, UcalBackendApiV1 *api) {
    if (!api || api->size != sizeof(*api))
        return 1;
    if (abi != UCAL_BACKEND_ABI_V1)
        return 2;
    *api = {sizeof(*api),
            UCAL_BACKEND_ABI_V1,
            Ucal::Protocol::majorVersion,
            Ucal::Protocol::minorVersion,
            UCAL_BACKEND_MAX_PAYLOAD,
            0u,
            create,
            destroy,
            invoke};
    return 0;
}
