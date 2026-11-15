#if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0601
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include "ucal/Client.hpp"
#include "ucal/Protocol.hpp"
#include "ucal/CanTiming.hpp"
#include "BackendWorkerProtocol.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <iostream>
#include <chrono>
#include <cstring>
#include <utility>
#include <vector>

namespace Ucal {
namespace {

std::atomic<std::uint64_t> nextRequestId{1};

class Handle final {
public:
    explicit Handle(const HANDLE value) : value_(value) {}
    ~Handle() {
        if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const { return value_; }
private:
    HANDLE value_;
};

using Deadline = std::chrono::steady_clock::time_point;

bool cancelOverlappedIo(const HANDLE handle, OVERLAPPED* overlapped) {
    using Function = BOOL(WINAPI*)(HANDLE, LPOVERLAPPED);
    static const auto function = reinterpret_cast<Function>(GetProcAddress(
        GetModuleHandleW(L"kernel32.dll"), "CancelIoEx"));
    return function != nullptr ? function(handle, overlapped) != FALSE
                               : CancelIo(handle) != FALSE;
}

bool namedPipeServerProcessId(const HANDLE pipe, DWORD* processId) {
    using Function = BOOL(WINAPI*)(HANDLE, PULONG);
    static const auto function = reinterpret_cast<Function>(GetProcAddress(
        GetModuleHandleW(L"kernel32.dll"), "GetNamedPipeServerProcessId"));
    return function != nullptr && function(pipe, processId) != FALSE;
}

DWORD remainingTimeout(const Deadline deadline) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) return 0;
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - now).count();
    return static_cast<DWORD>((std::min<std::int64_t>)(remaining, MAXDWORD - 1));
}

bool transferExact(const HANDLE handle, void* data, std::uint32_t bytes,
                    const bool write, const Deadline deadline) {
    auto* current = static_cast<std::byte*>(data);
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (event.get() == nullptr) return false;
    while (bytes != 0) {
        ResetEvent(event.get());
        OVERLAPPED overlapped{};
        overlapped.hEvent = event.get();
        DWORD transferred = 0;
        const BOOL started = write
            ? WriteFile(handle, current, bytes, &transferred, &overlapped)
            : ReadFile(handle, current, bytes, &transferred, &overlapped);
        if (!started) {
            if (GetLastError() != ERROR_IO_PENDING) return false;
            const DWORD timeout = remainingTimeout(deadline);
            if (timeout == 0 || WaitForSingleObject(event.get(), timeout) != WAIT_OBJECT_0) {
                cancelOverlappedIo(handle, &overlapped);
                GetOverlappedResult(handle, &overlapped, &transferred, TRUE);
                SetLastError(ERROR_TIMEOUT);
                return false;
            }
            if (!GetOverlappedResult(handle, &overlapped, &transferred, FALSE)) return false;
        }
        if (transferred == 0) return false;
        current += transferred;
        bytes -= transferred;
    }
    return true;
}

bool writeExact(const HANDLE handle, const void* data, const std::uint32_t bytes,
                 const Deadline deadline) {
    return transferExact(handle, const_cast<void*>(data), bytes, true, deadline);
}

bool readExact(const HANDLE handle, void* data, const std::uint32_t bytes,
                const Deadline deadline) {
    return transferExact(handle, data, bytes, false, deadline);
}

