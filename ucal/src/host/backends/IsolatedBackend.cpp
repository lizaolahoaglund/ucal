#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include "backends/IsolatedBackend.hpp"

#include "ucal/Client.hpp"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Ucal::Backend {
namespace {

bool processAlive(const DWORD pid) {
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (process == nullptr) return GetLastError() == ERROR_ACCESS_DENIED;
    const bool alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    return alive;
}

std::uint64_t randomValue() {
    std::uint64_t value = 0;
    while (value == 0u) {
        if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&value), sizeof(value),
                            BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
            return 0u;
        }
    }
    return value;
}

std::wstring executablePath() {
    std::vector<wchar_t> buffer(1024u);
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                                static_cast<DWORD>(buffer.size()));
        if (length == 0u) return {};
        if (length < buffer.size() - 1u) return std::wstring(buffer.data(), length);
        if (buffer.size() >= 32768u) return {};
        buffer.resize((std::min<std::size_t>)(buffer.size() * 2u, 32768u));
    }
}

}  // namespace

class WorkerClientFactory final {
public:
    static Client make(const std::wstring& pipeName, const std::uint32_t ownerPid,
                       const std::uint32_t workerPid, const std::uint32_t timeoutMs) {
        return Client(pipeName, ownerPid, workerPid, timeoutMs);
    }
};

struct IsolatedBackend::Implementation {
    explicit Implementation(std::string value) : family(std::move(value)) {}
    std::string family;
    PluginSpec plugin;
    bool bounded() const { return family == "j2534" || family == "plugin"; }

    struct Route {
        std::uint64_t workerToken{};
        std::uint32_t ownerPid{};
    };

    std::mutex mutex;
    std::unordered_map<std::uint64_t, Route> routes;
    std::atomic_uint32_t activeRoutes{0};
    HANDLE process{nullptr};
    HANDLE job{nullptr};
    DWORD workerPid{};
    std::wstring pipeName;
    std::chrono::steady_clock::time_point capabilityObservationUntil{};
    std::chrono::steady_clock::time_point catalogIdleUntil{};

    void logFailure(const char* stage, const DWORD error = 0u) const {
        DWORD exitCode = STILL_ACTIVE;
        if (process != nullptr) GetExitCodeProcess(process, &exitCode);
        std::cerr << "{\"event\":\"backend_worker_failure\",\"stage\":\"" << stage
                  << "\",\"win32Error\":" << error << ",\"workerPid\":"
                  << workerPid << ",\"workerExitCode\":" << exitCode << "}"
                  << std::endl;
    }

    ~Implementation() { stopWorker(true); }

    bool workerAlive() const {
        if (process == nullptr) return false;
        DWORD exitCode = 0;
        return GetExitCodeProcess(process, &exitCode) && exitCode == STILL_ACTIVE;
    }

    void releaseWorkerHandles() {
        if (process != nullptr) CloseHandle(process);
        if (job != nullptr) CloseHandle(job);
        process = nullptr;
        job = nullptr;
        workerPid = 0u;
        pipeName.clear();
    }

    void stopWorker(const bool force) {
        if (process == nullptr) return;
        if (!force && workerAlive()) {
            auto client = WorkerClientFactory::make(pipeName, GetCurrentProcessId(), workerPid, bounded() ? 1000u : 5000u);
            client.shutdownForTest();
            if (WaitForSingleObject(process, bounded() ? 500u : 5000u) != WAIT_OBJECT_0) {
                TerminateProcess(process, 0xEEu);
                WaitForSingleObject(process, bounded() ? 500u : 5000u);
            }
        } else if (force && workerAlive()) {
            TerminateProcess(process, 0xEEu);
            WaitForSingleObject(process, bounded() ? 500u : 5000u);
        }
        releaseWorkerHandles();
    }

    void invalidateWorker() {
        catalogIdleUntil = {};
        routes.clear();
        activeRoutes.store(0u);
        stopWorker(true);
    }

