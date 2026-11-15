#include "ucal/Model.hpp"
#include "ucal/BackendDispatch.hpp"
#include "ucal/Protocol.hpp"
#include "ucal/Backend.hpp"
#include "ipc/BackendWorkerProtocol.hpp"
#include "backends/IsolatedBackend.hpp"
#include "backends/CompositeBackend.hpp"
#include "backends/PluginRegistry.hpp"
#include "backends/PluginBackend.hpp"
#include "backends/j2534/J2534Backend.hpp"

#include <windows.h>
#include <bcrypt.h>
#include <sddl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "Bcrypt.lib")

namespace {

using Lease = Ucal::Backend::Wire::LegacyLease;

using Deadline = std::chrono::steady_clock::time_point;

DWORD remainingTimeout(const Deadline deadline) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) return 0;
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - now).count();
    return static_cast<DWORD>((std::min<std::int64_t>)(remaining, MAXDWORD - 1));
}

bool transferExact(HANDLE handle, void* data, std::uint32_t bytes,
                    const bool write, const Deadline deadline) {
    auto* current = static_cast<std::byte*>(data);
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (event == nullptr) return false;
    while (bytes != 0) {
        ResetEvent(event);
        OVERLAPPED overlapped{};
        overlapped.hEvent = event;
        DWORD transferred = 0;
        const BOOL started = write
            ? WriteFile(handle, current, bytes, &transferred, &overlapped)
            : ReadFile(handle, current, bytes, &transferred, &overlapped);
        if (!started) {
            if (GetLastError() != ERROR_IO_PENDING) {
                CloseHandle(event);
                return false;
            }
            const DWORD timeout = remainingTimeout(deadline);
            if (timeout == 0 || WaitForSingleObject(event, timeout) != WAIT_OBJECT_0) {
                CancelIoEx(handle, &overlapped);
                GetOverlappedResult(handle, &overlapped, &transferred, TRUE);
                CloseHandle(event);
                return false;
            }
            if (!GetOverlappedResult(handle, &overlapped, &transferred, FALSE)) {
                CloseHandle(event);
                return false;
            }
        }
        if (transferred == 0) {
            CloseHandle(event);
            return false;
        }
        current += transferred;
        bytes -= transferred;
    }
    CloseHandle(event);
    return true;
}

bool readExact(HANDLE handle, void* data, const std::uint32_t bytes,
                const Deadline deadline) {
    return transferExact(handle, data, bytes, false, deadline);
}

bool writeExact(HANDLE handle, const void* data, const std::uint32_t bytes,
                 const Deadline deadline) {
    return transferExact(handle, const_cast<void*>(data), bytes, true, deadline);
}

bool processIsAlive(const DWORD pid) {
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (process == nullptr) return GetLastError() == ERROR_ACCESS_DENIED;
    const bool alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    return alive;
}

bool currentUserSecurity(PSECURITY_DESCRIPTOR& descriptor, SECURITY_ATTRIBUTES& attributes) {
    const auto sid = Ucal::Protocol::currentUserSidString();
    if (sid == L"unknown") return false;
    const std::wstring sddl = L"D:P(A;;GA;;;" + sid + L")";
    descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor,
                                                               nullptr)) {
        return false;
    }
    attributes = {sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
    return true;
}

