#pragma once

#include "ucal/Backend.hpp"

#include <memory>

namespace Ucal::PcanBackend {

class Backend final : public Ucal::Backend::BackendInterface {
public:
    Backend();
    ~Backend() override;
    Backend(const Backend&) = delete;
    Backend& operator=(const Backend&) = delete;

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
    Status read(std::uint64_t token, std::uint32_t ownerPid,
                std::uint32_t capacity, std::vector<CanFrame>& frames) override;
    Status readEvents(std::uint64_t token, std::uint32_t ownerPid,
                       std::uint32_t capacity,
                       std::vector<CanEvent>& events) override;
    Status getStatus(std::uint64_t token, std::uint32_t ownerPid,
                      ChannelStatus& status) override;
    void pruneDeadSessions() override;
    std::string backendFamily() const override { return "pcan-basic"; }

private:
    struct Implementation;
    std::unique_ptr<Implementation> implementation_;
};

}  // namespace Ucal::pcan_backend
