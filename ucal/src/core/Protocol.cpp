#include "ucal/Protocol.hpp"

#include <windows.h>
#include <sddl.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <limits>
#include <stdexcept>

namespace Ucal::Protocol {
namespace {

std::string boundedString(const char* value, const std::size_t capacity) {
    const auto end = static_cast<const char*>(std::memchr(value, '\0', capacity));
    return std::string(value, end == nullptr ? capacity : static_cast<std::size_t>(end - value));
}

template <std::size_t Capacity>
void copyString(char (&target)[Capacity], const std::string& source) {
    std::memset(target, 0, Capacity);
    const auto count = (std::min)(source.size(), Capacity - 1);
    std::memcpy(target, source.data(), count);
}

bool exactRecordPayload(const std::size_t payloadSize, const std::uint32_t count,
                          const std::size_t recordSize) {
    if (payloadSize < sizeof(count) || recordSize == 0u) return false;
    const auto recordBytes = payloadSize - sizeof(count);
    return recordBytes % recordSize == 0u &&
           static_cast<std::size_t>(count) == recordBytes / recordSize;
}

constexpr std::uint32_t knownCapabilities =
    CapabilityClassicCan | CapabilityCanFdIso | CapabilityBrs | CapabilityLin |
    CapabilityLinMaster | CapabilityLinSlave | CapabilityListenOnly |
    CapabilityTermination | CapabilityNoAck | CapabilityTxCompletion |
    CapabilityInternalLoopback | CapabilityCanAdvancedTiming | CapabilityDiscoveryOnly;

constexpr std::uint32_t knownCanFrameFlags =
    FrameExtended | FrameRemote | FrameFd | FrameBrs | FrameEsi |
    FrameTxCompleted | FrameTxFailed | FrameSelfReceived;

constexpr std::uint32_t knownLinFrameFlags =
    LinTransmit | LinWakeup | LinSleep | LinEnhancedChecksum |
    LinUserChecksum | LinSyncBreak | LinTxCompleted | LinTxFailed;

bool validDeviceState(const std::uint32_t value) {
    return value <= static_cast<std::uint32_t>(DeviceState::Offline);
}

bool validBusType(const std::uint32_t value) {
    return value == static_cast<std::uint32_t>(BusType::Can) ||
           value == static_cast<std::uint32_t>(BusType::Lin);
}

bool validCapabilities(const std::uint32_t value) {
    return (value & ~knownCapabilities) == 0u;
}

bool validWireBool(const std::uint8_t value) {
    return value <= 1u;
}

bool validCanFrame(const WireCanFrameV1& frame) {
    if ((frame.flags & ~knownCanFrameFlags) != 0u || frame.dlc > 15u ||
        frame.dataLength > 64u || frame.reserved[0] != 0u || frame.reserved[1] != 0u) {
        return false;
    }
    const bool extended = (frame.flags & FrameExtended) != 0u;
    const bool remote = (frame.flags & FrameRemote) != 0u;
    const bool fd = (frame.flags & FrameFd) != 0u;
    if (frame.id > (extended ? 0x1fffffffu : 0x7ffu)) return false;
    if (remote && (fd || frame.dataLength != 0u)) return false;
    if (!fd && (frame.dlc > 8u || frame.dataLength > 8u)) return false;
    if (!fd && (frame.flags & (FrameBrs | FrameEsi)) != 0u) return false;
    return true;
}

bool validChannelStatusWire(const ChannelStatusResponseV1& wire) {
    constexpr std::uint8_t knownValidity =
        channelStatusBusStateValid | channelStatusErrorCountersValid;
    return validWireBool(wire.busOff) && validWireBool(wire.errorPassive) &&
           validWireBool(wire.warning) && (wire.reserved[0] & ~knownValidity) == 0u &&
           wire.reserved[1] == 0u && wire.reserved[2] == 0u;
}

bool validCanEventKind(const std::uint32_t value) {
    return value <= static_cast<std::uint32_t>(CanEventKind::QueueOverrun);
}

bool validLinEventKind(const std::uint32_t value) {
    return value <= static_cast<std::uint32_t>(LinEventKind::QueueOverrun);
}

bool validLinStatusWire(const LinStatusResponseV3& wire) {
    return validWireBool(wire.active) && validWireBool(wire.asleep) &&
           std::all_of(std::begin(wire.reserved), std::end(wire.reserved),
                       [](const std::uint8_t value) { return value == 0u; });
}

}  // namespace

std::wstring currentUserSidString() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return L"unknown";
    }

    DWORD required = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &required);
    std::vector<std::byte> storage(required);
    if (required == 0 || !GetTokenInformation(token, TokenUser, storage.data(), required, &required)) {
        CloseHandle(token);
        return L"unknown";
    }

    const auto user = reinterpret_cast<const TOKEN_USER*>(storage.data());
    LPWSTR sidText = nullptr;
    if (!ConvertSidToStringSidW(user->User.Sid, &sidText)) {
        CloseHandle(token);
        return L"unknown";
    }

    std::wstring result(sidText);
    LocalFree(sidText);
    CloseHandle(token);
    return result;
}

