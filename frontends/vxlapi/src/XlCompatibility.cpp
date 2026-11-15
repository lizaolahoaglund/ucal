#include "XlAbi.hpp"
#include "ucal/Client.hpp"
#include "ucal/Model.hpp"
#include "VectorFrontendCatalog.hpp"
#if defined(UCAL_XL_CANOE_PRESENTATION)
#include "canoe/CanoeHardwarePresentation.hpp"
#endif

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

namespace {

std::atomic_bool driverOpen{false};
std::atomic_uint32_t openDriverCalls{0};
std::atomic_uint32_t getDriverConfigCalls{0};
std::atomic_uint32_t discoveryTraceLines{0};
std::atomic_int32_t lastChannelCount{-1};
std::mutex channelCatalogMutex;
std::vector<Ucal::Xl::ChannelConfig> channelCatalog;

void copyText(char* const destination, const std::size_t capacity, const char* const source) {
    if (capacity == 0u) return;
    const auto sourceSize = std::strlen(source);
    const auto bytes = (std::min)(capacity - 1u, sourceSize);
    std::memcpy(destination, source, bytes);
    destination[bytes] = '\0';
}

std::uint32_t stableSerialNumber(const std::string& serial) {
    std::uint32_t value = 2166136261u;
    for (const auto character : serial) {
        value ^= static_cast<std::uint8_t>(character);
        value *= 16777619u;
    }
    return value;
}

const std::string& configuredSerial() {
    static const auto serial = [] {
        std::array<char, 64> value{};
        const auto length = GetEnvironmentVariableA(
            "UCAL_VCNDRVMS_SERIAL", value.data(), static_cast<DWORD>(value.size()));
        if (length == 0 || length >= value.size()) return std::string{};
        return std::string(value.data(), length);
    }();
    return serial;
}

const std::string& configuredCanChannels() {
    static const auto channels = [] {
        std::array<char, 4096> value{};
        const auto length = GetEnvironmentVariableA(
            "UCAL_VCNDRVMS_CAN_CHANNELS", value.data(), static_cast<DWORD>(value.size()));
        if (length == 0 || length >= value.size()) return std::string{};
        return std::string(value.data(), length);
    }();
    return channels;
}

const std::string& configuredCanEndpointIds() {
    static const auto endpoints = [] {
        std::array<char, 4096> value{};
        const auto length = GetEnvironmentVariableA(
            "UCAL_VCNDRVMS_CAN_ENDPOINTS", value.data(), static_cast<DWORD>(value.size()));
        if (length == 0 || length >= value.size()) return std::string{};
        return std::string(value.data(), length);
    }();
    return endpoints;
}

bool configuredCanOnly() {
    char value[8]{};
    return GetEnvironmentVariableA("UCAL_VECTOR_CAN_ONLY", value, sizeof(value)) == 1u &&
           value[0] == '1';
}

constexpr std::size_t driverConfig2HardwareOffset = 0x34u;
constexpr std::size_t driverConfig2HardwareStride = 0xA8u;
constexpr std::size_t driverConfig2HardwareCapacity = 64u;
constexpr std::size_t driverConfig2ChannelOffset =
    driverConfig2HardwareOffset + driverConfig2HardwareStride * driverConfig2HardwareCapacity;
constexpr std::size_t driverConfig2ChannelStride = 0xCCu;
constexpr std::size_t driverConfig2HardwareNameSize = 48u;
constexpr std::size_t driverConfig2HardwareChannelCountOffset = 152u;
constexpr std::size_t driverConfig2ChannelHardwareChannelOffset = 0u;
constexpr std::size_t driverConfig2ChannelIndexOffset = 4u;
constexpr std::size_t driverConfig2ChannelTransceiverNameOffset = 100u;
constexpr std::size_t driverConfig2ChannelTransceiverNameSize = 32u;

template <typename Value>
void writeUnaligned(std::uint8_t* const destination, const std::size_t offset, const Value value) {
    std::memcpy(destination + offset, &value, sizeof(value));
}

template <typename Value>
Value readUnaligned(const std::uint8_t* const source, const std::size_t offset) {
    Value value{};
    std::memcpy(&value, source + offset, sizeof(value));
    return value;
}

void traceDiscovery(const char* const name, const std::uint32_t status,
                     const std::array<std::uintptr_t, 5>& arguments = {},
                     const std::size_t count = 0u) {
    if (discoveryTraceLines.fetch_add(1u) >= 200u) return;
    std::array<wchar_t, 32768> path{};
    const auto length = GetEnvironmentVariableW(L"UCAL_VCNDRVMS_DISCOVERY_TRACE_PATH", path.data(),
                                                 static_cast<DWORD>(path.size()));
    if (length == 0u || length >= path.size()) return;

    const auto file = CreateFileW(path.data(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    std::array<char, 512> line{};
    int used = std::snprintf(line.data(), line.size(), "%s status=%u", name, status);
    for (std::size_t index = 0; index < count && used > 0 && used < static_cast<int>(line.size()); ++index) {
        used += std::snprintf(line.data() + used, line.size() - static_cast<std::size_t>(used),
                              " arg%u=%llx", static_cast<unsigned int>(index),
                              static_cast<unsigned long long>(arguments[index]));
    }
    if (used > 0 && used < static_cast<int>(line.size() - 2u)) {
        line[used++] = '\r';
        line[used++] = '\n';
        DWORD written = 0u;
        WriteFile(file, line.data(), static_cast<DWORD>(used), &written, nullptr);
    }
    CloseHandle(file);
}

using OriginGetDriverConfig2 = Ucal::Xl::Status(__stdcall*)(void*);
using OriginGetDriverConfigEx = Ucal::Xl::Status(__stdcall*)(void*, void*, unsigned int, void*, unsigned int);

HMODULE originDriverModule() {
#if defined(_WIN64)
    constexpr wchar_t originName[] = L"vcndrvms64_origin.dll";
#else
    constexpr wchar_t originName[] = L"vcndrvms_origin.dll";
#endif
    auto module = GetModuleHandleW(originName);
    if (module != nullptr) return module;

    std::array<wchar_t, MAX_PATH> modulePath{};
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&originDriverModule), &self);
    const auto length = self == nullptr
                            ? 0u
                            : GetModuleFileNameW(self, modulePath.data(),
                                                 static_cast<DWORD>(modulePath.size()));
    if (length == 0u || length >= modulePath.size()) return nullptr;

    const std::wstring currentPath(modulePath.data(), length);
    const auto separator = currentPath.find_last_of(L"\\/");
    if (separator == std::wstring::npos) return nullptr;
    const auto filename = separator + 1u;
    if (filename + std::size(originName) > modulePath.size()) return nullptr;
    std::copy(std::begin(originName), std::end(originName), modulePath.begin() + filename);
    return LoadLibraryW(modulePath.data());
}

OriginGetDriverConfig2 originGetDriverConfig2() {
    static const auto function = []() -> OriginGetDriverConfig2 {
        const auto module = originDriverModule();
        if (module == nullptr) return nullptr;
        return reinterpret_cast<OriginGetDriverConfig2>(GetProcAddress(module, "xlGetDriverConfig2"));
    }();
    return function;
}

OriginGetDriverConfigEx originGetDriverConfigEx() {
    static const auto function = []() -> OriginGetDriverConfigEx {
        const auto module = originDriverModule();
        if (module == nullptr) return nullptr;
        return reinterpret_cast<OriginGetDriverConfigEx>(GetProcAddress(module, "xlGetDriverConfigEx"));
    }();
    return function;
}

std::vector<Ucal::Xl::ChannelConfig> makeChannels(
    const std::vector<Ucal::EndpointDescriptor>& endpoints) {
    std::vector<Ucal::Xl::ChannelConfig> channels;
    std::vector<Ucal::AdapterId> adapters;
    std::uint8_t globalIndex = 0u;
    for (const auto& endpoint : endpoints) {
        if (globalIndex >= 64u) break;
        auto adapter = std::find(adapters.begin(), adapters.end(), endpoint.adapterId);
        if (adapter == adapters.end()) {
            adapters.push_back(endpoint.adapterId);
            adapter = std::prev(adapters.end());
        }
        Ucal::Xl::ChannelConfig channel{};
        copyText(channel.name, sizeof(channel.name), endpoint.name.c_str());
        channel.hardwareType = Ucal::Xl::hardwareTypeVirtual;
        channel.hardwareIndex = static_cast<std::uint8_t>(
            std::distance(adapters.begin(), adapter));
        channel.hardwareChannel = static_cast<std::uint8_t>(endpoint.physicalIndex);
        channel.channelIndex = globalIndex;
        channel.channelMask = Ucal::Xl::Access{1u} << globalIndex;
        if (endpoint.busType == Ucal::BusType::Can) {
            if ((endpoint.capabilities & Ucal::CapabilityCanFdIso) != 0u) {
                channel.channelCapabilities = Ucal::Xl::channelCanfdIso;
            }
            channel.channelBusCapabilities = Ucal::Xl::busCompatibleCan | Ucal::Xl::busActiveCan;
            channel.busParams.busType = Ucal::Xl::busTypeCan;
            channel.busParams.data.can.bitrate = 500000u;
            channel.busParams.data.can.canOperationMode = 1u;
            copyText(channel.transceiverName, sizeof(channel.transceiverName), "UCAL Virtual CAN");
        } else {
            channel.channelBusCapabilities = Ucal::Xl::busCompatibleLin | Ucal::Xl::busActiveLin;
            channel.busParams.busType = Ucal::Xl::busTypeLin;
            channel.maximalBaudrate = 20000u;
            copyText(channel.transceiverName, sizeof(channel.transceiverName), "UCAL Virtual LIN");
        }
        channel.connectedBusType = Ucal::Xl::busTypeNone;
        channel.driverVersion = 0x01000300u;
        channel.interfaceVersion = 4u;
        channel.serialNumber = stableSerialNumber(endpoint.serial);
        channels.push_back(channel);
        ++globalIndex;
    }
    return channels;
}

std::vector<Ucal::Xl::ChannelConfig> enumerateChannels(const bool refresh = false) {
    {
        std::lock_guard<std::mutex> lock(channelCatalogMutex);
        if (!refresh && !channelCatalog.empty()) return channelCatalog;
    }
    std::vector<Ucal::EndpointDescriptor> endpoints;
    if (Ucal::Client{}.enumerateEndpoints(endpoints) != Ucal::Status::Ok) return {};
    if (!Ucal::normalizeVectorFrontendCatalog(
            endpoints, configuredSerial(), configuredCanChannels(),
            configuredCanEndpointIds(),
            configuredCanOnly())) {
        return {};
    }
    auto channels = makeChannels(endpoints);
#if defined(UCAL_XL_CANOE_PRESENTATION)
    Ucal::CanoePresentation::update(endpoints);
#endif
    {
        std::lock_guard<std::mutex> lock(channelCatalogMutex);
        channelCatalog = channels;
    }
    return channels;
}

bool findChannel(const int hardwareType, const int hardwareIndex, const int hardwareChannel,
                  Ucal::Xl::ChannelConfig& result) {
    for (const auto& channel : enumerateChannels()) {
        if ((hardwareType == -1 || channel.hardwareType == hardwareType) &&
            (hardwareIndex == -1 || channel.hardwareIndex == hardwareIndex) &&
            (hardwareChannel == -1 || channel.hardwareChannel == hardwareChannel)) {
            result = channel;
            return true;
        }
    }
    return false;
}

std::vector<Ucal::Xl::ChannelConfig> enumerateCanChannels() {
    const auto catalog = enumerateChannels();
    std::vector<Ucal::Xl::ChannelConfig> canChannels;
    std::copy_if(catalog.begin(), catalog.end(), std::back_inserter(canChannels), [](const auto& channel) {
        return channel.busParams.busType == Ucal::Xl::busTypeCan;
    });
    return canChannels;
}

Ucal::Xl::Status rewriteDriverConfig2(void* const rawHeader, void* const rawHardware,
                                        const unsigned int hardwareStride, void* const rawChannels,
                                        const unsigned int channelStride) {
    if (rawHeader == nullptr || rawHardware == nullptr || rawChannels == nullptr ||
        hardwareStride < driverConfig2HardwareChannelCountOffset + sizeof(std::uint32_t) ||
        channelStride < driverConfig2ChannelTransceiverNameOffset +
                             driverConfig2ChannelTransceiverNameSize) {
        return Ucal::Xl::wrongParameter;
    }

    const auto canChannels = enumerateCanChannels();
    if (canChannels.empty()) return Ucal::Xl::cannotOpenDriver;
    if (canChannels.size() > driverConfig2HardwareCapacity) return Ucal::Xl::wrongParameter;

    auto* const header = static_cast<std::uint8_t*>(rawHeader);
    auto* const hardware = static_cast<std::uint8_t*>(rawHardware);
    auto* const firstChannel = static_cast<std::uint8_t*>(rawChannels);
    const auto channelCount = static_cast<std::uint32_t>(canChannels.size());
    writeUnaligned(header, 8u, std::uint32_t{1u});
    writeUnaligned(header, 12u, channelCount);

    writeUnaligned(hardware, driverConfig2HardwareChannelCountOffset, channelCount);
    copyText(reinterpret_cast<char*>(hardware), driverConfig2HardwareNameSize, "UCAL CAN");
    // Keep the qualified native controller profile: CANoe's generic Virtual
    // type loses its CAN FD timing editor. Its template identity is replaced
    // only in the isolated profile's presentation, using the mapped endpoint.

    std::vector<std::uint8_t> templateChannel(channelStride);
    std::memcpy(templateChannel.data(), firstChannel, templateChannel.size());
    for (std::size_t index = 0; index < canChannels.size(); ++index) {
        auto* const target = firstChannel + index * channelStride;
        std::memcpy(target, templateChannel.data(), templateChannel.size());
        writeUnaligned(target, driverConfig2ChannelHardwareChannelOffset,
                        static_cast<std::uint32_t>(index));
        writeUnaligned(target, driverConfig2ChannelIndexOffset,
                        static_cast<std::uint32_t>(index));
        copyText(reinterpret_cast<char*>(target + driverConfig2ChannelTransceiverNameOffset),
                  driverConfig2ChannelTransceiverNameSize, canChannels[index].name);
    }
    lastChannelCount.store(static_cast<std::int32_t>(channelCount));
    return Ucal::Xl::success;
}

}  // namespace

