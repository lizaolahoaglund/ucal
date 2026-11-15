#include "ucal/Client.hpp"
#include "ucal/Protocol.hpp"
#if __has_include("frontends/vxlapi/src/VectorFrontendCatalog.hpp")
#include "frontends/vxlapi/src/VectorFrontendCatalog.hpp"
#define UCAL_TEST_VECTOR_CATALOG
#endif
#include "ucal/ChannelClock.hpp"
#include "ucal/CanTiming.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <iostream>
#include <iomanip>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

namespace {

int validateAdvancedTimingContract() {
    Ucal::CanChannelConfig config{};
    config.canFd = true;
    config.nominalTimingSpecified = config.dataTimingSpecified = true;
    config.nominalTiming = {80000000u, 1u, 139u, 20u, 8u, 1u};
    config.dataTiming = {80000000u, 1u, 29u, 10u, 4u, 1u};
    if (!Ucal::validCanAdvancedTiming(config)) return 1;
    Ucal::CanBitTiming resolved{};
    if (!Ucal::resolveCanBitTiming(config.nominalTiming, 500000u, 80000000u, 1024u, 256u, 128u, 128u, resolved) ||
        resolved.tseg1 != 139u || resolved.tseg2 != 20u || resolved.sjw != 8u) return 2;
    if (Ucal::resolveCanBitTiming(config.nominalTiming, 500000u, 8000000u, 64u, 16u, 8u, 4u, resolved)) return 3;
    unsigned calls = 0u;
    const Ucal::Client client([&](std::uint32_t, Ucal::Protocol::Operation operation, const void* input,
        std::uint32_t bytes, std::vector<std::byte>& output) {
        ++calls;
        if (operation != Ucal::Protocol::Operation::OpenCanChannelAdvanced ||
            bytes != sizeof(Ucal::Protocol::OpenCanChannelRequestV11)) return Ucal::Status::InternalError;
        Ucal::Protocol::OpenCanChannelRequestV11 wire{};
        std::memcpy(&wire, input, bytes);
        if (wire.timingFlags != 3u || wire.nominal.tseg1 != 139u || wire.nominal.sjw != 8u || wire.data.sjw != 4u)
            return Ucal::Status::InternalError;
        const Ucal::Protocol::OpenDeviceResponseV1 result{42u};
        output.resize(sizeof(result)); std::memcpy(output.data(), &result, sizeof(result));
        return Ucal::Status::Ok;
    }, GetCurrentProcessId());
    if (client.openCanChannel({}, config).leaseToken != 42u || calls != 1u) return 4;
    config.nominalTiming.sjw = 0u;
    if (client.openCanChannel({}, config).status != Ucal::Status::InvalidArgument || calls != 1u) return 5;
    config.nominalTiming.sjw = 999u;
    if (client.openCanChannel({}, config).status != Ucal::Status::InvalidArgument || calls != 1u) return 6;
    std::cout << "advancedTimingContract=true serializedExact=true invalidBeforeMutation=true\n";
    return 0;
}

template <typename Wire>
std::vector<std::byte> countedPayload(const Wire& wire) {
    const std::uint32_t count = 1u;
    std::vector<std::byte> payload(sizeof(count) + sizeof(wire));
    std::memcpy(payload.data(), &count, sizeof(count));
    std::memcpy(payload.data() + sizeof(count), &wire, sizeof(wire));
    return payload;
}

int validateProtocolDecoders() {
    // Test namespace isolation without changing the environment for later cases.
    std::array<wchar_t, 32768> savedInstance{};
    const auto savedSize = GetEnvironmentVariableW(L"UCAL_BROKER_INSTANCE",
        savedInstance.data(), static_cast<DWORD>(savedInstance.size()));
    SetEnvironmentVariableW(L"UCAL_BROKER_INSTANCE", nullptr);
    const auto defaultName = Ucal::Protocol::defaultPipeName();
    SetEnvironmentVariableW(L"UCAL_BROKER_INSTANCE", L"acceptance-01_A");
    bool namespaceOk = Ucal::Protocol::defaultPipeName() == defaultName + L".acceptance-01_A";
    for (const auto* invalid : {L"a.b", L"a/b", L"a\\b", L"space name",
            L"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}) {
        SetEnvironmentVariableW(L"UCAL_BROKER_INSTANCE", invalid);
        namespaceOk = namespaceOk && Ucal::Protocol::defaultPipeName().empty();
    }
    SetEnvironmentVariableW(L"UCAL_BROKER_INSTANCE", savedSize ? savedInstance.data() : nullptr);
    if (!namespaceOk) return 209;
    auto header = Ucal::Protocol::makeRequest(
        Ucal::Protocol::Operation::Ping, 1u, 0u);
    header.status = static_cast<std::int32_t>(Ucal::Status::InternalError);
    if (!Ucal::Protocol::validResponseHeader(header)) return 210;
    header.status = 0x7fffffff;
    if (Ucal::Protocol::validResponseHeader(header)) return 211;

    std::vector<std::byte> overflow(sizeof(std::uint32_t));
    const std::uint32_t impossibleCount = 0xffffffffu;
    std::memcpy(overflow.data(), &impossibleCount, sizeof(impossibleCount));
    std::vector<Ucal::DeviceDescriptor> devices;
    std::vector<Ucal::EndpointDescriptor> endpoints;
    std::vector<Ucal::BackendHealth> health;
    std::vector<Ucal::CanFrame> frames;
    std::vector<Ucal::CanEvent> canEvents;
    std::vector<Ucal::LinEvent> linEvents;
    if (Ucal::Protocol::decodeDevices(overflow, devices) != Ucal::Status::TransportError ||
        Ucal::Protocol::decodeEndpoints(overflow, endpoints) != Ucal::Status::TransportError ||
        Ucal::Protocol::decodeEndpointsV6(overflow, endpoints) != Ucal::Status::TransportError ||
        Ucal::Protocol::decodeBackendHealth(overflow, health) != Ucal::Status::TransportError ||
        Ucal::Protocol::decodeBackendHealthV8(overflow, health) != Ucal::Status::TransportError ||
        Ucal::Protocol::decodeCanFrames(overflow, frames) != Ucal::Status::TransportError ||
        Ucal::Protocol::decodeCanEvents(overflow, canEvents) != Ucal::Status::TransportError ||
        Ucal::Protocol::decodeLinEvents(overflow, linEvents) != Ucal::Status::TransportError) {
        return 212;
    }

    const Ucal::BackendHealth runtimeHealth{"test", Ucal::Status::Ok, 2u,
        {"runtime.dll", "1.16.0.49", "1.16.0.49", std::string(64u, 'a')}};
    auto runtimePayload = Ucal::Protocol::encodeBackendHealthV8({runtimeHealth});
    if (Ucal::Protocol::decodeBackendHealthV8(runtimePayload, health) != Ucal::Status::Ok ||
        health.size() != 1u || health[0].runtime.path != runtimeHealth.runtime.path ||
        health[0].runtime.apiVersion != runtimeHealth.runtime.apiVersion ||
        health[0].runtime.sha256 != runtimeHealth.runtime.sha256) return 246;
    auto* runtimeWire = reinterpret_cast<Ucal::Protocol::WireBackendHealthV8*>(
        runtimePayload.data() + sizeof(std::uint32_t));
    std::memset(runtimeWire->runtimePath, 'x', sizeof(runtimeWire->runtimePath));
    if (Ucal::Protocol::decodeBackendHealthV8(runtimePayload, health) != Ucal::Status::TransportError ||
        !health.empty()) return 247;
    runtimePayload = Ucal::Protocol::encodeBackendHealthV8({runtimeHealth});
    runtimePayload.pop_back();
    if (Ucal::Protocol::decodeBackendHealthV8(runtimePayload, health) != Ucal::Status::TransportError)
        return 248;
    if (Ucal::Protocol::decodeBackendHealth(Ucal::Protocol::encodeBackendHealth({runtimeHealth}), health) !=
        Ucal::Status::Ok || health.size() != 1u || !health[0].runtime.path.empty()) return 249;

    Ucal::DeviceDescriptor device{};
    device.id[0] = 1u;
    device.channelCount = 1u;
    device.capabilities = Ucal::CapabilityClassicCan;
    device.state = Ucal::DeviceState::Available;
    device.name = "safe";
    auto devicePayload = Ucal::Protocol::encodeDevices({device});
    if (Ucal::Protocol::decodeDevices(devicePayload, devices) != Ucal::Status::Ok ||
        devices.size() != 1u) return 213;
    auto* wireDevice = reinterpret_cast<Ucal::Protocol::WireDeviceV1*>(
        devicePayload.data() + sizeof(std::uint32_t));
    wireDevice->state = 99u;
    if (Ucal::Protocol::decodeDevices(devicePayload, devices) !=
        Ucal::Status::TransportError) return 214;
    wireDevice->state = static_cast<std::uint32_t>(Ucal::DeviceState::Available);
    wireDevice->capabilities = 1u << 30;
    if (Ucal::Protocol::decodeDevices(devicePayload, devices) !=
        Ucal::Status::TransportError) return 215;

    Ucal::EndpointDescriptor endpoint{};
    endpoint.id[0] = 1u;
    endpoint.adapterId[0] = 2u;
    endpoint.backendFamily = "test";
    endpoint.busType = Ucal::BusType::Can;
    endpoint.capabilities = Ucal::CapabilityClassicCan;
    endpoint.state = Ucal::DeviceState::Available;
    auto endpointPayload = Ucal::Protocol::encodeEndpointsV6({endpoint});
    if (Ucal::Protocol::decodeEndpointsV6(endpointPayload, endpoints) !=
            Ucal::Status::Ok || endpoints.size() != 1u) return 216;
    auto* wireEndpoint = reinterpret_cast<Ucal::Protocol::WireEndpointV6*>(
        endpointPayload.data() + sizeof(std::uint32_t));
    wireEndpoint->endpoint.busType = 0u;
    if (Ucal::Protocol::decodeEndpointsV6(endpointPayload, endpoints) !=
        Ucal::Status::TransportError) return 217;

    endpoint.configuration.canFd = {Ucal::CanConfigNominalBitrate, 0u,
        Ucal::CanConfigNominalBitrate, Ucal::CanConfigNominalBitrate};
    endpoint.bindingIsDefault = true;
    auto accessPayload = Ucal::Protocol::encodeEndpointsV9({endpoint});
    if (Ucal::Protocol::decodeEndpointsV9(accessPayload, endpoints) != Ucal::Status::Ok ||
        endpoints.size() != 1u || !endpoints[0].bindingIsDefault ||
        endpoints[0].configuration.canFd.readSupported != 0u ||
        endpoints[0].configuration.canFd.writeSupported != Ucal::CanConfigNominalBitrate) return 240;
    auto* accessWire = reinterpret_cast<Ucal::Protocol::WireEndpointV9*>(accessPayload.data() + sizeof(std::uint32_t));
    accessWire->canFd.readSupported = Ucal::CanConfigDataBitrate;
    if (Ucal::Protocol::decodeEndpointsV9(accessPayload, endpoints) != Ucal::Status::TransportError) return 241;
    accessWire->canFd.readSupported = 0u;
    accessWire->canFd.writeKnown |= 0x80000000u;
    if (Ucal::Protocol::decodeEndpointsV9(accessPayload, endpoints) != Ucal::Status::TransportError) return 242;
    accessWire->canFd.writeKnown &= ~0x80000000u;
    accessWire->bindingIsDefault = 2u;
    if (Ucal::Protocol::decodeEndpointsV9(accessPayload, endpoints) != Ucal::Status::TransportError) return 243;
    accessWire->bindingIsDefault = 1u;
    accessPayload.push_back(std::byte{});
    if (Ucal::Protocol::decodeEndpointsV9(accessPayload, endpoints) != Ucal::Status::TransportError) return 244;

    Ucal::Protocol::WireCanFrameV1 wireFrame{};
    wireFrame.id = 0x123u;
    wireFrame.dlc = 8u;
    wireFrame.dataLength = 8u;
    auto framePayload = countedPayload(wireFrame);
    if (Ucal::Protocol::decodeCanFrames(framePayload, frames) != Ucal::Status::Ok ||
        frames.size() != 1u) return 218;
    auto* mutableFrame = reinterpret_cast<Ucal::Protocol::WireCanFrameV1*>(
        framePayload.data() + sizeof(std::uint32_t));
    mutableFrame->flags = 1u << 30;
    if (Ucal::Protocol::decodeCanFrames(framePayload, frames) !=
        Ucal::Status::TransportError) return 219;

    Ucal::Protocol::WireCanEventV2 wireCanEvent{};
    wireCanEvent.kind = static_cast<std::uint32_t>(Ucal::CanEventKind::Received);
    wireCanEvent.result = static_cast<std::int32_t>(Ucal::Status::Ok);
    wireCanEvent.frame.id = 0x123u;
    wireCanEvent.frame.dlc = 1u;
    wireCanEvent.frame.dataLength = 1u;
    wireCanEvent.channelStatus.reserved[0] =
        Ucal::Protocol::channelStatusBusStateValid;
    auto canEventPayload = countedPayload(wireCanEvent);
    if (Ucal::Protocol::decodeCanEvents(canEventPayload, canEvents) !=
            Ucal::Status::Ok || canEvents.size() != 1u) return 220;
    auto* mutableCanEvent = reinterpret_cast<Ucal::Protocol::WireCanEventV2*>(
        canEventPayload.data() + sizeof(std::uint32_t));
    mutableCanEvent->kind = 99u;
    if (Ucal::Protocol::decodeCanEvents(canEventPayload, canEvents) !=
        Ucal::Status::TransportError) return 221;
    mutableCanEvent->kind = static_cast<std::uint32_t>(Ucal::CanEventKind::Received);
    mutableCanEvent->channelStatus.reserved[0] = 0x80u;
    if (Ucal::Protocol::decodeCanEvents(canEventPayload, canEvents) !=
        Ucal::Status::TransportError) return 222;

    Ucal::Protocol::WireLinEventV3 wireLinEvent{};
    wireLinEvent.kind = static_cast<std::uint32_t>(Ucal::LinEventKind::Received);
    wireLinEvent.result = static_cast<std::int32_t>(Ucal::Status::Ok);
    wireLinEvent.id = 0x12u;
    wireLinEvent.dlc = 2u;
    auto linEventPayload = countedPayload(wireLinEvent);
    if (Ucal::Protocol::decodeLinEvents(linEventPayload, linEvents) !=
            Ucal::Status::Ok || linEvents.size() != 1u) return 223;
    auto* mutableLinEvent = reinterpret_cast<Ucal::Protocol::WireLinEventV3*>(
        linEventPayload.data() + sizeof(std::uint32_t));
    mutableLinEvent->result = 99;
    if (Ucal::Protocol::decodeLinEvents(linEventPayload, linEvents) !=
        Ucal::Status::TransportError) return 224;

    Ucal::Protocol::ChannelStatusResponseV1 channelStatusWire{};
    channelStatusWire.busOff = 2u;
    Ucal::ChannelStatus channelStatus{};
    if (Ucal::Protocol::decodeChannelStatus(channelStatusWire, channelStatus) !=
        Ucal::Status::TransportError) return 225;
    Ucal::Protocol::LinStatusResponseV3 linStatusWire{};
    linStatusWire.reserved[4] = 1u;
    Ucal::LinStatus linStatus{};
    if (Ucal::Protocol::decodeLinStatus(linStatusWire, linStatus) !=
        Ucal::Status::TransportError) return 226;

    std::cout << "{\"protocolDecoders\":true,\"divisionFirstBounds\":true,"
                 "\"knownEnumsAndMasks\":true}" << std::endl;
    return 0;
}

Ucal::EndpointDescriptor catalogEndpoint(const std::uint8_t adapter,
                                           const Ucal::BusType busType,
                                           const std::uint32_t physicalIndex) {
    Ucal::EndpointDescriptor endpoint{};
    endpoint.adapterId[0] = adapter;
    endpoint.id[0] = adapter;
    endpoint.id[1] = busType == Ucal::BusType::Can ? 1u : 2u;
    endpoint.id[2] = static_cast<std::uint8_t>(physicalIndex);
    endpoint.name = busType == Ucal::BusType::Can ? "Virtual CAN" : "LIN";
    endpoint.serial = "SERIAL-" + std::to_string(adapter);
    endpoint.busType = busType;
    endpoint.physicalIndex = physicalIndex;
    return endpoint;
}

#ifdef UCAL_TEST_VECTOR_CATALOG
int validateVectorFrontendCatalog() {
    std::vector<Ucal::EndpointDescriptor> endpoints;
    // Deliberately reproduce an XL4-style mixed and shuffled enumeration:
    // four CAN ports followed by four LIN ports per adapter, four adapters.
    for (const auto adapter : {3u, 1u, 2u, 0u}) {
        for (std::uint32_t port = 4u; port < 8u; ++port) {
            endpoints.push_back(catalogEndpoint(
                static_cast<std::uint8_t>(adapter), Ucal::BusType::Lin, port));
        }
        for (std::uint32_t port = 0u; port < 4u; ++port) {
            endpoints.push_back(catalogEndpoint(
                static_cast<std::uint8_t>(adapter), Ucal::BusType::Can, port));
        }
    }

    const auto original = endpoints;
    if (!Ucal::normalizeVectorFrontendCatalog(endpoints)) return 169;
    if (endpoints.size() != 32u) return 170;
    for (std::size_t index = 0; index < 16u; ++index) {
        const auto& endpoint = endpoints[index];
        if (endpoint.busType != Ucal::BusType::Can ||
            endpoint.adapterId[0] != index / 4u ||
            endpoint.physicalIndex != index % 4u) {
            return 171;
        }
    }
    for (std::size_t index = 16u; index < endpoints.size(); ++index) {
        if (endpoints[index].busType != Ucal::BusType::Lin) return 172;
    }

    if (!Ucal::normalizeVectorFrontendCatalog(endpoints, "SERIAL-2")) return 173;
    if (endpoints.size() != 8u) return 173;
    for (std::size_t index = 0; index < 4u; ++index) {
        if (endpoints[index].busType != Ucal::BusType::Can ||
            endpoints[index].physicalIndex != index) {
            return 174;
        }
    }
    for (std::size_t index = 4u; index < endpoints.size(); ++index) {
        if (endpoints[index].busType != Ucal::BusType::Lin) return 175;
    }
    endpoints = original;
    if (!Ucal::normalizeVectorFrontendCatalog(
            endpoints, {}, "SERIAL-2|3;SERIAL-0|0;SERIAL-3|1;SERIAL-1|2;SERIAL-2|0")) {
        return 176;
    }
    const std::array<std::uint8_t, 5> expectedAdapters{2u, 0u, 3u, 1u, 2u};
    const std::array<std::uint32_t, 5> expectedPorts{3u, 0u, 1u, 2u, 0u};
    for (std::size_t index = 0; index < expectedAdapters.size(); ++index) {
        if (endpoints[index].busType != Ucal::BusType::Can ||
            endpoints[index].adapterId[0] != expectedAdapters[index] ||
            endpoints[index].physicalIndex != expectedPorts[index]) {
            return 177;
        }
    }
    if (endpoints.size() != 21u) return 178;
    auto invalid = original;
    if (Ucal::normalizeVectorFrontendCatalog(invalid, {}, "SERIAL-9|0") ||
        !invalid.empty()) {
        return 179;
    }

    std::vector<Ucal::EndpointDescriptor> collision{
        catalogEndpoint(4u, Ucal::BusType::Can, 0u),
        catalogEndpoint(5u, Ucal::BusType::Can, 0u),
        catalogEndpoint(4u, Ucal::BusType::Lin, 4u)};
    collision[0].serial = "COLLISION";
    collision[1].serial = "COLLISION";
    collision[2].serial = "COLLISION";
    auto ambiguous = collision;
    if (Ucal::normalizeVectorFrontendCatalog(ambiguous, {}, "COLLISION|0") ||
        !ambiguous.empty()) {
        return 180;
    }
    const auto firstId = Ucal::deviceIdToString(collision[0].id);
    auto secondId = Ucal::deviceIdToString(collision[1].id);
    std::transform(secondId.begin(), secondId.end(), secondId.begin(),
                   [](const unsigned char character) {
                       return static_cast<char>(std::toupper(character));
                   });
    auto exact = collision;
    if (!Ucal::normalizeVectorFrontendCatalog(
            exact, {}, {}, secondId + ";" + firstId) ||
        exact.size() != 3u || exact[0].adapterId[0] != 5u ||
        exact[1].adapterId[0] != 4u || exact[2].busType != Ucal::BusType::Lin) {
        return 181;
    }
    for (const auto& invalidSelector : {
             std::string{"not-a-uuid"}, firstId + ";" + firstId, firstId + ";"}) {
        invalid = collision;
        if (Ucal::normalizeVectorFrontendCatalog(
                invalid, {}, {}, invalidSelector) || !invalid.empty()) {
            return 182;
        }
    }
    invalid = collision;
    if (Ucal::normalizeVectorFrontendCatalog(
            invalid, {}, "COLLISION|0", firstId) || !invalid.empty()) {
        return 183;
    }
    auto canOnly = original;
    auto unfiltered = original;
    if (!Ucal::normalizeVectorFrontendCatalog(canOnly, {}, {}, {}, true) ||
        !Ucal::normalizeVectorFrontendCatalog(unfiltered) ||
        canOnly.size() != 16u || !std::equal(canOnly.begin(), canOnly.end(),
            unfiltered.begin(), [](const auto& left, const auto& right) {
                return left.busType == Ucal::BusType::Can && left.id == right.id;
            })) return 184;
    canOnly = collision;
    if (!Ucal::normalizeVectorFrontendCatalog(canOnly, {}, {}, secondId + ";" + firstId, true) ||
        canOnly.size() != 2u || canOnly[0].adapterId[0] != 5u ||
        canOnly[1].adapterId[0] != 4u) return 185;
    std::cout << "{\"vectorFrontendCatalog\":true,\"canChannels\":16,"
                 "\"linDoesNotShiftCan\":true,\"serialFilter\":true,"
                 "\"explicitFiveChannelMap\":true,\"exactEndpointMap\":true}" << std::endl;
    return 0;
}
#else
int validateVectorFrontendCatalog() { return int(Ucal::Status::Unsupported); }
#endif


int enumerateAndValidate(Ucal::Client& client, std::vector<Ucal::DeviceDescriptor>& devices) {
    const auto status = client.enumerate(devices);
    if (status != Ucal::Status::Ok) return 10 + static_cast<int>(status);
    if (devices.size() != 2 || devices[0].name != "Unified CAN Mock X2" ||
        devices[0].channelCount != 2 || devices[1].name != "Unified CAN Mock X4" ||
        devices[1].channelCount != 4) return 20;
    for (const auto& device : devices) {
        const auto expected = Ucal::CapabilityClassicCan | Ucal::CapabilityCanFdIso |
                              Ucal::CapabilityBrs | Ucal::CapabilityDiscoveryOnly;
        if (device.capabilities != expected) return 21;
        std::cout << "{\"id\":\"" << Ucal::deviceIdToString(device.id) << "\",\"name\":\""
                  << device.name << "\",\"channels\":" << device.channelCount
                  << ",\"capabilities\":" << device.capabilities
                  << ",\"state\":" << static_cast<std::uint32_t>(device.state) << "}" << std::endl;
    }
    return 0;
}

int enumerateBackend(Ucal::Client& client, std::vector<Ucal::DeviceDescriptor>& devices) {
    const auto status = client.enumerate(devices);
    if (status != Ucal::Status::Ok) return 50 + static_cast<int>(status);
    for (const auto& device : devices) {
        std::cout << "{\"id\":\"" << Ucal::deviceIdToString(device.id) << "\",\"name\":\""
                  << device.name << "\",\"model\":\"" << device.model << "\",\"serial\":\""
                  << device.serial << "\",\"capabilities\":" << device.capabilities
                  << ",\"state\":" << static_cast<std::uint32_t>(device.state) << "}" << std::endl;
    }
    return devices.empty() ? 60 : 0;
}

int enumerateMockEndpoints(Ucal::Client& client) {
    std::vector<Ucal::EndpointDescriptor> endpoints;
    const auto status = client.enumerateEndpoints(endpoints);
    if (status != Ucal::Status::Ok) return 70 + static_cast<int>(status);
    if (endpoints.size() != 6u) return 80;
    std::size_t x2 = 0;
    std::size_t x4 = 0;
    for (const auto& endpoint : endpoints) {
        if (endpoint.busType != Ucal::BusType::Can) return 81;
        if (endpoint.backendFamily != "mock") return 83;
        if (endpoint.name.find("Mock X2") != std::string::npos) ++x2;
        if (endpoint.name.find("Mock X4") != std::string::npos) ++x4;
        std::cout << "{\"endpoint\":\"" << Ucal::deviceIdToString(endpoint.id)
                  << "\",\"adapter\":\"" << Ucal::deviceIdToString(endpoint.adapterId)
                  << "\",\"physicalIndex\":" << endpoint.physicalIndex << "}" << std::endl;
    }
    return x2 == 2u && x4 == 4u ? 0 : 82;
}

int validateBackendHealth(Ucal::Client& client) {
    std::vector<Ucal::BackendHealth> health;
    const auto status = client.enumerateBackendHealth(health);
    if (status != Ucal::Status::Ok) return 220 + static_cast<int>(status);
    if (health.empty()) return 230;
    for (const auto& item : health) {
        if (item.backendFamily.empty()) return 231;
        std::cout << "{\"backendFamily\":\"" << item.backendFamily
                  << "\",\"status\":" << static_cast<std::int32_t>(item.status)
                  << ",\"endpointCount\":" << item.endpointCount
                  << ",\"runtimePath\":" << std::quoted(item.runtime.path)
                  << ",\"fileVersion\":" << std::quoted(item.runtime.fileVersion)
                  << ",\"apiVersion\":" << std::quoted(item.runtime.apiVersion)
                  << ",\"sha256\":" << std::quoted(item.runtime.sha256) << "}" << std::endl;
    }
    return 0;
}

int validateCanFdAdmission(Ucal::Client& client) {
    Ucal::EndpointDescriptor contract{};
    contract.capabilities = Ucal::CapabilityClassicCan;
    if (Ucal::endpointMeetsDeviceContract(contract)) return 240;
    contract.capabilities |= Ucal::CapabilityCanFdIso;
    if (!Ucal::endpointMeetsDeviceContract(contract)) return 241;
    contract.capabilities = Ucal::CapabilityLin;
    contract.busType = Ucal::BusType::Lin;
    if (!Ucal::endpointMeetsDeviceContract(contract)) return 242;
    std::vector<Ucal::EndpointDescriptor> endpoints;
    if (client.enumerateEndpoints(endpoints) != Ucal::Status::Ok || endpoints.size() != 1u)
        return 243;
    const auto& rejected = endpoints.front();
    if (Ucal::endpointMeetsDeviceContract(rejected) ||
        rejected.state != Ucal::DeviceState::Offline) return 244;
    Ucal::CanChannelConfig config{};
    for (const bool fd : {false, true}) {
        config.canFd = fd;
        const auto result = client.openCanChannel(rejected.id, config);
        if (result.status != Ucal::Status::Unsupported || result.leaseToken != 0u) return 245;
    }
    std::cout << "CAN FD admission: rejected port retained; Classic and FD opens refused\n";
    return 0;
}

std::string selectedSerial() {
    std::array<char, 64> storage{};
    const auto size = GetEnvironmentVariableA(
        "UCAL_HARNESS_SERIAL", storage.data(), static_cast<DWORD>(storage.size()));
    return size == 0 || size >= storage.size() ? std::string{} :
        std::string(storage.data(), size);
}

unsigned selectedPort(const unsigned fallback) {
    std::array<char, 16> storage{};
    const auto size = GetEnvironmentVariableA(
        "UCAL_HARNESS_PORT", storage.data(), static_cast<DWORD>(storage.size()));
    return size == 0 || size >= storage.size() ? fallback :
        static_cast<unsigned>(std::strtoul(storage.data(), nullptr, 10));
}

bool environmentUnsigned(const char* const name, unsigned long& value) {
    std::array<char, 32> storage{};
    const auto size = GetEnvironmentVariableA(name, storage.data(),
                                               static_cast<DWORD>(storage.size()));
    if (size == 0 || size >= storage.size()) return false;
    char* end{};
    value = std::strtoul(storage.data(), &end, 0);
    return end != storage.data() && *end == '\0';
}

bool environmentSwitch(const char* const name, const bool fallback) {
    unsigned long value{};
    return environmentUnsigned(name, value) ? value != 0 : fallback;
}

std::string environmentString(const char* const name) {
    std::array<char, 128> storage{};
    const auto size = GetEnvironmentVariableA(
        name, storage.data(), static_cast<DWORD>(storage.size()));
    return size == 0 || size >= storage.size() ? std::string{} :
        std::string(storage.data(), size);
}

void applyCanEnvironment(Ucal::CanChannelConfig& config) {
    unsigned long value{};
    if (environmentUnsigned("UCAL_HARNESS_NBITRATE", value)) {
        config.arbitrationBitrate = static_cast<std::uint32_t>(value);
    }
    if (environmentUnsigned("UCAL_HARNESS_DBITRATE", value)) {
        config.dataBitrate = static_cast<std::uint32_t>(value);
    }
    if (environmentUnsigned("UCAL_HARNESS_NSP", value)) {
        config.arbitrationSamplePoint = static_cast<std::uint8_t>(value);
        config.arbitrationSamplePointSpecified = true;
    }
    if (environmentUnsigned("UCAL_HARNESS_DSP", value)) {
        config.dataSamplePoint = static_cast<std::uint8_t>(value);
        config.dataSamplePointSpecified = true;
    }
    config.listenOnly = environmentSwitch("UCAL_HARNESS_LISTEN_ONLY", false);
    config.noAck = environmentSwitch("UCAL_HARNESS_NO_ACK", false);
    config.internalLoopback = environmentSwitch("UCAL_HARNESS_INTERNAL_LOOPBACK", false);
    if (environmentUnsigned("UCAL_HARNESS_TERMINATION", value)) {
        config.termination = value != 0u;
        config.terminationSpecified = true;
    }
    const auto addFilter = [&](const char* codeName, const char* maskName,
                                const char* rejectName,
                                const Ucal::CanFilterFrameType frameType) {
        unsigned long code{};
        unsigned long mask{};
        const auto hasCode = environmentUnsigned(codeName, code);
        const auto hasMask = environmentUnsigned(maskName, mask);
        if (!hasCode && !hasMask && !environmentSwitch(rejectName, false)) return;
        if (config.acceptanceFilterCount >= config.acceptanceFilters.size()) return;
        auto& filter = config.acceptanceFilters[config.acceptanceFilterCount++];
        filter.frameType = frameType;
        filter.idCode = static_cast<std::uint32_t>(code);
        filter.idMask = static_cast<std::uint32_t>(mask);
        filter.rejectAll = environmentSwitch(rejectName, false);
    };
    addFilter("UCAL_HARNESS_STD_FILTER_CODE", "UCAL_HARNESS_STD_FILTER_MASK",
               "UCAL_HARNESS_STD_REJECT_ALL", Ucal::CanFilterFrameType::Standard);
    addFilter("UCAL_HARNESS_EXT_FILTER_CODE", "UCAL_HARNESS_EXT_FILTER_MASK",
               "UCAL_HARNESS_EXT_REJECT_ALL", Ucal::CanFilterFrameType::Extended);
}

int probeBackend(Ucal::Client& client) {
    std::vector<Ucal::EndpointDescriptor> endpoints;
    const auto status = client.enumerateEndpoints(endpoints);
    if (status != Ucal::Status::Ok) return 90 + static_cast<int>(status);
    std::cout << "{\"backendProbe\":true,\"endpointCount\":" << endpoints.size()
              << "}" << std::endl;
    return 0;
}

int listBackendEndpoints(Ucal::Client& client) {
    std::vector<Ucal::EndpointDescriptor> endpoints;
    const auto status = client.enumerateEndpoints(endpoints);
    if (status != Ucal::Status::Ok) return 90 + static_cast<int>(status);
    for (const auto& endpoint : endpoints) {
        std::cout << "{\"endpoint\":\"" << Ucal::deviceIdToString(endpoint.id)
                  << "\",\"adapter\":\"" << Ucal::deviceIdToString(endpoint.adapterId)
                  << "\",\"backendFamily\":\"" << endpoint.backendFamily
                  << "\",\"busType\":" << static_cast<std::uint32_t>(endpoint.busType)
                  << ",\"physicalIndex\":" << endpoint.physicalIndex
                  << ",\"capabilities\":" << endpoint.capabilities
                  << ",\"state\":" << static_cast<std::uint32_t>(endpoint.state)
                  << "}" << std::endl;
    }
    return 0;
}

int validateCanHardwareSmoke(Ucal::Client& client,
                                const bool fixedSelfReceiveContract,
                                const bool submitFrame = true,
                                const bool configurationOnly = false) {
    std::vector<Ucal::EndpointDescriptor> endpoints;
    const auto enumeration = client.enumerateEndpoints(endpoints);
    if (enumeration != Ucal::Status::Ok) return 180 + static_cast<int>(enumeration);
    std::vector<Ucal::EndpointDescriptor> canEndpoints;
    std::copy_if(endpoints.begin(), endpoints.end(), std::back_inserter(canEndpoints),
                 [](const auto& endpoint) { return endpoint.busType == Ucal::BusType::Can; });
    const std::string requestedEndpoint = environmentString("UCAL_HARNESS_ENDPOINT_ID");
    auto selected = canEndpoints.cend();
    if (!requestedEndpoint.empty()) {
        selected = std::find_if(canEndpoints.cbegin(), canEndpoints.cend(),
                                [&requestedEndpoint](const auto& endpoint) {
            return Ucal::deviceIdToString(endpoint.id) == requestedEndpoint;
        });
    } else {
        unsigned long endpointIndex = 0;
        environmentUnsigned("UCAL_HARNESS_ENDPOINT_INDEX", endpointIndex);
        if (endpointIndex < canEndpoints.size()) {
            selected = canEndpoints.cbegin() + static_cast<std::ptrdiff_t>(endpointIndex);
        }
    }
    if (selected == canEndpoints.cend()) {
        std::cout << "{\"canHardwareSmoke\":false,\"stage\":\"selection\","
                     "\"requestedEndpoint\":\"" << requestedEndpoint << "\"}" << std::endl;
        return 190;
    }

    const bool expectNoReceive = !configurationOnly &&
        environmentSwitch("UCAL_HARNESS_EXPECT_NO_RECEIVE", false);
    const bool requireReceived = configurationOnly || expectNoReceive ? false :
        fixedSelfReceiveContract ? true :
        environmentSwitch("UCAL_HARNESS_REQUIRE_RECEIVED", true);
    const bool requireSelfReceived = configurationOnly ? false :
        fixedSelfReceiveContract ? true :
        environmentSwitch("UCAL_HARNESS_REQUIRE_SELF_RECEIVED", false);
    const bool requireTxCompletion = configurationOnly ? false :
        fixedSelfReceiveContract ? false :
        environmentSwitch("UCAL_HARNESS_REQUIRE_TX_COMPLETION", false);
    const bool requireTimestamp = configurationOnly ? false :
        fixedSelfReceiveContract ? true :
        environmentSwitch("UCAL_HARNESS_REQUIRE_TIMESTAMP", true);
    const bool allowSubmitOnly = !configurationOnly &&
        environmentSwitch("UCAL_HARNESS_ALLOW_SUBMIT_ONLY", false);
    const bool allowDuplicates = !configurationOnly &&
        environmentSwitch("UCAL_HARNESS_ALLOW_DUPLICATES", false);
    const bool measureReceiveLatency = !configurationOnly &&
        environmentSwitch("UCAL_HARNESS_MEASURE_RX_LATENCY", true);
    if (!configurationOnly && !expectNoReceive &&
        !requireReceived && !requireTxCompletion && !allowSubmitOnly) return 191;
    if (!submitFrame && requireTxCompletion) return 218;

    Ucal::CanChannelConfig config{};
    config.canFd = environmentSwitch("UCAL_HARNESS_CAN_FD", false);
    applyCanEnvironment(config);
    if (config.canFd &&
        (selected->capabilities & Ucal::CapabilityCanFdIso) == 0u) return 192;
    if (requireTxCompletion &&
        (selected->capabilities & Ucal::CapabilityTxCompletion) == 0u) return 193;
    unsigned long beforeOpenDelayMs = 0u;
    environmentUnsigned("UCAL_HARNESS_BEFORE_OPEN_DELAY_MS", beforeOpenDelayMs);
    if (beforeOpenDelayMs != 0u) {
        std::this_thread::sleep_for(std::chrono::milliseconds((std::min)(beforeOpenDelayMs, 30000ul)));
    }
    const auto opened = client.openCanChannel(selected->id, config);
    std::cout << "{\"canHardwareSmokeOpen\":true,\"endpoint\":\""
              << Ucal::deviceIdToString(selected->id) << "\",\"adapter\":\""
              << Ucal::deviceIdToString(selected->adapterId) << "\",\"physicalIndex\":"
              << selected->physicalIndex << ",\"capabilities\":"
              << selected->capabilities << ",\"status\":"
              << static_cast<int>(opened.status) << ",\"nominalBitrate\":"
              << config.arbitrationBitrate << ",\"dataBitrate\":"
              << config.dataBitrate << ",\"canFd\":"
              << (config.canFd ? "true" : "false") << "}" << std::endl;
    if (opened.status != Ucal::Status::Ok || opened.leaseToken == 0u) {
        return 194 + static_cast<int>(opened.status);
    }

    int result = 0;
    Ucal::ChannelStatus channelStatus{};
    const auto statusRead = client.getChannelStatus(opened.leaseToken, channelStatus);
    if (statusRead != Ucal::Status::Ok) result = 204 + static_cast<int>(statusRead);
    unsigned long configuredFrameCount = 1u;
    environmentUnsigned("UCAL_HARNESS_FRAME_COUNT", configuredFrameCount);
    const auto frameCount = static_cast<std::size_t>(
        (std::max)(1ul, (std::min)(configuredFrameCount, 100000ul)));
    unsigned long interFrameDelayUs = 0u;
    environmentUnsigned("UCAL_HARNESS_INTER_FRAME_US", interFrameDelayUs);
    interFrameDelayUs = (std::min)(interFrameDelayUs, 1000000ul);
    unsigned long maximumP95LatencyUs = 0u;
    environmentUnsigned("UCAL_HARNESS_MAX_P95_LATENCY_US", maximumP95LatencyUs);
    unsigned long maximumP99LatencyUs = 0u;
    environmentUnsigned("UCAL_HARNESS_MAX_P99_LATENCY_US", maximumP99LatencyUs);
    unsigned long maximumP999LatencyUs = 0u;
    environmentUnsigned("UCAL_HARNESS_MAX_P999_LATENCY_US", maximumP999LatencyUs);
    unsigned long pollEveryFrames = 1u;
    environmentUnsigned("UCAL_HARNESS_POLL_EVERY_FRAMES", pollEveryFrames);
    pollEveryFrames = (std::max)(1ul, (std::min)(pollEveryFrames, 10000ul));
    unsigned long configuredPayloadLength = config.canFd ? 12u : 8u;
    environmentUnsigned("UCAL_HARNESS_PAYLOAD_LENGTH", configuredPayloadLength);
    const bool remoteFrame = environmentSwitch("UCAL_HARNESS_REMOTE_FRAME", false);
    unsigned long remoteDlc = 8u;
    environmentUnsigned("UCAL_HARNESS_REMOTE_DLC", remoteDlc);
    const auto payloadDlc = [](const unsigned long length) -> unsigned {
        if (length <= 8u) return static_cast<unsigned>(length);
        switch (length) {
            case 12u: return 9u;
            case 16u: return 10u;
            case 20u: return 11u;
            case 24u: return 12u;
            case 32u: return 13u;
            case 48u: return 14u;
            case 64u: return 15u;
            default: return 0xffu;
        }
    };
    const auto configuredDlc = remoteFrame ? static_cast<unsigned>(remoteDlc) :
                                               payloadDlc(configuredPayloadLength);
    if ((remoteFrame && (config.canFd || remoteDlc > 8u || frameCount != 1u)) ||
        (!remoteFrame && ((configuredPayloadLength < 4u && frameCount != 1u) ||
                           configuredDlc == 0xffu ||
                           (!config.canFd && configuredPayloadLength > 8u)))) {
        client.closeDevice(opened.leaseToken);
        return 223;
    }
    if (remoteFrame) configuredPayloadLength = 0u;

    const bool extendedFrame = environmentSwitch("UCAL_HARNESS_EXTENDED_FRAME", false);
    unsigned long configuredTxFrameId = extendedFrame ? 0x18daf110ul : 0x5a5ul;
    environmentUnsigned("UCAL_HARNESS_TX_FRAME_ID", configuredTxFrameId);
    unsigned long configuredRxFrameId = configuredTxFrameId;
    environmentUnsigned("UCAL_HARNESS_RX_FRAME_ID", configuredRxFrameId);
    const auto maximumFrameId = extendedFrame ? 0x1ffffffful : 0x7fful;
    if (configuredTxFrameId > maximumFrameId ||
        configuredRxFrameId > maximumFrameId) {
        client.closeDevice(opened.leaseToken);
        return 227;
    }
    const auto txFrameId = static_cast<std::uint32_t>(configuredTxFrameId);
    const auto rxFrameId = static_cast<std::uint32_t>(configuredRxFrameId);
    constexpr std::uint32_t firstTransactionId = 0x5e1fu;
    std::vector<Ucal::CanFrame> frames(frameCount);
    for (std::size_t sequence = 0; sequence < frameCount; ++sequence) {
        auto& frame = frames[sequence];
        frame.id = txFrameId;
        frame.transactionId = firstTransactionId + static_cast<std::uint32_t>(sequence);
        if (extendedFrame) frame.flags |= Ucal::FrameExtended;
        if (remoteFrame) {
            frame.flags |= Ucal::FrameRemote;
            frame.dlc = static_cast<std::uint8_t>(configuredDlc);
            frame.dataLength = 0u;
        } else if (config.canFd) {
            frame.flags |= Ucal::FrameFd | Ucal::FrameBrs;
            frame.dlc = static_cast<std::uint8_t>(configuredDlc);
            frame.dataLength = static_cast<std::uint8_t>(configuredPayloadLength);
        } else {
            frame.dlc = static_cast<std::uint8_t>(configuredDlc);
            frame.dataLength = static_cast<std::uint8_t>(configuredPayloadLength);
        }
        if (!remoteFrame) {
            const auto wireSequence = static_cast<std::uint32_t>(sequence);
            frame.data[0] = static_cast<std::uint8_t>(wireSequence & 0xffu);
            frame.data[1] = static_cast<std::uint8_t>((wireSequence >> 8u) & 0xffu);
            frame.data[2] = static_cast<std::uint8_t>((wireSequence >> 16u) & 0xffu);
            frame.data[3] = static_cast<std::uint8_t>((wireSequence >> 24u) & 0xffu);
            for (std::uint8_t index = 4u; index < frame.dataLength; ++index) {
                frame.data[index] = static_cast<std::uint8_t>(
                    0x80u + ((sequence + index) & 0x3fu));
            }
        }
    }

    unsigned long startDelayMs = 0u;
    environmentUnsigned("UCAL_HARNESS_START_DELAY_MS", startDelayMs);
    startDelayMs = (std::min)(startDelayMs, 60000ul);
    if (startDelayMs != 0u) {
        std::this_thread::sleep_for(std::chrono::milliseconds(startDelayMs));
    }

    using SteadyClock = std::chrono::steady_clock;
    std::vector<SteadyClock::time_point> submittedAt(frameCount);
    std::size_t submittedCount = 0u;
    const auto observationStarted = SteadyClock::now();
    std::vector<std::uint8_t> receivedFrames(frameCount, 0u);
    std::vector<std::uint8_t> selfReceivedFrames(frameCount, 0u);
    std::vector<std::uint8_t> completedFrames(frameCount, 0u);
    std::vector<std::uint64_t> latencyUs;
    latencyUs.reserve(frameCount);
    std::uint64_t latencyPeakUs = 0u;
    std::size_t latencyPeakSequence = 0u;
    std::vector<std::uint64_t> submitCallUs;
    submitCallUs.reserve(frameCount);
    std::uint64_t submitPeakUs = 0u;
    std::size_t submitPeakSequence = 0u;
    std::vector<std::uint64_t> pollCallUs;
    pollCallUs.reserve(frameCount * 2u);
    std::uint64_t pollPeakUs = 0u;
    std::size_t pollPeakIndex = 0u;
    std::size_t emptyPollCount = 0u;
    std::size_t receivedCount = 0u;
    std::size_t selfReceivedCount = 0u;
    std::size_t txCompletedCount = 0u;
    std::size_t timestampedReceivedCount = 0u;
    std::size_t timestampedCompletionCount = 0u;
    std::size_t duplicateCount = 0u;
    std::size_t unexpectedCount = 0u;
    std::size_t outOfOrderCount = 0u;
    std::size_t nextReceivedSequence = 0u;
    unsigned long timeoutMs = 3000;
    environmentUnsigned("UCAL_HARNESS_TIMEOUT_MS", timeoutMs);
    timeoutMs = (std::max)(100ul, (std::min)(timeoutMs, 60000ul));
    unsigned long emptyPollDelayUs = 1000u;
    environmentUnsigned("UCAL_HARNESS_EMPTY_POLL_DELAY_US", emptyPollDelayUs);
    emptyPollDelayUs = (std::min)(emptyPollDelayUs, 1000000ul);
    const auto evidenceComplete = [&]() {
        if (expectNoReceive) return false;
        return (!requireReceived || receivedCount == frameCount) &&
               (!requireSelfReceived || selfReceivedCount == frameCount) &&
               (!requireTxCompletion || txCompletedCount == frameCount) &&
               (!requireTimestamp ||
                ((!requireReceived || timestampedReceivedCount == frameCount) &&
                 (!requireTxCompletion || timestampedCompletionCount == frameCount)));
    };
    const auto consumeEvents = [&]() {
        std::vector<Ucal::CanEvent> events;
        const auto pollStarted = SteadyClock::now();
        const auto readStatus = client.readCanEvents(opened.leaseToken, 64u, events);
        const auto pollDurationUs = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                SteadyClock::now() - pollStarted).count());
        pollCallUs.push_back(pollDurationUs);
        if (pollDurationUs > pollPeakUs) {
            pollPeakUs = pollDurationUs;
            pollPeakIndex = pollCallUs.size() - 1u;
        }
        if (readStatus != Ucal::Status::Ok) {
            result = 203 + static_cast<int>(readStatus);
            return;
        }
        if (events.empty()) ++emptyPollCount;
        for (const auto& event : events) {
            if (event.kind == Ucal::CanEventKind::TransmitFailed &&
                event.frame.transactionId >= firstTransactionId &&
                event.frame.transactionId < firstTransactionId + frameCount) {
                result = 212;
                return;
            }
            if (event.kind == Ucal::CanEventKind::TransmitCompleted) {
                if (event.frame.transactionId < firstTransactionId ||
                    event.frame.transactionId >= firstTransactionId + frameCount) {
                    continue;
                }
                const auto sequence = static_cast<std::size_t>(
                    event.frame.transactionId - firstTransactionId);
                if (completedFrames[sequence] != 0u) {
                    ++duplicateCount;
                    continue;
                }
                completedFrames[sequence] = 1u;
                ++txCompletedCount;
                if (event.frame.timestampUs != 0u) ++timestampedCompletionCount;
                if (submitFrame && requireTxCompletion) {
                    const auto deliveryLatencyUs = static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::microseconds>(
                            SteadyClock::now() - submittedAt[sequence]).count());
                    latencyUs.push_back(deliveryLatencyUs);
                    if (deliveryLatencyUs > latencyPeakUs) {
                        latencyPeakUs = deliveryLatencyUs;
                        latencyPeakSequence = sequence;
                    }
                }
                continue;
            }
            if (event.kind != Ucal::CanEventKind::Received || event.frame.id != rxFrameId) {
                continue;
            }
            if (!remoteFrame && event.frame.dataLength < 4u && frameCount != 1u) {
                ++unexpectedCount;
                continue;
            }
            const auto wireSequence = remoteFrame || event.frame.dataLength < 4u ? 0u :
                static_cast<std::uint32_t>(event.frame.data[0]) |
                (static_cast<std::uint32_t>(event.frame.data[1]) << 8u) |
                (static_cast<std::uint32_t>(event.frame.data[2]) << 16u) |
                (static_cast<std::uint32_t>(event.frame.data[3]) << 24u);
            if (wireSequence >= frameCount) {
                ++unexpectedCount;
                continue;
            }
            const auto sequence = static_cast<std::size_t>(wireSequence);
            const auto& expected = frames[sequence];
            if (event.frame.dlc != expected.dlc ||
                event.frame.dataLength != expected.dataLength ||
                (event.frame.flags & (Ucal::FrameExtended | Ucal::FrameRemote |
                                      Ucal::FrameFd | Ucal::FrameBrs)) != expected.flags ||
                !std::equal(expected.data.begin(), expected.data.begin() + expected.dataLength,
                            event.frame.data.begin())) {
                ++unexpectedCount;
                continue;
            }
            if (receivedFrames[sequence] != 0u) {
                ++duplicateCount;
                continue;
            }
            if (sequence != nextReceivedSequence) ++outOfOrderCount;
            nextReceivedSequence = sequence + 1u;
            receivedFrames[sequence] = 1u;
            ++receivedCount;
            if ((event.frame.flags & Ucal::FrameSelfReceived) != 0u) {
                selfReceivedFrames[sequence] = 1u;
                ++selfReceivedCount;
            }
            if (event.frame.timestampUs != 0u) ++timestampedReceivedCount;
            if (submitFrame && requireReceived && !requireTxCompletion &&
                measureReceiveLatency) {
                const auto deliveryLatencyUs = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        SteadyClock::now() - submittedAt[sequence]).count());
                latencyUs.push_back(deliveryLatencyUs);
                if (deliveryLatencyUs > latencyPeakUs) {
                    latencyPeakUs = deliveryLatencyUs;
                    latencyPeakSequence = sequence;
                }
            }
        }
    };
    for (std::size_t sequence = 0; result == 0 && submitFrame && sequence < frameCount;
         ++sequence) {
        submittedAt[sequence] = SteadyClock::now();
        if (client.submitCan(opened.leaseToken, frames[sequence]) != Ucal::Status::Ok) {
            result = 202;
            break;
        }
        const auto submitDurationUs = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                SteadyClock::now() - submittedAt[sequence]).count());
        submitCallUs.push_back(submitDurationUs);
        if (submitDurationUs > submitPeakUs) {
            submitPeakUs = submitDurationUs;
            submitPeakSequence = sequence;
        }
        ++submittedCount;
        // Drain the event plane while producing traffic. Delaying all reads until the
        // final submission would measure an artificial harness backlog instead of the
        // host-observed delivery latency seen by a normally polling application.
        if ((sequence + 1u) % pollEveryFrames == 0u) consumeEvents();
        if (interFrameDelayUs != 0u && sequence + 1u < frameCount) {
            std::this_thread::sleep_for(std::chrono::microseconds(interFrameDelayUs));
        }
    }
    const auto deadline = SteadyClock::now() +
                          std::chrono::milliseconds(timeoutMs);
    while (result == 0 && !evidenceComplete() &&
           SteadyClock::now() < deadline) {
        consumeEvents();
        if (!evidenceComplete() && result == 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(emptyPollDelayUs));
        }
    }
    const auto observationCompleted = SteadyClock::now();
    // A queue-only transmitter may still have messages pending in the vendor
    // runtime when submission finishes. Keep its lease alive for an explicitly
    // bounded observation window; only the independent peer proves delivery.
    unsigned long postSubmitHoldMs = 0u;
    environmentUnsigned("UCAL_HARNESS_POST_SUBMIT_HOLD_MS", postSubmitHoldMs);
    postSubmitHoldMs = (std::min)(postSubmitHoldMs, 60000ul);
    if (allowSubmitOnly && submittedCount != 0u && postSubmitHoldMs != 0u) {
        std::this_thread::sleep_for(std::chrono::milliseconds(postSubmitHoldMs));
    }
    if (result == 0 && requireReceived && receivedCount != frameCount) result = 213;
    if (result == 0 && requireSelfReceived && selfReceivedCount != frameCount) result = 214;
    if (result == 0 && requireTimestamp &&
        ((requireReceived && timestampedReceivedCount != frameCount) ||
         (requireTxCompletion && timestampedCompletionCount != frameCount))) result = 215;
    if (result == 0 && requireTxCompletion && txCompletedCount != frameCount) result = 217;
    if (result == 0 && !allowDuplicates && duplicateCount != 0u) result = 219;
    if (result == 0 && unexpectedCount != 0u) result = 220;
    if (result == 0 && expectNoReceive && receivedCount != 0u) result = 222;
    if (result == 0 && requireReceived && outOfOrderCount != 0u) result = 224;

    std::sort(latencyUs.begin(), latencyUs.end());
    std::sort(submitCallUs.begin(), submitCallUs.end());
    std::sort(pollCallUs.begin(), pollCallUs.end());
    const auto percentile = [](const std::vector<std::uint64_t>& values,
                               const std::size_t numerator,
                               const std::size_t denominator) -> std::uint64_t {
        if (values.empty()) return 0u;
        const auto rank = (values.size() * numerator + denominator - 1u) / denominator;
        return values[(std::max)(std::size_t{1u}, rank) - 1u];
    };
    const auto latencyP50Us = percentile(latencyUs, 50u, 100u);
    const auto latencyP95Us = percentile(latencyUs, 95u, 100u);
    const auto latencyP99Us = percentile(latencyUs, 99u, 100u);
    const auto latencyP999Us = percentile(latencyUs, 999u, 1000u);
    const auto latencyMaxUs = latencyUs.empty() ? 0u : latencyUs.back();
    const auto submitP50Us = percentile(submitCallUs, 50u, 100u);
    const auto submitP95Us = percentile(submitCallUs, 95u, 100u);
    const auto submitP99Us = percentile(submitCallUs, 99u, 100u);
    const auto submitP999Us = percentile(submitCallUs, 999u, 1000u);
    const auto submitMaxUs = submitCallUs.empty() ? 0u : submitCallUs.back();
    const auto pollP50Us = percentile(pollCallUs, 50u, 100u);
    const auto pollP95Us = percentile(pollCallUs, 95u, 100u);
    const auto pollP99Us = percentile(pollCallUs, 99u, 100u);
    const auto pollP999Us = percentile(pollCallUs, 999u, 1000u);
    const auto pollMaxUs = pollCallUs.empty() ? 0u : pollCallUs.back();
    if (result == 0 && maximumP95LatencyUs != 0u &&
        latencyP95Us > maximumP95LatencyUs) result = 221;
    if (result == 0 && maximumP99LatencyUs != 0u &&
        latencyP99Us > maximumP99LatencyUs) result = 225;
    if (result == 0 && maximumP999LatencyUs != 0u &&
        latencyP999Us > maximumP999LatencyUs) result = 226;
    const auto durationUs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            observationCompleted - observationStarted).count());
    const auto framesPerSecond = durationUs == 0u ? 0.0 :
        static_cast<double>(frameCount) * 1000000.0 / static_cast<double>(durationUs);
    Ucal::ChannelStatus finalChannelStatus{};
    const auto finalStatusRead = client.getChannelStatus(
        opened.leaseToken, finalChannelStatus);
    if (result == 0 && finalStatusRead != Ucal::Status::Ok) result = 240;
    const auto closeStatus = client.closeDevice(opened.leaseToken);
    if (result == 0 && closeStatus != Ucal::Status::Ok) result = 216;
    std::cout << "{\"canHardwareSmoke\":" << (result == 0 ? "true" : "false")
              << ",\"statusRead\":" << (statusRead == Ucal::Status::Ok ? "true" : "false")
              << ",\"busStateValid\":" << (channelStatus.busStateValid ? "true" : "false")
              << ",\"errorCountersValid\":" << (channelStatus.errorCountersValid ? "true" : "false")
              << ",\"busOff\":" << (channelStatus.busOff ? "true" : "false")
              << ",\"errorPassive\":" << (channelStatus.errorPassive ? "true" : "false")
              << ",\"finalStatusRead\":" << (finalStatusRead == Ucal::Status::Ok ? "true" : "false")
              << ",\"finalBusStateValid\":" << (finalChannelStatus.busStateValid ? "true" : "false")
              << ",\"finalErrorCountersValid\":" << (finalChannelStatus.errorCountersValid ? "true" : "false")
              << ",\"finalBusOff\":" << (finalChannelStatus.busOff ? "true" : "false")
              << ",\"finalErrorPassive\":" << (finalChannelStatus.errorPassive ? "true" : "false")
              << ",\"finalWarning\":" << (finalChannelStatus.warning ? "true" : "false")
              << ",\"finalTxErrorCounter\":" << static_cast<unsigned>(finalChannelStatus.txErrorCounter)
              << ",\"finalRxErrorCounter\":" << static_cast<unsigned>(finalChannelStatus.rxErrorCounter)
              << ",\"frameCount\":" << frameCount
              << ",\"payloadLength\":" << configuredPayloadLength
              << ",\"dlc\":" << configuredDlc
              << ",\"txFrameId\":" << txFrameId
              << ",\"rxFrameId\":" << rxFrameId
              << ",\"submittedCount\":" << submittedCount
              << ",\"receivedCount\":" << receivedCount
              << ",\"selfReceivedCount\":" << selfReceivedCount
              << ",\"txCompletedCount\":" << txCompletedCount
              << ",\"duplicateCount\":" << duplicateCount
              << ",\"unexpectedCount\":" << unexpectedCount
              << ",\"outOfOrderCount\":" << outOfOrderCount
              << ",\"received\":" << (receivedCount == frameCount ? "true" : "false")
              << ",\"selfReceived\":" << (selfReceivedCount == frameCount ? "true" : "false")
              << ",\"txCompleted\":" << (txCompletedCount == frameCount ? "true" : "false")
              << ",\"timestamped\":"
              << ((!requireTimestamp ||
                   ((!requireReceived || timestampedReceivedCount == frameCount) &&
                    (!requireTxCompletion || timestampedCompletionCount == frameCount))) ?
                      "true" : "false")
              << ",\"latencySampleCount\":" << latencyUs.size()
              << ",\"latencyP50Us\":" << latencyP50Us
              << ",\"latencyP95Us\":" << latencyP95Us
              << ",\"latencyP99Us\":" << latencyP99Us
              << ",\"latencyP999Us\":" << latencyP999Us
              << ",\"latencyMaxUs\":" << latencyMaxUs
              << ",\"latencyMaxSequence\":" << latencyPeakSequence
              << ",\"submitCallSampleCount\":" << submitCallUs.size()
              << ",\"submitCallP50Us\":" << submitP50Us
              << ",\"submitCallP95Us\":" << submitP95Us
              << ",\"submitCallP99Us\":" << submitP99Us
              << ",\"submitCallP999Us\":" << submitP999Us
              << ",\"submitCallMaxUs\":" << submitMaxUs
              << ",\"submitCallMaxSequence\":" << submitPeakSequence
              << ",\"pollCallSampleCount\":" << pollCallUs.size()
              << ",\"pollCallP50Us\":" << pollP50Us
              << ",\"pollCallP95Us\":" << pollP95Us
              << ",\"pollCallP99Us\":" << pollP99Us
              << ",\"pollCallP999Us\":" << pollP999Us
              << ",\"pollCallMaxUs\":" << pollMaxUs
              << ",\"pollCallMaxIndex\":" << pollPeakIndex
              << ",\"emptyPollCount\":" << emptyPollCount
              << ",\"emptyPollDelayUs\":" << emptyPollDelayUs
              << ",\"maximumP95LatencyUs\":" << maximumP95LatencyUs
              << ",\"maximumP99LatencyUs\":" << maximumP99LatencyUs
              << ",\"maximumP999LatencyUs\":" << maximumP999LatencyUs
              << ",\"pollEveryFrames\":" << pollEveryFrames
              << ",\"startDelayMs\":" << startDelayMs
              << ",\"postSubmitHoldMs\":" << postSubmitHoldMs
              << ",\"durationUs\":" << durationUs
              << ",\"framesPerSecond\":" << framesPerSecond
              << ",\"requiredReceived\":" << (requireReceived ? "true" : "false")
              << ",\"requiredSelfReceived\":" << (requireSelfReceived ? "true" : "false")
              << ",\"requiredTxCompletion\":" << (requireTxCompletion ? "true" : "false")
              << ",\"requiredTimestamp\":" << (requireTimestamp ? "true" : "false")
              << ",\"expectedNoReceive\":" << (expectNoReceive ? "true" : "false")
              << ",\"allowSubmitOnly\":" << (allowSubmitOnly ? "true" : "false")
              << ",\"allowDuplicates\":" << (allowDuplicates ? "true" : "false")
              << ",\"receiveLatencyMeasured\":"
              << (measureReceiveLatency ? "true" : "false")
              << ",\"extendedFrame\":" << (extendedFrame ? "true" : "false")
              << ",\"remoteFrame\":" << (remoteFrame ? "true" : "false")
              << ",\"submitted\":" << (submitFrame ? "true" : "false")
              << ",\"closed\":" << (closeStatus == Ucal::Status::Ok ? "true" : "false")
              << ",\"result\":" << result << "}" << std::endl;
    return result;
}