    bool startWorker() {
        if (workerAlive()) return true;
        if (process != nullptr) invalidateWorker();

        const auto nonce = randomValue();
        auto image = executablePath();
        if (family == "j2534") {
            std::vector<wchar_t> configured(32768u);
            auto length = GetEnvironmentVariableW(L"UCAL_J2534_WORKER_PATH",
                configured.data(), static_cast<DWORD>(configured.size()));
            if (length >= configured.size()) return false;
            if (length == 0u) {
                std::vector<wchar_t> configPath(32768u);
                const auto configLength = GetEnvironmentVariableW(L"UCAL_J2534_CONFIG",
                    configPath.data(), static_cast<DWORD>(configPath.size()));
                if (configLength >= configPath.size()) return false;
                if (configLength != 0u)
                    length = GetPrivateProfileStringW(L"provider", L"worker_path", L"", configured.data(),
                        static_cast<DWORD>(configured.size()), configPath.data());
                if (length >= configured.size() - 1u) return false;
            }
            if (length != 0u) {
                image.assign(configured.data(), length);
                // Worker selection is explicit; do not search PATH/current directory.
                if (image.size() < 3u || image[1] != L':' ||
                    (image[2] != L'\\' && image[2] != L'/')) return false;
            }
        }
        if (family == "plugin" && !plugin.worker.empty()) image = plugin.worker;
        const std::wstring wideFamily(family.begin(), family.end());
        if (nonce == 0u || image.empty()) return false;
        pipeName = L"\\\\.\\pipe\\ucal." + wideFamily + L".worker." +
                    std::to_wstring(GetCurrentProcessId()) + L"." +
                    std::to_wstring(nonce);

        job = CreateJobObjectW(nullptr, nullptr);
        if (job == nullptr) {
            logFailure("create_job", GetLastError());
            pipeName.clear();
            return false;
        }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                     &limits, sizeof(limits))) {
            logFailure("configure_job", GetLastError());
            releaseWorkerHandles();
            return false;
        }

        const bool native = family != "j2534" && family != "plugin";
        std::wstring command = L"\"" + image + L"\" --" + (native ? std::wstring(L"native") : wideFamily) + L"-worker \"" + pipeName +
                               L"\" " + std::to_wstring(GetCurrentProcessId());
        if (native) command += L" \"" + wideFamily + L"\"";
        if(family=="plugin") {
            const std::wstring id(plugin.id.begin(),plugin.id.end()), kind(plugin.family.begin(),plugin.family.end());
            command += L" \""+plugin.library+L"\" \""+plugin.configuration+L"\" \""+id+L"\" \""+kind+L"\" "+(plugin.preserveIdentity ? L"1" : L"0");
        }
        std::vector<wchar_t> mutableCommand(command.begin(), command.end());
        mutableCommand.push_back(L'\0');
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION processInfo{};
        if (!CreateProcessW(image.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE,
                            CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr,
                            &startup, &processInfo)) {
            logFailure("create_process", GetLastError());
            releaseWorkerHandles();
            return false;
        }
        process = processInfo.hProcess;
        workerPid = processInfo.dwProcessId;
        if (!AssignProcessToJobObject(job, process)) {
            logFailure("assign_job", GetLastError());
            TerminateProcess(process, 0xEFu);
            WaitForSingleObject(process, bounded() ? 500u : 5000u);
            CloseHandle(processInfo.hThread);
            releaseWorkerHandles();
            return false;
        }
        if (ResumeThread(processInfo.hThread) == static_cast<DWORD>(-1)) {
            logFailure("resume_worker", GetLastError());
            TerminateProcess(process, 0xEFu);
            WaitForSingleObject(process, bounded() ? 500u : 5000u);
            CloseHandle(processInfo.hThread);
            releaseWorkerHandles();
            return false;
        }
        CloseHandle(processInfo.hThread);
        return true;
    }

    Client client(const std::uint32_t ownerPid, const bool opening = false) const {
        return WorkerClientFactory::make(pipeName, ownerPid, workerPid,
            bounded() ? (opening ? 3000u : 1000u) : 5000u);
    }

    void finishIdle() {
        if (routes.empty() && std::chrono::steady_clock::now() >= capabilityObservationUntil &&
            std::chrono::steady_clock::now() >= catalogIdleUntil)
            stopWorker(false);
    }

    Route* findRoute(const std::uint64_t token, const std::uint32_t ownerPid) {
        const auto found = routes.find(token);
        if (found == routes.end() || found->second.ownerPid != ownerPid) return nullptr;
        return &found->second;
    }

    std::uint64_t newPublicToken() const {
        for (int attempt = 0; attempt < 8; ++attempt) {
            const auto candidate = randomValue();
            if (candidate != 0u && routes.find(candidate) == routes.end()) return candidate;
        }
        return 0u;
    }

    Status observe(const Status status) {
        if (status == Status::TransportError) {
            logFailure("transport");
            invalidateWorker();
        }
        return status;
    }
};

