#pragma once

#include "ucal/Backend.hpp"

#include <memory>
#include <vector>

namespace Ucal::Backend {

class Composite final : public BackendInterface {
public:
    explicit Composite(std::vector<std::unique_ptr<BackendInterface>> backends);
    ~Composite() override;
    Composite(const Composite&) = delete;
    Composite& operator=(const Composite&) = delete;

    Status initialize() override;
    Status enumerate(std::vector<DeviceDescriptor>& devices) override;
    Status enumerateEndpoints(std::vector<EndpointDescriptor>& endpoints) override;
    OpenResult open(const DeviceId& id, const CanChannelConfig& config,
                    std::uint32_t ownerPid) override;
    OpenResult openLin(const EndpointId& id, const LinChannelConfig& config,
                        std::uint32_t ownerPid) override;
    Status close(std::uint64_t token, std::uint32_t ownerPid) override;
    Status write(std::uint64_t token, std::uint32_t ownerPid,
                 const CanFrame& frame) override;
    Status submit(std::uint64_t token, std::uint32_t ownerPid,
                  const CanFrame& frame) override;
    Status read(std::uint64_t token, std::uint32_t ownerPid,
                std::uint32_t capacity, std::vector<CanFrame>& frames) override;
    Status readEvents(std::uint64_t token, std::uint32_t ownerPid,
                       std::uint32_t capacity, std::vector<CanEvent>& events) override;
    Status submitLin(std::uint64_t token, std::uint32_t ownerPid,
                      const LinFrame& frame) override;
    Status readLinEvents(std::uint64_t token, std::uint32_t ownerPid,
                           std::uint32_t capacity,
                           std::vector<LinEvent>& events) override;
    Status getStatus(std::uint64_t token, std::uint32_t ownerPid,
                      ChannelStatus& status) override;
    Status getLinStatus(std::uint64_t token, std::uint32_t ownerPid,
                          LinStatus& status) override;
    Status configureLinSlaveResponse(std::uint64_t token,
                                        std::uint32_t ownerPid,
                                        const LinSlaveResponse& response) override;
    void pruneDeadSessions() override;
    std::string backendFamily() const override { return "composite"; }
    void enumerateBackendHealth(std::vector<BackendHealth>& health) override;
    // Registry owner serializes catalog changes against its own operations.
    void addBackend(std::unique_ptr<BackendInterface> backend);
    Status configurePlugins(const std::wstring& path) override;

private:
    struct Implementation;
    std::unique_ptr<Implementation> implementation_;
};

}  // namespace Ucal::backend