std::wstring defaultPipeName() {
    auto name = L"\\\\.\\pipe\\ucal.discovery.v1." + currentUserSidString();
    // An explicit instance lets a bounded acceptance run coexist with the
    // user's ordinary Switch/Broker. The default per-user namespace is unchanged.
    wchar_t instance[65]{};
    const auto size = GetEnvironmentVariableW(L"UCAL_BROKER_INSTANCE", instance, 65u);
    if (size >= 65u) return {};
    if (size != 0u) {
        for (DWORD i = 0; i < size; ++i) {
            const auto c = instance[i];
            if (!((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
                  (c >= L'0' && c <= L'9') || c == L'-' || c == L'_')) return {};
        }
        name += L"." + std::wstring(instance, size);
    }
    return name;
}

MessageHeader makeRequest(const Operation operation, const std::uint64_t requestId,
                           const std::uint32_t payloadBytes) {
    return {magic, majorVersion, minorVersion, static_cast<std::uint16_t>(operation), 0, requestId,
            payloadBytes, static_cast<std::int32_t>(Status::Ok)};
}

bool validRequestHeader(const MessageHeader& header) {
    return header.magicValue == magic && header.major == majorVersion &&
           header.minor <= minorVersion && header.reserved == 0u &&
           header.payloadBytes <= maxPayloadBytes &&
           header.status == static_cast<std::int32_t>(Status::Ok);
}

bool validResponseHeader(const MessageHeader& header) {
    return header.magicValue == magic && header.major == majorVersion &&
           header.minor <= minorVersion && header.reserved == 0u &&
           header.payloadBytes <= maxPayloadBytes && validStatusValue(header.status);
}

bool validStatusValue(const std::int32_t value) {
    return value >= static_cast<std::int32_t>(Status::Ok) &&
           value <= static_cast<std::int32_t>(Status::InternalError);
}

std::vector<std::byte> encodeDevices(const std::vector<DeviceDescriptor>& devices) {
    if (devices.size() > (std::numeric_limits<std::uint32_t>::max)()) {
        throw std::length_error("too many devices");
    }
    const auto payloadSize = sizeof(std::uint32_t) + devices.size() * sizeof(WireDeviceV1);
    std::vector<std::byte> payload(payloadSize);
    const auto count = static_cast<std::uint32_t>(devices.size());
    std::memcpy(payload.data(), &count, sizeof(count));

    auto* output = reinterpret_cast<WireDeviceV1*>(payload.data() + sizeof(count));
    for (std::size_t index = 0; index < devices.size(); ++index) {
        WireDeviceV1 wire{};
        std::memcpy(wire.id, devices[index].id.data(), devices[index].id.size());
        wire.channelCount = devices[index].channelCount;
        wire.capabilities = devices[index].capabilities;
        wire.state = static_cast<std::uint32_t>(devices[index].state);
        copyString(wire.name, devices[index].name);
        copyString(wire.model, devices[index].model);
        copyString(wire.serial, devices[index].serial);
        output[index] = wire;
    }
    return payload;
}

Status decodeDevices(const std::vector<std::byte>& payload, std::vector<DeviceDescriptor>& devices) {
    devices.clear();
    if (payload.size() < sizeof(std::uint32_t)) {
        return Status::TransportError;
    }
    std::uint32_t count = 0;
    std::memcpy(&count, payload.data(), sizeof(count));
    if (!exactRecordPayload(payload.size(), count, sizeof(WireDeviceV1))) {
        return Status::TransportError;
    }

    const auto* input = reinterpret_cast<const WireDeviceV1*>(payload.data() + sizeof(count));
    devices.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        DeviceDescriptor device{};
        std::memcpy(device.id.data(), input[index].id, device.id.size());
        device.channelCount = input[index].channelCount;
        if (!validCapabilities(input[index].capabilities) ||
            !validDeviceState(input[index].state)) return Status::TransportError;
        device.capabilities = input[index].capabilities;
        device.state = static_cast<DeviceState>(input[index].state);
        device.name = boundedString(input[index].name, sizeof(input[index].name));
        device.model = boundedString(input[index].model, sizeof(input[index].model));
        device.serial = boundedString(input[index].serial, sizeof(input[index].serial));
        devices.push_back(std::move(device));
    }
    return Status::Ok;
}