IsolatedBackend::IsolatedBackend(std::string family)
    : implementation_(std::make_unique<Implementation>(std::move(family))) {}

IsolatedBackend::IsolatedBackend(PluginSpec plugin)
    : implementation_(std::make_unique<Implementation>("plugin")) { implementation_->plugin=std::move(plugin); }

std::string IsolatedBackend::backendFamily() const {
    return implementation_->family=="plugin"?implementation_->plugin.family:implementation_->family;
}

IsolatedBackend::~IsolatedBackend() = default;
bool IsolatedBackend::hasActiveSessions() const noexcept { return implementation_->activeRoutes.load() != 0u; }

Status IsolatedBackend::initialize() {
    std::lock_guard<std::mutex> lock(implementation_->mutex);
    if (!implementation_->startWorker()) return Status::TransportError;
    const auto status = implementation_->observe(
        implementation_->client(GetCurrentProcessId()).ping());
    if (status == Status::Ok && implementation_->family == "plugin") implementation_->catalogIdleUntil = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    implementation_->finishIdle();
    return status;
}

Status IsolatedBackend::enumerate(std::vector<DeviceDescriptor>& devices) {
    std::lock_guard<std::mutex> lock(implementation_->mutex);
    devices.clear();
    if (!implementation_->startWorker()) return Status::TransportError;
    const auto status = implementation_->observe(
        implementation_->client(GetCurrentProcessId(), true).enumerate(devices));
    if (status == Status::Ok && implementation_->family == "plugin") implementation_->catalogIdleUntil = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    implementation_->finishIdle();
    return status;
}

Status IsolatedBackend::enumerateEndpoints(std::vector<EndpointDescriptor>& endpoints) {
    std::lock_guard<std::mutex> lock(implementation_->mutex);
    endpoints.clear();
    if (!implementation_->startWorker()) return Status::TransportError;
    const auto status = implementation_->observe(
        implementation_->client(GetCurrentProcessId(), true).enumerateEndpoints(endpoints));
    if (status == Status::Ok && implementation_->family == "plugin") implementation_->catalogIdleUntil = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    implementation_->finishIdle();
    return status;
}

void IsolatedBackend::enumerateBackendHealth(std::vector<BackendHealth>& health) {
    std::lock_guard<std::mutex> lock(implementation_->mutex);
    health.clear();
    if (!implementation_->startWorker()) {
        health.push_back({backendFamily(), Status::TransportError, 0u});
        return;
    }
    const auto status = implementation_->observe(
        implementation_->client(GetCurrentProcessId(), true).enumerateBackendHealth(health));
    if (status == Status::Ok && implementation_->family == "plugin") implementation_->catalogIdleUntil = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    if (status != Status::Ok) health = {{backendFamily(), status, 0u}};
    implementation_->finishIdle();
}