extern "C" {

__declspec(dllexport) Ucal::Xl::Status __stdcall xlOpenDriver() {
    openDriverCalls.fetch_add(1u);
    const auto status = Ucal::Client{}.ping();
    driverOpen.store(status == Ucal::Status::Ok);
    if (status == Ucal::Status::Ok) enumerateChannels(true);
    const auto xlStatus =
        status == Ucal::Status::Ok ? Ucal::Xl::success : Ucal::Xl::cannotOpenDriver;
    traceDiscovery("xlOpenDriver", static_cast<std::uint32_t>(xlStatus));
    return xlStatus;
}

__declspec(dllexport) Ucal::Xl::Status __stdcall xlCloseDriver() {
    driverOpen.store(false);
    std::lock_guard<std::mutex> lock(channelCatalogMutex);
    channelCatalog.clear();
    return Ucal::Xl::success;
}

__declspec(dllexport) Ucal::Xl::Status __stdcall xlGetDriverConfig(Ucal::Xl::DriverConfig* const config) {
    getDriverConfigCalls.fetch_add(1u);
    if (!driverOpen.load()) return Ucal::Xl::cannotOpenDriver;
    if (config == nullptr) return Ucal::Xl::wrongParameter;
    const auto channels = enumerateChannels();
    if (channels.empty()) return Ucal::Xl::cannotOpenDriver;
    std::memset(config, 0, sizeof(*config));
    config->dllVersion = 0x01000000u;
    config->channelCount = static_cast<std::uint32_t>((std::min)(channels.size(), std::size(config->channel)));
    std::copy_n(channels.begin(), config->channelCount, config->channel);
    lastChannelCount.store(static_cast<std::int32_t>(config->channelCount));
    return Ucal::Xl::success;
}

__declspec(dllexport) Ucal::Xl::Status __stdcall xlGetDriverConfig2(void* const rawConfig) {
    traceDiscovery("xlGetDriverConfig2.enter", 0u,
                    {reinterpret_cast<std::uintptr_t>(rawConfig)}, 1u);
    if (!driverOpen.load()) return Ucal::Xl::cannotOpenDriver;
    if (rawConfig == nullptr) return Ucal::Xl::wrongParameter;
    const auto origin = originGetDriverConfig2();
    if (origin == nullptr) return Ucal::Xl::notImplemented;

    const auto originStatus = origin(rawConfig);
    if (originStatus != Ucal::Xl::success) return originStatus;

    auto* const bytes = static_cast<std::uint8_t*>(rawConfig);
    const auto status = rewriteDriverConfig2(bytes, bytes + driverConfig2HardwareOffset,
                                               static_cast<unsigned int>(driverConfig2HardwareStride),
                                               bytes + driverConfig2ChannelOffset,
                                               static_cast<unsigned int>(driverConfig2ChannelStride));
    traceDiscovery("xlGetDriverConfig2.exit", static_cast<std::uint32_t>(status),
                    {readUnaligned<std::uint32_t>(bytes, 8u),
                     readUnaligned<std::uint32_t>(bytes, 12u)},
                    2u);
    return status;
}

__declspec(dllexport) Ucal::Xl::Status __stdcall xlGetDriverConfigEx(
    void* const rawHeader, void* const rawHardware, const unsigned int hardwareStride,
    void* const rawChannels, const unsigned int channelStride) {
    auto* const header = static_cast<std::uint8_t*>(rawHeader);
    const auto inputHardwareCount = header == nullptr ? 0u : readUnaligned<std::uint32_t>(header, 8u);
    const auto inputChannelCount = header == nullptr ? 0u : readUnaligned<std::uint32_t>(header, 12u);
    traceDiscovery("xlGetDriverConfigEx.enter", 0u,
                    {hardwareStride, channelStride, inputHardwareCount, inputChannelCount}, 4u);
    if (!driverOpen.load()) return Ucal::Xl::cannotOpenDriver;
    const auto origin = originGetDriverConfigEx();
    if (origin == nullptr) return Ucal::Xl::notImplemented;

    const auto originStatus =
        origin(rawHeader, rawHardware, hardwareStride, rawChannels, channelStride);
    const auto canChannels = enumerateCanChannels();
    const auto requiredChannelCount = static_cast<std::uint32_t>(canChannels.size());
    if (header != nullptr && (rawHardware == nullptr || rawChannels == nullptr)) {
        writeUnaligned(header, 8u, std::uint32_t{1u});
        writeUnaligned(header, 12u, requiredChannelCount);
        traceDiscovery("xlGetDriverConfigEx.query", static_cast<std::uint32_t>(originStatus),
                        {1u, requiredChannelCount}, 2u);
        return originStatus;
    }
    if (originStatus != Ucal::Xl::success) {
        traceDiscovery("xlGetDriverConfigEx.origin", static_cast<std::uint32_t>(originStatus));
        return originStatus;
    }
    if (inputHardwareCount < 1u || inputChannelCount < requiredChannelCount) {
        writeUnaligned(header, 8u, std::uint32_t{1u});
        writeUnaligned(header, 12u, requiredChannelCount);
        constexpr auto bufferTooSmall = static_cast<Ucal::Xl::Status>(135u);
        traceDiscovery("xlGetDriverConfigEx.capacity", static_cast<std::uint32_t>(bufferTooSmall),
                        {inputHardwareCount, inputChannelCount, requiredChannelCount}, 3u);
        return bufferTooSmall;
    }
    const auto status = rewriteDriverConfig2(rawHeader, rawHardware, hardwareStride, rawChannels,
                                               channelStride);
    traceDiscovery("xlGetDriverConfigEx.exit", static_cast<std::uint32_t>(status),
                    {readUnaligned<std::uint32_t>(header, 8u),
                     readUnaligned<std::uint32_t>(header, 12u)},
                    2u);
    return status;
}

__declspec(dllexport) int __stdcall xlGetChannelIndex(const int hardwareType, const int hardwareIndex,
                                                       const int hardwareChannel) {
    Ucal::Xl::ChannelConfig channel{};
    const auto result =
        findChannel(hardwareType, hardwareIndex, hardwareChannel, channel) ? channel.channelIndex : -1;
    traceDiscovery("xlGetChannelIndex", static_cast<std::uint32_t>(result),
                    {static_cast<std::uintptr_t>(hardwareType),
                     static_cast<std::uintptr_t>(hardwareIndex),
                     static_cast<std::uintptr_t>(hardwareChannel)},
                    3u);
    return result;
}

__declspec(dllexport) Ucal::Xl::Access __stdcall xlGetChannelMask(const int hardwareType,
                                                                  const int hardwareIndex,
                                                                  const int hardwareChannel) {
    Ucal::Xl::ChannelConfig channel{};
    const auto result =
        findChannel(hardwareType, hardwareIndex, hardwareChannel, channel) ? channel.channelMask : 0u;
    traceDiscovery("xlGetChannelMask", static_cast<std::uint32_t>(result),
                    {static_cast<std::uintptr_t>(hardwareType),
                     static_cast<std::uintptr_t>(hardwareIndex),
                     static_cast<std::uintptr_t>(hardwareChannel)},
                    3u);
    return result;
}

__declspec(dllexport) Ucal::Xl::Status __stdcall xlGetApplConfig(char*, const unsigned int applicationChannel,
                                                                 unsigned int* const hardwareType,
                                                                 unsigned int* const hardwareIndex,
                                                                 unsigned int* const hardwareChannel,
                                                                 unsigned int) {
    if (hardwareType == nullptr || hardwareIndex == nullptr || hardwareChannel == nullptr) {
        return Ucal::Xl::wrongParameter;
    }
    const auto channels = enumerateChannels();
    if (applicationChannel >= channels.size()) return Ucal::Xl::wrongParameter;
    *hardwareType = channels[applicationChannel].hardwareType;
    *hardwareIndex = channels[applicationChannel].hardwareIndex;
    *hardwareChannel = channels[applicationChannel].hardwareChannel;
    traceDiscovery("xlGetApplConfig", Ucal::Xl::success,
                    {applicationChannel, *hardwareType, *hardwareIndex, *hardwareChannel}, 4u);
    return Ucal::Xl::success;
}

__declspec(dllexport) Ucal::Xl::Status __stdcall xlSetApplConfig(
    char*, const unsigned int applicationChannel, const unsigned int hardwareType,
    const unsigned int hardwareIndex, const unsigned int hardwareChannel, unsigned int) {
    Ucal::Xl::ChannelConfig channel{};
    const auto channels = enumerateChannels();
    const auto status = applicationChannel < channels.size() &&
                                findChannel(static_cast<int>(hardwareType),
                                             static_cast<int>(hardwareIndex),
                                             static_cast<int>(hardwareChannel), channel)
                            ? Ucal::Xl::success
                            : Ucal::Xl::wrongParameter;
    traceDiscovery("xlSetApplConfig", static_cast<std::uint32_t>(status),
                    {applicationChannel, hardwareType, hardwareIndex, hardwareChannel}, 4u);
    return status;
}

__declspec(dllexport) Ucal::Xl::Status __stdcall xlOpenPort(Ucal::Xl::PortHandle* const portHandle, char*,
                                                            const Ucal::Xl::Access accessMask,
                                                            Ucal::Xl::Access* const permissionMask,
                                                            unsigned int, unsigned int, unsigned int) {
    if (!driverOpen.load()) return Ucal::Xl::cannotOpenDriver;
    if (portHandle == nullptr || permissionMask == nullptr || accessMask == 0u) return Ucal::Xl::wrongParameter;
    *portHandle = 1;
    *permissionMask &= accessMask;
    return Ucal::Xl::success;
}

__declspec(dllexport) Ucal::Xl::Status __stdcall xlClosePort(const Ucal::Xl::PortHandle portHandle) {
    return portHandle == 1 ? Ucal::Xl::success : Ucal::Xl::invalidPort;
}

__declspec(dllexport) const char* __stdcall xlGetErrorString(const Ucal::Xl::Status status) {
    return status == Ucal::Xl::success ? "XL_SUCCESS" : "UCAL XL discovery operation is unsupported";
}

__declspec(dllexport) Ucal::Xl::Status __stdcall xlActivateChannel(Ucal::Xl::PortHandle, Ucal::Xl::Access,
                                                                   unsigned int, unsigned int) {
    return Ucal::Xl::notImplemented;
}
__declspec(dllexport) Ucal::Xl::Status __stdcall xlDeactivateChannel(Ucal::Xl::PortHandle, Ucal::Xl::Access) {
    return Ucal::Xl::notImplemented;
}
__declspec(dllexport) Ucal::Xl::Status __stdcall xlCanFdSetConfiguration(Ucal::Xl::PortHandle, Ucal::Xl::Access,
                                                                         const Ucal::Xl::CanFdConfig*) {
    return Ucal::Xl::notImplemented;
}
__declspec(dllexport) Ucal::Xl::Status __stdcall xlReceive(Ucal::Xl::PortHandle, unsigned int* const count, void*) {
    if (count != nullptr) *count = 0u;
    return Ucal::Xl::queueEmpty;
}

__declspec(dllexport) Ucal::Xl::Status __stdcall UcalXlGetDiagnostics(Ucal::Xl::Diagnostics* const diagnostics) {
    if (diagnostics == nullptr || diagnostics->size != sizeof(*diagnostics)) return Ucal::Xl::wrongParameter;
    diagnostics->openDriverCalls = openDriverCalls.load();
    diagnostics->getDriverConfigCalls = getDriverConfigCalls.load();
    diagnostics->lastChannelCount = lastChannelCount.load();
    return Ucal::Xl::success;
}

}  // extern "C"
