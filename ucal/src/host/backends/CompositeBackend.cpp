#include "backends/CompositeBackend.hpp"

#include <windows.h>

#include <algorithm>
#include <iterator>
#include <mutex>
#include <utility>
#include <map>

namespace Ucal::Backend {
namespace {

bool processAlive(const std::uint32_t pid) {
    const auto process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (process == nullptr) return GetLastError() == ERROR_ACCESS_DENIED;
    const auto alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    return alive;
}

}  // namespace

struct Composite::Implementation {
    struct Route {
        std::uint64_t token{};
        std::uint32_t ownerPid{};
        BackendInterface* backend{};
    };

    explicit Implementation(std::vector<std::unique_ptr<BackendInterface>> values)
        : backends(std::move(values)) {}

    BackendInterface* route(const std::uint64_t token, const std::uint32_t ownerPid) const {
        std::lock_guard<std::mutex> lock(mutex);
        const auto found = std::find_if(routes.begin(), routes.end(), [&](const auto& item) {
            return item.token == token && item.ownerPid == ownerPid;
        });
        return found == routes.end() ? nullptr : found->backend;
    }

    bool addRoute(const OpenResult& result, const std::uint32_t ownerPid,
                   BackendInterface* const backend) {
        if (result.status != Status::Ok || result.leaseToken == 0u) return false;
        std::lock_guard<std::mutex> lock(mutex);
        if (std::any_of(routes.begin(), routes.end(), [&](const auto& item) {
                return item.token == result.leaseToken;
            })) return false;
        routes.push_back({result.leaseToken, ownerPid, backend});
        return true;
    }

    void removeRoute(const std::uint64_t token, const std::uint32_t ownerPid) {
        std::lock_guard<std::mutex> lock(mutex);
        routes.erase(std::remove_if(routes.begin(), routes.end(), [&](const auto& item) {
            return item.token == token && item.ownerPid == ownerPid;
        }), routes.end());
    }