Status transactImpl(const std::wstring& configuredPipeName,
                     const std::uint32_t forwardedOwnerPid,
                     const std::uint32_t expectedServerPid,
                     const std::uint32_t timeoutMs,
                     const Protocol::Operation operation, const void* requestPayload,
                     const std::uint32_t requestBytes,
                     std::vector<std::byte>& responsePayload) {
    responsePayload.clear();
    const auto pipeName = configuredPipeName.empty()
        ? Protocol::defaultPipeName() : configuredPipeName;
    HANDLE rawPipe = INVALID_HANDLE_VALUE;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    auto failure = [&](const char* stage, const DWORD error = GetLastError()) {
        wchar_t enabled[2]{};
        if (expectedServerPid != 0u &&
            GetEnvironmentVariableW(L"UCAL_IPC_DIAGNOSTICS", enabled, 2u) == 1u && enabled[0] == L'1') {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - deadline).count() + timeoutMs;
            std::cerr << "{\"event\":\"worker_request_failure\",\"stage\":\"" << stage
                      << "\",\"operation\":" << static_cast<std::uint16_t>(operation)
                      << ",\"workerPid\":" << expectedServerPid
                      << ",\"elapsedMs\":" << elapsed
                      << ",\"win32Error\":" << error << "}" << std::endl;
        }
        return Status::TransportError;
    };
    do {
        rawPipe = CreateFileW(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
        if (rawPipe != INVALID_HANDLE_VALUE) break;
        const auto error = GetLastError();
        if (error == ERROR_PIPE_BUSY) {
            WaitNamedPipeW(pipeName.c_str(), 50);
        } else if (error == ERROR_FILE_NOT_FOUND) {
            // The single-request Broker briefly has no published pipe instance while
            // it retires one connection and creates the next.  This is also the error
            // seen while the Broker is still starting, so keep retrying to the same
            // bounded deadline without adding a fixed 10 ms tax to every adjacent
            // request made by an already-connected client.
            Sleep(1);
        } else {
            return failure("connect");
        }
    } while (std::chrono::steady_clock::now() < deadline);
    Handle pipe(rawPipe);
    if (pipe.get() == INVALID_HANDLE_VALUE) return failure("connect-deadline");
    if (expectedServerPid != 0u) {
        DWORD serverPid = 0;
        if (!namedPipeServerProcessId(pipe.get(), &serverPid) ||
            serverPid != expectedServerPid || forwardedOwnerPid == 0u) {
            return failure("server-identity", ERROR_ACCESS_DENIED);
        }
    }

    const auto requestId = nextRequestId.fetch_add(1);
    auto request = Protocol::makeRequest(operation, requestId, requestBytes);
    // Use the minimum required envelope for each operation. Read-only catalogs
    // can negotiate an older contract; advanced opens retain their full request.
    if(operation==Protocol::Operation::OpenCanChannelAdvanced ||
       operation==Protocol::Operation::EnumerateEndpointsV11)request.minor=11u;
    else if(operation==Protocol::Operation::EnumerateEndpointsV10 ||
       operation==Protocol::Operation::EnumerateBackendHealthV10 ||
       operation==Protocol::Operation::ConfigureBackendPlugins)request.minor=10u;
    else if(operation==Protocol::Operation::EnumerateEndpointsV9)request.minor=9u;
    else request.minor=8u;
    bool requestWritten = false;
    if (expectedServerPid != 0u) {
        const Ucal::Backend::Worker::RequestHeader workerRequest{
            Ucal::Backend::Worker::magic, Ucal::Backend::Worker::version, 0u,
            forwardedOwnerPid, request};
        requestWritten = writeExact(pipe.get(), &workerRequest,
                                      sizeof(workerRequest), deadline);
    } else {
        requestWritten = writeExact(pipe.get(), &request, sizeof(request), deadline);
    }
    if (!requestWritten ||
        (requestBytes != 0 &&
         !writeExact(pipe.get(), requestPayload, requestBytes, deadline))) {
        return failure("write-request");
    }

    Protocol::MessageHeader response{};
    if (!readExact(pipe.get(), &response, sizeof(response), deadline)) {
        return failure("read-header");
    }
    if (!Protocol::validResponseHeader(response) || response.requestId != requestId ||
        response.operation != static_cast<std::uint16_t>(operation)) {
        return failure("validate-header", ERROR_INVALID_DATA);
    }
    responsePayload.resize(response.payloadBytes);
    if (response.payloadBytes != 0 &&
        !readExact(pipe.get(), responsePayload.data(), response.payloadBytes, deadline)) {
        return failure("read-payload");
    }
    if (static_cast<Status>(response.status) == Status::TransportError)
        return failure("backend-response", ERROR_SUCCESS);
    return static_cast<Status>(response.status);
}

}  // namespace

Client::Client(std::wstring pipeName, const std::uint32_t forwardedOwnerPid,
               const std::uint32_t expectedServerPid, const std::uint32_t timeoutMs)
    : pipeName_(std::move(pipeName)),
      forwardedOwnerPid_(forwardedOwnerPid),
      expectedServerPid_(expectedServerPid), timeoutMs_(timeoutMs) {}

