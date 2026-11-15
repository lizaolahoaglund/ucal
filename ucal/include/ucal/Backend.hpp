#pragma once

#include "ucal/Model.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace Ucal::Backend {

class BackendInterface {
public:
    virtual ~BackendInterface() = default;

    virtual Status initialize() = 0;
    virtual Status enumerate(std::vector<DeviceDescriptor>& devices) = 0;
    virtual Status enumerateEndpoints(std::vector<EndpointDescriptor>& endpoints) = 0;
    virtual OpenResult open(const DeviceId& id, const CanChannelConfig& config,
                            std::uint32_t ownerPid) = 0;
    virtual Status close(std::uint64_t token, std::uint32_t ownerPid) = 0;
    virtual Status write(std::uint64_t token, std::uint32_t ownerPid,
                         const CanFrame& frame) = 0;
    virtual Status submit(std::uint64_t token, std::uint32_t ownerPid,
                          const CanFrame& frame) = 0;
    virtual Status submitBatch(std::uint64_t token, std::uint32_t ownerPid,
                                const std::vector<CanFrame>& frames,
                                std::uint32_t& acceptedCount) {
        acceptedCount = 0u;
        for (const auto& frame : frames) {
            const auto status = submit(token, ownerPid, frame);
            if (status != Status::Ok) return status;
            ++acceptedCount;
        }
        return Status::Ok;
    }
    virtual Status read(std::uint64_t token, std::uint32_t ownerPid,
                        std::uint32_t capacity, std::vector<CanFrame>& frames) = 0;
    virtual Status readEvents(std::uint64_t token, std::uint32_t ownerPid,
                               std::uint32_t capacity,
                               std::vector<CanEvent>& events) = 0;
    virtual Status getStatus(std::uint64_t token, std::uint32_t ownerPid,
                              ChannelStatus& status) = 0;
    virtual void pruneDeadSessions() = 0;
    // Control-plane plugin catalog; optional for an individual hardware adapter.
    virtual Status configurePlugins(const std::wstring&) { return Status::Unsupported; }

    virtual std::string backendFamily() const { return "unknown"; }
    virtual BackendRuntimeIdentity runtimeIdentity() const { return {}; }
    virtual void enumerateBackendHealth(std::vector<BackendHealth>& health) {
        std::vector<EndpointDescriptor> endpoints;
        const auto result = enumerateEndpoints(endpoints);
        std::uint32_t admitted = 0u;
        bool incompatible = false;
        for (const auto& endpoint : endpoints) {
            if (endpointMeetsDeviceContract(endpoint)) ++admitted;
            else incompatible = true;
        }
        health.push_back({backendFamily(), result == Status::Ok && incompatible
                              ? Status::Unsupported : result,
                          result == Status::Ok ? admitted : 0u, runtimeIdentity()});
    }

    virtual OpenResult openLin(const EndpointId&, const LinChannelConfig&,
                                std::uint32_t) {
        return {Status::Unsupported, 0};
    }
    virtual Status submitLin(std::uint64_t, std::uint32_t, const LinFrame&) {
        return Status::Unsupported;
    }
    virtual Status readLinEvents(std::uint64_t, std::uint32_t, std::uint32_t,
                                   std::vector<LinEvent>& events) {
        events.clear();
        return Status::Unsupported;
    }
    virtual Status getLinStatus(std::uint64_t, std::uint32_t, LinStatus&) {
        return Status::Unsupported;
    }
    virtual Status configureLinSlaveResponse(std::uint64_t, std::uint32_t,
                                                const LinSlaveResponse&) {
        return Status::Unsupported;
    }
};

}  // namespace Ucal::backend
