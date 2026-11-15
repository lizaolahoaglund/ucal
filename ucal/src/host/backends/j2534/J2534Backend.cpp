#include "backends/j2534/J2534Backend.hpp"
#include "ucal/J2534Abi.hpp"
#include "ucal/J2534Discovery.h"
#include "ucal/ProviderQueries.h"
#include "backends/j2534/DiscoveryProcess.hpp"
#include "ucal/SerialExecutor.hpp"
#include "ucal/RuntimeIdentity.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <windows.h>

#include <bcrypt.h>

namespace Ucal::J2534Backend {
namespace {
using Abi::U32;
constexpr std::array<std::uint8_t, 16> lengths{0, 1,  2,  3,  4,  5,  6,  7,
                                               8, 12, 16, 20, 24, 32, 48, 64};

std::wstring environment(const wchar_t *key) {
    std::vector<wchar_t> buffer(32768u);
    const auto n = GetEnvironmentVariableW(key, buffer.data(), static_cast<DWORD>(buffer.size()));
    return n > 0u && n < buffer.size() ? std::wstring(buffer.data(), n) : std::wstring{};
}
bool absolutePath(const std::wstring &value) {
    return value.size() >= 3u && value[1] == L':' && (value[2] == L'\\' || value[2] == L'/');
}
std::string utf8(const std::wstring &value) {
    if (value.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                      static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0)
        return {};
    std::string output(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                        output.data(), n, nullptr, nullptr);
    return output;
}
std::wstring setting(const std::wstring &path, const wchar_t *section, const wchar_t *key) {
    std::array<wchar_t, 2048> value{};
    const auto n = GetPrivateProfileStringW(section, key, L"", value.data(),
                                            static_cast<DWORD>(value.size()), path.c_str());
    return n < value.size() - 1u ? std::wstring(value.data(), n) : std::wstring{};
}
bool number(const std::wstring &text, U32 &result) {
    if (text.empty() || text.find_first_not_of(L"0123456789") != std::wstring::npos)
        return false;
    wchar_t *end = nullptr;
    const auto n = std::wcstoull(text.c_str(), &end, 10);
    if (*end != L'\0' || n > UINT32_MAX)
        return false;
    result = static_cast<U32>(n);
    return true;
}
DeviceId identity(const std::string &text) {
    std::array<std::uint8_t, 32> digest{};
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
        return {};
    const auto status = BCryptHash(
        algorithm, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char *>(text.data())),
        static_cast<ULONG>(text.size()), digest.data(), static_cast<ULONG>(digest.size()));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status != 0)
        return {};
    DeviceId id{};
    std::copy_n(digest.begin(), id.size(), id.begin());
    return id;
}
std::uint64_t randomToken() {
    std::uint64_t value = 0;
    if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&value), sizeof(value),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        return 0;
    return value;
}
bool alive(const U32 pid) {
    const auto handle = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!handle)
        return GetLastError() == ERROR_ACCESS_DENIED;
    const bool result = WaitForSingleObject(handle, 0) == WAIT_TIMEOUT;
    CloseHandle(handle);
    return result;
}
Status mapResult(const Abi::Result result) {
    switch (result) {
    case Abi::ok:
        return Status::Ok;
    case 1:
    case 3:
    case 0xf:
        return Status::Unsupported;
    case 2:
    case Abi::disconnected:
    case 0x1a:
        return Status::NotFound;
    case 7: // ERR_FAILED is an API operation failure, not a broken Worker pipe.
        return Status::InternalError;
    case 4:
    case 5:
    case 6:
    case 0xa:
    case 0x13:
    case 0x15:
    case 0x19:
        return Status::InvalidArgument;
    case 0xc:
    case 0xe:
    case Abi::timeout:
    case Abi::full:
    case 0x14:
        return Status::Busy;
    default:
        return Status::TransportError;
    }
}
bool validFrame(const CanFrame &frame, const CanChannelConfig &config, const U32 capabilities) {
    constexpr U32 allowed = FrameExtended | FrameFd | FrameBrs;
    if (frame.flags & ~allowed)
        return false;
    if (frame.id > ((frame.flags & FrameExtended) ? 0x1fffffffu : 0x7ffu))
        return false;
    if (frame.dlc >= lengths.size() || frame.dataLength != lengths[frame.dlc])
        return false;
    if (!(frame.flags & FrameFd) && frame.dataLength > 8u)
        return false;
    if ((frame.flags & FrameFd) && !config.canFd)
        return false;
    if ((frame.flags & FrameBrs) &&
        (!(frame.flags & FrameFd) || !(capabilities & CapabilityBrs)))
        return false;
    return true;
}
bool accepts(const CanChannelConfig &config, const CanFrame &frame) {
    if (config.acceptanceFilterCount == 0u)
        return true;
    for (std::size_t i = 0; i < config.acceptanceFilterCount; ++i) {
        const auto &f = config.acceptanceFilters[i];
        if (!f.rejectAll &&
            (f.frameType == CanFilterFrameType::Extended) ==
                ((frame.flags & FrameExtended) != 0u) &&
            (frame.id & f.idMask) == (f.idCode & f.idMask))
            return true;
    }
    return false;
}
} // namespace

struct Backend::Implementation {
    struct Entry {
        EndpointDescriptor endpoint;
        std::string selector;
        bool configured{true};
        bool defaultSelector{};
        U32 connectionModel{UCAL_J2534_OPEN_PER_PORT};
        U32 classicProtocol{Abi::can}, fdProtocol{Abi::gmCanFd}, pins{};
        std::string deviceKey;
        DeviceState observedPresence{DeviceState::Offline};
        std::chrono::steady_clock::time_point presenceValidUntil{};
    };
    struct Session {
        U32 owner{}, device{}, channel{};
        bool deviceOpen{}, channelConnected{};
        std::string deviceKey;
        U32 protocol{Abi::gmCanFd};
        std::size_t entry{};
        CanChannelConfig config{};
        std::vector<U32> filters;
        bool timestampSeen{}, timestampInvalid{};
        bool overflowPending{};
        bool unavailable{};
        U32 lastTimestamp{};
        std::uint64_t timestampEpoch{}, lastObservation{};
    };
    Ucal::Backend::SerialExecutor executor;
    std::wstring configurationPath;
    HMODULE module{};
    DiscoveryProcess discovery;
    struct DeviceSession {
        U32 handle{}, references{};
    };
    std::map<std::string, DeviceSession> devices;
    Abi::Open open{};
    Abi::Close close{};
    Abi::Connect connect{};
    Abi::Disconnect disconnect{};
    Abi::Messages read{}, write{};
    Abi::StartFilter startFilter{};
    Abi::StopFilter stopFilter{};
    Abi::ReadVersion version{};
    Abi::LastError lastError{};
    Abi::Ioctl ioctl{};
    UcalProviderReadCanTimingFunctionV1 readTiming{};
    UcalProviderEnumeratePortsFunctionV1 enumeratePorts{};
    std::map<std::string, U32> physicalPorts;
    BackendRuntimeIdentity runtimeIdentity;
    std::vector<Entry> entries;
    std::map<std::uint64_t, Session> sessions;
    U32 pinAssignment{};
    U32 fixedFdNominalSample{}, fixedFdDataSample{};
    U32 fixedFdNominalBitrate{}, fixedFdDataBitrate{};
    std::string fixedTimingVersion;
    std::string fixedTimingHash;
    bool nativeConfigList{};
    bool dynamicDiscovery{};
    bool loaded{};
    std::string loadedConfiguration;

