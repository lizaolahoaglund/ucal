#pragma once
#include "ucal/Backend.hpp"
#include <memory>
namespace Ucal::Backend {
// Consumes the validated control-plane catalog; each DLL instance owns a worker.
class PluginRegistry final : public BackendInterface {
  public:
    PluginRegistry();
    ~PluginRegistry() override;
    Status initialize() override;
    Status configurePlugins(const std::wstring &path) override;
    Status enumerate(std::vector<DeviceDescriptor> &) override;
    Status enumerateEndpoints(std::vector<EndpointDescriptor> &) override;
    OpenResult open(const DeviceId &, const CanChannelConfig &, std::uint32_t) override;
    Status close(std::uint64_t, std::uint32_t) override;
    Status write(std::uint64_t, std::uint32_t, const CanFrame &) override;
    Status submit(std::uint64_t, std::uint32_t, const CanFrame &) override;
    Status submitBatch(std::uint64_t, std::uint32_t, const std::vector<CanFrame> &,
                        std::uint32_t &) override;
    Status read(std::uint64_t, std::uint32_t, std::uint32_t, std::vector<CanFrame> &) override;
    Status readEvents(std::uint64_t, std::uint32_t, std::uint32_t, std::vector<CanEvent> &) override;
    Status getStatus(std::uint64_t, std::uint32_t, ChannelStatus &) override;
    void pruneDeadSessions() override;
    std::string backendFamily() const override { return "plugins"; }
    void enumerateBackendHealth(std::vector<BackendHealth> &) override;
    OpenResult openLin(const EndpointId &, const LinChannelConfig &, std::uint32_t) override;
    Status submitLin(std::uint64_t, std::uint32_t, const LinFrame &) override;
    Status readLinEvents(std::uint64_t, std::uint32_t, std::uint32_t, std::vector<LinEvent> &) override;
    Status getLinStatus(std::uint64_t, std::uint32_t, LinStatus &) override;
    Status configureLinSlaveResponse(std::uint64_t, std::uint32_t, const LinSlaveResponse &) override;

  private:
    struct Implementation;
    std::unique_ptr<Implementation> implementation_;
};
} // namespace Ucal::backend