int validateCanSelfReceive(Ucal::Client& client) {
    return validateCanHardwareSmoke(client, true);
}

int validateCanHardwareSoak(Ucal::Client& client) {
    std::vector<Ucal::EndpointDescriptor> endpoints;
    if (client.enumerateEndpoints(endpoints) != Ucal::Status::Ok) return 230;
    const auto requestedEndpoint = environmentString("UCAL_HARNESS_ENDPOINT_ID");
    const auto selected = std::find_if(endpoints.cbegin(), endpoints.cend(),
                                       [&](const auto& endpoint) {
        return endpoint.busType == Ucal::BusType::Can &&
               Ucal::deviceIdToString(endpoint.id) == requestedEndpoint;
    });
    if (selected == endpoints.cend() || selected->state != Ucal::DeviceState::Available) {
        return 231;
    }
    const auto requiredCapabilities = Ucal::CapabilityCanFdIso | Ucal::CapabilityBrs |
        Ucal::CapabilityTxCompletion | Ucal::CapabilityInternalLoopback;
    if ((selected->capabilities & requiredCapabilities) != requiredCapabilities) return 232;

    Ucal::CanChannelConfig config{};
    config.canFd = true;
    config.internalLoopback = true;
    applyCanEnvironment(config);
    if (!config.canFd || !config.internalLoopback || config.listenOnly || config.noAck) {
        return 233;
    }
    const auto opened = client.openCanChannel(selected->id, config);
    std::cout << "{\"canHardwareSoakOpen\":true,\"endpoint\":\""
              << Ucal::deviceIdToString(selected->id) << "\",\"status\":"
              << static_cast<int>(opened.status) << ",\"capabilities\":"
              << selected->capabilities << "}" << std::endl;
    if (opened.status != Ucal::Status::Ok || opened.leaseToken == 0u) return 234;

    unsigned long durationSeconds = 10u;
    environmentUnsigned("UCAL_HARNESS_SOAK_SECONDS", durationSeconds);
    durationSeconds = (std::max)(1ul, (std::min)(durationSeconds, 3600ul));
    unsigned long targetFps = 15250u;
    environmentUnsigned("UCAL_HARNESS_TARGET_FPS", targetFps);
    targetFps = (std::max)(1ul, (std::min)(targetFps, 100000ul));
    unsigned long minimumFps = 15000u;
    environmentUnsigned("UCAL_HARNESS_MINIMUM_FPS", minimumFps);
    minimumFps = (std::max)(1ul, (std::min)(minimumFps, targetFps));
    unsigned long configuredBatchSize = 256u;
    environmentUnsigned("UCAL_HARNESS_BATCH_SIZE", configuredBatchSize);
    const auto batchSize = static_cast<std::size_t>(
        (std::max)(1ul, (std::min)(configuredBatchSize, 512ul)));
    unsigned long progressSeconds = 300u;
    environmentUnsigned("UCAL_HARNESS_PROGRESS_SECONDS", progressSeconds);
    progressSeconds = (std::max)(1ul, (std::min)(progressSeconds, durationSeconds));

    constexpr std::uint32_t firstTransactionId = 0x00100000u;
    constexpr std::uint32_t frameId = 0x5a5u;
    constexpr std::uint64_t maximumOutstanding = 2048u;
    using SteadyClock = std::chrono::steady_clock;
    const auto started = SteadyClock::now();
    const auto trafficDeadline = started + std::chrono::seconds(durationSeconds);
    auto nextProgress = started + std::chrono::seconds(progressSeconds);
    std::uint64_t submitted = 0u;
    std::uint64_t received = 0u;
    std::uint64_t completed = 0u;
    std::uint64_t nextReceived = 0u;
    std::uint64_t nextCompleted = 0u;
    std::uint64_t busyCount = 0u;
    std::uint64_t emptyPollCount = 0u;
    std::uint64_t queueOverruns = 0u;
    std::uint64_t failedTransmissions = 0u;
    std::uint64_t invalidEvents = 0u;
    bool receivedTimestamped = true;
    bool completionTimestamped = true;
    int result = 0;

    const auto validSoakFrame = [&](const Ucal::CanFrame& frame,
                                      const std::uint64_t sequence,
                                      const std::uint32_t requiredFlags) {
        const auto protocolFlags = frame.flags &
            (Ucal::FrameExtended | Ucal::FrameRemote | Ucal::FrameFd |
             Ucal::FrameBrs | Ucal::FrameEsi | Ucal::FrameTxCompleted |
             Ucal::FrameTxFailed | Ucal::FrameSelfReceived);
        return frame.id == frameId && frame.dlc == 8u && frame.dataLength == 8u &&
               protocolFlags == requiredFlags &&
               frame.data[0] == static_cast<std::uint8_t>(sequence & 0xffu) &&
               frame.data[1] == static_cast<std::uint8_t>((sequence >> 8u) & 0xffu) &&
               frame.data[2] == static_cast<std::uint8_t>((sequence >> 16u) & 0xffu) &&
               frame.data[3] == static_cast<std::uint8_t>((sequence >> 24u) & 0xffu) &&
               frame.data[4] == 0xa5u && frame.data[5] == 0x5au &&
               frame.data[6] == static_cast<std::uint8_t>(sequence ^ 0x3cu) &&
               frame.data[7] == static_cast<std::uint8_t>((sequence >> 8u) ^ 0xc3u);
    };

    const auto consumeEvents = [&]() {
        std::vector<Ucal::CanEvent> events;
        const auto status = client.readCanEvents(opened.leaseToken, 256u, events);
        if (status != Ucal::Status::Ok) {
            result = 235 + static_cast<int>(status);
            return false;
        }
        if (events.empty()) {
            ++emptyPollCount;
            return false;
        }
        for (const auto& event : events) {
            if (event.kind == Ucal::CanEventKind::QueueOverrun) {
                queueOverruns += (std::max)(1u, event.backendStatus);
                result = 244;
                continue;
            }
            if (event.kind == Ucal::CanEventKind::TransmitFailed) {
                ++failedTransmissions;
                result = 245;
                continue;
            }
            if (event.kind == Ucal::CanEventKind::TransmitCompleted) {
                if (event.frame.transactionId < firstTransactionId) {
                    ++invalidEvents;
                    result = 246;
                    continue;
                }
                const auto sequence = static_cast<std::uint64_t>(
                    event.frame.transactionId - firstTransactionId);
                const auto expectedFlags = Ucal::FrameFd | Ucal::FrameBrs |
                                            Ucal::FrameTxCompleted;
                if (sequence != nextCompleted || sequence >= submitted ||
                    !validSoakFrame(event.frame, sequence, expectedFlags)) {
                    ++invalidEvents;
                    result = 247;
                    continue;
                }
                completionTimestamped = completionTimestamped &&
                                           event.frame.timestampUs != 0u;
                ++nextCompleted;
                ++completed;
                continue;
            }
            if (event.kind != Ucal::CanEventKind::Received ||
                event.frame.dataLength < 4u) {
                continue;
            }
            const auto sequence = static_cast<std::uint64_t>(event.frame.data[0]) |
                (static_cast<std::uint64_t>(event.frame.data[1]) << 8u) |
                (static_cast<std::uint64_t>(event.frame.data[2]) << 16u) |
                (static_cast<std::uint64_t>(event.frame.data[3]) << 24u);
            const auto expectedFlags = Ucal::FrameFd | Ucal::FrameBrs |
                                        Ucal::FrameSelfReceived;
            if (sequence != nextReceived || sequence >= submitted ||
                !validSoakFrame(event.frame, sequence, expectedFlags)) {
                ++invalidEvents;
                result = 248;
                continue;
            }
            receivedTimestamped = receivedTimestamped &&
                                   event.frame.timestampUs != 0u;
            ++nextReceived;
            ++received;
        }
        return true;
    };

    while (result == 0 && SteadyClock::now() < trafficDeadline) {
        const auto now = SteadyClock::now();
        const auto elapsedUs = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(now - started).count());
        const auto allowed = elapsedUs * static_cast<std::uint64_t>(targetFps) / 1000000u +
                             static_cast<std::uint64_t>(batchSize);
        const auto evidenced = (std::min)(received, completed);
        const auto outstanding = submitted - evidenced;
        const auto sendable = submitted < allowed && outstanding < maximumOutstanding
            ? (std::min)(allowed - submitted, maximumOutstanding - outstanding)
            : 0u;
        if (sendable >= batchSize) {
            // Pacing must preserve the transport batch instead of submitting every
            // few newly accrued frames. Small batches multiply IPC connections and
            // BM_WriteMultiple calls without adding useful load. The one-batch lead
            // in `allowed` keeps a complete batch ready at t=0 and thereafter at the
            // configured average frame rate.
            const auto count = batchSize;
            std::vector<Ucal::CanFrame> frames(count);
            for (std::size_t index = 0; index < count; ++index) {
                const auto sequence = submitted + index;
                auto& frame = frames[index];
                frame.id = frameId;
                frame.flags = Ucal::FrameFd | Ucal::FrameBrs;
                frame.dlc = 8u;
                frame.dataLength = 8u;
                frame.transactionId = firstTransactionId +
                    static_cast<std::uint32_t>(sequence);
                frame.data[0] = static_cast<std::uint8_t>(sequence & 0xffu);
                frame.data[1] = static_cast<std::uint8_t>((sequence >> 8u) & 0xffu);
                frame.data[2] = static_cast<std::uint8_t>((sequence >> 16u) & 0xffu);
                frame.data[3] = static_cast<std::uint8_t>((sequence >> 24u) & 0xffu);
                frame.data[4] = 0xa5u;
                frame.data[5] = 0x5au;
                frame.data[6] = static_cast<std::uint8_t>(sequence ^ 0x3cu);
                frame.data[7] = static_cast<std::uint8_t>((sequence >> 8u) ^ 0xc3u);
            }
            std::uint32_t accepted = 0u;
            const auto submitStatus = client.submitCanBatch(opened.leaseToken, frames,
                                                                accepted);
            submitted += accepted;
            if (submitStatus == Ucal::Status::Busy) {
                ++busyCount;
            } else if (submitStatus != Ucal::Status::Ok) {
                result = 249 + static_cast<int>(submitStatus);
            }
        }
        const auto consumed = consumeEvents();
        if (now >= nextProgress) {
            const auto seconds = std::chrono::duration<double>(now - started).count();
            std::cout << "{\"canHardwareSoakProgress\":true,\"elapsedSeconds\":"
                      << seconds << ",\"submitted\":" << submitted
                      << ",\"received\":" << received
                      << ",\"completed\":" << completed << "}" << std::endl;
            nextProgress += std::chrono::seconds(progressSeconds);
        }
        if (!consumed && sendable < batchSize) {
            std::this_thread::sleep_for(std::chrono::microseconds(250));
        }
    }
    const auto trafficFinished = SteadyClock::now();
    const auto drainDeadline = trafficFinished + std::chrono::seconds(15);
    while (result == 0 && (received < submitted || completed < submitted) &&
           SteadyClock::now() < drainDeadline) {
        if (!consumeEvents()) std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    if (result == 0 && (received != submitted || completed != submitted)) result = 260;

    bool finalFunction = false;
    if (result == 0) {
        Ucal::CanFrame finalFrame{};
        finalFrame.id = frameId;
        finalFrame.flags = Ucal::FrameFd | Ucal::FrameBrs;
        finalFrame.dlc = 8u;
        finalFrame.dataLength = 8u;
        finalFrame.transactionId = firstTransactionId + static_cast<std::uint32_t>(submitted);
        const auto sequence = submitted;
        finalFrame.data[0] = static_cast<std::uint8_t>(sequence & 0xffu);
        finalFrame.data[1] = static_cast<std::uint8_t>((sequence >> 8u) & 0xffu);
        finalFrame.data[2] = static_cast<std::uint8_t>((sequence >> 16u) & 0xffu);
        finalFrame.data[3] = static_cast<std::uint8_t>((sequence >> 24u) & 0xffu);
        finalFrame.data[4] = 0xa5u;
        finalFrame.data[5] = 0x5au;
        finalFrame.data[6] = static_cast<std::uint8_t>(sequence ^ 0x3cu);
        finalFrame.data[7] = static_cast<std::uint8_t>((sequence >> 8u) ^ 0xc3u);
        std::vector<Ucal::CanFrame> finalBatch{finalFrame};
        std::uint32_t accepted = 0u;
        const auto finalSubmit = client.submitCanBatch(opened.leaseToken, finalBatch,
                                                           accepted);
        if (finalSubmit != Ucal::Status::Ok || accepted != 1u) {
            result = 261;
        } else {
            ++submitted;
            const auto finalDeadline = SteadyClock::now() + std::chrono::seconds(5);
            while (result == 0 && (received < submitted || completed < submitted) &&
                   SteadyClock::now() < finalDeadline) {
                if (!consumeEvents()) std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
            finalFunction = received == submitted && completed == submitted;
            if (!finalFunction) result = 262;
        }
    }

    Ucal::ChannelStatus channelStatus{};
    const auto statusRead = client.getChannelStatus(opened.leaseToken, channelStatus);
    if (result == 0 && statusRead != Ucal::Status::Ok) result = 263;
    const auto closeStatus = client.closeDevice(opened.leaseToken);
    if (result == 0 && closeStatus != Ucal::Status::Ok) result = 264;
    const auto durationUs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(trafficFinished - started).count());
    const auto trafficFrames = submitted - (finalFunction ? 1u : 0u);
    const auto framesPerSecond = durationUs == 0u ? 0.0 :
        static_cast<double>(trafficFrames) * 1000000.0 /
        static_cast<double>(durationUs);
    if (result == 0 && framesPerSecond < static_cast<double>(minimumFps)) result = 265;
    if (result == 0 && (!receivedTimestamped || queueOverruns != 0u ||
                        failedTransmissions != 0u || invalidEvents != 0u)) result = 266;

    std::cout << "{\"canHardwareSoak\":" << (result == 0 ? "true" : "false")
              << ",\"endpoint\":\"" << Ucal::deviceIdToString(selected->id)
              << "\",\"durationSeconds\":" << durationSeconds
              << ",\"targetFramesPerSecond\":" << targetFps
              << ",\"minimumFramesPerSecond\":" << minimumFps
              << ",\"batchSize\":" << batchSize
              << ",\"trafficFrames\":" << trafficFrames
              << ",\"submitted\":" << submitted
              << ",\"received\":" << received
              << ",\"completed\":" << completed
              << ",\"framesPerSecond\":" << framesPerSecond
              << ",\"busyCount\":" << busyCount
              << ",\"emptyPollCount\":" << emptyPollCount
              << ",\"queueOverruns\":" << queueOverruns
              << ",\"failedTransmissions\":" << failedTransmissions
              << ",\"invalidEvents\":" << invalidEvents
              << ",\"receivedTimestamped\":"
              << (receivedTimestamped ? "true" : "false")
              << ",\"completionTimestamped\":"
              << (completionTimestamped ? "true" : "false")
              << ",\"finalFunction\":" << (finalFunction ? "true" : "false")
              << ",\"statusRead\":" << (statusRead == Ucal::Status::Ok ? "true" : "false")
              << ",\"busOff\":" << (channelStatus.busOff ? "true" : "false")
              << ",\"closed\":" << (closeStatus == Ucal::Status::Ok ? "true" : "false")
              << ",\"result\":" << result << "}" << std::endl;
    return result;
}

