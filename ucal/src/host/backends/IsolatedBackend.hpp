#pragma once

#include "ucal/Backend.hpp"
#include "backends/PluginBackend.hpp"

#include <memory>

namespace Ucal::Backend {

class IsolatedBackend final : public Ucal::Backend::BackendInterface {
public:
    explicit IsolatedBackend(std::string family);
    explicit IsolatedBackend(PluginSpec plugin);
    ~IsolatedBackend() override;
    bool hasActiveSessions() const noexcept;

    Status initialize() override;
    Status enumerate(std::vector<DeviceDescriptor>& devices) override;
    Status enumerateEndpoints(std::vector<EndpointDescriptor>& endpoints) override;
    OpenResult open(const DeviceId& id, const CanChannelConfig& config,
                    std::uint32_t ownerPid) override;
    Status close(std::uint64_t token, std::uint32_t ownerPid) override;
    Status write(std::uint64_t token, std::uint32_t ownerPid,
                 const CanFrame& frame) override;
    Status submit(std::uint64_t token, std::uint32_t ownerPid,
                  const CanFrame& frame) override;
    Status submitBatch(std::uint64_t token, std::uint32_t ownerPid,
                        const std::vector<CanFrame>& frames,
                        std::uint32_t& acceptedCount) override;
    Status read(std::uint64_t token, std::uint32_t ownerPid,
                std::uint32_t capacity, std::vector<CanFrame>& frames) override;
    Status readEvents(std::uint64_t token, std::uint32_t ownerPid,
                       std::uint32_t capacity,
                       std::vector<CanEvent>& events) override;
    Status getStatus(std::uint64_t token, std::uint32_t ownerPid,
                      ChannelStatus& status) override;
    void pruneDeadSessions() override;
    std::string backendFamily() const override;
    void enumerateBackendHealth(std::vector<BackendHealth>& health) override;

    OpenResult openLin(const EndpointId& id, const LinChannelConfig& config,
                        std::uint32_t ownerPid) override;
    Status submitLin(std::uint64_t token, std::uint32_t ownerPid,
                      const LinFrame& frame) override;
    Status readLinEvents(std::uint64_t token, std::uint32_t ownerPid,
                           std::uint32_t capacity,
                           std::vector<LinEvent>& events) override;
    Status getLinStatus(std::uint64_t token, std::uint32_t ownerPid,
                          LinStatus& status) override;
    Status configureLinSlaveResponse(std::uint64_t token,
                                        std::uint32_t ownerPid,
                                        const LinSlaveResponse& response) override;

private:
    struct Implementation;
    std::unique_ptr<Implementation> implementation_;
};

}  // namespace Ucal::backend