Status Client::transact(const Protocol::Operation operation, const void* requestPayload,
                        const std::uint32_t requestBytes,
                        std::vector<std::byte>& responsePayload) const {
    if (transport_) return transport_(forwardedOwnerPid_, operation, requestPayload, requestBytes, responsePayload);
    return transactImpl(pipeName_, forwardedOwnerPid_, expectedServerPid_, timeoutMs_,
                         operation, requestPayload, requestBytes, responsePayload);
}

Status Client::ping() const {
    std::vector<std::byte> response;
    return transact(Protocol::Operation::Ping, nullptr, 0, response);
}

Status Client::configureBackendPlugins(const std::string& path) const {
    if(path.empty()||path.size()>32767u||path.find('\0')!=path.npos)return Status::InvalidArgument;
    std::vector<std::byte> output;
    return transact(Protocol::Operation::ConfigureBackendPlugins,path.data(),static_cast<std::uint32_t>(path.size()),output);
}

Status Client::enumerate(std::vector<DeviceDescriptor>& devices) const {
    std::vector<std::byte> response;
    const auto status = transact(Protocol::Operation::Enumerate, nullptr, 0, response);
    return status == Status::Ok ? Protocol::decodeDevices(response, devices) : status;
}

Status Client::enumerateEndpoints(std::vector<EndpointDescriptor>& endpoints) const {
    std::vector<std::byte> response;
    auto status = transact(Protocol::Operation::EnumerateEndpointsV11, nullptr, 0, response);
    if (status == Status::Ok) return Protocol::decodeEndpointsV10(response, endpoints);
    // Older Brokers can close a future-minor envelope. Only this read-only
    // discovery probe may retry with its older contract; opens never do.
    if (status != Status::Unsupported && status != Status::IncompatibleVersion &&
        status != Status::TransportError) return status;
    response.clear();
    status = transact(Protocol::Operation::EnumerateEndpointsV10, nullptr, 0, response);
    if(status==Status::Ok)return Protocol::decodeEndpointsV10(response,endpoints);
    if(status!=Status::Unsupported&&status!=Status::IncompatibleVersion)return status;
    response.clear();
    status = transact(Protocol::Operation::EnumerateEndpointsV9, nullptr, 0, response);
    if (status == Status::Ok) return Protocol::decodeEndpointsV9(response, endpoints);
    if (status != Status::Unsupported && status != Status::IncompatibleVersion) return status;
    response.clear();
    status = transact(Protocol::Operation::EnumerateEndpointsV6, nullptr, 0, response);
    if (status == Status::Ok) {
        return Protocol::decodeEndpointsV6(response, endpoints);
    }
    if (status != Status::Unsupported && status != Status::IncompatibleVersion) return status;
    response.clear();
    status = transact(Protocol::Operation::EnumerateEndpoints, nullptr, 0, response);
    return status == Status::Ok ? Protocol::decodeEndpoints(response, endpoints) : status;
}

Status Client::enumerateBackendHealth(std::vector<BackendHealth>& health) const {
    std::vector<std::byte> response;
    auto status = transact(Protocol::Operation::EnumerateBackendHealthV10, nullptr, 0, response);
    if(status==Status::Ok)return Protocol::decodeBackendHealthV10(response,health);
    if(status!=Status::Unsupported&&status!=Status::IncompatibleVersion)return status;
    response.clear();
    status = transact(Protocol::Operation::EnumerateBackendHealthV8, nullptr, 0, response);
    if(status==Status::Ok)return Protocol::decodeBackendHealthV8(response,health);
    if(status!=Status::Unsupported&&status!=Status::IncompatibleVersion)return status;
    response.clear();
    status = transact(Protocol::Operation::EnumerateBackendHealth, nullptr, 0, response);
    return status == Status::Ok ? Protocol::decodeBackendHealth(response, health) : status;
}