int validateCanPeerLatency(Ucal::Client& client) {
    std::vector<Ucal::EndpointDescriptor> endpoints;
    if (client.enumerateEndpoints(endpoints) != Ucal::Status::Ok) return 270;
    const auto endpointAText = environmentString("UCAL_HARNESS_ENDPOINT_ID");
    const auto endpointBText = environmentString("UCAL_HARNESS_PEER_ENDPOINT_ID");
    const auto findEndpoint = [&](const std::string& text) {
        return std::find_if(endpoints.cbegin(), endpoints.cend(), [&](const auto& endpoint) {
            return endpoint.busType == Ucal::BusType::Can &&
                   Ucal::deviceIdToString(endpoint.id) == text;
        });
    };
    const auto endpointA = findEndpoint(endpointAText);
    const auto endpointB = findEndpoint(endpointBText);
    if (endpointA == endpoints.cend() || endpointB == endpoints.cend() ||
        endpointA == endpointB || endpointA->state != Ucal::DeviceState::Available ||
        endpointB->state != Ucal::DeviceState::Available) {
        return 271;
    }

    Ucal::CanChannelConfig config{};
    config.canFd = environmentSwitch("UCAL_HARNESS_CAN_FD", false);
    applyCanEnvironment(config);
    if (config.listenOnly || config.noAck || config.internalLoopback) return 272;
    if (config.canFd) {
        const auto required = Ucal::CapabilityCanFdIso | Ucal::CapabilityBrs;
        if ((endpointA->capabilities & required) != required ||
            (endpointB->capabilities & required) != required) return 273;
    }

    const auto openedA = client.openCanChannel(endpointA->id, config);
    if (openedA.status != Ucal::Status::Ok || openedA.leaseToken == 0u) return 274;
    const auto openedB = client.openCanChannel(endpointB->id, config);
    if (openedB.status != Ucal::Status::Ok || openedB.leaseToken == 0u) {
        client.closeDevice(openedA.leaseToken);
        return 275;
    }

    unsigned long configuredFrameCount = 1000u;
    environmentUnsigned("UCAL_HARNESS_FRAME_COUNT", configuredFrameCount);
    const auto frameCount = static_cast<std::size_t>(
        (std::max)(1ul, (std::min)(configuredFrameCount, 100000ul)));
    unsigned long configuredPayloadLength = config.canFd ? 64u : 8u;
    environmentUnsigned("UCAL_HARNESS_PAYLOAD_LENGTH", configuredPayloadLength);
    if ((config.canFd && configuredPayloadLength != 64u) ||
        (!config.canFd && configuredPayloadLength != 8u)) {
        client.closeDevice(openedB.leaseToken);
        client.closeDevice(openedA.leaseToken);
        return 276;
    }
    const auto payloadLength = static_cast<std::uint8_t>(configuredPayloadLength);
    unsigned long timeoutMs = 1000u;
    environmentUnsigned("UCAL_HARNESS_TIMEOUT_MS", timeoutMs);
    timeoutMs = (std::max)(100ul, (std::min)(timeoutMs, 60000ul));
    unsigned long maximumP95LatencyUs = 0u;
    unsigned long maximumP99LatencyUs = 0u;
    unsigned long maximumP999LatencyUs = 0u;
    environmentUnsigned("UCAL_HARNESS_MAX_P95_LATENCY_US", maximumP95LatencyUs);
    environmentUnsigned("UCAL_HARNESS_MAX_P99_LATENCY_US", maximumP99LatencyUs);
    environmentUnsigned("UCAL_HARNESS_MAX_P999_LATENCY_US", maximumP999LatencyUs);

    using SteadyClock = std::chrono::steady_clock;
    std::vector<std::uint64_t> aToBUs;
    std::vector<std::uint64_t> bToAUs;
    std::vector<std::uint64_t> roundTripUs;
    aToBUs.reserve(frameCount);
    bToAUs.reserve(frameCount);
    roundTripUs.reserve(frameCount);
    std::size_t timestamped = 0u;
    std::size_t emptyPolls = 0u;
    int result = 0;

    const auto readSequence = [&](const std::uint64_t token,
                                   const std::uint32_t expectedId,
                                   const std::uint32_t expectedSequence) {
        const auto deadline = SteadyClock::now() + std::chrono::milliseconds(timeoutMs);
        while (SteadyClock::now() < deadline) {
            std::vector<Ucal::CanEvent> events;
            const auto readStatus = client.readCanEvents(token, 64u, events);
            if (readStatus != Ucal::Status::Ok) {
                result = 277 + static_cast<int>(readStatus);
                return false;
            }
            if (events.empty()) {
                ++emptyPolls;
                std::this_thread::sleep_for(std::chrono::microseconds(100));
                continue;
            }
            for (const auto& event : events) {
                if (event.kind != Ucal::CanEventKind::Received ||
                    event.frame.id != expectedId ||
                    event.frame.dataLength != payloadLength ||
                    event.frame.dlc != (config.canFd ? 15u : 8u)) {
                    continue;
                }
                const auto sequence = static_cast<std::uint32_t>(event.frame.data[0]) |
                    (static_cast<std::uint32_t>(event.frame.data[1]) << 8u) |
                    (static_cast<std::uint32_t>(event.frame.data[2]) << 16u) |
                    (static_cast<std::uint32_t>(event.frame.data[3]) << 24u);
                const auto requiredFlags = config.canFd ?
                    (Ucal::FrameFd | Ucal::FrameBrs) : 0u;
                const auto observedFlags = event.frame.flags &
                    (Ucal::FrameExtended | Ucal::FrameRemote | Ucal::FrameFd |
                     Ucal::FrameBrs | Ucal::FrameSelfReceived);
                if (sequence != expectedSequence || observedFlags != requiredFlags) {
                    result = 284;
                    return false;
                }
                if (event.frame.timestampUs == 0u) {
                    result = 285;
                    return false;
                }
                ++timestamped;
                return true;
            }
        }
        result = 286;
        return false;
    };

    for (std::size_t sequence = 0u; sequence < frameCount && result == 0; ++sequence) {
        Ucal::CanFrame request{};
        request.id = 0x5a5u;
        request.flags = config.canFd ? Ucal::FrameFd | Ucal::FrameBrs : 0u;
        request.dlc = config.canFd ? 15u : 8u;
        request.dataLength = payloadLength;
        request.transactionId = 0x100000u + static_cast<std::uint32_t>(sequence);
        const auto wireSequence = static_cast<std::uint32_t>(sequence);
        request.data[0] = static_cast<std::uint8_t>(wireSequence & 0xffu);
        request.data[1] = static_cast<std::uint8_t>((wireSequence >> 8u) & 0xffu);
        request.data[2] = static_cast<std::uint8_t>((wireSequence >> 16u) & 0xffu);
        request.data[3] = static_cast<std::uint8_t>((wireSequence >> 24u) & 0xffu);
        for (std::uint8_t index = 4u; index < request.dataLength; ++index) {
            request.data[index] = static_cast<std::uint8_t>(0x40u + ((sequence + index) & 0x3fu));
        }

        const auto roundTripStarted = SteadyClock::now();
        if (client.submitCan(openedA.leaseToken, request) != Ucal::Status::Ok) {
            result = 287;
            break;
        }
        if (!readSequence(openedB.leaseToken, request.id, wireSequence)) break;
        const auto receivedB = SteadyClock::now();
        aToBUs.push_back(static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                receivedB - roundTripStarted).count()));

        auto response = request;
        response.id = 0x5a6u;
        response.transactionId = 0x200000u + static_cast<std::uint32_t>(sequence);
        const auto responseStarted = SteadyClock::now();
        if (client.submitCan(openedB.leaseToken, response) != Ucal::Status::Ok) {
            result = 288;
            break;
        }
        if (!readSequence(openedA.leaseToken, response.id, wireSequence)) break;
        const auto receivedA = SteadyClock::now();
        bToAUs.push_back(static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                receivedA - responseStarted).count()));
        roundTripUs.push_back(static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                receivedA - roundTripStarted).count()));
    }

    const auto closeB = client.closeDevice(openedB.leaseToken);
    const auto closeA = client.closeDevice(openedA.leaseToken);
    if (result == 0 && (closeA != Ucal::Status::Ok || closeB != Ucal::Status::Ok)) result = 289;
    const auto percentile = [](std::vector<std::uint64_t> values,
                               const std::size_t numerator,
                               const std::size_t denominator) {
        if (values.empty()) return std::uint64_t{0u};
        std::sort(values.begin(), values.end());
        const auto rank = (values.size() * numerator + denominator - 1u) / denominator;
        return values[(std::max)(std::size_t{1u}, rank) - 1u];
    };
    const auto aP50 = percentile(aToBUs, 50u, 100u);
    const auto aP95 = percentile(aToBUs, 95u, 100u);
    const auto aP99 = percentile(aToBUs, 99u, 100u);
    const auto aP999 = percentile(aToBUs, 999u, 1000u);
    const auto bP50 = percentile(bToAUs, 50u, 100u);
    const auto bP95 = percentile(bToAUs, 95u, 100u);
    const auto bP99 = percentile(bToAUs, 99u, 100u);
    const auto bP999 = percentile(bToAUs, 999u, 1000u);
    const auto rttP50 = percentile(roundTripUs, 50u, 100u);
    const auto rttP95 = percentile(roundTripUs, 95u, 100u);
    const auto rttP99 = percentile(roundTripUs, 99u, 100u);
    const auto rttP999 = percentile(roundTripUs, 999u, 1000u);
    if (result == 0 && maximumP95LatencyUs != 0u &&
        (aP95 > maximumP95LatencyUs || bP95 > maximumP95LatencyUs)) result = 290;
    if (result == 0 && maximumP99LatencyUs != 0u &&
        (aP99 > maximumP99LatencyUs || bP99 > maximumP99LatencyUs)) result = 291;
    if (result == 0 && maximumP999LatencyUs != 0u &&
        (aP999 > maximumP999LatencyUs || bP999 > maximumP999LatencyUs)) result = 292;

    std::cout << "{\"canPeerLatency\":" << (result == 0 ? "true" : "false")
              << ",\"frameCount\":" << frameCount
              << ",\"completedCount\":" << roundTripUs.size()
              << ",\"timestampedReceiveCount\":" << timestamped
              << ",\"emptyPollCount\":" << emptyPolls
              << ",\"canFd\":" << (config.canFd ? "true" : "false")
              << ",\"payloadLength\":" << static_cast<unsigned>(payloadLength)
              << ",\"aToBUs\":{\"p50\":" << aP50 << ",\"p95\":" << aP95
              << ",\"p99\":" << aP99 << ",\"p999\":" << aP999 << "}"
              << ",\"bToAUs\":{\"p50\":" << bP50 << ",\"p95\":" << bP95
              << ",\"p99\":" << bP99 << ",\"p999\":" << bP999 << "}"
              << ",\"roundTripUs\":{\"p50\":" << rttP50 << ",\"p95\":" << rttP95
              << ",\"p99\":" << rttP99 << ",\"p999\":" << rttP999 << "}"
              << ",\"closed\":"
              << (closeA == Ucal::Status::Ok && closeB == Ucal::Status::Ok ? "true" : "false")
              << ",\"result\":" << result << "}" << std::endl;
    return result;
}

