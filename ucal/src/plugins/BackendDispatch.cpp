#include "ucal/BackendDispatch.hpp"
#include "ucal/CanTiming.hpp"
#include <windows.h>

#include <algorithm>
#include <array>
#include <bcrypt.h>
#include <cstring>
#include <iterator>
namespace Ucal::Backend::Wire {
using Lease = LegacyLease;
namespace {
bool processIsAlive(const DWORD pid) {
    const auto p = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!p)
        return GetLastError() == ERROR_ACCESS_DENIED;
    const bool alive = WaitForSingleObject(p, 0) == WAIT_TIMEOUT;
    CloseHandle(p);
    return alive;
}
void pruneDeadLeases(std::vector<Lease> &leases) {
    leases.erase(std::remove_if(leases.begin(), leases.end(),
                                [](const Lease &lease) { return !processIsAlive(lease.ownerPid); }),
                 leases.end());
}

std::uint64_t randomToken() {
    std::uint64_t token = 0;
    while (token == 0) {
        if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&token), sizeof(token),
                            BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
            return 0;
        }
    }
    return token;
}

template <typename Wire> constexpr std::size_t maximumWireItems() {
    return (Ucal::Protocol::maxPayloadBytes - sizeof(std::uint32_t)) / sizeof(Wire);
}

template <typename Value, std::size_t Count> bool allZero(const Value (&values)[Count]) {
    return std::all_of(std::begin(values), std::end(values), [](const Value value) { return value == 0; });
}

bool validWireBool(const std::uint8_t value) { return value <= 1u; }

bool validFilter(const Ucal::Protocol::WireCanAcceptanceFilterV4 &filter) {
    return filter.frameType <= static_cast<std::uint8_t>(Ucal::CanFilterFrameType::Extended) &&
           validWireBool(filter.rejectAll) && allZero(filter.reserved);
}

bool validCanOpenCommon(const std::uint8_t samplePointFlags, const std::uint8_t canFd,
                           const std::uint8_t listenOnly, const std::uint8_t noAck,
                           const std::uint8_t termination, const std::uint8_t acceptanceFilterCount,
                           const Ucal::Protocol::WireCanAcceptanceFilterV4 (&filters)[2]) {
    return (samplePointFlags & ~0x03u) == 0u && validWireBool(canFd) && validWireBool(listenOnly) &&
           validWireBool(noAck) && validWireBool(termination) && acceptanceFilterCount <= 2u &&
           std::all_of(std::begin(filters), std::end(filters), validFilter);
}

bool validLinRole(const std::uint8_t role) {
    return role >= static_cast<std::uint8_t>(Ucal::LinRole::Master) &&
           role <= static_cast<std::uint8_t>(Ucal::LinRole::ListenOnly);
}

bool validLinChecksum(const std::uint8_t checksum) {
    return checksum <= static_cast<std::uint8_t>(Ucal::LinChecksum::Manual);
}