OpenResult Client::openDevice(const DeviceId& id) const {
    Protocol::OpenDeviceRequestV1 request{};
    std::memcpy(request.id, id.data(), id.size());
    std::vector<std::byte> response;
    const auto status = transact(Protocol::Operation::OpenDevice, &request, sizeof(request), response);
    if (status != Status::Ok || response.size() != sizeof(Protocol::OpenDeviceResponseV1)) {
        return {status == Status::Ok ? Status::TransportError : status, 0};
    }
    Protocol::OpenDeviceResponseV1 wire{};
    std::memcpy(&wire, response.data(), sizeof(wire));
    return {Status::Ok, wire.leaseToken};
}

OpenResult Client::openCanChannel(const DeviceId& id, const CanChannelConfig& config) const {
    if (!validCanAdvancedTiming(config)) return {Status::InvalidArgument, 0};
    Protocol::OpenCanChannelRequestV5 request{};
    std::memcpy(request.id, id.data(), id.size());
    request.arbitrationBitrate = config.arbitrationBitrate;
    request.dataBitrate = config.dataBitrate;
    request.arbitrationSamplePoint = config.arbitrationSamplePoint;
    request.dataSamplePoint = config.dataSamplePoint;
    request.samplePointFlags = (config.arbitrationSamplePointSpecified ? 0x01u : 0u) |
                                 (config.dataSamplePointSpecified ? 0x02u : 0u);
    request.canFd = config.canFd ? 1u : 0u;
    request.listenOnly = config.listenOnly ? 1u : 0u;
    request.noAck = config.noAck ? 1u : 0u;
    request.termination = config.termination ? 1u : 0u;
    request.configurationFlags = config.terminationSpecified ?
        Protocol::canConfigTerminationSpecified : 0u;
    if (config.internalLoopback) {
        request.configurationFlags |= Protocol::CanConfigInternalLoopback;
    }
    request.acceptanceFilterCount = (std::min<std::uint8_t>)(
        config.acceptanceFilterCount,
        static_cast<std::uint8_t>(config.acceptanceFilters.size()));
    for (std::uint8_t index = 0; index < request.acceptanceFilterCount; ++index) {
        const auto& source = config.acceptanceFilters[index];
        auto& target = request.acceptanceFilters[index];
        target.idCode = source.idCode;
        target.idMask = source.idMask;
        target.frameType = static_cast<std::uint8_t>(source.frameType);
        target.rejectAll = source.rejectAll ? 1u : 0u;
    }
    std::vector<std::byte> response;
    auto status = Status::InternalError;
    if (config.nominalTimingSpecified || config.dataTimingSpecified) {
        Protocol::OpenCanChannelRequestV11 advanced{};
        advanced.base = request;
        advanced.timingFlags = (config.nominalTimingSpecified ? 1u : 0u) |
                               (config.dataTimingSpecified ? 2u : 0u);
        const auto encode = [](const CanBitTiming& t) {
            return Protocol::WireCanBitTimingV11{t.clockHz, t.prescaler, t.tseg1, t.tseg2, t.sjw, t.sampleCount};
        };
        if (config.nominalTimingSpecified) advanced.nominal = encode(config.nominalTiming);
        if (config.dataTimingSpecified) advanced.data = encode(config.dataTiming);
        status = transact(Protocol::Operation::OpenCanChannelAdvanced, &advanced, sizeof(advanced), response);
    } else {
        status = transact(Protocol::Operation::OpenCanChannel, &request, sizeof(request), response);
    }
    if (status != Status::Ok || response.size() != sizeof(Protocol::OpenDeviceResponseV1)) {
        return {status == Status::Ok ? Status::TransportError : status, 0};
    }
    Protocol::OpenDeviceResponseV1 wire{};
    std::memcpy(&wire, response.data(), sizeof(wire));
    return {Status::Ok, wire.leaseToken};
}