    std::vector<std::unique_ptr<BackendInterface>> backends;
    std::vector<Route> routes;
    std::map<DeviceId, std::pair<BackendInterface*, EndpointDescriptor>> catalog;
    std::pair<BackendInterface*, EndpointDescriptor> catalogEntry(const DeviceId& id) const {
        std::lock_guard<std::mutex> lock(mutex);
        const auto found = catalog.find(id);
        return found == catalog.end() ? std::pair<BackendInterface*, EndpointDescriptor>{} : found->second;
    }
    mutable std::mutex mutex;
};

Composite::Composite(std::vector<std::unique_ptr<BackendInterface>> backends)
    : implementation_(std::make_unique<Implementation>(std::move(backends))) {}

Composite::~Composite() = default;

void Composite::addBackend(std::unique_ptr<BackendInterface> backend) {
    implementation_->backends.push_back(std::move(backend));
}

Status Composite::configurePlugins(const std::wstring& path) {
    for(const auto& backend:implementation_->backends){
        const auto s=backend->configurePlugins(path);
        if(s!=Status::Unsupported)return s;
    }
    return Status::Unsupported;
}

Status Composite::initialize() {
    Status last = Status::NotFound;
    bool available = false;
    for (const auto& backend : implementation_->backends) {
        const auto status = backend->initialize();
        if (status == Status::Ok) available = true;
        else last = status;
    }
    return available ? Status::Ok : last;
}

Status Composite::enumerateEndpoints(std::vector<EndpointDescriptor>& endpoints) {
    endpoints.clear();
    std::map<DeviceId, std::pair<BackendInterface*, EndpointDescriptor>> catalog;
    Status last = Status::NotFound;
    bool available = false;
    for (const auto& backend : implementation_->backends) {
        std::vector<EndpointDescriptor> current;
        const auto status = backend->enumerateEndpoints(current);
        if (status != Status::Ok) {
            last = status;
            continue;
        }
        available = true;
        for (auto& endpoint : current) {
            if (std::any_of(endpoints.begin(), endpoints.end(), [&](const auto& existing) {
                    return existing.id == endpoint.id;
                })) {
                std::lock_guard<std::mutex> lock(implementation_->mutex);
                implementation_->catalog.clear();
                return Status::InternalError;
            }
            catalog.emplace(endpoint.id, std::make_pair(backend.get(), endpoint));
            endpoints.push_back(std::move(endpoint));
        }
    }
    { std::lock_guard<std::mutex> lock(implementation_->mutex); implementation_->catalog = std::move(catalog); }
    return available ? Status::Ok : last;
}

Status Composite::enumerate(std::vector<DeviceDescriptor>& devices) {
    devices.clear();
    std::vector<EndpointDescriptor> endpoints;
    const auto status = enumerateEndpoints(endpoints);
    if (status != Status::Ok) return status;
    for (const auto& endpoint : endpoints) {
        if (endpoint.busType != BusType::Can) continue;
        DeviceDescriptor device{};
        device.id = endpoint.id;
        device.name = endpoint.name;
        device.model = endpoint.model;
        device.serial = endpoint.serial;
        device.channelCount = 1u;
        device.capabilities = endpoint.capabilities;
        device.state = endpoint.state;
        devices.push_back(std::move(device));
    }
    return Status::Ok;
}

void Composite::enumerateBackendHealth(std::vector<BackendHealth>& health) {
    health.clear();
    for (const auto& backend : implementation_->backends) {
        std::vector<BackendHealth> current;
        backend->enumerateBackendHealth(current);
        health.insert(health.end(), std::make_move_iterator(current.begin()),
                      std::make_move_iterator(current.end()));
    }
}

OpenResult Composite::open(const DeviceId& id, const CanChannelConfig& config,
                           const std::uint32_t ownerPid) {
    auto selected = implementation_->catalogEntry(id);
    if (!selected.first) {
        std::vector<EndpointDescriptor> endpoints;
        enumerateEndpoints(endpoints);
        selected = implementation_->catalogEntry(id);
    }
    if (!selected.first || selected.second.busType != BusType::Can) return {Status::NotFound, 0};
    std::vector<EndpointDescriptor> current;
    const auto listed = selected.first->enumerateEndpoints(current);
    if (listed != Status::Ok) return {listed, 0};
    const auto live = std::find_if(current.begin(), current.end(), [&](const auto& endpoint) { return endpoint.id == id && endpoint.busType == BusType::Can; });
    if (live == current.end()) return {Status::NotFound, 0};
    if (!endpointMeetsDeviceContract(*live)) return {Status::Unsupported, 0};
    // The selected backend validates live presence/configuration. Never scan or
    // reopen unrelated vendors just to route an already discovered stable ID.
    const auto result = selected.first->open(id, config, ownerPid);
    if (result.status == Status::Ok && !implementation_->addRoute(result, ownerPid, selected.first)) {
        selected.first->close(result.leaseToken, ownerPid);
        return {Status::InternalError, 0};
    }
    return result;
}

OpenResult Composite::openLin(const EndpointId& id, const LinChannelConfig& config,
                               const std::uint32_t ownerPid) {
    auto selected = implementation_->catalogEntry(id);
    if (!selected.first) {
        std::vector<EndpointDescriptor> endpoints;
        enumerateEndpoints(endpoints);
        selected = implementation_->catalogEntry(id);
    }
    if (!selected.first || selected.second.busType != BusType::Lin) return {Status::NotFound, 0};
    std::vector<EndpointDescriptor> current;
    const auto listed = selected.first->enumerateEndpoints(current);
    if (listed != Status::Ok) return {listed, 0};
    if (std::none_of(current.begin(), current.end(), [&](const auto& endpoint) { return endpoint.id == id && endpoint.busType == BusType::Lin; })) return {Status::NotFound, 0};
    const auto result = selected.first->openLin(id, config, ownerPid);
    if (result.status == Status::Ok && !implementation_->addRoute(result, ownerPid, selected.first)) {
        selected.first->close(result.leaseToken, ownerPid);
        return {Status::InternalError, 0};
    }
    return result;
}

Status Composite::close(const std::uint64_t token, const std::uint32_t ownerPid) {
    auto* const backend = implementation_->route(token, ownerPid);
    if (backend == nullptr) return Status::NotFound;
    const auto status = backend->close(token, ownerPid);
    if (status == Status::Ok || status == Status::NotFound) {
        implementation_->removeRoute(token, ownerPid);
    }
    return status;
}

Status Composite::write(const std::uint64_t token, const std::uint32_t ownerPid,
                        const CanFrame& frame) {
    auto* const backend = implementation_->route(token, ownerPid);
    return backend == nullptr ? Status::NotFound : backend->write(token, ownerPid, frame);
}

Status Composite::submit(const std::uint64_t token, const std::uint32_t ownerPid,
                         const CanFrame& frame) {
    auto* const backend = implementation_->route(token, ownerPid);
    return backend == nullptr ? Status::NotFound : backend->submit(token, ownerPid, frame);
}

Status Composite::read(const std::uint64_t token, const std::uint32_t ownerPid,
                       const std::uint32_t capacity, std::vector<CanFrame>& frames) {
    auto* const backend = implementation_->route(token, ownerPid);
    return backend == nullptr ? Status::NotFound :
        backend->read(token, ownerPid, capacity, frames);
}

Status Composite::readEvents(const std::uint64_t token, const std::uint32_t ownerPid,
                              const std::uint32_t capacity,
                              std::vector<CanEvent>& events) {
    auto* const backend = implementation_->route(token, ownerPid);
    return backend == nullptr ? Status::NotFound :
        backend->readEvents(token, ownerPid, capacity, events);
}

Status Composite::submitLin(const std::uint64_t token, const std::uint32_t ownerPid,
                             const LinFrame& frame) {
    auto* const backend = implementation_->route(token, ownerPid);
    return backend == nullptr ? Status::NotFound :
        backend->submitLin(token, ownerPid, frame);
}

Status Composite::readLinEvents(const std::uint64_t token,
                                  const std::uint32_t ownerPid,
                                  const std::uint32_t capacity,
                                  std::vector<LinEvent>& events) {
    auto* const backend = implementation_->route(token, ownerPid);
    return backend == nullptr ? Status::NotFound :
        backend->readLinEvents(token, ownerPid, capacity, events);
}

Status Composite::getStatus(const std::uint64_t token, const std::uint32_t ownerPid,
                             ChannelStatus& status) {
    auto* const backend = implementation_->route(token, ownerPid);
    return backend == nullptr ? Status::NotFound : backend->getStatus(token, ownerPid, status);
}

Status Composite::getLinStatus(const std::uint64_t token,
                                 const std::uint32_t ownerPid, LinStatus& status) {
    auto* const backend = implementation_->route(token, ownerPid);
    return backend == nullptr ? Status::NotFound :
        backend->getLinStatus(token, ownerPid, status);
}

Status Composite::configureLinSlaveResponse(const std::uint64_t token,
                                               const std::uint32_t ownerPid,
                                               const LinSlaveResponse& response) {
    auto* const backend = implementation_->route(token, ownerPid);
    return backend == nullptr ? Status::NotFound :
        backend->configureLinSlaveResponse(token, ownerPid, response);
}

void Composite::pruneDeadSessions() {
    for (const auto& backend : implementation_->backends) backend->pruneDeadSessions();
    std::lock_guard<std::mutex> lock(implementation_->mutex);
    implementation_->routes.erase(
        std::remove_if(implementation_->routes.begin(), implementation_->routes.end(),
            [](const auto& route) { return !processAlive(route.ownerPid); }),
        implementation_->routes.end());
}

}  // namespace Ucal::backend