Ucal::Status enumerateEndpointCatalog(Ucal::Backend::BackendInterface *const backend,
                                        std::vector<Ucal::EndpointDescriptor> &endpoints,
                                        const bool advancedTiming = false) {
    endpoints.clear();
    if (backend != nullptr) {
        const auto result = backend->enumerateEndpoints(endpoints);
        if (result != Ucal::Status::Ok)
            return result;
        for (auto &endpoint : endpoints) {
            if (!advancedTiming) endpoint.capabilities &= ~CapabilityCanAdvancedTiming;
            // Keep rejected identities visible for diagnostics and existing
            // mappings, but never advertise them as an available CAN port.
            if (!Ucal::endpointMeetsDeviceContract(endpoint))
                endpoint.state = Ucal::DeviceState::Offline;
        }
        return Ucal::Status::Ok;
    }
    for (const auto &device : Ucal::mockCatalog()) {
        for (std::uint32_t channel = 0; channel < device.channelCount; ++channel) {
            Ucal::EndpointDescriptor endpoint{};
            endpoint.id = device.id;
            endpoint.id[12] = static_cast<std::uint8_t>(channel);
            endpoint.adapterId = device.id;
            endpoint.name = device.name + " CH" + std::to_string(channel + 1);
            endpoint.model = device.model;
            endpoint.serial = device.serial;
            endpoint.backendFamily = "mock";
            endpoint.busType = Ucal::BusType::Can;
            endpoint.physicalIndex = channel;
            endpoint.capabilities = device.capabilities;
            endpoint.state = device.state;
            endpoints.push_back(std::move(endpoint));
        }
    }
    return Ucal::Status::Ok;
}

} // namespace
Ucal::Status handleRequest(const Ucal::Protocol::MessageHeader &request, const std::vector<std::byte> &input,
                            const std::uint32_t clientPid, std::vector<Lease> &leases,
                            std::vector<std::byte> &output, Ucal::Backend::BackendInterface *const backend,
                            std::mutex &mockMutex, const bool allowTestShutdown,
                            std::atomic_bool &shutdown) {
    using Ucal::Status;
    using Ucal::Protocol::Operation;
    output.clear();
    std::unique_lock<std::mutex> mockLock(mockMutex, std::defer_lock);
    if (backend == nullptr) {
        mockLock.lock();
        pruneDeadLeases(leases);
    }
    if (backend != nullptr)
        backend->pruneDeadSessions();
    const auto operation = static_cast<Operation>(request.operation);
    if (operation == Operation::ConfigureBackendPlugins) {
        if (request.minor < 10u)
            return Status::IncompatibleVersion;
        if (input.empty() || input.size() > 32767u || std::memchr(input.data(), 0, input.size()))
            return Status::InvalidArgument;
        const auto text = reinterpret_cast<const char *>(input.data());
        const auto n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text,
                                           static_cast<int>(input.size()), nullptr, 0);
        if (n <= 0)
            return Status::InvalidArgument;
        std::wstring path(n, 0);
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, static_cast<int>(input.size()), path.data(),
                            n);
        return backend ? backend->configurePlugins(path) : Status::Unsupported;
    }
    if (operation == Operation::Ping) {
        if (!input.empty())
            return Status::InvalidArgument;
        return backend != nullptr ? backend->initialize() : Status::Ok;
    }
    if (operation == Operation::Enumerate) {
        if (!input.empty())
            return Status::InvalidArgument;
        std::vector<Ucal::DeviceDescriptor> devices;
        if (backend != nullptr) {
            const auto status = backend->enumerate(devices);
            if (status != Status::Ok)
                return status;
        } else {
            devices = Ucal::mockCatalog();
        }
        for (auto &device : devices) {
            device.capabilities &= ~CapabilityCanAdvancedTiming;
            const auto leased = std::any_of(leases.begin(), leases.end(),
                                            [&](const Lease &lease) { return lease.deviceId == device.id; });
            if (leased)
                device.state = Ucal::DeviceState::Leased;
        }
        if (devices.size() > maximumWireItems<Ucal::Protocol::WireDeviceV1>()) {
            return Status::InternalError;
        }
        output = Ucal::Protocol::encodeDevices(devices);
        return Status::Ok;
    }
    if (operation == Operation::EnumerateEndpoints) {
        if (!input.empty())
            return Status::InvalidArgument;
        std::vector<Ucal::EndpointDescriptor> endpoints;
        const auto enumerateStatus = enumerateEndpointCatalog(backend, endpoints);
        if (enumerateStatus != Status::Ok)
            return enumerateStatus;
        if (endpoints.size() > maximumWireItems<Ucal::Protocol::WireEndpointV2>()) {
            return Status::InternalError;
        }
        output = Ucal::Protocol::encodeEndpoints(endpoints);
        return Status::Ok;
    }
    if (operation == Operation::EnumerateEndpointsV10 || operation == Operation::EnumerateEndpointsV11) {
        const bool current = operation == Operation::EnumerateEndpointsV11;
        if (request.minor < (current ? 11u : 10u))
            return Status::IncompatibleVersion;
        if (!input.empty())
            return Status::InvalidArgument;
        std::vector<EndpointDescriptor> endpoints;
        const auto status = enumerateEndpointCatalog(backend, endpoints, current);
        if (status != Status::Ok)
            return status;
        if (endpoints.size() > maximumWireItems<Protocol::WireEndpointV10>())
            return Status::InternalError;
        output = Protocol::encodeEndpointsV10(endpoints);
        return Status::Ok;
    }
    if (operation == Operation::EnumerateBackendHealthV10) {
        if (request.minor < 10u)
            return Status::IncompatibleVersion;
        if (!input.empty())
            return Status::InvalidArgument;
        std::vector<BackendHealth> health;
        if (backend)
            backend->enumerateBackendHealth(health);
        else
            health.push_back({"mock", Status::Ok, 0u});
        if (health.size() > maximumWireItems<Protocol::WireBackendHealthV10>())
            return Status::InternalError;
        output = Protocol::encodeBackendHealthV10(health);
        return Status::Ok;
    }
    if (operation == Operation::EnumerateEndpointsV9) {
        if (request.minor < 9u)
            return Status::IncompatibleVersion;
        if (!input.empty())
            return Status::InvalidArgument;
        std::vector<Ucal::EndpointDescriptor> endpoints;
        const auto status = enumerateEndpointCatalog(backend, endpoints);
        if (status != Status::Ok)
            return status;
        if (endpoints.size() > maximumWireItems<Ucal::Protocol::WireEndpointV9>())
            return Status::InternalError;
        output = Ucal::Protocol::encodeEndpointsV9(endpoints);
        return Status::Ok;
    }
    if (operation == Operation::EnumerateEndpointsV6) {
        if (request.minor < 6u)
            return Status::IncompatibleVersion;
        if (!input.empty())
            return Status::InvalidArgument;
        std::vector<Ucal::EndpointDescriptor> endpoints;
        const auto enumerateStatus = enumerateEndpointCatalog(backend, endpoints);
        if (enumerateStatus != Status::Ok)
            return enumerateStatus;
        if (endpoints.size() > maximumWireItems<Ucal::Protocol::WireEndpointV6>()) {
            return Status::InternalError;
        }
        if (std::any_of(endpoints.begin(), endpoints.end(),
                        [](const auto &endpoint) { return endpoint.backendFamily.empty(); })) {
            return Status::InternalError;
        }
        output = Ucal::Protocol::encodeEndpointsV6(endpoints);
        return Status::Ok;
    }
    if (operation == Operation::EnumerateBackendHealth ||
        operation == Operation::EnumerateBackendHealthV8) {
        if (request.minor < 6u)
            return Status::IncompatibleVersion;
        if (operation == Operation::EnumerateBackendHealthV8 && request.minor < 8u)
            return Status::IncompatibleVersion;
        if (!input.empty())
            return Status::InvalidArgument;
        std::vector<Ucal::BackendHealth> health;
        if (backend != nullptr) {
            backend->enumerateBackendHealth(health);
        } else {
            std::vector<Ucal::EndpointDescriptor> endpoints;
            const auto enumerateStatus = enumerateEndpointCatalog(nullptr, endpoints);
            health.push_back({"mock", enumerateStatus, static_cast<std::uint32_t>(endpoints.size())});
        }
        if (health.size() > maximumWireItems<Ucal::Protocol::WireBackendHealthV8>()) {
            return Status::InternalError;
        }
        output = operation == Operation::EnumerateBackendHealthV8
                     ? Ucal::Protocol::encodeBackendHealthV8(health)
                     : Ucal::Protocol::encodeBackendHealth(health);
        return Status::Ok;
    }
    if (operation == Operation::OpenDevice) {
        if (backend != nullptr)
            return Status::Unsupported;
        if (input.size() != sizeof(Ucal::Protocol::OpenDeviceRequestV1))
            return Status::InvalidArgument;
        Ucal::Protocol::OpenDeviceRequestV1 wire{};
        std::memcpy(&wire, input.data(), sizeof(wire));
        Ucal::DeviceId id{};
        std::memcpy(id.data(), wire.id, id.size());
        const auto known = std::any_of(Ucal::mockCatalog().begin(), Ucal::mockCatalog().end(),
                                       [&](const auto &device) { return device.id == id; });
        if (!known)
            return Status::NotFound;
        const auto leased = std::any_of(leases.begin(), leases.end(),
                                        [&](const Lease &lease) { return lease.deviceId == id; });
        if (leased)
            return Status::Busy;
        const auto token = randomToken();
        if (token == 0)
            return Status::InternalError;
        leases.push_back({id, token, clientPid});
        const Ucal::Protocol::OpenDeviceResponseV1 response{token};
        output.resize(sizeof(response));
        std::memcpy(output.data(), &response, sizeof(response));
        return Status::Ok;
    }
    if (operation == Operation::OpenCanChannel || operation == Operation::OpenCanChannelAdvanced) {
        const auto advanced = operation == Operation::OpenCanChannelAdvanced;
        if (advanced && request.minor < 11u) return Status::IncompatibleVersion;
        if ((advanced && input.size() != sizeof(Ucal::Protocol::OpenCanChannelRequestV11)) ||
            (!advanced && input.size() != sizeof(Ucal::Protocol::OpenCanChannelRequestV1) &&
            input.size() != sizeof(Ucal::Protocol::OpenCanChannelRequestV4) &&
            input.size() != sizeof(Ucal::Protocol::OpenCanChannelRequestV5))) {
            return Status::InvalidArgument;
        }
        Ucal::DeviceId id{};
        Ucal::CanChannelConfig config{};
        if (advanced || input.size() == sizeof(Ucal::Protocol::OpenCanChannelRequestV5)) {
            Ucal::Protocol::OpenCanChannelRequestV5 wire{};
            std::memcpy(&wire, input.data(), sizeof(wire));
            if (!validCanOpenCommon(wire.samplePointFlags, wire.canFd, wire.listenOnly, wire.noAck,
                                       wire.termination, wire.acceptanceFilterCount,
                                       wire.acceptanceFilters) ||
                (wire.configurationFlags & ~(Ucal::Protocol::canConfigTerminationSpecified |
                                              Ucal::Protocol::CanConfigInternalLoopback)) != 0u ||
                !allZero(wire.reserved)) {
                return Status::InvalidArgument;
            }
            std::memcpy(id.data(), wire.id, id.size());
            config.arbitrationBitrate = wire.arbitrationBitrate;
            config.dataBitrate = wire.dataBitrate;
            config.arbitrationSamplePoint = wire.arbitrationSamplePoint;
            config.dataSamplePoint = wire.dataSamplePoint;
            config.arbitrationSamplePointSpecified = (wire.samplePointFlags & 0x01u) != 0;
            config.dataSamplePointSpecified = (wire.samplePointFlags & 0x02u) != 0;
            config.canFd = wire.canFd != 0;
            config.listenOnly = wire.listenOnly != 0;
            config.noAck = wire.noAck != 0;
            config.termination = wire.termination != 0;
            config.terminationSpecified =
                (wire.configurationFlags & Ucal::Protocol::canConfigTerminationSpecified) != 0u;
            config.internalLoopback =
                (wire.configurationFlags & Ucal::Protocol::CanConfigInternalLoopback) != 0u;
            if (wire.acceptanceFilterCount > config.acceptanceFilters.size()) {
                return Status::InvalidArgument;
            }
            config.acceptanceFilterCount = wire.acceptanceFilterCount;
            for (std::uint8_t index = 0; index < config.acceptanceFilterCount; ++index) {
                const auto &source = wire.acceptanceFilters[index];
                if (source.frameType > static_cast<std::uint8_t>(Ucal::CanFilterFrameType::Extended)) {
                    return Status::InvalidArgument;
                }
                auto &target = config.acceptanceFilters[index];
                target.idCode = source.idCode;
                target.idMask = source.idMask;
                target.frameType = static_cast<Ucal::CanFilterFrameType>(source.frameType);
                target.rejectAll = source.rejectAll != 0;
            }
        } else if (input.size() == sizeof(Ucal::Protocol::OpenCanChannelRequestV4)) {
            Ucal::Protocol::OpenCanChannelRequestV4 wire{};
            std::memcpy(&wire, input.data(), sizeof(wire));
            if (!validCanOpenCommon(wire.samplePointFlags, wire.canFd, wire.listenOnly, wire.noAck,
                                       wire.termination, wire.acceptanceFilterCount,
                                       wire.acceptanceFilters)) {
                return Status::InvalidArgument;
            }
            std::memcpy(id.data(), wire.id, id.size());
            config.arbitrationBitrate = wire.arbitrationBitrate;
            config.dataBitrate = wire.dataBitrate;
            config.arbitrationSamplePoint = wire.arbitrationSamplePoint;
            config.dataSamplePoint = wire.dataSamplePoint;
            config.arbitrationSamplePointSpecified = (wire.samplePointFlags & 0x01u) != 0;
            config.dataSamplePointSpecified = (wire.samplePointFlags & 0x02u) != 0;
            config.canFd = wire.canFd != 0;
            config.listenOnly = wire.listenOnly != 0;
            config.noAck = wire.noAck != 0;
            config.termination = wire.termination != 0;
            config.terminationSpecified = true;
            if (wire.acceptanceFilterCount > config.acceptanceFilters.size()) {
                return Status::InvalidArgument;
            }
            config.acceptanceFilterCount = wire.acceptanceFilterCount;
            for (std::uint8_t index = 0; index < config.acceptanceFilterCount; ++index) {
                const auto &source = wire.acceptanceFilters[index];
                if (source.frameType > static_cast<std::uint8_t>(Ucal::CanFilterFrameType::Extended)) {
                    return Status::InvalidArgument;
                }
                auto &target = config.acceptanceFilters[index];
                target.idCode = source.idCode;
                target.idMask = source.idMask;
                target.frameType = static_cast<Ucal::CanFilterFrameType>(source.frameType);
                target.rejectAll = source.rejectAll != 0;
            }
        } else {
            Ucal::Protocol::OpenCanChannelRequestV1 wire{};
            std::memcpy(&wire, input.data(), sizeof(wire));
            if (!validWireBool(wire.canFd) || !validWireBool(wire.listenOnly) ||
                !validWireBool(wire.termination) || !allZero(wire.reserved)) {
                return Status::InvalidArgument;
            }
            std::memcpy(id.data(), wire.id, id.size());
            config.arbitrationBitrate = wire.arbitrationBitrate;
            config.dataBitrate = wire.dataBitrate;
            config.arbitrationSamplePoint = wire.arbitrationSamplePoint;
            config.dataSamplePoint = wire.dataSamplePoint;
            config.canFd = wire.canFd != 0;
            config.listenOnly = wire.listenOnly != 0;
            config.termination = wire.termination != 0;
            config.terminationSpecified = true;
        }
        if (advanced) {
            Ucal::Protocol::OpenCanChannelRequestV11 wire{};
            std::memcpy(&wire, input.data(), sizeof(wire));
            if (!wire.timingFlags || (wire.timingFlags & ~3u) || !allZero(wire.reserved))
                return Status::InvalidArgument;
            const auto decode = [](const Ucal::Protocol::WireCanBitTimingV11& t) {
                return CanBitTiming{t.clockHz, t.prescaler, t.tseg1, t.tseg2, t.sjw, t.sampleCount};
            };
            config.nominalTimingSpecified = (wire.timingFlags & 1u) != 0u;
            config.dataTimingSpecified = (wire.timingFlags & 2u) != 0u;
            config.nominalTiming = decode(wire.nominal);
            config.dataTiming = decode(wire.data);
            const auto zero = [](const auto& t) {
                return !t.clockHz && !t.prescaler && !t.tseg1 && !t.tseg2 && !t.sjw && !t.sampleCount;
            };
            if ((!config.nominalTimingSpecified && !zero(wire.nominal)) ||
                (!config.dataTimingSpecified && !zero(wire.data)) || !validCanAdvancedTiming(config))
                return Status::InvalidArgument;
        }
        if (backend == nullptr)
            return Status::Unsupported;
        std::vector<Ucal::EndpointDescriptor> endpoints;
        const auto catalogStatus = enumerateEndpointCatalog(backend, endpoints, advanced);
        if (catalogStatus != Status::Ok)
            return catalogStatus;
        const auto endpoint = std::find_if(endpoints.begin(), endpoints.end(), [&](const auto &item) {
            return item.id == id && item.busType == Ucal::BusType::Can;
        });
        if (endpoint == endpoints.end())
            return Status::NotFound;
        if (!Ucal::endpointMeetsDeviceContract(*endpoint))
            return Status::Unsupported;
        if (advanced && (endpoint->capabilities & CapabilityCanAdvancedTiming) == 0u)
            return Status::Unsupported;
        const auto result = backend->open(id, config, clientPid);
        if (result.status != Status::Ok)
            return result.status;
        const Ucal::Protocol::OpenDeviceResponseV1 response{result.leaseToken};
        output.resize(sizeof(response));
        std::memcpy(output.data(), &response, sizeof(response));
        return Status::Ok;
    }
    if (operation == Operation::OpenLinChannel) {
        if (input.size() != sizeof(Ucal::Protocol::OpenLinChannelRequestV3)) {
            return Status::InvalidArgument;
        }
        Ucal::Protocol::OpenLinChannelRequestV3 wire{};
        std::memcpy(&wire, input.data(), sizeof(wire));
        if (!validLinRole(wire.role) || !validLinChecksum(wire.checksum) ||
            !validWireBool(wire.pullup) || !validWireBool(wire.power) || !allZero(wire.reserved)) {
            return Status::InvalidArgument;
        }
        Ucal::EndpointId id{};
        std::memcpy(id.data(), wire.id, id.size());
        Ucal::LinChannelConfig config{};
        config.bitrate = wire.bitrate;
        config.role = static_cast<Ucal::LinRole>(wire.role);
        config.protocolVersion = wire.protocolVersion;
        config.checksum = static_cast<Ucal::LinChecksum>(wire.checksum);
        config.pullup = wire.pullup != 0;
        config.power = wire.power != 0;
        if (backend == nullptr)
            return Status::Unsupported;
        const auto result = backend->openLin(id, config, clientPid);
        if (result.status != Status::Ok)
            return result.status;
        const Ucal::Protocol::OpenDeviceResponseV1 response{result.leaseToken};
        output.resize(sizeof(response));
        std::memcpy(output.data(), &response, sizeof(response));
        return Status::Ok;
    }
    if (operation == Operation::CloseDevice) {
        if (input.size() != sizeof(Ucal::Protocol::CloseDeviceRequestV1))
            return Status::InvalidArgument;
        Ucal::Protocol::CloseDeviceRequestV1 wire{};
        std::memcpy(&wire, input.data(), sizeof(wire));
        if (backend != nullptr)
            return backend->close(wire.leaseToken, clientPid);
        const auto match = std::find_if(leases.begin(), leases.end(), [&](const Lease &lease) {
            return lease.token == wire.leaseToken && lease.ownerPid == clientPid;
        });
        if (match == leases.end())
            return Status::NotFound;
        leases.erase(match);
        return Status::Ok;
    }
    if (operation == Operation::WriteCan) {
        if (input.size() != sizeof(Ucal::Protocol::WriteCanRequestV1))
            return Status::InvalidArgument;
        Ucal::Protocol::WriteCanRequestV1 wire{};
        std::memcpy(&wire, input.data(), sizeof(wire));
        if (wire.dataLength > 64u || wire.dlc > 15u || !allZero(wire.reserved) ||
            (wire.flags & ~0xffu) != 0u)
            return Status::InvalidArgument;
        Ucal::CanFrame frame{};
        frame.id = wire.id;
        frame.flags = wire.flags;
        frame.dlc = wire.dlc;
        frame.dataLength = wire.dataLength;
        frame.transactionId = wire.transactionId;
        std::memcpy(frame.data.data(), wire.data, frame.data.size());
        if (backend == nullptr)
            return Status::Unsupported;
        return backend->write(wire.leaseToken, clientPid, frame);
    }
    if (operation == Operation::SubmitCan) {
        if (input.size() != sizeof(Ucal::Protocol::WriteCanRequestV1))
            return Status::InvalidArgument;
        Ucal::Protocol::WriteCanRequestV1 wire{};
        std::memcpy(&wire, input.data(), sizeof(wire));
        if (wire.dataLength > 64u || wire.dlc > 15u || !allZero(wire.reserved) ||
            (wire.flags & ~0xffu) != 0u)
            return Status::InvalidArgument;
        Ucal::CanFrame frame{};
        frame.id = wire.id;
        frame.flags = wire.flags;
        frame.dlc = wire.dlc;
        frame.dataLength = wire.dataLength;
        frame.transactionId = wire.transactionId;
        std::memcpy(frame.data.data(), wire.data, frame.data.size());
        if (backend == nullptr)
            return Status::Unsupported;
        return backend->submit(wire.leaseToken, clientPid, frame);
    }
    if (operation == Operation::SubmitCanBatch) {
        if (request.minor < 7u)
            return Status::IncompatibleVersion;
        if (input.size() < sizeof(Ucal::Protocol::SubmitCanBatchRequestV7)) {
            return Status::InvalidArgument;
        }
        Ucal::Protocol::SubmitCanBatchRequestV7 header{};
        std::memcpy(&header, input.data(), sizeof(header));
        const auto maximumCount = static_cast<std::uint32_t>(
            (Ucal::Protocol::maxPayloadBytes - sizeof(header)) / sizeof(Ucal::Protocol::WireCanFrameV1));
        if (header.count == 0u || header.count > maximumCount || header.reserved != 0u) {
            return Status::InvalidArgument;
        }
        const auto expected =
            sizeof(header) + static_cast<std::size_t>(header.count) * sizeof(Ucal::Protocol::WireCanFrameV1);
        if (input.size() != expected)
            return Status::InvalidArgument;

        std::vector<Ucal::CanFrame> frames;
        frames.reserve(header.count);
        for (std::uint32_t index = 0; index < header.count; ++index) {
            Ucal::Protocol::WireCanFrameV1 wire{};
            std::memcpy(&wire, input.data() + sizeof(header) + static_cast<std::size_t>(index) * sizeof(wire),
                        sizeof(wire));
            if (wire.dataLength > 64u || wire.dlc > 15u || !allZero(wire.reserved) ||
                wire.timestampUs != 0u || (wire.flags & ~0xffu) != 0u) {
                return Status::InvalidArgument;
            }
            Ucal::CanFrame frame{};
            frame.id = wire.id;
            frame.flags = wire.flags;
            frame.dlc = wire.dlc;
            frame.dataLength = wire.dataLength;
            frame.transactionId = wire.transactionId;
            std::memcpy(frame.data.data(), wire.data, frame.data.size());
            frames.push_back(std::move(frame));
        }

        std::uint32_t accepted = 0u;
        const auto itemStatus =
            backend == nullptr ? Status::Unsupported
                               : backend->submitBatch(header.leaseToken, clientPid, frames, accepted);
        const Ucal::Protocol::SubmitCanBatchResponseV7 response{accepted,
                                                                static_cast<std::int32_t>(itemStatus)};
        output.resize(sizeof(response));
        std::memcpy(output.data(), &response, sizeof(response));
        return itemStatus;
    }
    if (operation == Operation::SubmitLin) {
        if (input.size() != sizeof(Ucal::Protocol::WriteLinRequestV3)) {
            return Status::InvalidArgument;
        }
        Ucal::Protocol::WriteLinRequestV3 wire{};
        std::memcpy(&wire, input.data(), sizeof(wire));
        if (wire.id > 0x3fu || wire.dlc > 8u || wire.reserved != 0u || (wire.flags & ~0xffu) != 0u)
            return Status::InvalidArgument;
        Ucal::LinFrame frame{};
        frame.flags = wire.flags;
        frame.transactionId = wire.transactionId;
        frame.id = wire.id;
        frame.dlc = wire.dlc;
        frame.checksum = wire.checksum;
        std::memcpy(frame.data.data(), wire.data, frame.data.size());
        if (backend == nullptr)
            return Status::Unsupported;
        return backend->submitLin(wire.leaseToken, clientPid, frame);
    }
    if (operation == Operation::ReadCan) {
        if (backend == nullptr)
            return Status::Unsupported;
        if (input.size() != sizeof(Ucal::Protocol::ReadCanRequestV1))
            return Status::InvalidArgument;
        Ucal::Protocol::ReadCanRequestV1 wire{};
        std::memcpy(&wire, input.data(), sizeof(wire));
        if (wire.capacity > maximumWireItems<Ucal::Protocol::WireCanFrameV1>()) {
            return Status::InvalidArgument;
        }
        std::vector<Ucal::CanFrame> frames;
        const auto status = backend->read(wire.leaseToken, clientPid, wire.capacity, frames);
        if (status != Status::Ok)
            return status;
        if (frames.size() > wire.capacity ||
            frames.size() > maximumWireItems<Ucal::Protocol::WireCanFrameV1>()) {
            return Status::InternalError;
        }
        const auto count = static_cast<std::uint32_t>(frames.size());
        output.resize(sizeof(count) + frames.size() * sizeof(Ucal::Protocol::WireCanFrameV1));
        std::memcpy(output.data(), &count, sizeof(count));
        auto *destination = reinterpret_cast<Ucal::Protocol::WireCanFrameV1 *>(output.data() + sizeof(count));
        for (std::size_t index = 0; index < frames.size(); ++index) {
            destination[index].id = frames[index].id;
            destination[index].flags = frames[index].flags;
            destination[index].dlc = frames[index].dlc;
            destination[index].dataLength = frames[index].dataLength;
            std::memcpy(destination[index].data, frames[index].data.data(), frames[index].data.size());
            destination[index].timestampUs = frames[index].timestampUs;
            destination[index].transactionId = frames[index].transactionId;
        }
        return Status::Ok;
    }
    if (operation == Operation::ReadCanEvents) {
        if (backend == nullptr)
            return Status::Unsupported;
        if (input.size() != sizeof(Ucal::Protocol::ReadCanRequestV1))
            return Status::InvalidArgument;
        Ucal::Protocol::ReadCanRequestV1 wire{};
        std::memcpy(&wire, input.data(), sizeof(wire));
        if (wire.capacity > maximumWireItems<Ucal::Protocol::WireCanEventV2>()) {
            return Status::InvalidArgument;
        }
        std::vector<Ucal::CanEvent> events;
        const auto status = backend->readEvents(wire.leaseToken, clientPid, wire.capacity, events);
        if (status != Status::Ok)
            return status;
        if (events.size() > wire.capacity ||
            events.size() > maximumWireItems<Ucal::Protocol::WireCanEventV2>()) {
            return Status::InternalError;
        }
        const auto count = static_cast<std::uint32_t>(events.size());
        output.resize(sizeof(count) + events.size() * sizeof(Ucal::Protocol::WireCanEventV2));
        std::memcpy(output.data(), &count, sizeof(count));
        auto *destination = reinterpret_cast<Ucal::Protocol::WireCanEventV2 *>(output.data() + sizeof(count));
        for (std::size_t index = 0; index < events.size(); ++index) {
            auto &wireEvent = destination[index];
            wireEvent = {};
            wireEvent.kind = static_cast<std::uint32_t>(events[index].kind);
            wireEvent.result = static_cast<std::int32_t>(events[index].result);
            wireEvent.backendStatus = events[index].backendStatus;
            wireEvent.frame.id = events[index].frame.id;
            wireEvent.frame.flags = events[index].frame.flags;
            wireEvent.frame.dlc = events[index].frame.dlc;
            wireEvent.frame.dataLength = events[index].frame.dataLength;
            std::memcpy(wireEvent.frame.data, events[index].frame.data.data(),
                        events[index].frame.data.size());
            wireEvent.frame.timestampUs = events[index].frame.timestampUs;
            wireEvent.frame.transactionId = events[index].frame.transactionId;
            wireEvent.channelStatus.busOff = events[index].channelStatus.busOff ? 1u : 0u;
            wireEvent.channelStatus.errorPassive = events[index].channelStatus.errorPassive ? 1u : 0u;
            wireEvent.channelStatus.warning = events[index].channelStatus.warning ? 1u : 0u;
            wireEvent.channelStatus.txErrorCounter = events[index].channelStatus.txErrorCounter;
            wireEvent.channelStatus.rxErrorCounter = events[index].channelStatus.rxErrorCounter;
            wireEvent.channelStatus.reserved[0] = static_cast<std::uint8_t>(
                (events[index].channelStatus.busStateValid ? Ucal::Protocol::channelStatusBusStateValid
                                                              : 0u) |
                (events[index].channelStatus.errorCountersValid
                     ? Ucal::Protocol::channelStatusErrorCountersValid
                     : 0u));
        }
        return Status::Ok;
    }
    if (operation == Operation::ReadLinEvents) {
        if (backend == nullptr)
            return Status::Unsupported;
        if (input.size() != sizeof(Ucal::Protocol::ReadCanRequestV1))
            return Status::InvalidArgument;
        Ucal::Protocol::ReadCanRequestV1 wire{};
        std::memcpy(&wire, input.data(), sizeof(wire));
        if (wire.capacity > maximumWireItems<Ucal::Protocol::WireLinEventV3>()) {
            return Status::InvalidArgument;
        }
        std::vector<Ucal::LinEvent> events;
        const auto status = backend->readLinEvents(wire.leaseToken, clientPid, wire.capacity, events);
        if (status != Status::Ok)
            return status;
        if (events.size() > wire.capacity ||
            events.size() > maximumWireItems<Ucal::Protocol::WireLinEventV3>()) {
            return Status::InternalError;
        }
        const auto count = static_cast<std::uint32_t>(events.size());
        output.resize(sizeof(count) + events.size() * sizeof(Ucal::Protocol::WireLinEventV3));
        std::memcpy(output.data(), &count, sizeof(count));
        auto *destination = reinterpret_cast<Ucal::Protocol::WireLinEventV3 *>(output.data() + sizeof(count));
        for (std::size_t index = 0; index < events.size(); ++index) {
            auto &wireEvent = destination[index];
            wireEvent = {};
            wireEvent.kind = static_cast<std::uint32_t>(events[index].kind);
            wireEvent.result = static_cast<std::int32_t>(events[index].result);
            wireEvent.backendStatus = events[index].backendStatus;
            wireEvent.flags = events[index].frame.flags;
            wireEvent.timestampUs = events[index].frame.timestampUs;
            wireEvent.transactionId = events[index].frame.transactionId;
            wireEvent.id = events[index].frame.id;
            wireEvent.dlc = events[index].frame.dlc;
            wireEvent.checksum = events[index].frame.checksum;
            wireEvent.errorFlags = events[index].frame.errorFlags;
            std::memcpy(wireEvent.data, events[index].frame.data.data(), events[index].frame.data.size());
            wireEvent.channelStatus.active = events[index].channelStatus.active ? 1u : 0u;
            wireEvent.channelStatus.asleep = events[index].channelStatus.asleep ? 1u : 0u;
            wireEvent.channelStatus.errorFlags = events[index].channelStatus.errorFlags;
        }
        return Status::Ok;
    }
    if (operation == Operation::GetChannelStatus) {
        if (backend == nullptr)
            return Status::Unsupported;
        if (input.size() != sizeof(Ucal::Protocol::ChannelStatusRequestV1))
            return Status::InvalidArgument;
        Ucal::Protocol::ChannelStatusRequestV1 wire{};
        std::memcpy(&wire, input.data(), sizeof(wire));
        Ucal::ChannelStatus channelStatus{};
        const auto status = backend->getStatus(wire.leaseToken, clientPid, channelStatus);
        if (status != Status::Ok)
            return status;
        const Ucal::Protocol::ChannelStatusResponseV1 response{
            channelStatus.busOff ? 1u : 0u,
            channelStatus.errorPassive ? 1u : 0u,
            channelStatus.warning ? 1u : 0u,
            channelStatus.txErrorCounter,
            channelStatus.rxErrorCounter,
            {static_cast<std::uint8_t>(
                 (channelStatus.busStateValid ? Ucal::Protocol::channelStatusBusStateValid : 0u) |
                 (channelStatus.errorCountersValid ? Ucal::Protocol::channelStatusErrorCountersValid
                                                      : 0u)),
             0u, 0u}};
        output.resize(sizeof(response));
        std::memcpy(output.data(), &response, sizeof(response));
        return Status::Ok;
    }
    if (operation == Operation::GetLinStatus) {
        if (backend == nullptr)
            return Status::Unsupported;
        if (input.size() != sizeof(Ucal::Protocol::ChannelStatusRequestV1)) {
            return Status::InvalidArgument;
        }
        Ucal::Protocol::ChannelStatusRequestV1 wire{};
        std::memcpy(&wire, input.data(), sizeof(wire));
        Ucal::LinStatus linStatus{};
        const auto status = backend->getLinStatus(wire.leaseToken, clientPid, linStatus);
        if (status != Status::Ok)
            return status;
        const Ucal::Protocol::LinStatusResponseV3 response{linStatus.active ? 1u : 0u,
                                                           linStatus.asleep ? 1u : 0u,
                                                           linStatus.errorFlags,
                                                           {0u, 0u, 0u, 0u, 0u}};
        output.resize(sizeof(response));
        std::memcpy(output.data(), &response, sizeof(response));
        return Status::Ok;
    }
    if (operation == Operation::ConfigureLinSlaveResponse) {
        if (input.size() != sizeof(Ucal::Protocol::ConfigureLinSlaveResponseRequestV3)) {
            return Status::InvalidArgument;
        }
        Ucal::Protocol::ConfigureLinSlaveResponseRequestV3 wire{};
        std::memcpy(&wire, input.data(), sizeof(wire));
        if (wire.id > 0x3fu || wire.dlc > 8u || !validWireBool(wire.enabled) ||
            !validLinChecksum(wire.checksum) || !allZero(wire.reserved)) {
            return Status::InvalidArgument;
        }
        Ucal::LinSlaveResponse response{};
        response.id = wire.id;
        response.dlc = wire.dlc;
        response.enabled = wire.enabled != 0;
        response.checksum = static_cast<Ucal::LinChecksum>(wire.checksum);
        response.checksumValue = wire.checksumValue;
        std::copy(std::begin(wire.data), std::end(wire.data), response.data.begin());
        if (backend == nullptr)
            return Status::Unsupported;
        return backend->configureLinSlaveResponse(wire.leaseToken, clientPid, response);
    }
    if (operation == Operation::ShutdownForTest) {
        if (!input.empty())
            return Status::InvalidArgument;
        if (!allowTestShutdown)
            return Status::Unsupported;
        shutdown.store(true);
        return Status::Ok;
    }
    return Status::Unsupported;
}

} // namespace Ucal::backend::wire