    template <class F> void symbol(F &target, const char *name) {
        target = reinterpret_cast<F>(GetProcAddress(module, name));
    }
    void diagnostic(const char *stage, const Abi::Result code, U32 accepted=0u, U32 requested=0u) const {
        // Never log opaque selectors or payloads from a backend failure.
        std::cerr << "{\"event\":\"j2534_failure\",\"stage\":\"" << stage
                  << "\",\"status\":" << code << "}" << std::endl;
        const auto path=environment(L"UCAL_J2534_DIAGNOSTIC_PATH");
        if(!absolutePath(path))return;
        const auto file=CreateFileW(path.c_str(),FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(file==INVALID_HANDLE_VALUE)return;
        const auto line=std::string("{\"tickMs\":")+std::to_string(GetTickCount64())+",\"pid\":"+std::to_string(GetCurrentProcessId())+",\"stage\":\""+stage+"\",\"result\":"+std::to_string(code)+",\"accepted\":"+std::to_string(accepted)+",\"requested\":"+std::to_string(requested)+"}\n";
        DWORD written{};WriteFile(file,line.data(),static_cast<DWORD>(line.size()),&written,nullptr);CloseHandle(file);
    }
    void releaseDiscovery() { discovery.reset(); }
    Status load(const bool refresh = false) {
        const auto path = configurationPath.empty() ? environment(L"UCAL_J2534_CONFIG") : configurationPath;
        if (path.empty())
            return Status::NotFound;
        if (!absolutePath(path))
            return Status::InvalidArgument;
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
            return Status::NotFound;
        if (setting(path, L"provider", L"profile") != L"04.04-gmw17753-raw-iso-fd")
            return Status::IncompatibleVersion;
        const auto runtime = setting(path, L"provider", L"runtime");
        const auto provider = utf8(setting(path, L"provider", L"id"));
        const auto fixedNominal = setting(path, L"provider", L"fixed_fd_nominal_sample_point");
        const auto fixedData = setting(path, L"provider", L"fixed_fd_data_sample_point");
        const auto fixedNominalRate = setting(path, L"provider", L"fixed_fd_nominal_bitrate");
        const auto fixedDataRate = setting(path, L"provider", L"fixed_fd_data_bitrate");
        fixedTimingVersion = utf8(setting(path, L"provider", L"fixed_timing_dll_version"));
        fixedTimingHash = utf8(setting(path, L"provider", L"fixed_timing_sha256"));
        fixedFdNominalSample = fixedFdDataSample = 0u;
        fixedFdNominalBitrate = fixedFdDataBitrate = 0u;
        const auto timingQuery = setting(path, L"provider", L"timing_query");
        const auto discoveryQuery = setting(path, L"provider", L"discovery_query");
        const auto validExport = [](const std::wstring& value) {
            return value.empty() || (value.size() <= 128u &&
                std::all_of(value.begin(), value.end(), [](wchar_t c) {
                    return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
                           (c >= L'0' && c <= L'9') || c == L'_';
                }));
        };
        if (!validExport(timingQuery) || !validExport(discoveryQuery))
            return Status::IncompatibleVersion;
        if (!fixedNominal.empty() || !fixedData.empty() || !fixedNominalRate.empty() ||
            !fixedDataRate.empty() || !fixedTimingVersion.empty() || !fixedTimingHash.empty()) {
            if (!number(fixedNominal, fixedFdNominalSample) || fixedFdNominalSample < 1u ||
                fixedFdNominalSample > 99u || !number(fixedData, fixedFdDataSample) ||
                fixedFdDataSample < 1u || fixedFdDataSample > 99u ||
                !number(fixedNominalRate, fixedFdNominalBitrate) || fixedFdNominalBitrate == 0u ||
                !number(fixedDataRate, fixedFdDataBitrate) || fixedFdDataBitrate == 0u ||
                fixedTimingVersion.empty() || fixedTimingHash.size() != 64u ||
                !std::all_of(fixedTimingHash.begin(), fixedTimingHash.end(), [](const char c) {
                    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
                }))
                return Status::InvalidArgument;
        }
        if (!number(setting(path, L"provider", L"pins"), pinAssignment) || pinAssignment > 0xffffu) return Status::InvalidArgument;
        const auto discoveryLibrary = setting(path, L"provider", L"discovery_library");
        const auto configListLayout = setting(path, L"provider", L"config_list_layout");
        if (!configListLayout.empty() && configListLayout != L"packed" && configListLayout != L"native")
            return Status::InvalidArgument;
        const bool useNativeConfigList = configListLayout == L"native";
        const auto configuration = utf8(runtime) + "|" + provider + "|" + utf8(timingQuery) + "|" + utf8(discoveryQuery) + "|" +
            fixedTimingVersion + "|" + fixedTimingHash + "|" + std::to_string(fixedFdNominalSample) +
            "|" + std::to_string(fixedFdDataSample) + "|" + std::to_string(fixedFdNominalBitrate) +
            "|" + std::to_string(fixedFdDataBitrate) + "|" + std::to_string(pinAssignment) + "|" + utf8(discoveryLibrary) +
            "|" + (useNativeConfigList ? "native" : "packed");
        if (loaded && configuration != loadedConfiguration) {
            if (!sessions.empty())
                return Status::Busy;
            FreeLibrary(module);
            module = nullptr;
            loaded = false;
            runtimeIdentity = {};
            entries.clear();
            releaseDiscovery();
        }
        if (!discoveryLibrary.empty() && loaded && !refresh && configuration == loadedConfiguration) return Status::Ok;
        nativeConfigList = useNativeConfigList;
        std::vector<UcalJ2534PortV1> discovered;
        std::vector<UcalJ2534PortV2> routes;
        U32 count = 0u;
        if (!absolutePath(runtime) || provider.empty() || provider.size() > 128u ||
            provider.find('|') != std::string::npos ||
            (discoveryLibrary.empty() && !number(setting(path, L"provider", L"endpoint_count"), count)) ||
            count > 64u || !number(setting(path, L"provider", L"pins"), pinAssignment) ||
            pinAssignment > 0xffffu)
            return Status::InvalidArgument;
        if (!discoveryLibrary.empty()) {
            if (!absolutePath(discoveryLibrary)) return Status::InvalidArgument;
            const auto discoveryStatus = discovery.discover(discoveryLibrary, runtime, discovered, routes);
            if (discoveryStatus != Status::Ok)
                return discoveryStatus;
            count = static_cast<U32>(discovered.size());
            for (const auto& port : discovered) {
                if (port.size != (routes.empty() ? sizeof(port) : sizeof(UcalJ2534PortV2)) ||
                    port.version != (routes.empty() ? 1u : 2u) || port.reserved || port.physicalPort > 255 ||
                    (port.capabilities & ~7u) || !(port.capabilities & UCAL_J2534_DISCOVERY_FD))
                    return Status::TransportError;
                for (const auto field : {std::make_pair(port.identity, sizeof(port.identity)), std::make_pair(port.adapterIdentity, sizeof(port.adapterIdentity)),
                    std::make_pair(port.selector, sizeof(port.selector)), std::make_pair(port.name, sizeof(port.name)),
                    std::make_pair(port.model, sizeof(port.model)), std::make_pair(port.serial, sizeof(port.serial))}) {
                    const auto end = static_cast<const char*>(std::memchr(field.first, 0, field.second));
                    if (!end || std::any_of(field.first, end, [](char c) { return c == '\r' || c == '\n'; }) ||
                        (end != field.first && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, field.first, static_cast<int>(end-field.first), nullptr, 0))) return Status::TransportError;
                }
            }
        }
        std::vector<Entry> candidate;
        std::set<std::string> selectors;
        std::set<std::string> fdRoutes;
        std::set<std::string> groupedPorts;
        std::set<DeviceId> ids;
        for (U32 i = 0; i < count; ++i) {
            const auto section = L"endpoint." + std::to_wstring(i);
            const auto readSetting = [&](const wchar_t *key) {
                if (discoveryLibrary.empty()) return setting(path, section.c_str(), key);
                const auto& p = discovered[i];
                const std::map<std::wstring, std::string> fields{
                    {L"selector", p.selector}, {L"adapter_identity", p.identity}, {L"physical_adapter_identity", p.adapterIdentity},
                    {L"open_mode", p.selector[0] ? "named" : "default"}, {L"physical_port", std::to_string(p.physicalPort)},
                    {L"name", p.name}, {L"model", p.model}, {L"serial", p.serial},
                    {L"brs", (p.capabilities & UCAL_J2534_DISCOVERY_BRS) ? "1" : "0"},
                    {L"termination", (p.capabilities & UCAL_J2534_DISCOVERY_TERMINATION) ? "1" : "0"}};
                const auto found = fields.find(key); if (found == fields.end()) return std::wstring{};
                const auto& text = found->second;
                const auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
                std::wstring value(static_cast<std::size_t>(size), L'\0');
                if (size) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), value.data(), size);
                return value;
            };
            Entry entry;
            entry.pins = pinAssignment;
            if (!routes.empty()) {
                const auto &route = routes[i];
                entry.connectionModel = route.connectionModel;
                entry.classicProtocol = route.classicProtocol;
                entry.fdProtocol = route.fdProtocol;
                entry.pins = route.pinAssignment ? route.pinAssignment : pinAssignment;
                if ((entry.connectionModel != UCAL_J2534_OPEN_PER_PORT &&
                     entry.connectionModel != UCAL_J2534_OPEN_SHARED) ||
                    entry.fdProtocol != Abi::gmCanFd || entry.pins > 0xffffu ||
                    (entry.classicProtocol != Abi::can &&
                     (entry.classicProtocol < 0x9000u || entry.classicProtocol > 0x907fu)) ||
                    !std::memchr(route.deviceKey, 0, sizeof(route.deviceKey)) ||
                    std::any_of(std::begin(route.reserved), std::end(route.reserved),
                                [](U32 v) { return v != 0; }))
                    return Status::TransportError;
                entry.deviceKey = route.deviceKey;
                if (entry.deviceKey.find_first_of("\r\n|") != std::string::npos ||
                    (!entry.deviceKey.empty() &&
                     !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, entry.deviceKey.data(),
                                          static_cast<int>(entry.deviceKey.size()), nullptr, 0)) ||
                    (entry.connectionModel == UCAL_J2534_OPEN_SHARED && entry.deviceKey.empty()))
                    return Status::TransportError;
            }
            entry.selector = utf8(readSetting(L"selector"));
            const auto openMode = readSetting(L"open_mode");
            entry.defaultSelector = openMode == L"default";
            if ((!openMode.empty() && openMode != L"default" && openMode != L"named") ||
                (entry.defaultSelector && !entry.selector.empty())) return Status::InvalidArgument;
            const auto adapter = utf8(readSetting(L"adapter_identity"));
            const auto physicalAdapter = utf8(readSetting(L"physical_adapter_identity"));
            U32 port = 0u, brs = 0u, termination = 0u;
            if ((!entry.defaultSelector && entry.selector.empty()) || entry.selector.size() > 512u ||
                adapter.empty() || adapter.size() > 128u || adapter.find('|') != std::string::npos ||
                physicalAdapter.size() > 128u || physicalAdapter.find('|') != std::string::npos ||
                !selectors
                     .insert(entry.connectionModel == UCAL_J2534_OPEN_SHARED
                                 ? "shared|" + entry.deviceKey + "|" +
                                       std::to_string(entry.classicProtocol) + "|" +
                                       std::to_string(entry.pins)
                                 : "port|" + entry.selector)
                     .second ||
                !number(readSetting(L"physical_port"), port) || port > 255u ||
                !number(readSetting(L"brs"), brs) || brs > 1u ||
                !number(readSetting(L"termination"), termination) || termination > 1u)
                return Status::InvalidArgument;
            if (entry.connectionModel == UCAL_J2534_OPEN_SHARED &&
                !fdRoutes
                     .insert(entry.deviceKey + "|" + std::to_string(entry.fdProtocol) + "|" +
                             std::to_string(entry.pins))
                     .second)
                return Status::InvalidArgument;
            auto &e = entry.endpoint;
            const auto material = (std::string(endpointIdentityNamespaceV1.data(), endpointIdentityNamespaceV1.size()) + ".j2534/1|") + provider + "|" + adapter;
            e.adapterId = identity(material);
            if(!physicalAdapter.empty()) e.adapterId = identity((std::string(endpointIdentityNamespaceV1.data(), endpointIdentityNamespaceV1.size()) + ".j2534/1|") + provider + "|" + physicalAdapter);
            e.id = identity(material + "|can|" + std::to_string(port));
            if(!physicalAdapter.empty()&&!groupedPorts.insert(physicalAdapter+"|"+std::to_string(port)).second)
                return Status::InvalidArgument;
            if (e.id == DeviceId{} || !ids.insert(e.id).second)
                return Status::InvalidArgument;
            e.name = utf8(readSetting(L"name"));
            e.model = utf8(readSetting(L"model"));
            if (e.name.empty() || e.name.size() > 127u || e.model.size() > 127u)
                return Status::InvalidArgument;
            e.serial = utf8(readSetting(L"serial"));
            if (e.serial.size() > 127u)
                return Status::InvalidArgument;
            e.backendFamily = "j2534";
            e.bindingIsDefault = entry.defaultSelector;
            e.physicalIndex = port;
            e.capabilities = CapabilityClassicCan | CapabilityCanFdIso |
                             (brs ? CapabilityBrs : 0u) |
                             (termination ? CapabilityTermination : 0u);
            e.configuration.classicCan = baselineAccess(false);
            e.configuration.canFd = baselineAccess(true);
            candidate.push_back(std::move(entry));
        }
        for (const auto &a : candidate)
            for (const auto &b : candidate) {
                if (&a != &b && a.connectionModel == UCAL_J2534_OPEN_SHARED &&
                    b.connectionModel == UCAL_J2534_OPEN_SHARED && a.deviceKey == b.deviceKey &&
                    (a.selector != b.selector || a.defaultSelector != b.defaultSelector))
                    return Status::InvalidArgument;
            }
        dynamicDiscovery = !discoveryLibrary.empty();
        if (loaded && configuration == loadedConfiguration) {
            // Validate the entire update before modifying indices referenced by
            // live sessions. Adding ports never moves an existing route.
            for (const auto& next : candidate) {
                for (std::size_t i = 0; i < entries.size(); ++i) {
                    if (entries[i].endpoint.id != next.endpoint.id) continue;
                    if ((entries[i].selector != next.selector ||
                         entries[i].defaultSelector != next.defaultSelector ||
                         entries[i].connectionModel != next.connectionModel ||
                         entries[i].deviceKey != next.deviceKey ||
                         entries[i].classicProtocol != next.classicProtocol ||
                         entries[i].fdProtocol != next.fdProtocol || entries[i].pins != next.pins) &&
                        std::any_of(sessions.begin(), sessions.end(),
                                    [&](const auto &item) { return item.second.entry == i; }))
                        return Status::Busy;
                    if (((entries[i].endpoint.capabilities & ~CapabilityTermination) !=
                            (next.endpoint.capabilities & ~CapabilityTermination) || entries[i].endpoint.adapterId != next.endpoint.adapterId) &&
                        std::any_of(sessions.begin(), sessions.end(), [&](const auto& item) { return item.second.entry == i; }))
                        return Status::Busy;
                }
            }
            for (auto& entry : entries)if(!ids.count(entry.endpoint.id)){
                    // A public SDK in another process may hide the whole physical
                    // adapter while PassThru owns one port. Retain only this live
                    // adapter's in-memory routes; I/O failure or last Close ends it.
                    const bool owned = dynamicDiscovery &&
                                       std::any_of(sessions.begin(), sessions.end(), [&](const auto &item) {
                                           return !item.second.unavailable &&
                                                  entries[item.second.entry].endpoint.adapterId ==
                                                      entry.endpoint.adapterId;
                                       });
                    if (!owned) {
                        entry.configured = false;
                        entry.presenceValidUntil = {};
                    }
            }
            for (auto& next : candidate) {
                const auto found = std::find_if(entries.begin(), entries.end(), [&](const auto& old) {
                    return old.endpoint.id == next.endpoint.id;
                });
                if (found == entries.end()) entries.push_back(std::move(next));
                else {
                    if(found->configured && next.endpoint.adapterId==found->endpoint.adapterId){
                        next.observedPresence=found->observedPresence;
                        next.presenceValidUntil=found->presenceValidUntil;
                    }
                    next.endpoint.configuration = found->endpoint.configuration;
                    // API-observed optional support survives catalog refresh.
                    next.endpoint.capabilities |= found->endpoint.capabilities & CapabilityTermination;
                    *found = std::move(next);
                }
            }
            return Status::Ok;
        }
        if (loaded) {
            if (!sessions.empty()) return Status::Busy;
            FreeLibrary(module); module = nullptr; loaded = false;
            runtimeIdentity = {}; entries.clear();
        }
        module = LoadLibraryExW(runtime.c_str(), nullptr,
                                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module)
            return Status::NotFound;
        runtimeIdentity = Ucal::Backend::loadedRuntimeIdentity(module);
        symbol(open, "PassThruOpen");
        symbol(close, "PassThruClose");
        symbol(connect, "PassThruConnect");
        symbol(disconnect, "PassThruDisconnect");
        symbol(read, "PassThruReadMsgs");
        symbol(write, "PassThruWriteMsgs");
        symbol(startFilter, "PassThruStartMsgFilter");
        symbol(stopFilter, "PassThruStopMsgFilter");
        symbol(version, "PassThruReadVersion");
        symbol(lastError, "PassThruGetLastError");
        symbol(ioctl, "PassThruIoctl");
        readTiming = nullptr;
        enumeratePorts = nullptr;
        if (!timingQuery.empty()) symbol(readTiming, utf8(timingQuery).c_str());
        if (!discoveryQuery.empty()) symbol(enumeratePorts, utf8(discoveryQuery).c_str());
        const std::array<const char*, 3> additionalExports{
            "PassThruStartPeriodicMsg", "PassThruStopPeriodicMsg", "PassThruSetProgrammingVoltage"};
        if (!open || !close || !connect || !disconnect || !read || !write || !startFilter ||
            !stopFilter || !version || !lastError || !ioctl ||
            (!timingQuery.empty() && !readTiming) ||
            (!discoveryQuery.empty() && !enumeratePorts) ||
            (!fixedTimingHash.empty() && fixedTimingHash != runtimeIdentity.sha256) ||
            !std::all_of(additionalExports.begin(), additionalExports.end(), [&](const char* name) {
                return GetProcAddress(module, name) != nullptr;
            })) {
            FreeLibrary(module);
            module = nullptr;
            runtimeIdentity = {};
            return Status::IncompatibleVersion;
        }
        entries = std::move(candidate);
        loadedConfiguration = configuration;
        loaded = true;
        return Status::Ok;
    }
    static CanParameterAccess baselineAccess(const bool fd) {
        CanParameterAccess access{};
        access.writeKnown = CanConfigListenOnly | CanConfigNoAck | CanConfigInternalLoopback |
            (fd ? CanConfigNominalSample | CanConfigDataSample
                : CanConfigDataBitrate | CanConfigDataSample);
        return access;
    }
    CanParameterAccess& access(Session& session) {
        auto& configuration = entries[session.entry].endpoint.configuration;
        return session.protocol == entries[session.entry].fdProtocol ? configuration.canFd
                                                                      : configuration.classicCan;
    }
    static U32 parameterFlag(const U32 parameter) {
        if (parameter == Abi::dataRate) return CanConfigNominalBitrate;
        if (parameter == Abi::gmDataRate) return CanConfigDataBitrate;
        if (parameter == Abi::samplePoint) return CanConfigNominalSample;
        if (parameter == Abi::gmTermination || parameter == Abi::hsTermination) return CanConfigTermination;
        return 0u;
    }
    std::pair<Abi::Result, bool> configIoctl(U32 channel, U32 operation, Abi::Config& config) const {
        const auto call = [&](auto list) {
            const auto result = ioctl(channel, operation, &list, nullptr);
            return std::make_pair(result, list.count == 1u && list.items == &config);
        };
        return nativeConfigList ? call(Abi::NativeConfigList{1u, &config})
                                : call(Abi::ConfigList{1u, &config});
    }
    Status configure(Session& session, const U32 parameter, const U32 value,
                     const bool optionalDefault = false) {
        auto& knowledge = access(session);
        const auto flag = parameterFlag(parameter);
        Abi::Config config{parameter, value};
        const auto result = configIoctl(session.channel, Abi::setConfig, config).first;
        if (result != Abi::ok) {
            // Invalid values and transient failures do not permanently disable
            // a setter. Unsupported describes this protocol/configuration only.
            if (result == Abi::notSupported || result == 0xf) {
                knowledge.writeKnown |= flag;
                knowledge.writeSupported &= ~flag;
                // A refused setter may still have a readable parameter.
                Abi::Config query{parameter, 0u};
                const auto [readResult, validList] = configIoctl(session.channel, Abi::getConfig, query);
                if (readResult == Abi::ok) {
                    if (!validList || query.parameter != parameter)
                        return Status::TransportError;
                    knowledge.readKnown |= flag;
                    knowledge.readSupported |= flag;
                    if (optionalDefault && query.value != value) return Status::Unsupported;
                } else if (readResult == Abi::notSupported || readResult == 0xf || readResult == 5) {
                    knowledge.readKnown |= flag;
                    knowledge.readSupported &= ~flag;
                } else if (mapResult(readResult) == Status::NotFound ||
                           mapResult(readResult) == Status::TransportError) {
                    return mapResult(readResult);
                }
                if (optionalDefault) return Status::Ok;
            }
            diagnostic("set_config", result);
            return mapResult(result);
        }
        knowledge.writeKnown |= flag;
        knowledge.writeSupported |= flag;
        if (flag == CanConfigTermination)
            entries[session.entry].endpoint.capabilities |= CapabilityTermination;
        // GET is optional. A driver that accepts SET must remain usable when it
        // cannot read the parameter. No requested value is advertised as readback.
        config = {parameter, ~value};
        const auto [queried, validList] = configIoctl(session.channel, Abi::getConfig, config);
        if (queried == Abi::ok) {
            if (!validList || config.parameter != parameter)
                return Status::TransportError;
            knowledge.readKnown |= flag;
            knowledge.readSupported |= flag;
            if (config.value != value) {
                diagnostic("config_mismatch", queried);
                return Status::Unsupported;
            }
        } else if (queried == Abi::notSupported || queried == 0xf || queried == 5) {
            knowledge.readKnown |= flag;
            knowledge.readSupported &= ~flag;
        } else if (mapResult(queried) == Status::NotFound || mapResult(queried) == Status::TransportError) {
            return mapResult(queried);
        }
        return Status::Ok;
    }
    Status installFilters(Session &session) {
        for (const U32 flags : {0u, Abi::extended}) {
            Abi::Message mask{}, pattern{};
            mask.protocol = pattern.protocol = session.protocol;
            mask.txFlags = pattern.txFlags = flags;
            mask.dataSize = pattern.dataSize = 4u;
            U32 filter{};
            const auto result = startFilter(session.channel, 1u, &mask, &pattern, nullptr, &filter);
            if (result != Abi::ok)
                return mapResult(result);
            session.filters.push_back(filter);
        }
        return Status::Ok;
    }
    Status destroy(Session &s) {
        Status status = Status::Ok;
        const auto observe = [&](const Abi::Result result) {
            const auto mapped = mapResult(result);
            if (mapped == Status::NotFound) s.unavailable = true;
            // A disconnected device can still own runtime handles. Attempt every
            // cleanup step, but an absent endpoint must not invalidate its peers.
            // Runtimes may report failed/unsupported hardware cleanup after loss.
            if (s.unavailable && (mapped == Status::Unsupported || mapped == Status::InternalError))
                return;
            if (mapped != Status::Ok && mapped != Status::NotFound && status == Status::Ok)
                status = mapped;
        };
        for (const auto filter : s.filters) {
            observe(stopFilter(s.channel, filter));
        }
        s.filters.clear();
        if (s.channelConnected) observe(disconnect(s.channel));
        if (s.deviceOpen) {
            const auto device = devices.find(s.deviceKey);
            if (device == devices.end() || device->second.handle != s.device ||
                device->second.references == 0u)
                status = Status::InternalError;
            else if (--device->second.references == 0u) {
                observe(close(device->second.handle));
                devices.erase(device);
            }
        }
        s.channelConnected = s.deviceOpen = false;
        s.channel = s.device = 0u;
        return status;
    }
    Status acquireDevice(const Entry &entry, Session &session) {
        session.deviceKey = entry.connectionModel == UCAL_J2534_OPEN_SHARED ? "shared|" + entry.deviceKey
                                                                              : "port|" + entry.selector;
        const auto existing = devices.find(session.deviceKey);
        if (existing != devices.end()) {
            if (entry.connectionModel != UCAL_J2534_OPEN_SHARED)
                return Status::Busy;
            session.device = existing->second.handle;
            ++existing->second.references;
            session.deviceOpen = true;
            return Status::Ok;
        }
        auto selector = entry.selector;
        const auto result = open(entry.defaultSelector ? nullptr : selector.data(), &session.device);
        if (result != Abi::ok)
            return mapResult(result);
        devices.emplace(session.deviceKey, DeviceSession{session.device, 1u});
        session.deviceOpen = true;
        return Status::Ok;
    }
    Status sessionResult(Session &s, const Abi::Result result) {
        const auto status = mapResult(result);
        if (status == Status::NotFound) s.unavailable = true;
        return status;
    }
    Session *session(const std::uint64_t token, const U32 owner) {
        const auto found = sessions.find(token);
        return found != sessions.end() && found->second.owner == owner ? &found->second : nullptr;
    }
    DeviceState presence(Entry &entry) {
        if (!entry.configured) return DeviceState::Offline;
        if (dynamicDiscovery) return DeviceState::Available;
        if (enumeratePorts) {
            const auto found = physicalPorts.find(entry.selector);
            return found != physicalPorts.end() && (found->second & 3u) == 3u
                ? DeviceState::Available : DeviceState::Offline;
        }
        // 04.04 has no portable enumeration API. Probe only the explicitly
        // configured selector. Never Connect, configure or transmit on refresh.
        // A leased adapter is already present; do not reopen it during traffic.
        for (const auto &item : sessions) {
            if (entries[item.second.entry].endpoint.adapterId == entry.endpoint.adapterId)
                return DeviceState::Available;
        }
        // Descriptor validation and Open both inspect the catalog. Reuse a
        // short idle observation; the actual Open below remains authoritative.
        if(std::chrono::steady_clock::now()<entry.presenceValidUntil)return entry.observedPresence;
        const auto observePresence=[&](DeviceState state){entry.observedPresence=state;entry.presenceValidUntil=std::chrono::steady_clock::now()+std::chrono::seconds(1);return state;};
        auto selector = entry.selector;
        U32 device = 0u;
        const auto opened = open(entry.defaultSelector ? nullptr : selector.data(), &device);
        if (opened != Abi::ok) {
            return observePresence(DeviceState::Offline);
        }
        std::array<char, 80> firmware{}, dll{}, api{};
        const auto versionResult = version(device, firmware.data(), dll.data(), api.data());
        const auto closed = close(device);
        if (versionResult == Abi::ok && std::memchr(api.data(), 0, api.size()) != nullptr)
            runtimeIdentity.apiVersion = api.data();
        return observePresence(versionResult == Abi::ok && closed == Abi::ok &&
                       std::memchr(api.data(), 0, api.size()) != nullptr &&
                       std::string(api.data()) == "04.04"
                   ? DeviceState::Available
                   : DeviceState::Offline);
    }
    Status receive(Session &s, U32 capacity, std::vector<CanEvent> &events) {
        events.clear();
        if (s.unavailable) return Status::NotFound;
        if (capacity == 0u || capacity > 4096u)
            return Status::InvalidArgument;
        const auto reportOverflow = [&] {
            CanEvent event{};
            event.kind = CanEventKind::QueueOverrun;
            event.backendStatus = static_cast<U32>(Abi::overflow);
            event.result = Status::Busy;
            events.push_back(event);
            s.overflowPending = false;
        };
        if (s.overflowPending)
            reportOverflow();
        if (events.size() == capacity)
            return Status::Ok;
        std::vector<Abi::Message> buffer(
            (std::min)(capacity - static_cast<U32>(events.size()), 32u));
        U32 count = static_cast<U32>(buffer.size());
        const auto result = read(s.channel, buffer.data(), &count, 0u);
        if (count > buffer.size()) {
            diagnostic("read_count", result);
            return Status::TransportError;
        }
        // Preserve a valid returned prefix even when the runtime reports overflow.
        if (result != Abi::ok && result != Abi::empty && result != Abi::timeout &&
            result != Abi::overflow) {
            diagnostic("read_api", result);
            return sessionResult(s, result);
        }
        for (U32 i = 0u; i < count; ++i) {
            const auto &message = buffer[i];
            auto rejected = [&](const char* reason) {
                std::cerr << "{\"event\":\"j2534_receive_rejected\",\"reason\":\"" << reason
                          << "\",\"protocol\":" << message.protocol
                          << ",\"rxStatus\":" << message.rxStatus
                          << ",\"dataSize\":" << message.dataSize
                          << ",\"extraDataIndex\":" << message.extraDataIndex << "}" << std::endl;
                return Status::TransportError;
            };
            if (message.protocol != s.protocol)
                return rejected("protocol");
            if (message.rxStatus & (Abi::txEcho | Abi::txIndication))
                continue;
            if (message.rxStatus & Abi::errorIndication) {
                CanEvent event{};
                event.kind = CanEventKind::ControllerState;
                event.result = Status::TransportError;
                event.backendStatus = message.rxStatus;
                events.push_back(event); // Partial indication is never a valid full-state snapshot.
                continue;
            }
            constexpr U32 allowed = Abi::extended | Abi::fdBrs | Abi::fdFormat | Abi::fdEsi;
            if ((message.rxStatus & ~allowed) || message.dataSize < 4u ||
                message.dataSize > 68u ||
                (message.extraDataIndex != 0u && message.extraDataIndex != message.dataSize))
                return rejected("envelope");
            CanEvent event{};
            auto &frame = event.frame;
            frame.id = (static_cast<U32>(message.data[0]) << 24u) |
                       (static_cast<U32>(message.data[1]) << 16u) |
                       (static_cast<U32>(message.data[2]) << 8u) | message.data[3];
            if (message.rxStatus & Abi::extended)
                frame.flags |= FrameExtended;
            if (message.rxStatus & Abi::fdFormat)
                frame.flags |= FrameFd;
            if (message.rxStatus & Abi::fdBrs)
                frame.flags |= FrameBrs;
            if (message.rxStatus & Abi::fdEsi)
                frame.flags |= FrameEsi;
            const auto length = static_cast<std::uint8_t>(message.dataSize - 4u);
            const auto dlc = std::find(lengths.begin(), lengths.end(), length);
            if (dlc == lengths.end() ||
                frame.id > ((frame.flags & FrameExtended) ? 0x1fffffffu : 0x7ffu) ||
                (!(frame.flags & FrameFd) &&
                 (length > 8u || (frame.flags & (FrameBrs | FrameEsi)))))
                return rejected("frame-shape");
            frame.dlc = static_cast<std::uint8_t>(dlc - lengths.begin());
            frame.dataLength = length;
            std::copy_n(message.data + 4, length, frame.data.begin());
            const auto now = GetTickCount64();
            if (s.timestampSeen && now - s.lastObservation >= 4294967u)
                s.timestampInvalid = true;
            if (s.timestampSeen && message.timestamp < s.lastTimestamp) {
                if (s.lastTimestamp - message.timestamp > 0x80000000u)
                    s.timestampEpoch += 1ull << 32u;
                else
                    s.timestampInvalid = true;
            }
            s.timestampSeen = true;
            s.lastObservation = now;
            s.lastTimestamp = message.timestamp;
            frame.timestampUs = s.timestampInvalid ? 0u : s.timestampEpoch + message.timestamp;
            if (!(frame.flags & FrameFd) || s.config.canFd) {
                if (accepts(s.config, frame))
                    events.push_back(event);
            }
        }
        if (result == Abi::overflow) {
            s.overflowPending = true;
            if (events.size() < capacity)
                reportOverflow();
        }
        return Status::Ok;
    }
};