OpenResult IsolatedBackend::open(const DeviceId& id, const CanChannelConfig& config,
                                 const std::uint32_t ownerPid) {
    std::lock_guard<std::mutex> lock(implementation_->mutex);
    // A rejected idle configuration is observable briefly, but a new Open is a
    // fresh generation and must not reuse its runtime/device assumptions.
    if (implementation_->routes.empty() &&
        implementation_->capabilityObservationUntil != std::chrono::steady_clock::time_point{})
        implementation_->stopWorker(false);
    implementation_->capabilityObservationUntil = {};
    implementation_->catalogIdleUntil = {};
    if (!implementation_->startWorker()) return {Status::TransportError, 0u};
    const auto opened = implementation_->client(ownerPid, true).openCanChannel(id, config);
    if (opened.status != Status::Ok) {
        implementation_->observe(opened.status);
        // Keep a bounded, handle-free observation window after a rejected
        // configuration so Manager can read the parameter result. The failed
        // Open has already released every vendor handle; faults get no grace.
        if (implementation_->bounded() &&
            (opened.status == Status::Unsupported || opened.status == Status::InvalidArgument))
            implementation_->capabilityObservationUntil = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        implementation_->finishIdle();
        return opened;
    }
    const auto token = implementation_->newPublicToken();
    if (token == 0u) {
        implementation_->client(ownerPid).closeDevice(opened.leaseToken);
        implementation_->finishIdle();
        return {Status::InternalError, 0u};
    }
    implementation_->routes.emplace(token, Implementation::Route{opened.leaseToken, ownerPid});
    implementation_->activeRoutes.store(static_cast<std::uint32_t>(implementation_->routes.size()));
    return {Status::Ok, token};
}

Status IsolatedBackend::close(const std::uint64_t token, const std::uint32_t ownerPid) {
    std::lock_guard<std::mutex> lock(implementation_->mutex);
    auto* route = implementation_->findRoute(token, ownerPid);
    if (route == nullptr) return Status::NotFound;
    implementation_->catalogIdleUntil = {};
    const auto workerToken = route->workerToken;
    const auto status = implementation_->observe(
        implementation_->client(ownerPid).closeDevice(workerToken));
    if (status != Status::TransportError) implementation_->routes.erase(token);
    implementation_->activeRoutes.store(static_cast<std::uint32_t>(implementation_->routes.size()));
    implementation_->finishIdle();
    return status;
}

Status IsolatedBackend::write(const std::uint64_t token, const std::uint32_t ownerPid,
                              const CanFrame& frame) {
    std::lock_guard<std::mutex> lock(implementation_->mutex);
    const auto* route = implementation_->findRoute(token, ownerPid);
    if (route == nullptr) return Status::NotFound;
    return implementation_->observe(
        implementation_->client(ownerPid).writeCan(route->workerToken, frame));
}

Status IsolatedBackend::submit(const std::uint64_t token, const std::uint32_t ownerPid,
                               const CanFrame& frame) {
    std::lock_guard<std::mutex> lock(implementation_->mutex);
    const auto* route = implementation_->findRoute(token, ownerPid);
    if (route == nullptr) return Status::NotFound;
    return implementation_->observe(
        implementation_->client(ownerPid).submitCan(route->workerToken, frame));
}

Status IsolatedBackend::submitBatch(const std::uint64_t token,
                                     const std::uint32_t ownerPid,
                                     const std::vector<CanFrame>& frames,
                                     std::uint32_t& acceptedCount) {
    std::lock_guard<std::mutex> lock(implementation_->mutex);
    acceptedCount = 0u;
    const auto* route = implementation_->findRoute(token, ownerPid);
    if (route == nullptr) return Status::NotFound;
    return implementation_->observe(implementation_->client(ownerPid).submitCanBatch(
        route->workerToken, frames, acceptedCount));
}

Status IsolatedBackend::read(const std::uint64_t token, const std::uint32_t ownerPid,
                             const std::uint32_t capacity,
                             std::vector<CanFrame>& frames) {
    std::lock_guard<std::mutex> lock(implementation_->mutex);
    frames.clear();
    const auto* route = implementation_->findRoute(token, ownerPid);
    if (route == nullptr) return Status::NotFound;
    return implementation_->observe(implementation_->client(ownerPid).readCan(
        route->workerToken, capacity, frames));
}

Status IsolatedBackend::readEvents(const std::uint64_t token,
                                    const std::uint32_t ownerPid,
                                    const std::uint32_t capacity,
                                    std::vector<CanEvent>& events) {
    std::lock_guard<std::mutex> lock(implementation_->mutex);
    events.clear();
    const auto* route = implementation_->findRoute(token, ownerPid);
    if (route == nullptr) return Status::NotFound;
    return implementation_->observe(implementation_->client(ownerPid).readCanEvents(
        route->workerToken, capacity, events));
}