int validateCanPeerErrorRecovery(Ucal::Client& client) {
    std::vector<Ucal::EndpointDescriptor> endpoints;
    if (client.enumerateEndpoints(endpoints) != Ucal::Status::Ok) return 293;
    const auto endpointAText = environmentString("UCAL_HARNESS_ENDPOINT_ID");
    const auto endpointBText = environmentString("UCAL_HARNESS_PEER_ENDPOINT_ID");
    const auto findEndpoint = [&](const std::string& text) {
        return std::find_if(endpoints.cbegin(), endpoints.cend(), [&](const auto& endpoint) {
            return endpoint.busType == Ucal::BusType::Can &&
                   Ucal::deviceIdToString(endpoint.id) == text;
        });
    };
    const auto endpointA = findEndpoint(endpointAText);
    const auto endpointB = findEndpoint(endpointBText);
    if (endpointA == endpoints.cend() || endpointB == endpoints.cend() ||
        endpointA == endpointB || endpointA->state != Ucal::DeviceState::Available ||
        endpointB->state != Ucal::DeviceState::Available ||
        (endpointB->capabilities & Ucal::CapabilityListenOnly) == 0u) {
        return 294;
    }

    Ucal::CanChannelConfig normal{};
    applyCanEnvironment(normal);
    normal.canFd = false;
    normal.listenOnly = false;
    normal.noAck = false;
    normal.internalLoopback = false;
    Ucal::CanChannelConfig listener = normal;
    listener.listenOnly = true;
    const auto openedListener = client.openCanChannel(endpointB->id, listener);
    if (openedListener.status != Ucal::Status::Ok || openedListener.leaseToken == 0u) {
        return 295;
    }
    const auto openedTransmitter = client.openCanChannel(endpointA->id, normal);
    if (openedTransmitter.status != Ucal::Status::Ok ||
        openedTransmitter.leaseToken == 0u) {
        client.closeDevice(openedListener.leaseToken);
        return 296;
    }

    Ucal::CanFrame frame{};
    frame.id = 0x5a5u;
    frame.dlc = 8u;
    frame.dataLength = 8u;
    frame.transactionId = 0x330001u;
    for (std::uint8_t index = 0u; index < frame.dataLength; ++index) {
        frame.data[index] = static_cast<std::uint8_t>(0xa0u + index);
    }
    int result = 0;
    if (client.submitCan(openedTransmitter.leaseToken, frame) != Ucal::Status::Ok) {
        result = 297;
    }
    std::size_t listenerReceives = 0u;
    bool listenerTimestamped = true;
    bool busOff = false;
    bool errorPassiveObserved = false;
    bool warningObserved = false;
    bool faultStateObserved = false;
    Ucal::ChannelStatus faultStatus{};
    const auto faultDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (result == 0 && std::chrono::steady_clock::now() < faultDeadline &&
           (!faultStateObserved || listenerReceives == 0u)) {
        std::vector<Ucal::CanEvent> events;
        const auto readStatus = client.readCanEvents(openedListener.leaseToken, 512u, events);
        if (readStatus != Ucal::Status::Ok) {
            result = 298 + static_cast<int>(readStatus);
            break;
        }
        for (const auto& event : events) {
            if (event.kind == Ucal::CanEventKind::Received && event.frame.id == frame.id) {
                ++listenerReceives;
                listenerTimestamped = listenerTimestamped && event.frame.timestampUs != 0u;
            }
        }
        const auto status = client.getChannelStatus(openedTransmitter.leaseToken, faultStatus);
        if (status != Ucal::Status::Ok) {
            result = 305 + static_cast<int>(status);
            break;
        }
        busOff = busOff || faultStatus.busOff;
        errorPassiveObserved = errorPassiveObserved || faultStatus.errorPassive;
        warningObserved = warningObserved || faultStatus.warning;
        faultStateObserved = busOff || errorPassiveObserved || warningObserved;
        if (!faultStateObserved || listenerReceives == 0u) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    if (result == 0 && (listenerReceives == 0u || !listenerTimestamped)) result = 312;
    if (result == 0 && !faultStateObserved) result = 313;
    const auto closeFaultTx = client.closeDevice(openedTransmitter.leaseToken);
    const auto closeFaultListener = client.closeDevice(openedListener.leaseToken);
    if (result == 0 &&
        (closeFaultTx != Ucal::Status::Ok || closeFaultListener != Ucal::Status::Ok)) {
        result = 314;
    }

    bool recoveryReceived = false;
    bool recoveryTimestamped = false;
    bool recoveredNormal = false;
    Ucal::Status recoveryCloseA = Ucal::Status::NotFound;
    Ucal::Status recoveryCloseB = Ucal::Status::NotFound;
    if (result == 0) {
        const auto recoveryA = client.openCanChannel(endpointA->id, normal);
        const auto recoveryB = client.openCanChannel(endpointB->id, normal);
        if (recoveryA.status != Ucal::Status::Ok || recoveryA.leaseToken == 0u ||
            recoveryB.status != Ucal::Status::Ok || recoveryB.leaseToken == 0u) {
            if (recoveryB.leaseToken != 0u) client.closeDevice(recoveryB.leaseToken);
            if (recoveryA.leaseToken != 0u) client.closeDevice(recoveryA.leaseToken);
            result = 315;
        } else {
            frame.id = 0x5a6u;
            frame.transactionId = 0x330002u;
            if (client.submitCan(recoveryA.leaseToken, frame) != Ucal::Status::Ok) {
                result = 316;
            }
            const auto recoveryDeadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (result == 0 && !recoveryReceived &&
                   std::chrono::steady_clock::now() < recoveryDeadline) {
                std::vector<Ucal::CanEvent> events;
                const auto readStatus = client.readCanEvents(recoveryB.leaseToken, 64u, events);
                if (readStatus != Ucal::Status::Ok) {
                    result = 317 + static_cast<int>(readStatus);
                    break;
                }
                for (const auto& event : events) {
                    if (event.kind == Ucal::CanEventKind::Received &&
                        event.frame.id == frame.id && event.frame.dlc == frame.dlc &&
                        event.frame.dataLength == frame.dataLength &&
                        std::equal(frame.data.begin(), frame.data.begin() + frame.dataLength,
                                   event.frame.data.begin())) {
                        recoveryReceived = true;
                        recoveryTimestamped = event.frame.timestampUs != 0u;
                        break;
                    }
                }
                if (!recoveryReceived) std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
            Ucal::ChannelStatus recoveryStatus{};
            const auto recoveryStatusRead =
                client.getChannelStatus(recoveryA.leaseToken, recoveryStatus);
            recoveredNormal = recoveryStatusRead == Ucal::Status::Ok &&
                               recoveryStatus.busStateValid && !recoveryStatus.busOff &&
                               !recoveryStatus.errorPassive && !recoveryStatus.warning;
            recoveryCloseB = client.closeDevice(recoveryB.leaseToken);
            recoveryCloseA = client.closeDevice(recoveryA.leaseToken);
            if (result == 0 &&
                (!recoveryReceived || !recoveryTimestamped || !recoveredNormal ||
                 recoveryCloseA != Ucal::Status::Ok ||
                 recoveryCloseB != Ucal::Status::Ok)) {
                result = 324;
            }
        }
    }

    std::cout << "{\"canPeerErrorRecovery\":" << (result == 0 ? "true" : "false")
              << ",\"listenOnlyReceives\":" << listenerReceives
              << ",\"listenOnlyTimestamped\":"
              << (listenerTimestamped ? "true" : "false")
              << ",\"busOffObserved\":" << (busOff ? "true" : "false")
              << ",\"errorPassiveObserved\":"
              << (errorPassiveObserved ? "true" : "false")
              << ",\"warningObserved\":" << (warningObserved ? "true" : "false")
              << ",\"errorCountersValid\":"
              << (faultStatus.errorCountersValid ? "true" : "false")
              << ",\"recoveryReceived\":" << (recoveryReceived ? "true" : "false")
              << ",\"recoveryTimestamped\":"
              << (recoveryTimestamped ? "true" : "false")
              << ",\"recoveredNormal\":" << (recoveredNormal ? "true" : "false")
              << ",\"result\":" << result << "}" << std::endl;
    return result;
}

int validatePcanMock(Ucal::Client& client) {
    std::vector<Ucal::EndpointDescriptor> endpoints;
    if (client.enumerateEndpoints(endpoints) != Ucal::Status::Ok || endpoints.size() != 2u) {
        return 100;
    }
    const auto expected = Ucal::CapabilityClassicCan | Ucal::CapabilityCanFdIso |
                          Ucal::CapabilityBrs | Ucal::CapabilityListenOnly | Ucal::CapabilityCanAdvancedTiming;
    if (endpoints[0].capabilities != expected || endpoints[1].capabilities != expected ||
        endpoints[0].id == endpoints[1].id ||
        endpoints[0].adapterId != endpoints[1].adapterId ||
        endpoints[0].physicalIndex == endpoints[1].physicalIndex ||
        endpoints[0].state != Ucal::DeviceState::Available) return 101;

    Ucal::CanChannelConfig config{};
    config.arbitrationBitrate = 500000u;
    config.dataBitrate = 2000000u;
    config.arbitrationSamplePoint = 75u;
    config.dataSamplePoint = 75u;
    config.arbitrationSamplePointSpecified = true;
    config.dataSamplePointSpecified = true;
    config.canFd = true;
    config.terminationSpecified = false;
    const auto opened = client.openCanChannel(endpoints[0].id, config);
    if (opened.status != Ucal::Status::Ok || opened.leaseToken == 0u) return 102;
    Ucal::ChannelStatus channelStatus{};
    if (client.getChannelStatus(opened.leaseToken, channelStatus) != Ucal::Status::Ok ||
        !channelStatus.busStateValid || channelStatus.errorCountersValid ||
        channelStatus.busOff || channelStatus.errorPassive || channelStatus.warning) {
        return 103;
    }
    Ucal::CanFrame frame{};
    frame.id = 0x456u;
    frame.flags = Ucal::FrameFd | Ucal::FrameBrs;
    frame.dlc = 9u;
    frame.dataLength = 12u;
    frame.transactionId = 0x321u;
    for (std::uint8_t index = 0; index < frame.dataLength; ++index) frame.data[index] = index;
    if (client.submitCan(opened.leaseToken, frame) != Ucal::Status::Ok) return 104;
    std::vector<Ucal::CanEvent> events;
    if (client.readCanEvents(opened.leaseToken, 16u, events) != Ucal::Status::Ok) return 105;
    const auto received = std::find_if(events.begin(), events.end(), [&](const auto& event) {
        return event.kind == Ucal::CanEventKind::Received && event.frame.id == frame.id &&
               event.frame.timestampUs == 123456u && event.frame.dataLength == 12u &&
               (event.frame.flags & Ucal::FrameSelfReceived) != 0u;
    });
    const auto inventedCompletion = std::any_of(events.begin(), events.end(), [](const auto& event) {
        return event.kind == Ucal::CanEventKind::TransmitCompleted;
    });
    if (received == events.end() || inventedCompletion) return 106;
    if (client.closeDevice(opened.leaseToken) != Ucal::Status::Ok) return 107;

    config.arbitrationSamplePointSpecified = false;
    config.dataSamplePointSpecified = false;
    config.listenOnly = true;
    const auto listener = client.openCanChannel(endpoints[0].id, config);
    if (listener.status != Ucal::Status::Ok || listener.leaseToken == 0u) return 108;
    if (client.closeDevice(listener.leaseToken) != Ucal::Status::Ok) return 109;

    // The test runtime deliberately retains the preset across Uninitialize. A normal
    // reopen therefore proves that the backend explicitly writes listen-only off.
    config.canFd = false;
    config.listenOnly = false;
    const auto classic = client.openCanChannel(endpoints[0].id, config);
    if (classic.status != Ucal::Status::Ok || classic.leaseToken == 0u) return 112;
    Ucal::CanFrame remote{};
    remote.id = 0x321u;
    remote.flags = Ucal::FrameRemote;
    remote.dlc = 6u;
    remote.dataLength = 0u;
    remote.transactionId = 0x654u;
    if (client.submitCan(classic.leaseToken, remote) != Ucal::Status::Ok) return 114;
    events.clear();
    if (client.readCanEvents(classic.leaseToken, 16u, events) != Ucal::Status::Ok) return 115;
    const auto remoteReceived = std::find_if(events.begin(), events.end(), [&](const auto& event) {
        return event.kind == Ucal::CanEventKind::Received && event.frame.id == remote.id &&
               event.frame.dlc == remote.dlc && event.frame.dataLength == 0u &&
               (event.frame.flags & (Ucal::FrameRemote | Ucal::FrameSelfReceived)) ==
                   (Ucal::FrameRemote | Ucal::FrameSelfReceived) &&
               event.frame.timestampUs != 0u;
    });
    if (remoteReceived == events.end()) return 116;
    if (client.closeDevice(classic.leaseToken) != Ucal::Status::Ok) return 113;

    config.terminationSpecified = true;
    if (client.openCanChannel(endpoints[0].id, config).status != Ucal::Status::Unsupported) {
        return 110;
    }
    config.terminationSpecified = false;
    config.noAck = true;
    if (client.openCanChannel(endpoints[0].id, config).status != Ucal::Status::Unsupported) {
        return 111;
    }
    std::cout << "{\"pcanBackendMock\":true,\"statusValidity\":true,"
                 "\"selfReceiveEcho\":true,\"duplicateIdentityResolved\":true,"
                 "\"remoteFrameSemantics\":true,"
                 "\"noSyntheticCompletion\":true,\"unsupportedControls\":true}"
              << std::endl;
    return 0;
}

int validateVectorMock(Ucal::Client& client) {
    std::vector<Ucal::EndpointDescriptor> endpoints;
    if (client.enumerateEndpoints(endpoints) != Ucal::Status::Ok || endpoints.size() != 2u) {
        return 120;
    }
    const auto expected = Ucal::CapabilityClassicCan | Ucal::CapabilityCanFdIso |
                          Ucal::CapabilityBrs | Ucal::CapabilityListenOnly |
                          Ucal::CapabilityTxCompletion | Ucal::CapabilityCanAdvancedTiming;
    if (endpoints[0].capabilities != expected || endpoints[1].capabilities != expected ||
        endpoints[0].adapterId != endpoints[1].adapterId ||
        endpoints[0].state != Ucal::DeviceState::Available) return 121;

    Ucal::CanChannelConfig config{};
    config.arbitrationBitrate = 500000u;
    config.dataBitrate = 2000000u;
    config.arbitrationSamplePoint = 75u;
    config.dataSamplePoint = 75u;
    config.arbitrationSamplePointSpecified = true;
    config.dataSamplePointSpecified = true;
    config.canFd = true;
    config.terminationSpecified = false;
    const auto opened = client.openCanChannel(endpoints[0].id, config);
    if (opened.status != Ucal::Status::Ok || opened.leaseToken == 0u) return 122;
    if (client.openCanChannel(endpoints[0].id, config).status != Ucal::Status::Busy) return 123;

    Ucal::ChannelStatus channelStatus{};
    if (client.getChannelStatus(opened.leaseToken, channelStatus) != Ucal::Status::Ok ||
        !channelStatus.busStateValid || !channelStatus.errorCountersValid ||
        channelStatus.busOff || channelStatus.errorPassive || channelStatus.warning ||
        channelStatus.txErrorCounter != 3u || channelStatus.rxErrorCounter != 4u) {
        return 124;
    }

    Ucal::CanFrame frame{};
    frame.id = 0x18daf110u;
    frame.flags = Ucal::FrameExtended | Ucal::FrameFd | Ucal::FrameBrs;
    frame.dlc = 9u;
    frame.dataLength = 12u;
    frame.transactionId = 0x1234321u;
    for (std::uint8_t index = 0; index < frame.dataLength; ++index) {
        frame.data[index] = static_cast<std::uint8_t>(0xa0u + index);
    }
    if (client.submitCan(opened.leaseToken, frame) != Ucal::Status::Ok) return 125;
    std::vector<Ucal::CanEvent> events;
    if (client.readCanEvents(opened.leaseToken, 16u, events) != Ucal::Status::Ok) return 126;
    const auto received = std::find_if(events.begin(), events.end(), [&](const auto& event) {
        return event.kind == Ucal::CanEventKind::Received && event.frame.id == frame.id &&
               event.frame.dataLength == frame.dataLength &&
               event.frame.timestampUs != 0u &&
               (event.frame.flags & (Ucal::FrameExtended | Ucal::FrameFd | Ucal::FrameBrs)) ==
                   (Ucal::FrameExtended | Ucal::FrameFd | Ucal::FrameBrs) &&
               std::equal(frame.data.begin(), frame.data.begin() + frame.dataLength,
                          event.frame.data.begin());
    });
    const auto completed = std::find_if(events.begin(), events.end(), [&](const auto& event) {
        return event.kind == Ucal::CanEventKind::TransmitCompleted &&
               event.frame.transactionId == frame.transactionId &&
               event.frame.timestampUs != 0u &&
               (event.frame.flags & Ucal::FrameTxCompleted) != 0u;
    });
    if (received == events.end() || completed == events.end()) return 127;
    std::vector<Ucal::CanFrame> batch(4u, frame);
    for (std::size_t index = 0; index < batch.size(); ++index) {
        batch[index].transactionId = 0x1234400u + static_cast<std::uint32_t>(index);
        batch[index].data[0] = static_cast<std::uint8_t>(index);
    }
    std::uint32_t acceptedCount = 0u;
    if (client.submitCanBatch(opened.leaseToken, batch, acceptedCount) != Ucal::Status::Ok ||
        acceptedCount != batch.size()) return 170;
    events.clear();
    if (client.readCanEvents(opened.leaseToken, 32u, events) != Ucal::Status::Ok) return 171;
    for (const auto& expectedFrame : batch) {
        const auto batchCompletion = std::find_if(
            events.begin(), events.end(), [&](const auto& event) {
                return event.kind == Ucal::CanEventKind::TransmitCompleted &&
                       event.frame.transactionId == expectedFrame.transactionId;
            });
        if (batchCompletion == events.end()) return 172;
    }
    auto failedFrame = frame;
    failedFrame.id = 0x6afu;
    failedFrame.transactionId = 0x7654321u;
    if (client.submitCan(opened.leaseToken, failedFrame) != Ucal::Status::Ok) return 128;
    events.clear();
    if (client.readCanEvents(opened.leaseToken, 16u, events) != Ucal::Status::Ok) return 129;
    const auto failed = std::find_if(events.begin(), events.end(), [&](const auto& event) {
        return event.kind == Ucal::CanEventKind::TransmitFailed &&
               event.result == Ucal::Status::TransportError &&
               event.frame.transactionId == failedFrame.transactionId &&
               event.frame.timestampUs != 0u;
    });
    if (failed == events.end()) return 130;
    frame.flags |= Ucal::FrameRemote;
    if (client.submitCan(opened.leaseToken, frame) != Ucal::Status::InvalidArgument) return 131;
    if (client.closeDevice(opened.leaseToken) != Ucal::Status::Ok) return 132;

    config.canFd = false;
    config.dataSamplePointSpecified = false;
    config.listenOnly = true;
    config.acceptanceFilterCount = 1u;
    config.acceptanceFilters[0].frameType = Ucal::CanFilterFrameType::Standard;
    config.acceptanceFilters[0].idCode = 0x123u;
    config.acceptanceFilters[0].idMask = 0x7ffu;
    const auto listener = client.openCanChannel(endpoints[0].id, config);
    if (listener.status != Ucal::Status::Ok || listener.leaseToken == 0u) return 133;
    frame = {};
    frame.id = 0x456u;
    frame.dlc = 2u;
    frame.dataLength = 2u;
    frame.data[0] = 0x55u;
    frame.data[1] = 0xaau;
    frame.transactionId = 0x12345678u;
    if (client.submitCan(listener.leaseToken, frame) != Ucal::Status::Ok) return 134;
    events.clear();
    if (client.readCanEvents(listener.leaseToken, 16u, events) != Ucal::Status::Ok) return 135;
    const auto filteredRx = std::any_of(events.begin(), events.end(), [](const auto& event) {
        return event.kind == Ucal::CanEventKind::Received;
    });
    const auto classicCompletion = std::any_of(events.begin(), events.end(), [&](const auto& event) {
        return event.kind == Ucal::CanEventKind::TransmitCompleted &&
               event.frame.transactionId == frame.transactionId;
    });
    if (filteredRx || !classicCompletion) return 136;
    if (client.closeDevice(listener.leaseToken) != Ucal::Status::Ok) return 137;

    config.listenOnly = false;
    config.acceptanceFilterCount = 0u;
    const auto classic = client.openCanChannel(endpoints[0].id, config);
    if (classic.status != Ucal::Status::Ok || classic.leaseToken == 0u) return 138;
    Ucal::CanFrame remote{};
    remote.id = 0x456u;
    remote.flags = Ucal::FrameRemote;
    remote.dlc = 4u;
    remote.dataLength = 0u;
    remote.transactionId = 0x4567u;
    if (client.submitCan(classic.leaseToken, remote) != Ucal::Status::Ok) return 139;
    auto collidingRemote = remote;
    collidingRemote.transactionId += 0x100u;
    if (client.submitCan(classic.leaseToken, collidingRemote) != Ucal::Status::Busy) {
        return 235;
    }
    events.clear();
    if (client.readCanEvents(classic.leaseToken, 16u, events) != Ucal::Status::Ok) return 140;
    const auto remoteReceived = std::any_of(events.begin(), events.end(), [](const auto& event) {
        return event.kind == Ucal::CanEventKind::Received && event.frame.id == 0x456u &&
               (event.frame.flags & Ucal::FrameRemote) != 0u && event.frame.dlc == 4u &&
               event.frame.dataLength == 0u && event.frame.timestampUs != 0u;
    });
    if (!remoteReceived) return 141;
    const auto remoteCompleted = std::any_of(events.begin(), events.end(), [&](const auto& event) {
        return event.kind == Ucal::CanEventKind::TransmitCompleted &&
               event.frame.transactionId == remote.transactionId;
    });
    if (!remoteCompleted) return 236;
    if (client.submitCan(classic.leaseToken, collidingRemote) != Ucal::Status::Ok) return 237;
    events.clear();
    if (client.readCanEvents(classic.leaseToken, 16u, events) != Ucal::Status::Ok) return 238;
    const auto reusedCompletion = std::any_of(events.begin(), events.end(), [&](const auto& event) {
        return event.kind == Ucal::CanEventKind::TransmitCompleted &&
               event.frame.transactionId == collidingRemote.transactionId;
    });
    if (!reusedCompletion) return 239;
    remote.flags = Ucal::FrameBrs;
    remote.dataLength = 4u;
    if (client.submitCan(classic.leaseToken, remote) != Ucal::Status::InvalidArgument) return 142;
    if (client.closeDevice(classic.leaseToken) != Ucal::Status::Ok) return 143;

    config.terminationSpecified = true;
    if (client.openCanChannel(endpoints[0].id, config).status != Ucal::Status::Unsupported) {
        return 144;
    }
    config.terminationSpecified = false;
    config.noAck = true;
    if (client.openCanChannel(endpoints[0].id, config).status != Ucal::Status::Unsupported) {
        return 145;
    }
    config.noAck = false;
    config.internalLoopback = true;
    if (client.openCanChannel(endpoints[0].id, config).status != Ucal::Status::Unsupported) {
        return 173;
    }

    std::cout << "{\"vectorBackendMock\":true,\"txCompletion\":true,"
                 "\"statusValidity\":true,\"softwareFilters\":true,"
                 "\"batchedTransport\":true,\"unsupportedControls\":true}"
              << std::endl;
    return 0;
}

int validateVectorIdentity(Ucal::Client& client) {
    // The dedicated runtime fixture changes catalog order, XL access masks,
    // hardware indices and display names between these enumeration requests.
    std::array<std::vector<Ucal::EndpointDescriptor>, 7> catalogs;
    for (auto& catalog : catalogs) {
        if (client.enumerateEndpoints(catalog) != Ucal::Status::Ok ||
            catalog.size() != 2u || catalog[0].id == catalog[1].id ||
            catalog[0].adapterId != catalog[1].adapterId) return 230;
    }
    for (std::size_t port = 0; port < 2u; ++port) {
        const auto& before = catalogs[1][port];
        const auto& moved = catalogs[2][port];
        if (before.serial != "4242" || moved.serial != before.serial ||
            moved.name == before.name || before.id != moved.id ||
            before.adapterId != moved.adapterId ||
            before.physicalIndex != moved.physicalIndex) return 231;
        if (catalogs[3][port].id == before.id ||
            catalogs[3][port].adapterId == before.adapterId) return 232;
        if (!catalogs[4][port].serial.empty() || !catalogs[5][port].serial.empty() ||
            catalogs[4][port].id == catalogs[5][port].id ||
            catalogs[4][port].adapterId == catalogs[5][port].adapterId) return 233;
        if (catalogs[0][port].id != catalogs[6][port].id ||
            catalogs[0][port].adapterId != catalogs[6][port].adapterId ||
            catalogs[0][port].name == catalogs[6][port].name ||
            catalogs[0][port].physicalIndex != catalogs[6][port].physicalIndex) return 234;
    }
    std::cout << "{\"vectorIdentity\":true,\"physicalOrderAndNameIndependent\":true,"
                 "\"distinctSerialsAndPorts\":true,\"anonymousFallbackSeparated\":true,"
                 "\"virtualOrderAndNameIndependent\":true}" << std::endl;
    return 0;
}

int validateVectorRuntime(Ucal::Client& client) {
    std::vector<Ucal::EndpointDescriptor> endpoints;
    const auto enumerateStatus = client.enumerateEndpoints(endpoints);
    if (enumerateStatus != Ucal::Status::Ok) {
        std::cerr << "vector runtime enumeration status="
                  << static_cast<int>(enumerateStatus) << '\n';
        return 140 + static_cast<int>(enumerateStatus);
    }
    const auto selected = std::find_if(endpoints.begin(), endpoints.end(), [](const auto& endpoint) {
        return endpoint.busType == Ucal::BusType::Can &&
               (endpoint.capabilities & Ucal::CapabilityCanFdIso) != 0u;
    });
    if (selected == endpoints.end()) return 149;
    Ucal::CanChannelConfig config{};
    config.arbitrationBitrate = 500000u;
    config.dataBitrate = 2000000u;
    config.arbitrationSamplePoint = 75u;
    config.dataSamplePoint = 70u;
    config.arbitrationSamplePointSpecified = true;
    config.dataSamplePointSpecified = true;
    config.canFd = true;
    config.terminationSpecified = false;
    const auto opened = client.openCanChannel(selected->id, config);
    std::cout << "{\"vectorRuntimeOpenStatus\":" << static_cast<int>(opened.status)
              << ",\"endpointCount\":" << endpoints.size() << "}" << std::endl;
    if (opened.status != Ucal::Status::Ok || opened.leaseToken == 0u) {
        return 150 + static_cast<int>(opened.status);
    }
    Ucal::CanFrame frame{};
    frame.id = 0x6a8u;
    frame.flags = Ucal::FrameFd | Ucal::FrameBrs;
    frame.dlc = 15u;
    frame.dataLength = 64u;
    frame.transactionId = 0x7a51u;
    for (std::size_t index = 0; index < frame.data.size(); ++index) {
        frame.data[index] = static_cast<std::uint8_t>(index ^ 0xa5u);
    }
    if (client.submitCan(opened.leaseToken, frame) != Ucal::Status::Ok) return 160;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    bool completed = false;
    bool timestamped = false;
    do {
        std::vector<Ucal::CanEvent> events;
        if (client.readCanEvents(opened.leaseToken, 64u, events) != Ucal::Status::Ok) return 161;
        for (const auto& event : events) {
            if (event.kind == Ucal::CanEventKind::TransmitCompleted &&
                event.frame.transactionId == frame.transactionId) {
                completed = true;
                timestamped = event.frame.timestampUs != 0u;
            }
        }
        if (!completed) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (!completed && std::chrono::steady_clock::now() < deadline);
    const auto closeStatus = client.closeDevice(opened.leaseToken);
    if (closeStatus != Ucal::Status::Ok) return 162;
    if (!completed) return 163;
    if (!timestamped) return 164;
    std::cout << "{\"vectorInstalledRuntime\":true,\"txCompletion\":true,"
                 "\"timestamped\":true}" << std::endl;
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: UcalHarness <protocol-decoders|vector-frontend-catalog|ping|enumerate|endpoints|backend-health|backend-probe|backend-endpoints|can-self-receive|can-hardware-smoke|can-hardware-receive|can-hardware-config|can-hardware-soak|can-peer-latency|can-peer-error-recovery|pcan-mock|vector-mock|vector-identity|vector-runtime|lifecycle|lease-and-exit|bm-enumerate|bm-endpoints|bm-lifecycle|bm-lease-and-exit|bm-invalid|bm-txrx|bm-async|bm-async-fd|bm-async-fail|bm-receive|bm-hold|bm-lin-lifecycle|bm-lin-tx|shutdown>\n";
        return 2;
    }
    Ucal::Client client;
    const std::string command(argv[1]);
    if (command == "clock-domains") {
        Ucal::Timing::ChannelClockMapper clock;
        if(clock.translate(0,0,1)!=0 || clock.hasOrigin(0))return 230;
        if(clock.translate(0,1000000,2000)!=2000)return 231;
        if(clock.translate(1,50,2500)!=2500)return 232;
        if(clock.translate(0,1005000,8000)!=7000)return 233;
        if(clock.translate(1,150,2600)!=2600)return 234;
        if(clock.translate(0,100,9000)!=9000)return 235;
        if(clock.translate(1,160,9010)!=2610)return 236;
        if(clock.translate(0,120,9020)!=9020)return 237;
        clock.reset();
        if(clock.hasOrigin(0)||clock.hasOrigin(1)||clock.translate(1,999,6000)!=6000)return 238;
        if(clock.translate(64,1,1)!=0||clock.hasOrigin(63))return 239;
        clock.reset();
        clock.translate(0,1,~std::uint64_t{}-10u);
        if(clock.translate(0,20,1)!=0)return 240;
        std::cout<<"independent-clock-domains=true hardware-intervals=true reset-isolation=true"<<std::endl;
        return 0;
    }
    if (command == "advanced-timing-contract") return validateAdvancedTimingContract();
    if (command == "protocol-decoders") return validateProtocolDecoders();
    if (command == "vector-frontend-catalog") return validateVectorFrontendCatalog();
    if (command == "ping") return static_cast<int>(client.ping());
    if (command == "enumerate") {
        std::vector<Ucal::DeviceDescriptor> devices;
        return enumerateAndValidate(client, devices);
    }
    if (command == "endpoints") return enumerateMockEndpoints(client);
    if (command == "backend-health") return validateBackendHealth(client);
    if (command == "can-fd-admission") return validateCanFdAdmission(client);
    if (command == "backend-probe") return probeBackend(client);
    if (command == "backend-endpoints") return listBackendEndpoints(client);
    if (command == "can-self-receive") return validateCanSelfReceive(client);
    if (command == "can-hardware-smoke") return validateCanHardwareSmoke(client, false);
    if (command == "can-hardware-receive") {
        return validateCanHardwareSmoke(client, false, false);
    }
    if (command == "can-hardware-config") {
        return validateCanHardwareSmoke(client, false, false, true);
    }
    if (command == "can-hardware-soak") return validateCanHardwareSoak(client);
    if (command == "can-peer-latency") return validateCanPeerLatency(client);
    if (command == "can-peer-error-recovery") {
        return validateCanPeerErrorRecovery(client);
    }
    if (command == "pcan-mock") return validatePcanMock(client);
    if (command == "vector-mock") return validateVectorMock(client);
    if (command == "vector-identity") return validateVectorIdentity(client);
    if (command == "vector-runtime") return validateVectorRuntime(client);
    if (command == "bm-enumerate") {
        std::vector<Ucal::DeviceDescriptor> devices;
        return enumerateBackend(client, devices);
    }
    if (command == "bm-endpoints") {
        std::vector<Ucal::EndpointDescriptor> endpoints;
        const auto status = client.enumerateEndpoints(endpoints);
        if (status != Ucal::Status::Ok) return 83 + static_cast<int>(status);
        for (const auto& endpoint : endpoints) {
            std::cout << "{\"name\":\"" << endpoint.name << "\",\"busType\":"
                      << static_cast<std::uint32_t>(endpoint.busType)
                      << ",\"physicalIndex\":" << endpoint.physicalIndex
                      << ",\"capabilities\":" << endpoint.capabilities << "}" << std::endl;
        }
        return endpoints.empty() ? 84 : 0;
    }
    if (command == "bm-lifecycle" || command == "bm-lease-and-exit" ||
        command == "bm-invalid" ||
        command == "bm-txrx" || command == "bm-async" ||
        command == "bm-async-fd" || command == "bm-async-fail" ||
        command == "bm-receive" || command == "bm-hold") {
        std::vector<Ucal::DeviceDescriptor> devices;
        const int validation = enumerateBackend(client, devices);
        if (validation != 0) return validation;
        Ucal::CanChannelConfig config{};
        config.arbitrationBitrate = 500000;
        config.dataBitrate = 2000000;
        config.canFd = command == "bm-async-fd" || command == "bm-invalid";
        config.termination = true;
        applyCanEnvironment(config);
        const auto serial = selectedSerial();
        const auto port = selectedPort(0u);
        const auto selected = std::find_if(devices.begin(), devices.end(), [&](const auto& device) {
            return (serial.empty() || device.serial == serial) && device.id[12] == port;
        });
        if (selected == devices.end()) return 68;
        const auto opened = client.openCanChannel(selected->id, config);
        if (opened.status != Ucal::Status::Ok || opened.leaseToken == 0) return 61;
        Ucal::ChannelStatus channelStatus{};
        if (!environmentSwitch("UCAL_HARNESS_SKIP_STATUS", false)) {
            if (client.getChannelStatus(opened.leaseToken, channelStatus) != Ucal::Status::Ok) return 62;
            std::cout << "{\"opened\":true,\"busOff\":" << (channelStatus.busOff ? "true" : "false")
                      << ",\"txErrorCounter\":" << static_cast<unsigned>(channelStatus.txErrorCounter)
                      << ",\"rxErrorCounter\":" << static_cast<unsigned>(channelStatus.rxErrorCounter)
                      << "}" << std::endl;
        } else {
            std::cout << "{\"opened\":true,\"statusQuerySkipped\":true}" << std::endl;
        }
        if (command == "bm-lease-and-exit") {
            std::cout << "{\"bmLeaseAbandonedByProcessExit\":true}" << std::endl;
            return 0;
        }
        if (command == "bm-invalid") {
            Ucal::CanFrame invalid{};
            invalid.id = 0x800u;
            invalid.dlc = 1u;
            invalid.dataLength = 1u;
            if (client.submitCan(opened.leaseToken, invalid) != Ucal::Status::InvalidArgument) {
                return 99;
            }
            invalid = {};
            invalid.id = 0x123u;
            invalid.flags = Ucal::FrameBrs;
            invalid.dlc = 1u;
            invalid.dataLength = 1u;
            if (client.submitCan(opened.leaseToken, invalid) != Ucal::Status::InvalidArgument) {
                return 100;
            }
            invalid = {};
            invalid.id = 0x123u;
            invalid.flags = Ucal::FrameRemote;
            invalid.dlc = 4u;
            invalid.dataLength = 4u;
            if (client.submitCan(opened.leaseToken, invalid) != Ucal::Status::InvalidArgument) {
                return 101;
            }
            invalid.flags = Ucal::FrameFd | Ucal::FrameRemote;
            invalid.dataLength = 0u;
            if (client.submitCan(opened.leaseToken, invalid) != Ucal::Status::InvalidArgument) {
                return 102;
            }
            std::cout << "{\"invalidCanFramesRejected\":true}" << std::endl;
        }
        if (command == "bm-txrx" || command == "bm-async" ||
            command == "bm-async-fd" || command == "bm-async-fail") {
            Ucal::CanFrame frame{};
            frame.id = 0x6a5;
            frame.dlc = 8;
            frame.dataLength = 8;
            frame.transactionId = 0x5a;
            if (command == "bm-async-fd") {
                frame.flags = Ucal::FrameFd | Ucal::FrameBrs;
                frame.dlc = 15;
                frame.dataLength = 64;
                for (std::size_t index = 0; index < frame.data.size(); ++index) {
                    frame.data[index] = static_cast<std::uint8_t>(index);
                }
            } else {
                const std::uint8_t payload[8]{'U', 'C', 'A', 'I', '-', 'B', 'M', '!'};
                std::memcpy(frame.data.data(), payload, sizeof(payload));
            }
            const auto asynchronous = command != "bm-txrx";
            const auto writeStatus = asynchronous ?
                client.submitCan(opened.leaseToken, frame) :
                client.writeCan(opened.leaseToken, frame);
            if (writeStatus != Ucal::Status::Ok) return 63;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            bool completed = false;
            do {
                std::vector<Ucal::CanEvent> events;
                if (client.readCanEvents(opened.leaseToken, 64, events) != Ucal::Status::Ok) return 64;
                for (const auto& event : events) {
                    std::cout << "{\"kind\":" << static_cast<std::uint32_t>(event.kind)
                              << ",\"id\":" << event.frame.id
                              << ",\"transactionId\":" << event.frame.transactionId
                              << ",\"backendStatus\":" << event.backendStatus << "}" << std::endl;
                    if (event.kind == Ucal::CanEventKind::TransmitFailed &&
                        event.frame.transactionId == frame.transactionId) {
                        if (command == "bm-async-fail") completed = true;
                        else return 67;
                    }
                    completed = completed || (event.kind == Ucal::CanEventKind::TransmitCompleted &&
                                              event.frame.transactionId == frame.transactionId);
                    if (command == "bm-async-fail" &&
                        event.kind == Ucal::CanEventKind::TransmitCompleted &&
                        event.frame.transactionId == frame.transactionId) return 69;
                }
                if (!completed) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            } while (!completed && std::chrono::steady_clock::now() < deadline);
            if (!completed) return 65;
        }
        if (command == "bm-receive") {
            unsigned long durationSeconds = 3;
            environmentUnsigned("UCAL_HARNESS_SECONDS", durationSeconds);
            const auto deadline = std::chrono::steady_clock::now() +
                                  std::chrono::seconds(durationSeconds);
            std::size_t received = 0;
            do {
                std::vector<Ucal::CanEvent> events;
                if (client.readCanEvents(opened.leaseToken, 64, events) !=
                    Ucal::Status::Ok) return 64;
                for (const auto& event : events) {
                    if (event.kind != Ucal::CanEventKind::Received) continue;
                    ++received;
                    std::cout << "{\"rx\":true,\"id\":" << event.frame.id
                              << ",\"flags\":" << event.frame.flags
                              << ",\"length\":"
                              << static_cast<unsigned>(event.frame.dataLength) << "}"
                              << std::endl;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            } while (std::chrono::steady_clock::now() < deadline);
            std::cout << "{\"received\":" << received << "}" << std::endl;
            if (environmentSwitch("UCAL_HARNESS_EXPECT_RX", true) && received == 0) return 98;
        }
        if (command == "bm-hold") {
            unsigned long durationSeconds = 5;
            environmentUnsigned("UCAL_HARNESS_SECONDS", durationSeconds);
            std::this_thread::sleep_for(std::chrono::seconds(durationSeconds));
            std::cout << "{\"heldSeconds\":" << durationSeconds << "}" << std::endl;
        }
        if (client.closeDevice(opened.leaseToken) != Ucal::Status::Ok) return 66;
        std::cout << "{\"closed\":true}" << std::endl;
        return 0;
    }
    if (command == "bm-lin-lifecycle" || command == "bm-lin-tx") {
        std::vector<Ucal::EndpointDescriptor> endpoints;
        const auto enumeration = client.enumerateEndpoints(endpoints);
        if (enumeration != Ucal::Status::Ok) return 85 + static_cast<int>(enumeration);
        const auto serial = selectedSerial();
        const auto port = selectedPort(4u);
        const auto selected = std::find_if(endpoints.begin(), endpoints.end(), [&](const auto& endpoint) {
            return endpoint.busType == Ucal::BusType::Lin &&
                   (serial.empty() || endpoint.serial == serial) &&
                   endpoint.physicalIndex == port;
        });
        if (selected == endpoints.end()) return 90;
        Ucal::LinChannelConfig config{};
        config.bitrate = 19200;
        config.role = command == "bm-lin-tx" ? Ucal::LinRole::Master : Ucal::LinRole::ListenOnly;
        config.protocolVersion = 0x21;
        config.checksum = Ucal::LinChecksum::Enhanced;
        config.pullup = command == "bm-lin-tx";
        config.power = command == "bm-lin-tx";
        const auto opened = client.openLinChannel(selected->id, config);
        if (opened.status != Ucal::Status::Ok || opened.leaseToken == 0) return 91;
        Ucal::LinStatus linStatus{};
        if (client.getLinStatus(opened.leaseToken, linStatus) != Ucal::Status::Ok) return 92;
        std::cout << "{\"linOpened\":true,\"active\":"
                  << (linStatus.active ? "true" : "false") << ",\"asleep\":"
                  << (linStatus.asleep ? "true" : "false") << ",\"errors\":"
                  << static_cast<unsigned>(linStatus.errorFlags) << "}" << std::endl;
        if (command == "bm-lin-tx") {
            Ucal::LinFrame frame{};
            frame.id = 0x30;
            frame.dlc = 8;
            frame.flags = Ucal::LinTransmit | Ucal::LinEnhancedChecksum;
            frame.transactionId = 0x5a;
            const std::uint8_t payload[8]{'U', 'C', 'A', 'I', '-', 'L', 'I', 'N'};
            std::memcpy(frame.data.data(), payload, sizeof(payload));
            if (client.submitLin(opened.leaseToken, frame) != Ucal::Status::Ok) return 93;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            bool completed = false;
            do {
                std::vector<Ucal::LinEvent> events;
                if (client.readLinEvents(opened.leaseToken, 64, events) != Ucal::Status::Ok) return 94;
                for (const auto& event : events) {
                    std::cout << "{\"linKind\":" << static_cast<std::uint32_t>(event.kind)
                              << ",\"id\":" << static_cast<unsigned>(event.frame.id)
                              << ",\"transactionId\":" << event.frame.transactionId
                              << ",\"backendStatus\":" << event.backendStatus << "}" << std::endl;
                    if (event.frame.transactionId != frame.transactionId) continue;
                    if (event.kind == Ucal::LinEventKind::TransmitFailed) return 95;
                    completed = event.kind == Ucal::LinEventKind::TransmitCompleted;
                }
                if (!completed) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            } while (!completed && std::chrono::steady_clock::now() < deadline);
            if (!completed) return 96;
        }
        if (client.closeDevice(opened.leaseToken) != Ucal::Status::Ok) return 97;
        std::cout << "{\"linClosed\":true}" << std::endl;
        return 0;
    }
    if (command == "lifecycle") {
        std::vector<Ucal::DeviceDescriptor> devices;
        const int validation = enumerateAndValidate(client, devices);
        if (validation != 0) return validation;
        const auto first = client.openDevice(devices.front().id);
        if (first.status != Ucal::Status::Ok || first.leaseToken == 0) return 30;
        const auto duplicate = client.openDevice(devices.front().id);
        if (duplicate.status != Ucal::Status::Busy) return 31;
        if (client.closeDevice(first.leaseToken) != Ucal::Status::Ok) return 32;
        const auto reopened = client.openDevice(devices.front().id);
        if (reopened.status != Ucal::Status::Ok || reopened.leaseToken == 0) return 33;
        if (client.closeDevice(reopened.leaseToken) != Ucal::Status::Ok) return 34;
        std::cout << "{\"lifecycle\":\"passed\"}" << std::endl;
        return 0;
    }
    if (command == "lease-and-exit") {
        std::vector<Ucal::DeviceDescriptor> devices;
        const int validation = enumerateAndValidate(client, devices);
        if (validation != 0) return validation;
        const auto opened = client.openDevice(devices.front().id);
        if (opened.status != Ucal::Status::Ok || opened.leaseToken == 0) return 40;
        std::cout << "{\"leaseAbandonedByProcessExit\":true}" << std::endl;
        return 0;
    }
    if (command == "shutdown") return static_cast<int>(client.shutdownForTest());
    return 3;
}