std::vector<std::byte> encodeEndpoints(const std::vector<EndpointDescriptor>& endpoints) {
    if (endpoints.size() > (std::numeric_limits<std::uint32_t>::max)()) {
        throw std::length_error("too many endpoints");
    }
    const auto payloadSize = sizeof(std::uint32_t) + endpoints.size() * sizeof(WireEndpointV2);
    std::vector<std::byte> payload(payloadSize);
    const auto count = static_cast<std::uint32_t>(endpoints.size());
    std::memcpy(payload.data(), &count, sizeof(count));
    auto* output = reinterpret_cast<WireEndpointV2*>(payload.data() + sizeof(count));
    for (std::size_t index = 0; index < endpoints.size(); ++index) {
        WireEndpointV2 wire{};
        std::memcpy(wire.id, endpoints[index].id.data(), endpoints[index].id.size());
        std::memcpy(wire.adapterId, endpoints[index].adapterId.data(),
                    endpoints[index].adapterId.size());
        wire.busType = static_cast<std::uint32_t>(endpoints[index].busType);
        wire.physicalIndex = endpoints[index].physicalIndex;
        wire.capabilities = endpoints[index].capabilities;
        wire.state = static_cast<std::uint32_t>(endpoints[index].state);
        copyString(wire.name, endpoints[index].name);
        copyString(wire.model, endpoints[index].model);
        copyString(wire.serial, endpoints[index].serial);
        output[index] = wire;
    }
    return payload;
}

Status decodeEndpoints(const std::vector<std::byte>& payload,
                        std::vector<EndpointDescriptor>& endpoints) {
    endpoints.clear();
    if (payload.size() < sizeof(std::uint32_t)) return Status::TransportError;
    std::uint32_t count = 0;
    std::memcpy(&count, payload.data(), sizeof(count));
    if (!exactRecordPayload(payload.size(), count, sizeof(WireEndpointV2))) {
        return Status::TransportError;
    }
    const auto* input = reinterpret_cast<const WireEndpointV2*>(payload.data() + sizeof(count));
    endpoints.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        EndpointDescriptor endpoint{};
        std::memcpy(endpoint.id.data(), input[index].id, endpoint.id.size());
        std::memcpy(endpoint.adapterId.data(), input[index].adapterId, endpoint.adapterId.size());
        if (!validBusType(input[index].busType) ||
            !validCapabilities(input[index].capabilities) ||
            !validDeviceState(input[index].state)) return Status::TransportError;
        endpoint.busType = static_cast<BusType>(input[index].busType);
        endpoint.physicalIndex = input[index].physicalIndex;
        endpoint.capabilities = input[index].capabilities;
        endpoint.state = static_cast<DeviceState>(input[index].state);
        endpoint.name = boundedString(input[index].name, sizeof(input[index].name));
        endpoint.model = boundedString(input[index].model, sizeof(input[index].model));
        endpoint.serial = boundedString(input[index].serial, sizeof(input[index].serial));
        endpoints.push_back(std::move(endpoint));
    }
    return Status::Ok;
}