void serviceClient(HANDLE pipe, std::vector<Lease>& leases,
                    Ucal::Backend::BackendInterface* const backend,
                    std::mutex& mockMutex, const bool allowTestShutdown,
                    const DWORD expectedParentPid,
                    std::atomic_bool& shutdown, std::atomic_uint32_t& activeClients,
                    std::mutex& completionMutex,
                    std::condition_variable& completionCondition,
                    std::mutex& logMutex) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    Ucal::Protocol::MessageHeader request{};
    std::vector<std::byte> input;
    std::vector<std::byte> output;
    Ucal::Status status = Ucal::Status::TransportError;
    DWORD clientPid = 0;
    try {
        DWORD connectedPid = 0;
        bool validClient = GetNamedPipeClientProcessId(pipe, &connectedPid) &&
                            connectedPid != 0u;
        if (validClient && expectedParentPid != 0u) {
            validClient = connectedPid == expectedParentPid;
            Ucal::Backend::Worker::RequestHeader workerRequest{};
            if (validClient &&
                readExact(pipe, &workerRequest, sizeof(workerRequest), deadline) &&
                Ucal::Backend::Worker::validRequest(workerRequest)) {
                request = workerRequest.request;
                clientPid = workerRequest.ownerPid;
            } else {
                validClient = false;
            }
        } else if (validClient) {
            clientPid = connectedPid;
            validClient = readExact(pipe, &request, sizeof(request), deadline) &&
                           Ucal::Protocol::validRequestHeader(request);
        }
        if (validClient) {
            input.resize(request.payloadBytes);
            if (input.empty() ||
                readExact(pipe, input.data(), request.payloadBytes, deadline)) {
                status = Ucal::Backend::Wire::handleRequest(request, input, clientPid, leases, output,
                                        backend, mockMutex, allowTestShutdown,
                                        shutdown);
            }
        }
    }
    catch (...) {
        status = Ucal::Status::InternalError;
        output.clear();
    }

    if (output.size() > Ucal::Protocol::maxPayloadBytes) {
        status = Ucal::Status::InternalError;
        output.clear();
    }
    auto response = request;
    response.magicValue = Ucal::Protocol::magic;
    response.major = Ucal::Protocol::majorVersion;
    // The response is encoded for the operation/version shape requested by the
    // client. Echo a supported older minor so a v1.5 client does not reject a
    // v1.7 Broker merely because the Broker has newer optional operations.
    response.minor = (std::min)(request.minor, Ucal::Protocol::minorVersion);
    response.reserved = 0;
    response.payloadBytes = static_cast<std::uint32_t>(output.size());
    response.status = static_cast<std::int32_t>(status);
    const bool headerWritten = writeExact(pipe, &response, sizeof(response), deadline);
    if (headerWritten && !output.empty()) {
        writeExact(pipe, output.data(), response.payloadBytes, deadline);
    }
    CloseHandle(pipe);

    const auto completedOperation = static_cast<Ucal::Protocol::Operation>(request.operation);
    const auto successfulDataPlane = status == Ucal::Status::Ok &&
        (completedOperation == Ucal::Protocol::Operation::WriteCan ||
         completedOperation == Ucal::Protocol::Operation::SubmitCan ||
         completedOperation == Ucal::Protocol::Operation::SubmitCanBatch ||
         completedOperation == Ucal::Protocol::Operation::ReadCan ||
         completedOperation == Ucal::Protocol::Operation::ReadCanEvents ||
         completedOperation == Ucal::Protocol::Operation::SubmitLin ||
         completedOperation == Ucal::Protocol::Operation::ReadLinEvents);
    if (!successfulDataPlane) {
        std::lock_guard<std::mutex> lock(logMutex);
        std::cout << "{\"event\":\"request\",\"operation\":" << request.operation
                  << ",\"status\":" << static_cast<std::int32_t>(status)
                  << ",\"clientPid\":" << clientPid << "}" << std::endl;
    }
    activeClients.fetch_sub(1);
    {
        std::lock_guard<std::mutex> lock(completionMutex);
    }
    completionCondition.notify_all();
}

}  // namespace

std::wstring nativeCatalogPath();

