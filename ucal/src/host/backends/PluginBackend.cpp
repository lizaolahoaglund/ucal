#include "backends/PluginBackend.hpp"
#include "ucal/RuntimeIdentity.hpp"
#include "ucal/BackendPlugin.h"
#include "ucal/Client.hpp"
#include "ucal/Protocol.hpp"
#include <windows.h>

#include <algorithm>
#include <bcrypt.h>
#include <map>
#include <mutex>

namespace Ucal::Backend {
namespace {
std::string utf8(const std::wstring &text) {
    if (text.empty())
        return {};
    const auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                                          static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0)
        return {};
    std::string result(size, 0);
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                        result.data(), size, nullptr, nullptr);
    return result;
}
DeviceId namespaced(const std::string &instance, const DeviceId &original) {
    const auto source = (std::string(endpointIdentityNamespaceV1.data(), endpointIdentityNamespaceV1.size()) + ".plugin/1|") + instance + "|" + deviceIdToString(original);
    BCRYPT_ALG_HANDLE algorithm{};
    std::array<unsigned char, 32> hash{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
        return {};
    const auto result =
        BCryptHash(algorithm, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char *>(source.data())),
                   static_cast<ULONG>(source.size()), hash.data(), static_cast<ULONG>(hash.size()));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    DeviceId id{};
    if (result == 0)
        std::copy_n(hash.begin(), id.size(), id.begin());
    return id;
}
bool absolute(const std::wstring &path) {
    return path.size() > 2u && path[1] == L':' && (path[2] == L'/' || path[2] == L'\\');
}
} // namespace
struct PluginBackend::Implementation {
    PluginSpec spec;
    HMODULE module{};
    UcalBackendApiV1 api{};
    void *instance{};
    std::mutex mutex;
    std::map<DeviceId, DeviceId> nativeIds;
    explicit Implementation(PluginSpec value) : spec(std::move(value)) {}
    ~Implementation() {
        if (instance)
            api.destroy(instance);
        if (module)
            FreeLibrary(module);
    }
    Status load() {
        if (instance)
            return Status::Ok;
        if (!absolute(spec.library) || (!spec.configuration.empty() && !absolute(spec.configuration)))
            return Status::InvalidArgument;
        if (!module) {
            module = LoadLibraryExW(spec.library.c_str(), nullptr,
                                    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (!module)
                return Status::NotFound;
            auto query =
                reinterpret_cast<UcalBackendGetApiFunctionV1>(GetProcAddress(module, "UcalBackendGetApi"));
            // ABI V1 tables from an existing plugin retain their binary layout.
            if (!query)
                query = reinterpret_cast<UcalBackendGetApiFunctionV1>(
                    GetProcAddress(module, "ucal_backend_get_api"));
            api = {};
            api.size = sizeof(api);
            if (!query || query(UCAL_BACKEND_ABI_V1, &api) != 0 || api.size != sizeof(api) ||
                api.abiVersion != UCAL_BACKEND_ABI_V1 || api.protocolMajor != 1u ||
                api.protocolMinor < 8u || api.maxPayloadBytes != UCAL_BACKEND_MAX_PAYLOAD ||
                api.reserved != 0u || !api.create || !api.destroy || !api.invoke) {
                FreeLibrary(module);
                module = nullptr;
                api = {};
                return Status::IncompatibleVersion;
            }
        }
        const auto configuration = utf8(spec.configuration);
        const auto code =
            api.create(configuration.data(), static_cast<std::uint32_t>(configuration.size()), &instance);
        if (code < 0 || code > 8 || (!code && !instance)) {
            instance = nullptr;
            return Status::TransportError;
        }
        if (code) {
            if (instance) {
                api.destroy(instance);
                instance = nullptr;
            }
            return static_cast<Status>(code);
        }
        return Status::Ok;
    }
    Client client(const std::uint32_t owner) {
        return Client(
            [this](std::uint32_t pid, Protocol::Operation operation, const void *input, std::uint32_t size,
                   std::vector<std::byte> &output) {
                std::lock_guard<std::mutex> lock(mutex);
                output.clear();
                const auto loaded = load();
                if (loaded != Status::Ok)
                    return loaded;
                const auto minor = (operation == Protocol::Operation::OpenCanChannelAdvanced ||
                                    operation == Protocol::Operation::EnumerateEndpointsV11) ? 11u :
                                   (operation == Protocol::Operation::EnumerateEndpointsV10 ||
                                    operation == Protocol::Operation::EnumerateBackendHealthV10)
                                       ? 10u
                                   : operation == Protocol::Operation::EnumerateEndpointsV9 ? 9u
                                                                                              : 8u;
                if (minor > api.protocolMinor)
                    return Status::IncompatibleVersion;
                output.resize(UCAL_BACKEND_MAX_PAYLOAD);
                std::uint32_t written{};
                const auto code = api.invoke(instance, pid, static_cast<std::uint16_t>(operation),
                                             static_cast<std::uint16_t>(minor), input, size, output.data(),
                                             static_cast<std::uint32_t>(output.size()), &written);
                if (code < 0 || code > 8 || written > output.size()) {
                    output.clear();
                    return Status::TransportError;
                }
                output.resize(written);
                return static_cast<Status>(code);
            },
            owner);
    }
    Status nativeId(const DeviceId &id, DeviceId &native) {
        std::lock_guard<std::mutex> lock(mutex);
        const auto i = nativeIds.find(id);
        if (i == nativeIds.end())
            return Status::NotFound;
        native = i->second;
        return Status::Ok;
    }
};
PluginBackend::PluginBackend(PluginSpec spec)
    : implementation_(std::make_unique<Implementation>(std::move(spec))) {}
PluginBackend::~PluginBackend() = default;
Status PluginBackend::initialize() { return implementation_->client(GetCurrentProcessId()).ping(); }
std::string PluginBackend::backendFamily() const { return implementation_->spec.family; }
Status PluginBackend::enumerateEndpoints(std::vector<EndpointDescriptor> &endpoints) {
    auto &impl = *implementation_;
    const auto status = impl.client(GetCurrentProcessId()).enumerateEndpoints(endpoints);
    if (status != Status::Ok)
        return status;
    std::lock_guard<std::mutex> lock(impl.mutex);
    std::map<DeviceId, DeviceId> ids;
    for (auto &e : endpoints) {
        const auto original = e.id;
        if (!impl.spec.preserveIdentity) {
            e.id = namespaced(impl.spec.id, original);
            e.adapterId = namespaced(impl.spec.id, e.adapterId);
        }
        if (e.id == DeviceId{} || e.adapterId == DeviceId{} || !ids.emplace(e.id, original).second) {
            endpoints.clear();
            return Status::TransportError;
        }
        e.backendFamily = impl.spec.family;
        e.backendInstanceId = impl.spec.id;
    }
    impl.nativeIds = std::move(ids);
    return Status::Ok;
}
Status PluginBackend::enumerate(std::vector<DeviceDescriptor> &devices) {
    devices.clear();
    std::vector<EndpointDescriptor> es;
    const auto s = enumerateEndpoints(es);
    if (s != Status::Ok)
        return s;
    for (const auto &e : es)
        if (e.busType == BusType::Can)
            devices.push_back({e.id, e.name, e.model, e.serial, 1u, e.capabilities, e.state});
    return Status::Ok;
}
OpenResult PluginBackend::open(const DeviceId &id, const CanChannelConfig &config, std::uint32_t owner) {
    DeviceId native{};
    const auto status = implementation_->nativeId(id, native);
    if (status != Status::Ok)
        return {status, 0};
    return implementation_->client(owner).openCanChannel(native, config);
}
OpenResult PluginBackend::openLin(const EndpointId &id, const LinChannelConfig &config,
                                   std::uint32_t owner) {
    DeviceId native{};
    const auto status = implementation_->nativeId(id, native);
    if (status != Status::Ok)
        return {status, 0};
    return implementation_->client(owner).openLinChannel(native, config);
}
Status PluginBackend::close(std::uint64_t t, std::uint32_t p) {
    return implementation_->client(p).closeDevice(t);
}
Status PluginBackend::write(std::uint64_t t, std::uint32_t p, const CanFrame &f) {
    return implementation_->client(p).writeCan(t, f);
}
Status PluginBackend::submit(std::uint64_t t, std::uint32_t p, const CanFrame &f) {
    return implementation_->client(p).submitCan(t, f);
}
Status PluginBackend::submitBatch(std::uint64_t t, std::uint32_t p, const std::vector<CanFrame> &f,
                                   std::uint32_t &n) {
    return implementation_->client(p).submitCanBatch(t, f, n);
}
Status PluginBackend::read(std::uint64_t t, std::uint32_t p, std::uint32_t n, std::vector<CanFrame> &f) {
    return implementation_->client(p).readCan(t, n, f);
}
Status PluginBackend::readEvents(std::uint64_t t, std::uint32_t p, std::uint32_t n,
                                  std::vector<CanEvent> &e) {
    return implementation_->client(p).readCanEvents(t, n, e);
}
Status PluginBackend::getStatus(std::uint64_t t, std::uint32_t p, ChannelStatus &s) {
    return implementation_->client(p).getChannelStatus(t, s);
}
Status PluginBackend::submitLin(std::uint64_t t, std::uint32_t p, const LinFrame &f) {
    return implementation_->client(p).submitLin(t, f);
}
Status PluginBackend::readLinEvents(std::uint64_t t, std::uint32_t p, std::uint32_t n,
                                      std::vector<LinEvent> &e) {
    return implementation_->client(p).readLinEvents(t, n, e);
}
Status PluginBackend::getLinStatus(std::uint64_t t, std::uint32_t p, LinStatus &s) {
    return implementation_->client(p).getLinStatus(t, s);
}
Status PluginBackend::configureLinSlaveResponse(std::uint64_t t, std::uint32_t p,
                                                   const LinSlaveResponse &s) {
    return implementation_->client(p).configureLinSlaveResponse(t, s);
}
void PluginBackend::pruneDeadSessions() {}
BackendRuntimeIdentity PluginBackend::runtimeIdentity() const {
    std::vector<BackendHealth> hs;
    auto &impl = *implementation_;
    if (impl.client(GetCurrentProcessId()).enumerateBackendHealth(hs) == Status::Ok)
        for (const auto &h : hs)
            if (!h.runtime.path.empty())
                return h.runtime;
    std::lock_guard<std::mutex> lock(impl.mutex);
    return impl.module ? loadedRuntimeIdentity(impl.module) : BackendRuntimeIdentity{};
}
void PluginBackend::enumerateBackendHealth(std::vector<BackendHealth> &health) {
    BackendInterface::enumerateBackendHealth(health);
    for (auto &h : health)
        h.backendInstanceId = implementation_->spec.id;
}
} // namespace Ucal::backend
