#pragma once
#include "ucal/Backend.hpp"
#include <memory>

namespace Ucal::J2534Backend {
// This adapter is instantiated only inside its owned architecture-matched worker.
class Backend final : public Ucal::Backend::BackendInterface {
  public:
    explicit Backend(std::wstring configurationPath = {});
    ~Backend() override;
    Status initialize() override;
    Status enumerate(std::vector<DeviceDescriptor> &devices) override;
    Status enumerateEndpoints(std::vector<EndpointDescriptor> &endpoints) override;
    OpenResult open(const DeviceId &, const CanChannelConfig &, std::uint32_t owner) override;
    Status close(std::uint64_t token, std::uint32_t owner) override;
    Status write(std::uint64_t token, std::uint32_t owner, const CanFrame &) override;
    Status submit(std::uint64_t token, std::uint32_t owner, const CanFrame &) override;
    Status submitBatch(std::uint64_t token, std::uint32_t owner, const std::vector<CanFrame> &,
                        std::uint32_t &accepted) override;
    Status read(std::uint64_t token, std::uint32_t owner, std::uint32_t capacity,
                std::vector<CanFrame> &frames) override;
    Status readEvents(std::uint64_t token, std::uint32_t owner, std::uint32_t capacity,
                       std::vector<CanEvent> &events) override;
    Status getStatus(std::uint64_t token, std::uint32_t owner, ChannelStatus &) override;
    void pruneDeadSessions() override;
    std::string backendFamily() const override {
        return "j2534";
    }
    BackendRuntimeIdentity runtimeIdentity() const override;

  private:
    struct Implementation;
    std::unique_ptr<Implementation> implementation_;
};
} // namespace Ucal::j2534_backend