std::unique_ptr<Ucal::Backend::BackendInterface> nativePlugin(const std::string& family) {
    wchar_t image[32768]{};
    GetModuleFileNameW(nullptr, image, 32768);
    const std::wstring executable(image);
    const auto directory = executable.substr(0, executable.find_last_of(L"/\\") + 1);
    const std::wstring id(family.begin(), family.end());
    wchar_t declaredName[512]{};
    GetPrivateProfileStringW(L"libraries", id.c_str(), L"", declaredName, 512,
                            nativeCatalogPath().c_str());
    // Direct development selectors use the plugin's declared artifact name.
    // Keep the original convention only for older native catalogs.
    const std::wstring name = declaredName[0] ? std::wstring(declaredName)
                                              : L"ucal_backend_" + id + L".dll";
    if (name.find_first_of(L"/\\:\"") != std::wstring::npos)
        return {};
    std::wstring library = directory + name;
    if (GetFileAttributesW(library.c_str()) == INVALID_FILE_ATTRIBUTES) {
        library = directory + L"../../../../../backends/" + id + (sizeof(void*) == 8 ? L"/x64/" : L"/x86/") + name;
    }
    Ucal::Backend::PluginSpec spec;
    spec.id = family; spec.family = family; spec.library = library; spec.preserveIdentity = true;
    return std::make_unique<Ucal::Backend::PluginBackend>(std::move(spec));
}