std::vector<std::byte> encodeEndpointsV9(const std::vector<EndpointDescriptor>& endpoints) {
    const auto legacy = encodeEndpointsV6(endpoints);
    const auto count = static_cast<std::uint32_t>(endpoints.size());
    std::vector<std::byte> payload(sizeof(count) + endpoints.size() * sizeof(WireEndpointV9));
    std::memcpy(payload.data(), &count, sizeof(count));
    for (std::size_t i = 0; i < endpoints.size(); ++i) {
        WireEndpointV9 wire{};
        std::memcpy(&wire.endpoint, legacy.data() + sizeof(count) + i * sizeof(WireEndpointV6), sizeof(WireEndpointV6));
        const auto copy = [](const CanParameterAccess& a) {
            return WireCanParameterAccessV9{a.readKnown, a.readSupported, a.writeKnown, a.writeSupported};
        };
        wire.classicCan = copy(endpoints[i].configuration.classicCan);
        wire.canFd = copy(endpoints[i].configuration.canFd);
        wire.bindingIsDefault = endpoints[i].bindingIsDefault ? 1u : 0u;
        std::memcpy(payload.data() + sizeof(count) + i * sizeof(wire), &wire, sizeof(wire));
    }
    return payload;
}

Status decodeEndpointsV9(const std::vector<std::byte>& payload,
                           std::vector<EndpointDescriptor>& endpoints) {
    endpoints.clear();
    if (payload.size() < sizeof(std::uint32_t)) return Status::TransportError;
    std::uint32_t count{};
    std::memcpy(&count, payload.data(), sizeof(count));
    if (!exactRecordPayload(payload.size(), count, sizeof(WireEndpointV9))) return Status::TransportError;
    std::vector<std::byte> legacy(sizeof(count) + static_cast<std::size_t>(count) * sizeof(WireEndpointV6));
    std::vector<CanConfigurationCapabilities> access(count);
    std::vector<bool> defaultBindings(count);
    std::memcpy(legacy.data(), &count, sizeof(count));
    for (std::uint32_t i = 0; i < count; ++i) {
        WireEndpointV9 wire{};
        std::memcpy(&wire, payload.data() + sizeof(count) + static_cast<std::size_t>(i) * sizeof(wire), sizeof(wire));
        const auto valid = [](const WireCanParameterAccessV9& a) {
            return ((a.readKnown | a.readSupported | a.writeKnown | a.writeSupported) & ~canConfigParameterMask) == 0u &&
                (a.readSupported & ~a.readKnown) == 0u && (a.writeSupported & ~a.writeKnown) == 0u;
        };
        if (!valid(wire.classicCan) || !valid(wire.canFd) || wire.bindingIsDefault > 1u) return Status::TransportError;
        defaultBindings[i] = wire.bindingIsDefault != 0u;
        const auto copy = [](const WireCanParameterAccessV9& a) {
            return CanParameterAccess{a.readKnown, a.readSupported, a.writeKnown, a.writeSupported};
        };
        access[i] = {copy(wire.classicCan), copy(wire.canFd)};
        std::memcpy(legacy.data() + sizeof(count) + static_cast<std::size_t>(i) * sizeof(WireEndpointV6), &wire.endpoint, sizeof(wire.endpoint));
    }
    const auto status = decodeEndpointsV6(legacy, endpoints);
    if (status != Status::Ok) return status;
    for (std::size_t i = 0; i < endpoints.size(); ++i) {
        endpoints[i].configuration = access[i];
        endpoints[i].bindingIsDefault = defaultBindings[i];
    }
    return Status::Ok;
}

std::vector<std::byte> encodeEndpointsV6(
    const std::vector<EndpointDescriptor>& endpoints) {
    if (endpoints.size() > (std::numeric_limits<std::uint32_t>::max)()) {
        throw std::length_error("too many endpoints");
    }
    const auto payloadSize = sizeof(std::uint32_t) +
        endpoints.size() * sizeof(WireEndpointV6);
    std::vector<std::byte> payload(payloadSize);
    const auto count = static_cast<std::uint32_t>(endpoints.size());
    std::memcpy(payload.data(), &count, sizeof(count));
    auto* output = reinterpret_cast<WireEndpointV6*>(
        payload.data() + sizeof(count));
    for (std::size_t index = 0; index < endpoints.size(); ++index) {
        WireEndpointV6 wire{};
        std::memcpy(wire.endpoint.id, endpoints[index].id.data(), endpoints[index].id.size());
        std::memcpy(wire.endpoint.adapterId, endpoints[index].adapterId.data(),
                    endpoints[index].adapterId.size());
        wire.endpoint.busType = static_cast<std::uint32_t>(endpoints[index].busType);
        wire.endpoint.physicalIndex = endpoints[index].physicalIndex;
        wire.endpoint.capabilities = endpoints[index].capabilities;
        wire.endpoint.state = static_cast<std::uint32_t>(endpoints[index].state);
        copyString(wire.endpoint.name, endpoints[index].name);
        copyString(wire.endpoint.model, endpoints[index].model);
        copyString(wire.endpoint.serial, endpoints[index].serial);
        copyString(wire.backendFamily, endpoints[index].backendFamily);
        output[index] = wire;
    }
    return payload;
}