Backend::Backend(std::wstring configurationPath) : implementation_(std::make_unique<Implementation>()) {
    implementation_->configurationPath = std::move(configurationPath);
}
Backend::~Backend() {
    auto &impl = *implementation_;
    impl.executor.invoke([&] {
        for (auto &item : impl.sessions)
            impl.destroy(item.second);
        impl.sessions.clear();
        if (impl.module)
            FreeLibrary(impl.module);
        impl.releaseDiscovery();
    });
}
Status Backend::initialize() {
    return implementation_->executor.invoke([&] { return implementation_->load(); });
}
BackendRuntimeIdentity Backend::runtimeIdentity() const {
    return implementation_->executor.invoke([&] { return implementation_->runtimeIdentity; });
}
Status Backend::enumerateEndpoints(std::vector<EndpointDescriptor> &endpoints) {
    return implementation_->executor.invoke([&] {
        endpoints.clear();
        const auto status = implementation_->load(true);
        if (status != Status::Ok)
            return status;
        if (implementation_->enumeratePorts) {
            std::vector<UcalProviderPortV1> ports(256u);
            U32 count = static_cast<U32>(ports.size());
            const auto listed = implementation_->enumeratePorts(sizeof(UcalProviderPortV1), 1u, ports.data(), &count);
            if (listed != Abi::ok) return mapResult(listed);
            if (count > ports.size()) return Status::TransportError;
            std::map<std::string, U32> observed;
            for (U32 i = 0; i < count; ++i) {
                const auto& port = ports[i];
                if (port.size != sizeof(port) || port.version != 1u || port.physicalPort > 255u ||
                    (port.capabilities & ~3u) != 0u ||
                    !std::memchr(port.selector, 0, sizeof(port.selector))) return Status::TransportError;
                const std::string selector(port.selector);
                if (selector.size() < 26u || selector.compare(24u, std::string::npos, ":" + std::to_string(port.physicalPort)) != 0 ||
                    !std::all_of(selector.begin(), selector.begin() + 24u, [](const char c) {
                        return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F');
                    }) || !observed.emplace(selector, port.capabilities).second) return Status::TransportError;
            }
            implementation_->physicalPorts = std::move(observed);
        }
        for (std::size_t i = 0; i < implementation_->entries.size(); ++i) {
            auto endpoint = implementation_->entries[i].endpoint;
            const auto observed = implementation_->physicalPorts.find(implementation_->entries[i].selector);
            if (implementation_->enumeratePorts && observed != implementation_->physicalPorts.end() &&
                (observed->second & 2u) == 0u)
                endpoint.capabilities &= ~(CapabilityCanFdIso | CapabilityBrs);
            endpoint.state = implementation_->presence(implementation_->entries[i]);
            for (const auto &item : implementation_->sessions)
                if (item.second.entry == i)
                    endpoint.state = item.second.unavailable ? DeviceState::Offline : DeviceState::Leased;
            if (!implementation_->dynamicDiscovery || implementation_->entries[i].configured || endpoint.state == DeviceState::Leased)
                endpoints.push_back(std::move(endpoint));
        }
        return Status::Ok;
    });
}
Status Backend::enumerate(std::vector<DeviceDescriptor> &devices) {
    devices.clear();
    std::vector<EndpointDescriptor> endpoints;
    const auto status = enumerateEndpoints(endpoints);
    if (status != Status::Ok)
        return status;
    for (const auto &e : endpoints)
        devices.push_back({e.id, e.name, e.model, e.serial, 1u, e.capabilities, e.state});
    return Status::Ok;
}
OpenResult Backend::open(const DeviceId &id, const CanChannelConfig &config, const U32 owner) {
    return implementation_->executor.invoke([&]() -> OpenResult {
        auto &impl = *implementation_;
        auto status = impl.load(true);
        if (status != Status::Ok)
            return {status, 0u};
        const auto found = std::find_if(impl.entries.begin(), impl.entries.end(),
                                        [&](const auto &e) { return e.endpoint.id == id; });
        if (found == impl.entries.end() || !found->configured)
            return {Status::NotFound, 0u};
        if (owner == 0u || config.arbitrationBitrate == 0u ||
            (config.canFd && config.dataBitrate == 0u) ||
            (config.arbitrationSamplePointSpecified &&
             (config.arbitrationSamplePoint == 0u || config.arbitrationSamplePoint >= 100u)) ||
            (config.dataSamplePointSpecified &&
             (config.dataSamplePoint == 0u || config.dataSamplePoint >= 100u)) ||
            config.acceptanceFilterCount > config.acceptanceFilters.size())
            return {Status::InvalidArgument, 0u};
        if (config.listenOnly || config.noAck || config.internalLoopback ||
            (!impl.readTiming && config.canFd &&
             (config.arbitrationSamplePointSpecified || config.dataSamplePointSpecified) &&
             (config.arbitrationBitrate != impl.fixedFdNominalBitrate ||
              config.dataBitrate != impl.fixedFdDataBitrate)) ||
            (!impl.readTiming && config.canFd && config.arbitrationSamplePointSpecified &&
             config.arbitrationSamplePoint != impl.fixedFdNominalSample) ||
            (!impl.readTiming && config.canFd && config.dataSamplePointSpecified &&
             config.dataSamplePoint != impl.fixedFdDataSample) ||
            (!config.canFd && config.dataSamplePointSpecified))
            return {Status::Unsupported, 0u};
        for (std::size_t i = 0; i < config.acceptanceFilterCount; ++i) {
            const auto &f = config.acceptanceFilters[i];
            if (f.frameType != CanFilterFrameType::Standard &&
                f.frameType != CanFilterFrameType::Extended)
                return {Status::InvalidArgument, 0u};
            const U32 maximum = f.frameType == CanFilterFrameType::Standard ? 0x7ffu : 0x1fffffffu;
            if (f.idCode > maximum || f.idMask > maximum)
                return {Status::InvalidArgument, 0u};
        }
        const auto index = static_cast<std::size_t>(found - impl.entries.begin());
        for (const auto &item : impl.sessions)
            if (item.second.entry == index)
                return {Status::Busy, 0u};
        Implementation::Session session{};
        session.owner = owner;
        session.entry = index;
        session.config = config;
        found->endpoint.configuration.canFd = Implementation::baselineAccess(true);
        if (!config.canFd) found->endpoint.configuration.classicCan = Implementation::baselineAccess(false);
        const auto abort = [&](const Status failure) -> OpenResult {
            impl.destroy(session);
            if (failure == Status::NotFound || failure == Status::TransportError)
                found->endpoint.state = DeviceState::Offline;
            return {failure, 0u};
        };
        status = impl.acquireDevice(*found, session);
        if (status != Status::Ok)
            return abort(status);
        Abi::Result result{};
        std::array<char, 80> firmware{}, dll{}, api{};
        result = impl.version(session.device, firmware.data(), dll.data(), api.data());
        if (result != Abi::ok || std::memchr(api.data(), 0, api.size()) == nullptr ||
            std::string(api.data()) != "04.04")
            return abort(Status::IncompatibleVersion);
        impl.runtimeIdentity.apiVersion = api.data();
        // Some drivers document one fixed FD sampling pair but cannot set/read it
        // through 04.04. Such an explicit deployment contract is version-scoped;
        // arbitrary requested points remain unsupported. No default is invented.
        if (!impl.readTiming && config.canFd &&
            (config.arbitrationSamplePointSpecified || config.dataSamplePointSpecified) &&
            (std::memchr(dll.data(), 0, dll.size()) == nullptr ||
             std::string(dll.data()) != impl.fixedTimingVersion))
            return abort(Status::Unsupported);
        session.protocol = found->fdProtocol;
        result = impl.connect(session.device, found->fdProtocol, Abi::bothIds, config.arbitrationBitrate,
                              &session.channel);
        if (result != Abi::ok)
            return abort(mapResult(result));
        session.channelConnected = true;
        impl.access(session).writeKnown |= CanConfigNominalBitrate;
        impl.access(session).writeSupported |= CanConfigNominalBitrate;
        // Qualify the physical port through GM raw FD before either business
        // mode. Classic CAN may then use its standard sampling-point parameter.
        const std::array<Abi::Config, 2> required{
            {{Abi::dataRate, config.arbitrationBitrate},
             {Abi::gmDataRate, config.canFd ? config.dataBitrate : 2000000u}}};
        for (const auto &item : required) {
            status = impl.configure(session, item.parameter, item.value);
            if (status != Status::Ok)
                return abort(status);
        }
        // ISO is the requested GM format. Drivers may expose only that default;
        // lack of the optional type setter does not add a new admission floor.
        status = impl.configure(session, Abi::gmType, 0u, true);
        if (status != Status::Ok) return abort(status);
        if (!config.canFd) {
            // Qualify the same pin route, with filters installed before any
            // activation. Additional Classic ProtocolIDs must never qualify a
            // different physical port by probing the driver's default FD port.
            status = impl.installFilters(session);
            if (status != Status::Ok)
                return abort(status);
            if (found->pins) {
                status = impl.configure(session, Abi::pins, found->pins, found->pins == 0x060eu);
                if (status != Status::Ok)
                    return abort(status);
            }
            while (!session.filters.empty()) {
                result = impl.stopFilter(session.channel, session.filters.back());
                if (result != Abi::ok)
                    return abort(mapResult(result));
                session.filters.pop_back();
            }
            result = impl.disconnect(session.channel);
            if (result != Abi::ok)
                return abort(mapResult(result));
            session.channel = 0u;
            session.channelConnected = false;
            session.protocol = found->classicProtocol;
            result = impl.connect(session.device, found->classicProtocol, Abi::bothIds,
                                  config.arbitrationBitrate, &session.channel);
            if (result != Abi::ok)
                return abort(mapResult(result));
            session.channelConnected = true;
            status = impl.configure(session, Abi::dataRate, config.arbitrationBitrate);
            if (status != Status::Ok)
                return abort(status);
            if (config.arbitrationSamplePointSpecified) {
                status = impl.configure(session, Abi::samplePoint,
                                        config.arbitrationSamplePoint);
                if (status != Status::Ok)
                    return abort(status);
            }
        }
        if (config.terminationSpecified) {
            status = impl.configure(session,
                                    config.canFd ? Abi::gmTermination : Abi::hsTermination,
                                    config.termination ? (config.canFd ? 1u : 3u) : 0u);
            if (status != Status::Ok)
                return abort(status);
        }
        if (config.canFd && impl.readTiming) {
            UcalProviderCanTimingV1 timing{sizeof(timing), 1u, 0u, 0u, 0u, 0u};
            const auto queried = impl.readTiming(session.channel, &timing);
            if (queried != Abi::ok || timing.size != sizeof(timing) || timing.version != 1u ||
                timing.nominalBitrate != config.arbitrationBitrate || timing.dataBitrate != config.dataBitrate ||
                (config.arbitrationSamplePointSpecified && timing.nominalSample != config.arbitrationSamplePoint) ||
                (config.dataSamplePointSpecified && timing.dataSample != config.dataSamplePoint))
                return abort(Status::Unsupported);
        }
        status = impl.installFilters(session);
        if (status != Status::Ok)
            return abort(status);
        impl.access(session).writeKnown |= CanConfigReceiveFilter;
        impl.access(session).writeSupported |= CanConfigReceiveFilter;
        // Activate only after timing validation and receive filters are ready.
        // A peer may already have a pending frame when this starts ACKing.
        if (found->pins != 0u && (config.canFd || found->pins != 0x060eu)) {
            // The conventional 6/14 assignment is the CAN default. Drivers
            // with a fixed/default route need not implement its setter. Other
            // explicitly selected pin assignments must still be enforceable.
            status = impl.configure(session, Abi::pins, found->pins, found->pins == 0x060eu);
            if (status != Status::Ok)
                return abort(status);
        }
        const auto token = randomToken();
        if (token == 0u || impl.sessions.count(token))
            return abort(Status::InternalError);
        found->endpoint.state = DeviceState::Available;
        impl.sessions.emplace(token, std::move(session));
        return {Status::Ok, token};
    });
}
Status Backend::close(const std::uint64_t token, const U32 owner) {
    return implementation_->executor.invoke([&] {
        auto *s = implementation_->session(token, owner);
        if (!s)
            return Status::NotFound;
        const auto status = implementation_->destroy(*s);
        implementation_->sessions.erase(token);
        return status;
    });
}
Status Backend::write(const std::uint64_t token, const U32 owner, const CanFrame &frame) {
    return submit(token, owner, frame);
}
Status Backend::submit(const std::uint64_t token, const U32 owner, const CanFrame &frame) {
    U32 accepted = 0;
    return submitBatch(token, owner, {frame}, accepted);
}
Status Backend::submitBatch(const std::uint64_t token, const U32 owner,
                             const std::vector<CanFrame> &frames, U32 &accepted) {
    return implementation_->executor.invoke([&] {
        accepted = 0u;
        auto &impl = *implementation_;
        auto *s = impl.session(token, owner);
        if (!s || s->unavailable)
            return Status::NotFound;
        if (frames.empty() || frames.size() > 256u)
            return Status::InvalidArgument;
        std::vector<Abi::Message> messages(frames.size());
        for (std::size_t i = 0; i < frames.size(); ++i) {
            const auto &f = frames[i];
            if (!validFrame(f, s->config, impl.entries[s->entry].endpoint.capabilities))
                return Status::Unsupported;
            auto &m = messages[i];
            m.protocol = s->protocol;
            if (f.flags & FrameExtended)
                m.txFlags |= Abi::extended;
            if (f.flags & FrameFd)
                m.txFlags |= Abi::fdFormat;
            if (f.flags & FrameBrs)
                m.txFlags |= Abi::fdBrs;
            m.dataSize = 4u + f.dataLength;
            for (U32 b = 0; b < 4u; ++b)
                m.data[b] = static_cast<std::uint8_t>(f.id >> (24u - b * 8u));
            std::copy_n(f.data.begin(), f.dataLength, m.data + 4);
        }
        U32 count = static_cast<U32>(messages.size());
        const auto result = impl.write(s->channel, messages.data(), &count, 0u);
        if (count > messages.size())
            return Status::TransportError;
        accepted = count;
        if (result == Abi::ok){
            if(count!=messages.size())impl.diagnostic("write_prefix",result,count,static_cast<U32>(messages.size()));
            return count == messages.size() ? Status::Ok : Status::Busy;
        }
        impl.diagnostic("write", result,count,static_cast<U32>(messages.size()));
        return impl.sessionResult(*s, result);
    });
}
Status Backend::readEvents(const std::uint64_t token, const U32 owner, const U32 capacity,
                            std::vector<CanEvent> &events) {
    return implementation_->executor.invoke([&] {
        events.clear();
        auto *s = implementation_->session(token, owner);
        return s ? implementation_->receive(*s, capacity, events) : Status::NotFound;
    });
}
Status Backend::read(const std::uint64_t token, const U32 owner, const U32 capacity,
                     std::vector<CanFrame> &frames) {
    frames.clear();
    std::vector<CanEvent> events;
    const auto status = readEvents(token, owner, capacity, events);
    if (status == Status::Ok)
        for (const auto &e : events)
            if (e.kind == CanEventKind::Received)
                frames.push_back(e.frame);
    return status;
}
Status Backend::getStatus(const std::uint64_t token, const U32 owner, ChannelStatus &status) {
    return implementation_->executor.invoke([&] {
        status = {};
        const auto *s = implementation_->session(token, owner);
        return s && !s->unavailable ? Status::Ok : Status::NotFound;
    });
}
void Backend::pruneDeadSessions() {
    implementation_->executor.invoke([&] {
        for (auto it = implementation_->sessions.begin(); it != implementation_->sessions.end();) {
            if (alive(it->second.owner)) {
                ++it;
                continue;
            }
            implementation_->destroy(it->second);
            it = implementation_->sessions.erase(it);
        }
    });
}
} // namespace Ucal::j2534_backend