std::wstring nativeCatalogPath() {
    wchar_t image[32768]{};
    GetModuleFileNameW(nullptr, image, 32768);
    const std::wstring path(image);
    return path.substr(0, path.find_last_of(L"/\\") + 1) + L"ucal_native_backends.ini";
}
std::string nativeFamily(const std::wstring& alias) {
    wchar_t value[128]{};
    GetPrivateProfileStringW(L"aliases", alias.c_str(), L"", value, 128, nativeCatalogPath().c_str());
    const std::wstring name(value);
    if (name.empty() || !std::all_of(name.begin(), name.end(), [](wchar_t c) {
            return (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c == L'-';
        })) return {};
    return std::string(name.begin(), name.end());
}
std::unique_ptr<Ucal::Backend::BackendInterface> configuredNative(const std::string& family) {
    const std::wstring name(family.begin(), family.end());
    if (GetPrivateProfileIntW(L"isolated", name.c_str(), 0, nativeCatalogPath().c_str()))
        return std::make_unique<Ucal::Backend::IsolatedBackend>(family);
    return nativePlugin(family);
}

int wmain(const int argc, wchar_t** argv) {
    bool workerMode = false;
    bool j2534Worker = false;
    bool pluginWorker = false;
    bool nativeWorker = false;
    std::string workerFamily;
    bool allowTestShutdown = false;
    DWORD workerParentPid = 0u;
    std::wstring pipeName;
    if ((argc == 4 && std::wstring(argv[1]) == L"--j2534-worker") ||
        (argc == 5 && std::wstring(argv[1]) == L"--native-worker") ||
        ((argc == 8 || argc == 9) && std::wstring(argv[1]) == L"--plugin-worker")) {
        pluginWorker = std::wstring(argv[1]) == L"--plugin-worker";
        j2534Worker = std::wstring(argv[1]) == L"--j2534-worker";
        nativeWorker = std::wstring(argv[1]) == L"--native-worker";
        if (nativeWorker) { workerFamily = nativeFamily(argv[4]); if (workerFamily.empty()) return 1; }
        workerMode = true;
        allowTestShutdown = true;
        pipeName = argv[2];
        wchar_t* end = nullptr;
        const auto parsed = std::wcstoul(argv[3], &end, 10);
        const std::wstring nativeName(workerFamily.begin(), workerFamily.end());
        if (end == argv[3] || *end != L'\0' || parsed == 0u || parsed > MAXDWORD ||
            pipeName.rfind(pluginWorker ? L"\\\\.\\pipe\\ucal.plugin.worker." : j2534Worker ? L"\\\\.\\pipe\\ucal.j2534.worker." :
                (L"\\\\.\\pipe\\ucal." + nativeName + L".worker."), 0u) != 0u) {
            return 1;
        }
        workerParentPid = static_cast<DWORD>(parsed);
        if (!processIsAlive(workerParentPid)) return 1;
    } else {
        allowTestShutdown = argc == 2 &&
                              std::wstring(argv[1]) == L"--allow-test-shutdown";
        if (argc > 2 || (argc == 2 && !allowTestShutdown)) return 1;
        pipeName = Ucal::Protocol::defaultPipeName();
    }
    std::vector<Lease> leases;
    std::array<wchar_t, 32> backendName{};
    const auto backendNameLength = GetEnvironmentVariableW(
        L"UCAL_BACKEND", backendName.data(), static_cast<DWORD>(backendName.size()));
    std::unique_ptr<Ucal::Backend::BackendInterface> backend;
    if (workerMode) {
        if(pluginWorker) {
            Ucal::Backend::PluginSpec spec;spec.library=argv[4];spec.configuration=argv[5];
            if (argc == 9) { if (std::wstring(argv[8]) != L"0" && std::wstring(argv[8]) != L"1") return 1; spec.preserveIdentity = std::wstring(argv[8]) == L"1"; }
            for(const auto c:std::wstring(argv[6])) { if(c>127)return 1;spec.id.push_back(static_cast<char>(c)); }
            for(const auto c:std::wstring(argv[7])) { if(c>127)return 1;spec.family.push_back(static_cast<char>(c)); }
            backend=std::make_unique<Ucal::Backend::PluginBackend>(std::move(spec));
        } else if (j2534Worker) backend = std::make_unique<Ucal::J2534Backend::Backend>();
        else backend = nativePlugin(workerFamily);
    } else if (backendNameLength == 5 && _wcsicmp(backendName.data(), L"J2534") == 0) {
        backend = std::make_unique<Ucal::Backend::IsolatedBackend>("j2534");
    } else if (backendNameLength == 7 && _wcsicmp(backendName.data(), L"PLUGINS") == 0) {
        backend = std::make_unique<Ucal::Backend::PluginRegistry>();
    } else if (backendNameLength == 3 && _wcsicmp(backendName.data(), L"ALL") == 0) {
        std::vector<std::unique_ptr<Ucal::Backend::BackendInterface>> backends;
        wchar_t entries[32768]{};
        GetPrivateProfileSectionW(L"isolated", entries, 32768, nativeCatalogPath().c_str());
        for (const wchar_t* entry = entries; *entry; entry += std::wcslen(entry) + 1) {
            const std::wstring line(entry);
            const auto family = nativeFamily(line.substr(0, line.find(L'=')));
            if (!family.empty()) backends.push_back(configuredNative(family));
        }
        backends.push_back(std::make_unique<Ucal::Backend::IsolatedBackend>("j2534"));
        backends.push_back(std::make_unique<Ucal::Backend::PluginRegistry>());
        backend = std::make_unique<Ucal::Backend::Composite>(std::move(backends));
    } else if (backendNameLength != 0) {
        const auto family = nativeFamily(backendName.data());
        if (family.empty()) return 6;
        backend = configuredNative(family);
    }
    std::atomic_bool shutdown{false};
    std::atomic_uint32_t activeClients{0};
    std::mutex mockMutex;
    std::mutex completionMutex;
    std::condition_variable completionCondition;
    std::mutex logMutex;
    const std::uint32_t maximumClients = workerMode ? 16u : 64u;

    HANDLE brokerMutex = nullptr;
    if (!workerMode) {
        PSECURITY_DESCRIPTOR mutexDescriptor = nullptr;
        SECURITY_ATTRIBUTES mutexSecurity{};
        if (!currentUserSecurity(mutexDescriptor, mutexSecurity)) return 3;
        if (pipeName.empty()) return 1;
        const auto mutexName = L"Local\\" + pipeName.substr(9u);
        brokerMutex = CreateMutexW(&mutexSecurity, FALSE, mutexName.c_str());
        LocalFree(mutexDescriptor);
        if (brokerMutex == nullptr) return 4;
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            CloseHandle(brokerMutex);
            return 5;
        }
    }
    std::wcout << L"{\"event\":\"" << (workerMode ? (j2534Worker ? L"j2534_worker_started" : L"plugin_worker_started") : L"broker_started")
               << L"\",\"pipe\":\"" << pipeName << L"\"}" << std::endl;

    std::mutex workMutex;
    std::condition_variable workCondition;
    std::deque<HANDLE> pendingClients;
    bool stopWorkers = false;
    std::vector<std::thread> workers;
    workers.reserve(maximumClients);
    try {
        for (std::uint32_t index = 0; index < maximumClients; ++index) {
            workers.emplace_back([&]() {
                for (;;) {
                    HANDLE clientPipe = INVALID_HANDLE_VALUE;
                    {
                        std::unique_lock<std::mutex> lock(workMutex);
                        workCondition.wait(lock, [&]() {
                            return stopWorkers || !pendingClients.empty();
                        });
                        if (pendingClients.empty()) {
                            if (stopWorkers) return;
                            continue;
                        }
                        clientPipe = pendingClients.front();
                        pendingClients.pop_front();
                    }
                    serviceClient(clientPipe, leases, backend.get(), mockMutex,
                                   allowTestShutdown, workerParentPid,
                                   shutdown, activeClients,
                                   completionMutex, completionCondition, logMutex);
                }
            });
        }
    }
    catch (...) {
        {
            std::lock_guard<std::mutex> lock(workMutex);
            stopWorkers = true;
        }
        workCondition.notify_all();
        for (auto& worker : workers) worker.join();
        if (brokerMutex != nullptr) CloseHandle(brokerMutex);
        return 7;
    }

    int exitCode = 0;
    while (!shutdown.load() &&
           (!workerMode || processIsAlive(workerParentPid))) {
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        SECURITY_ATTRIBUTES security{};
        if (!currentUserSecurity(descriptor, security)) {
            exitCode = 3;
            break;
        }
        HANDLE pipe = CreateNamedPipeW(
            pipeName.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            PIPE_UNLIMITED_INSTANCES, 64 * 1024, 64 * 1024, 0,
            &security);
        if (descriptor != nullptr) LocalFree(descriptor);
        if (pipe == INVALID_HANDLE_VALUE) {
            exitCode = 2;
            break;
        }

        HANDLE connectEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (connectEvent == nullptr) {
            CloseHandle(pipe);
            exitCode = 2;
            break;
        }
        OVERLAPPED overlapped{};
        overlapped.hEvent = connectEvent;
        bool connected = ConnectNamedPipe(pipe, &overlapped) != FALSE;
        if (!connected) {
            const auto error = GetLastError();
            if (error == ERROR_PIPE_CONNECTED) {
                connected = true;
            } else if (error == ERROR_IO_PENDING) {
                while (!shutdown.load() &&
                       (!workerMode || processIsAlive(workerParentPid))) {
                    const auto wait = WaitForSingleObject(connectEvent, 100);
                    if (wait == WAIT_OBJECT_0) {
                        DWORD transferred = 0;
                        connected = GetOverlappedResult(pipe, &overlapped, &transferred,
                                                        FALSE) != FALSE;
                        break;
                    }
                    if (wait == WAIT_FAILED) break;
                }
                if (!connected && (shutdown.load() ||
                    (workerMode && !processIsAlive(workerParentPid)))) {
                    CancelIoEx(pipe, &overlapped);
                    DWORD transferred = 0;
                    GetOverlappedResult(pipe, &overlapped, &transferred, TRUE);
                }
            }
        }
        CloseHandle(connectEvent);
        if (!connected) {
            CloseHandle(pipe);
            continue;
        }

        if (activeClients.fetch_add(1) >= maximumClients) {
            activeClients.fetch_sub(1);
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(workMutex);
            pendingClients.push_back(pipe);
        }
        workCondition.notify_one();
    }
    {
        std::unique_lock<std::mutex> lock(completionMutex);
        completionCondition.wait(lock, [&activeClients]() {
            return activeClients.load() == 0;
        });
    }
    {
        std::lock_guard<std::mutex> lock(workMutex);
        stopWorkers = true;
    }
    workCondition.notify_all();
    for (auto& worker : workers) worker.join();
    if (brokerMutex != nullptr) CloseHandle(brokerMutex);
    return exitCode;
}