Status decodeEndpointsV6(const std::vector<std::byte>& payload,
                           std::vector<EndpointDescriptor>& endpoints) {
    endpoints.clear();
    if (payload.size() < sizeof(std::uint32_t)) return Status::TransportError;
    std::uint32_t count = 0;
    std::memcpy(&count, payload.data(), sizeof(count));
    if (!exactRecordPayload(payload.size(), count, sizeof(WireEndpointV6))) {
        return Status::TransportError;
    }
    const auto* input = reinterpret_cast<const WireEndpointV6*>(
        payload.data() + sizeof(count));
    endpoints.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        EndpointDescriptor endpoint{};
        std::memcpy(endpoint.id.data(), input[index].endpoint.id, endpoint.id.size());
        std::memcpy(endpoint.adapterId.data(), input[index].endpoint.adapterId,
                    endpoint.adapterId.size());
        if (!validBusType(input[index].endpoint.busType) ||
            !validCapabilities(input[index].endpoint.capabilities) ||
            !validDeviceState(input[index].endpoint.state)) return Status::TransportError;
        endpoint.busType = static_cast<BusType>(input[index].endpoint.busType);
        endpoint.physicalIndex = input[index].endpoint.physicalIndex;
        endpoint.capabilities = input[index].endpoint.capabilities;
        endpoint.state = static_cast<DeviceState>(input[index].endpoint.state);
        endpoint.name = boundedString(input[index].endpoint.name,
                                       sizeof(input[index].endpoint.name));
        endpoint.model = boundedString(input[index].endpoint.model,
                                        sizeof(input[index].endpoint.model));
        endpoint.serial = boundedString(input[index].endpoint.serial,
                                         sizeof(input[index].endpoint.serial));
        endpoint.backendFamily = boundedString(input[index].backendFamily,
                                                  sizeof(input[index].backendFamily));
        if (endpoint.backendFamily.empty()) return Status::TransportError;
        endpoints.push_back(std::move(endpoint));
    }
    return Status::Ok;
}

std::vector<std::byte> encodeBackendHealth(const std::vector<BackendHealth>& health) {
    if (health.size() > (std::numeric_limits<std::uint32_t>::max)()) {
        throw std::length_error("too many backend health records");
    }
    const auto payloadSize = sizeof(std::uint32_t) +
        health.size() * sizeof(WireBackendHealthV6);
    std::vector<std::byte> payload(payloadSize);
    const auto count = static_cast<std::uint32_t>(health.size());
    std::memcpy(payload.data(), &count, sizeof(count));
    auto* output = reinterpret_cast<WireBackendHealthV6*>(
        payload.data() + sizeof(count));
    for (std::size_t index = 0; index < health.size(); ++index) {
        WireBackendHealthV6 wire{};
        copyString(wire.backendFamily, health[index].backendFamily);
        wire.status = static_cast<std::int32_t>(health[index].status);
        wire.endpointCount = health[index].endpointCount;
        output[index] = wire;
    }
    return payload;
}

Status decodeBackendHealth(const std::vector<std::byte>& payload,
                             std::vector<BackendHealth>& health) {
    health.clear();
    if (payload.size() < sizeof(std::uint32_t)) return Status::TransportError;
    std::uint32_t count = 0;
    std::memcpy(&count, payload.data(), sizeof(count));
    if (!exactRecordPayload(payload.size(), count, sizeof(WireBackendHealthV6))) {
        return Status::TransportError;
    }
    const auto* input = reinterpret_cast<const WireBackendHealthV6*>(
        payload.data() + sizeof(count));
    health.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        BackendHealth item{};
        item.backendFamily = boundedString(input[index].backendFamily,
                                             sizeof(input[index].backendFamily));
        if (item.backendFamily.empty() || !validStatusValue(input[index].status)) {
            return Status::TransportError;
        }
        item.status = static_cast<Status>(input[index].status);
        item.endpointCount = input[index].endpointCount;
        health.push_back(std::move(item));
    }
    return Status::Ok;
}