OpenResult Client::openLinChannel(const EndpointId& id, const LinChannelConfig& config) const {
    Protocol::OpenLinChannelRequestV3 request{};
    std::memcpy(request.id, id.data(), id.size());
    request.bitrate = config.bitrate;
    request.role = static_cast<std::uint8_t>(config.role);
    request.protocolVersion = config.protocolVersion;
    request.checksum = static_cast<std::uint8_t>(config.checksum);
    request.pullup = config.pullup ? 1u : 0u;
    request.power = config.power ? 1u : 0u;
    std::vector<std::byte> response;
    const auto status = transact(Protocol::Operation::OpenLinChannel, &request, sizeof(request),
                                 response);
    if (status != Status::Ok || response.size() != sizeof(Protocol::OpenDeviceResponseV1)) {
        return {status == Status::Ok ? Status::TransportError : status, 0};
    }
    Protocol::OpenDeviceResponseV1 wire{};
    std::memcpy(&wire, response.data(), sizeof(wire));
    return {Status::Ok, wire.leaseToken};
}

Status Client::closeDevice(const std::uint64_t leaseToken) const {
    const Protocol::CloseDeviceRequestV1 request{leaseToken};
    std::vector<std::byte> response;
    return transact(Protocol::Operation::CloseDevice, &request, sizeof(request), response);
}

Status Client::writeCan(const std::uint64_t leaseToken, const CanFrame& frame) const {
    Protocol::WriteCanRequestV1 request{};
    request.leaseToken = leaseToken;
    request.id = frame.id;
    request.flags = frame.flags;
    request.dlc = frame.dlc;
    request.dataLength = frame.dataLength;
    request.transactionId = frame.transactionId;
    std::memcpy(request.data, frame.data.data(), frame.data.size());
    std::vector<std::byte> response;
    return transact(Protocol::Operation::WriteCan, &request, sizeof(request), response);
}

Status Client::submitCan(const std::uint64_t leaseToken, const CanFrame& frame) const {
    Protocol::WriteCanRequestV1 request{};
    request.leaseToken = leaseToken;
    request.id = frame.id;
    request.flags = frame.flags;
    request.dlc = frame.dlc;
    request.dataLength = frame.dataLength;
    request.transactionId = frame.transactionId;
    std::memcpy(request.data, frame.data.data(), frame.data.size());
    std::vector<std::byte> response;
    return transact(Protocol::Operation::SubmitCan, &request, sizeof(request), response);
}

Status Client::submitCanBatch(const std::uint64_t leaseToken,
                                const std::vector<CanFrame>& frames,
                                std::uint32_t& acceptedCount) const {
    acceptedCount = 0u;
    const auto maximumCount = static_cast<std::size_t>(
        (Protocol::maxPayloadBytes - sizeof(Protocol::SubmitCanBatchRequestV7)) /
        sizeof(Protocol::WireCanFrameV1));
    if (frames.empty() || frames.size() > maximumCount) return Status::InvalidArgument;

    std::vector<std::byte> payload(
        sizeof(Protocol::SubmitCanBatchRequestV7) +
        frames.size() * sizeof(Protocol::WireCanFrameV1));
    Protocol::SubmitCanBatchRequestV7 request{
        leaseToken, static_cast<std::uint32_t>(frames.size()), 0u};
    std::memcpy(payload.data(), &request, sizeof(request));
    auto* wireFrames = reinterpret_cast<Protocol::WireCanFrameV1*>(
        payload.data() + sizeof(request));
    for (std::size_t index = 0; index < frames.size(); ++index) {
        wireFrames[index] = {};
        wireFrames[index].id = frames[index].id;
        wireFrames[index].flags = frames[index].flags;
        wireFrames[index].dlc = frames[index].dlc;
        wireFrames[index].dataLength = frames[index].dataLength;
        std::memcpy(wireFrames[index].data, frames[index].data.data(),
                    frames[index].data.size());
        wireFrames[index].transactionId = frames[index].transactionId;
    }

    std::vector<std::byte> response;
    const auto status = transact(Protocol::Operation::SubmitCanBatch,
                                 payload.data(), static_cast<std::uint32_t>(payload.size()),
                                 response);
    if (response.size() != sizeof(Protocol::SubmitCanBatchResponseV7)) {
        return status == Status::Ok ? Status::TransportError : status;
    }
    Protocol::SubmitCanBatchResponseV7 batch{};
    std::memcpy(&batch, response.data(), sizeof(batch));
    if (batch.acceptedCount > frames.size() ||
        batch.itemStatus != static_cast<std::int32_t>(status)) {
        return Status::TransportError;
    }
    acceptedCount = batch.acceptedCount;
    return status;
}

