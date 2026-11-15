#pragma once

#include "ucal/Model.hpp"

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <functional>

namespace Ucal::Backend {
class IsolatedBackend;
class WorkerClientFactory;
}

namespace Ucal::Protocol {
enum class Operation : std::uint16_t;
}

namespace Ucal {

class Client final {
public:
    using Transport = std::function<Status(std::uint32_t, Protocol::Operation, const void*,
                                           std::uint32_t, std::vector<std::byte>&)>;
    Client() = default;
    explicit Client(std::uint32_t timeoutMs)
        : timeoutMs_(timeoutMs < 1u ? 1u : timeoutMs > 300000u ? 300000u : timeoutMs) {}
    // In-process serialized transport: used by the C-ABI plugin host only.
    explicit Client(Transport transport, std::uint32_t ownerPid)
        : transport_(std::move(transport)), forwardedOwnerPid_(ownerPid) {}

    Status ping() const;
    Status configureBackendPlugins(const std::string& catalogPathUtf8) const;
    Status enumerate(std::vector<DeviceDescriptor>& devices) const;
    Status enumerateEndpoints(std::vector<EndpointDescriptor>& endpoints) const;
    Status enumerateBackendHealth(std::vector<BackendHealth>& health) const;
    OpenResult openDevice(const DeviceId& id) const;
    OpenResult openCanChannel(const DeviceId& id, const CanChannelConfig& config) const;
    OpenResult openLinChannel(const EndpointId& id, const LinChannelConfig& config) const;
    Status closeDevice(std::uint64_t leaseToken) const;
    Status writeCan(std::uint64_t leaseToken, const CanFrame& frame) const;
    // Queue acceptance is returned immediately. Physical completion or failure
    // is delivered later by read_can_events().
    Status submitCan(std::uint64_t leaseToken, const CanFrame& frame) const;
    // Submit a bounded ordered batch in one transport transaction. A non-ok
    // result may still report a committed prefix through accepted_count.
    Status submitCanBatch(std::uint64_t leaseToken, const std::vector<CanFrame>& frames,
                            std::uint32_t& acceptedCount) const;
    Status readCan(std::uint64_t leaseToken, std::uint32_t capacity,
                    std::vector<CanFrame>& frames) const;
    Status readCanEvents(std::uint64_t leaseToken, std::uint32_t capacity,
                           std::vector<CanEvent>& events) const;
    Status submitLin(std::uint64_t leaseToken, const LinFrame& frame) const;
    Status readLinEvents(std::uint64_t leaseToken, std::uint32_t capacity,
                           std::vector<LinEvent>& events) const;
    Status getLinStatus(std::uint64_t leaseToken, LinStatus& status) const;
    Status configureLinSlaveResponse(std::uint64_t leaseToken,
                                        const LinSlaveResponse& response) const;
    Status getChannelStatus(std::uint64_t leaseToken, ChannelStatus& status) const;
    Status shutdownForTest() const;

private:
    friend class Ucal::Backend::IsolatedBackend;
    friend class Ucal::Backend::WorkerClientFactory;
    Client(std::wstring pipeName, std::uint32_t forwardedOwnerPid,
           std::uint32_t expectedServerPid, std::uint32_t timeoutMs);
    Status transact(Protocol::Operation operation, const void* requestPayload,
                    std::uint32_t requestBytes,
                    std::vector<std::byte>& responsePayload) const;

    std::wstring pipeName_;
    Transport transport_;
    std::uint32_t forwardedOwnerPid_{};
    std::uint32_t expectedServerPid_{};
    std::uint32_t timeoutMs_{5000u};
};

}  // namespace ucal