std::vector<std::byte> encodeBackendHealthV8(const std::vector<BackendHealth>& health) {
    if (health.size() > (maxPayloadBytes - sizeof(std::uint32_t)) / sizeof(WireBackendHealthV8))
        throw std::length_error("too many runtime health records");
    const auto count = static_cast<std::uint32_t>(health.size());
    std::vector<std::byte> payload(sizeof(count) + health.size() * sizeof(WireBackendHealthV8));
    std::memcpy(payload.data(), &count, sizeof(count));
    for (std::size_t i = 0; i < health.size(); ++i) {
        const auto& source = health[i];
        WireBackendHealthV8 wire{};
        copyString(wire.health.backendFamily, source.backendFamily);
        wire.health.status = static_cast<std::int32_t>(source.status);
        wire.health.endpointCount = source.endpointCount;
        if (source.runtime.path.size() < sizeof(wire.runtimePath))
            copyString(wire.runtimePath, source.runtime.path);
        copyString(wire.fileVersion, source.runtime.fileVersion);
        copyString(wire.apiVersion, source.runtime.apiVersion);
        copyString(wire.sha256, source.runtime.sha256);
        std::memcpy(payload.data() + sizeof(count) + i * sizeof(wire), &wire, sizeof(wire));
    }
    return payload;
}

Status decodeBackendHealthV8(const std::vector<std::byte>& payload,
                               std::vector<BackendHealth>& health) {
    health.clear();
    if (payload.size() < sizeof(std::uint32_t)) return Status::TransportError;
    std::uint32_t count{};
    std::memcpy(&count, payload.data(), sizeof(count));
    if (payload.size() > maxPayloadBytes ||
        !exactRecordPayload(payload.size(), count, sizeof(WireBackendHealthV8)))
        return Status::TransportError;
    std::vector<BackendHealth> decoded;
    for (std::uint32_t i = 0; i < count; ++i) {
        WireBackendHealthV8 wire{};
        std::memcpy(&wire, payload.data() + sizeof(count) + i * sizeof(wire), sizeof(wire));
        if (!std::memchr(wire.health.backendFamily, 0, sizeof(wire.health.backendFamily)) ||
            !std::memchr(wire.runtimePath, 0, sizeof(wire.runtimePath)) ||
            !std::memchr(wire.fileVersion, 0, sizeof(wire.fileVersion)) ||
            !std::memchr(wire.apiVersion, 0, sizeof(wire.apiVersion)) ||
            !std::memchr(wire.sha256, 0, sizeof(wire.sha256)) ||
            !validStatusValue(wire.health.status) || wire.health.backendFamily[0] == 0)
            return Status::TransportError;
        const std::string hash(wire.sha256);
        if (!hash.empty() && (hash.size() != 64u || !std::all_of(hash.begin(), hash.end(), [](const char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
            }))) return Status::TransportError;
        decoded.push_back({wire.health.backendFamily, static_cast<Status>(wire.health.status),
            wire.health.endpointCount, {wire.runtimePath, wire.fileVersion, wire.apiVersion, hash}});
    }
    health = std::move(decoded);
    return Status::Ok;
}