Status Client::readCan(const std::uint64_t leaseToken, const std::uint32_t capacity,
                        std::vector<CanFrame>& frames) const {
    frames.clear();
    const Protocol::ReadCanRequestV1 request{leaseToken, capacity};
    std::vector<std::byte> response;
    const auto status = transact(Protocol::Operation::ReadCan, &request, sizeof(request), response);
    return status == Status::Ok ? Protocol::decodeCanFrames(response, frames) : status;
}

Status Client::readCanEvents(const std::uint64_t leaseToken, const std::uint32_t capacity,
                               std::vector<CanEvent>& events) const {
    events.clear();
    const Protocol::ReadCanRequestV1 request{leaseToken, capacity};
    std::vector<std::byte> response;
    const auto status = transact(Protocol::Operation::ReadCanEvents, &request, sizeof(request), response);
    return status == Status::Ok ? Protocol::decodeCanEvents(response, events) : status;
}

Status Client::submitLin(const std::uint64_t leaseToken, const LinFrame& frame) const {
    Protocol::WriteLinRequestV3 request{};
    request.leaseToken = leaseToken;
    request.flags = frame.flags;
    request.transactionId = frame.transactionId;
    request.id = frame.id;
    request.dlc = frame.dlc;
    request.checksum = frame.checksum;
    std::memcpy(request.data, frame.data.data(), frame.data.size());
    std::vector<std::byte> response;
    return transact(Protocol::Operation::SubmitLin, &request, sizeof(request), response);
}

Status Client::readLinEvents(const std::uint64_t leaseToken, const std::uint32_t capacity,
                               std::vector<LinEvent>& events) const {
    events.clear();
    const Protocol::ReadCanRequestV1 request{leaseToken, capacity};
    std::vector<std::byte> response;
    const auto status = transact(Protocol::Operation::ReadLinEvents, &request, sizeof(request),
                                 response);
    return status == Status::Ok ? Protocol::decodeLinEvents(response, events) : status;
}

Status Client::getLinStatus(const std::uint64_t leaseToken, LinStatus& statusInfo) const {
    const Protocol::ChannelStatusRequestV1 request{leaseToken};
    std::vector<std::byte> response;
    const auto status = transact(Protocol::Operation::GetLinStatus, &request, sizeof(request),
                                 response);
    if (status != Status::Ok) return status;
    if (response.size() != sizeof(Protocol::LinStatusResponseV3)) return Status::TransportError;
    Protocol::LinStatusResponseV3 wire{};
    std::memcpy(&wire, response.data(), sizeof(wire));
    return Protocol::decodeLinStatus(wire, statusInfo);
}

Status Client::configureLinSlaveResponse(const std::uint64_t leaseToken,
                                            const LinSlaveResponse& response) const {
    Protocol::ConfigureLinSlaveResponseRequestV3 request{};
    request.leaseToken = leaseToken;
    request.id = response.id;
    request.dlc = response.dlc;
    request.enabled = response.enabled ? 1u : 0u;
    request.checksum = static_cast<std::uint8_t>(response.checksum);
    request.checksumValue = response.checksumValue;
    std::copy(response.data.begin(), response.data.end(), request.data);
    std::vector<std::byte> output;
    return transact(Protocol::Operation::ConfigureLinSlaveResponse,
                    &request, sizeof(request), output);
}

Status Client::getChannelStatus(const std::uint64_t leaseToken, ChannelStatus& statusInfo) const {
    const Protocol::ChannelStatusRequestV1 request{leaseToken};
    std::vector<std::byte> response;
    const auto status = transact(Protocol::Operation::GetChannelStatus, &request, sizeof(request), response);
    if (status != Status::Ok) return status;
    if (response.size() != sizeof(Protocol::ChannelStatusResponseV1)) return Status::TransportError;
    Protocol::ChannelStatusResponseV1 wire{};
    std::memcpy(&wire, response.data(), sizeof(wire));
    return Protocol::decodeChannelStatus(wire, statusInfo);
}

Status Client::shutdownForTest() const {
    std::vector<std::byte> response;
    return transact(Protocol::Operation::ShutdownForTest, nullptr, 0, response);
}

}  // namespace ucal