Status IsolatedBackend::getStatus(const std::uint64_t token,
                                   const std::uint32_t ownerPid,
                                   ChannelStatus& statusInfo) {
    std::lock_guard<std::mutex> lock(implementation_->mutex);
    const auto* route = implementation_->findRoute(token, ownerPid);
    if (route == nullptr) return Status::NotFound;
    return implementation_->observe(implementation_->client(ownerPid).getChannelStatus(
        route->workerToken, statusInfo));
}

void IsolatedBackend::pruneDeadSessions() {
    // Maintenance for one worker must not make an unrelated instance wait for
    // that worker's in-flight vendor call. The next control request retries.
    std::unique_lock<std::mutex> lock(implementation_->mutex, std::try_to_lock);
    if (!lock.owns_lock()) return;
    for (auto route = implementation_->routes.begin();
         route != implementation_->routes.end();) {
        if (processAlive(route->second.ownerPid)) {
            ++route;
            continue;
        }
        if (implementation_->workerAlive()) {
            implementation_->client(route->second.ownerPid)
                .closeDevice(route->second.workerToken);
        }
        route = implementation_->routes.erase(route);
    }
    implementation_->activeRoutes.store(static_cast<std::uint32_t>(implementation_->routes.size()));
    implementation_->finishIdle();
}

OpenResult IsolatedBackend::openLin(const EndpointId& id,
                                     const LinChannelConfig& config,
                                     const std::uint32_t ownerPid) {
    std::lock_guard<std::mutex> lock(implementation_->mutex);
    if (!implementation_->startWorker()) return {Status::TransportError, 0u};
    const auto opened = implementation_->client(ownerPid).openLinChannel(id, config);
    if (opened.status != Status::Ok) {
        implementation_->observe(opened.status);
        implementation_->finishIdle();
        return opened;
    }
    const auto token = implementation_->newPublicToken();
    if (token == 0u) {
        implementation_->client(ownerPid).closeDevice(opened.leaseToken);
        implementation_->finishIdle();
        return {Status::InternalError, 0u};
    }
    implementation_->routes.emplace(token, Implementation::Route{opened.leaseToken, ownerPid});
    implementation_->activeRoutes.store(static_cast<std::uint32_t>(implementation_->routes.size()));
    return {Status::Ok, token};
}

Status IsolatedBackend::submitLin(const std::uint64_t token,
                                   const std::uint32_t ownerPid,
                                   const LinFrame& frame) {
    std::lock_guard<std::mutex> lock(implementation_->mutex);
    const auto* route = implementation_->findRoute(token, ownerPid);
    if (route == nullptr) return Status::NotFound;
    return implementation_->observe(
        implementation_->client(ownerPid).submitLin(route->workerToken, frame));
}

Status IsolatedBackend::readLinEvents(const std::uint64_t token,
                                        const std::uint32_t ownerPid,
                                        const std::uint32_t capacity,
                                        std::vector<LinEvent>& events) {
    std::lock_guard<std::mutex> lock(implementation_->mutex);
    events.clear();
    const auto* route = implementation_->findRoute(token, ownerPid);
    if (route == nullptr) return Status::NotFound;
    return implementation_->observe(implementation_->client(ownerPid).readLinEvents(
        route->workerToken, capacity, events));
}

Status IsolatedBackend::getLinStatus(const std::uint64_t token,
                                       const std::uint32_t ownerPid,
                                       LinStatus& statusInfo) {
    std::lock_guard<std::mutex> lock(implementation_->mutex);
    const auto* route = implementation_->findRoute(token, ownerPid);
    if (route == nullptr) return Status::NotFound;
    return implementation_->observe(implementation_->client(ownerPid).getLinStatus(
        route->workerToken, statusInfo));
}

Status IsolatedBackend::configureLinSlaveResponse(
    const std::uint64_t token, const std::uint32_t ownerPid,
    const LinSlaveResponse& response) {
    std::lock_guard<std::mutex> lock(implementation_->mutex);
    const auto* route = implementation_->findRoute(token, ownerPid);
    if (route == nullptr) return Status::NotFound;
    return implementation_->observe(
        implementation_->client(ownerPid).configureLinSlaveResponse(
            route->workerToken, response));
}

}  // namespace Ucal::backend