namespace {
template <class Extended, class Base, class Record>
std::vector<std::byte> extendInstances(const std::vector<std::byte> &legacy,
                                        const std::vector<Record> &records) {
    const auto count = static_cast<std::uint32_t>(records.size());
    std::vector<std::byte> output(sizeof(count) + records.size() * sizeof(Extended));
    if (output.size() > maxPayloadBytes)
        throw std::length_error("too many instance records");
    std::memcpy(output.data(), &count, sizeof(count));
    for (std::size_t i = 0; i < records.size(); ++i) {
        if (records[i].backendInstanceId.size() >= 128u)
            throw std::length_error("backend instance ID");
        Extended wire{};
        std::memcpy(&wire.base, legacy.data() + sizeof(count) + i * sizeof(Base), sizeof(Base));
        copyString(wire.backendInstanceId, records[i].backendInstanceId);
        std::memcpy(output.data() + sizeof(count) + i * sizeof(wire), &wire, sizeof(wire));
    }
    return output;
}
template <class Extended, class Base, class Record, class Decoder>
Status splitInstances(const std::vector<std::byte> &payload, std::vector<Record> &records, Decoder decode) {
    records.clear();
    if (payload.size() < 4u || payload.size() > maxPayloadBytes)
        return Status::TransportError;
    std::uint32_t count{};
    std::memcpy(&count, payload.data(), 4u);
    if (!exactRecordPayload(payload.size(), count, sizeof(Extended)))
        return Status::TransportError;
    std::vector<std::byte> legacy(4u + static_cast<std::size_t>(count) * sizeof(Base));
    std::memcpy(legacy.data(), &count, 4u);
    std::vector<std::string> instances;
    for (std::uint32_t i = 0; i < count; ++i) {
        Extended wire{};
        std::memcpy(&wire, payload.data() + 4u + static_cast<std::size_t>(i) * sizeof(wire), sizeof(wire));
        if (!std::memchr(wire.backendInstanceId, 0, sizeof(wire.backendInstanceId)))
            return Status::TransportError;
        const std::string id(wire.backendInstanceId);
        if (!std::all_of(id.begin(), id.end(), [](char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                       c == '.' || c == '_' || c == '-';
            }))
            return Status::TransportError;
        instances.push_back(id);
        std::memcpy(legacy.data() + 4u + static_cast<std::size_t>(i) * sizeof(Base), &wire.base,
                    sizeof(Base));
    }
    const auto status = decode(legacy, records);
    if (status != Status::Ok)
        return status;
    for (std::size_t i = 0; i < records.size(); ++i)
        records[i].backendInstanceId = instances[i];
    return Status::Ok;
}
} // namespace
std::vector<std::byte> encodeEndpointsV10(const std::vector<EndpointDescriptor> &values) {
    return extendInstances<WireEndpointV10, WireEndpointV9>(encodeEndpointsV9(values), values);
}
Status decodeEndpointsV10(const std::vector<std::byte> &p, std::vector<EndpointDescriptor> &v) {
    return splitInstances<WireEndpointV10, WireEndpointV9>(p, v, decodeEndpointsV9);
}
std::vector<std::byte> encodeBackendHealthV10(const std::vector<BackendHealth> &values) {
    return extendInstances<WireBackendHealthV10, WireBackendHealthV8>(encodeBackendHealthV8(values),
                                                                       values);
}
Status decodeBackendHealthV10(const std::vector<std::byte> &p, std::vector<BackendHealth> &v) {
    return splitInstances<WireBackendHealthV10, WireBackendHealthV8>(p, v, decodeBackendHealthV8);
}

Status decodeCanFrames(const std::vector<std::byte> &payload, std::vector<CanFrame> &frames) {
    frames.clear();
    if (payload.size() < sizeof(std::uint32_t)) return Status::TransportError;
    std::uint32_t count = 0u;
    std::memcpy(&count, payload.data(), sizeof(count));
    if (!exactRecordPayload(payload.size(), count, sizeof(WireCanFrameV1))) {
        return Status::TransportError;
    }
    const auto* input = reinterpret_cast<const WireCanFrameV1*>(
        payload.data() + sizeof(count));
    frames.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        if (!validCanFrame(input[index])) {
            frames.clear();
            return Status::TransportError;
        }
        CanFrame frame{};
        frame.id = input[index].id;
        frame.flags = input[index].flags;
        frame.dlc = input[index].dlc;
        frame.dataLength = input[index].dataLength;
        std::memcpy(frame.data.data(), input[index].data, frame.data.size());
        frame.timestampUs = input[index].timestampUs;
        frame.transactionId = input[index].transactionId;
        frames.push_back(frame);
    }
    return Status::Ok;
}

Status decodeChannelStatus(const ChannelStatusResponseV1& wire,
                             ChannelStatus& status) {
    if (!validChannelStatusWire(wire)) return Status::TransportError;
    status.busOff = wire.busOff != 0u;
    status.errorPassive = wire.errorPassive != 0u;
    status.warning = wire.warning != 0u;
    status.busStateValid =
        (wire.reserved[0] & channelStatusBusStateValid) != 0u;
    status.errorCountersValid =
        (wire.reserved[0] & channelStatusErrorCountersValid) != 0u;
    status.txErrorCounter = wire.txErrorCounter;
    status.rxErrorCounter = wire.rxErrorCounter;
    return Status::Ok;
}

Status decodeCanEvents(const std::vector<std::byte>& payload,
                         std::vector<CanEvent>& events) {
    events.clear();
    if (payload.size() < sizeof(std::uint32_t)) return Status::TransportError;
    std::uint32_t count = 0u;
    std::memcpy(&count, payload.data(), sizeof(count));
    if (!exactRecordPayload(payload.size(), count, sizeof(WireCanEventV2))) {
        return Status::TransportError;
    }
    const auto* input = reinterpret_cast<const WireCanEventV2*>(
        payload.data() + sizeof(count));
    events.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        if (!validCanEventKind(input[index].kind) ||
            !validStatusValue(input[index].result) || input[index].reserved != 0u ||
            !validCanFrame(input[index].frame)) {
            events.clear();
            return Status::TransportError;
        }
        CanEvent event{};
        event.kind = static_cast<CanEventKind>(input[index].kind);
        event.result = static_cast<Status>(input[index].result);
        event.backendStatus = input[index].backendStatus;
        event.frame.id = input[index].frame.id;
        event.frame.flags = input[index].frame.flags;
        event.frame.dlc = input[index].frame.dlc;
        event.frame.dataLength = input[index].frame.dataLength;
        std::memcpy(event.frame.data.data(), input[index].frame.data,
                    event.frame.data.size());
        event.frame.timestampUs = input[index].frame.timestampUs;
        event.frame.transactionId = input[index].frame.transactionId;
        if (decodeChannelStatus(input[index].channelStatus,
                                  event.channelStatus) != Status::Ok) {
            events.clear();
            return Status::TransportError;
        }
        events.push_back(std::move(event));
    }
    return Status::Ok;
}

Status decodeLinStatus(const LinStatusResponseV3& wire, LinStatus& status) {
    if (!validLinStatusWire(wire)) return Status::TransportError;
    status.active = wire.active != 0u;
    status.asleep = wire.asleep != 0u;
    status.errorFlags = wire.errorFlags;
    return Status::Ok;
}

Status decodeLinEvents(const std::vector<std::byte>& payload,
                         std::vector<LinEvent>& events) {
    events.clear();
    if (payload.size() < sizeof(std::uint32_t)) return Status::TransportError;
    std::uint32_t count = 0u;
    std::memcpy(&count, payload.data(), sizeof(count));
    if (!exactRecordPayload(payload.size(), count, sizeof(WireLinEventV3))) {
        return Status::TransportError;
    }
    const auto* input = reinterpret_cast<const WireLinEventV3*>(
        payload.data() + sizeof(count));
    events.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        if (!validLinEventKind(input[index].kind) ||
            !validStatusValue(input[index].result) || input[index].id > 0x3fu ||
            input[index].dlc > 8u ||
            (input[index].flags & ~knownLinFrameFlags) != 0u) {
            events.clear();
            return Status::TransportError;
        }
        LinEvent event{};
        event.kind = static_cast<LinEventKind>(input[index].kind);
        event.result = static_cast<Status>(input[index].result);
        event.backendStatus = input[index].backendStatus;
        event.frame.flags = input[index].flags;
        event.frame.timestampUs = input[index].timestampUs;
        event.frame.transactionId = input[index].transactionId;
        event.frame.id = input[index].id;
        event.frame.dlc = input[index].dlc;
        event.frame.checksum = input[index].checksum;
        event.frame.errorFlags = input[index].errorFlags;
        std::memcpy(event.frame.data.data(), input[index].data,
                    event.frame.data.size());
        if (decodeLinStatus(input[index].channelStatus,
                              event.channelStatus) != Status::Ok) {
            events.clear();
            return Status::TransportError;
        }
        events.push_back(std::move(event));
    }
    return Status::Ok;
}

}  // namespace Ucal::protocol
