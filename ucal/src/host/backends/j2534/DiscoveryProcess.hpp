#pragma once
#include "ucal/J2534Discovery.h"
#include "ucal/Model.hpp"
#include <windows.h>
#include <array>
#include <algorithm>
#include <string>
#include <vector>

namespace Ucal::J2534Backend {
// Discovery SDKs may not support a second initialization by a PassThru DLL.
// Keep their public API lifetime in a distinct, private UCAL process.
class DiscoveryProcess {
    HANDLE process_{}, job_{}, input_{}, output_{};
    std::wstring module_, runtime_;
    bool read(void *output, DWORD size) {
        auto *bytes = static_cast<unsigned char *>(output);
        DWORD done{};
        const auto deadline = GetTickCount64() + 2500;
        while (done < size) {
            DWORD available{}, got{};
            if (!PeekNamedPipe(output_, nullptr, 0, nullptr, &available, nullptr))
                return false;
            if (available) {
                if (!ReadFile(output_, bytes + done, (std::min)(available, size - done), &got, nullptr) ||
                    !got)
                    return false;
                done += got;
            } else {
                if (WaitForSingleObject(process_, 0) != WAIT_TIMEOUT || GetTickCount64() >= deadline)
                    return false;
                Sleep(1);
            }
        }
        return true;
    }
    bool start(const std::wstring &module, const std::wstring &runtime) {
        if (process_ && module_ == module && runtime_ == runtime &&
            WaitForSingleObject(process_, 0) == WAIT_TIMEOUT)
            return true;
        reset();
        if (module.find(L'"') != module.npos || runtime.find(L'"') != runtime.npos)
            return false;
        std::array<wchar_t, 32768> path{};
        const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (!length || length >= path.size())
            return false;
        const std::wstring image =
            std::wstring(path.data()).substr(0, std::wstring(path.data()).find_last_of(L"\\/") + 1) +
            L"UcalJ2534DiscoveryHost.exe";
        SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
        HANDLE childIn{}, childOut{};
        if (!CreatePipe(&childIn, &input_, &security, 0) ||
            !CreatePipe(&output_, &childOut, &security, 0)) {
            if (childIn)
                CloseHandle(childIn);
            if (childOut)
                CloseHandle(childOut);
            reset();
            return false;
        }
        SetHandleInformation(input_, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(output_, HANDLE_FLAG_INHERIT, 0);
        auto error = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
                                 OPEN_EXISTING, 0, nullptr);
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = childIn;
        startup.StartupInfo.hStdOutput = childOut;
        startup.StartupInfo.hStdError = error;
        SIZE_T bytes{};
        InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
        std::vector<unsigned char> attributes(bytes);
        startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
        const std::array<HANDLE, 3> handles{childIn, childOut, error};
        bool ready = error != INVALID_HANDLE_VALUE &&
                     InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &bytes);
        const bool initialized = ready;
        if (ready)
            ready = UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                              const_cast<HANDLE *>(handles.data()), sizeof(handles), nullptr,
                                              nullptr) != FALSE;
        job_ = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!job_ ||
            !SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
            ready = false;
        std::wstring command = L"\"" + image + L"\" \"" + module + L"\" \"" + runtime + L"\"";
        PROCESS_INFORMATION info{};
        if (ready)
            ready = CreateProcessW(image.c_str(), command.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
                                   nullptr, nullptr, &startup.StartupInfo, &info) != FALSE;
        if (initialized)
            DeleteProcThreadAttributeList(startup.lpAttributeList);
        CloseHandle(childIn);
        CloseHandle(childOut);
        if (error != INVALID_HANDLE_VALUE)
            CloseHandle(error);
        if (info.hProcess) {
            process_ = info.hProcess;
            if (!AssignProcessToJobObject(job_, process_) ||
                ResumeThread(info.hThread) == static_cast<DWORD>(-1))
                ready = false;
            CloseHandle(info.hThread);
        }
        if (!ready) {
            reset();
            return false;
        }
        module_ = module;
        runtime_ = runtime;
        return true;
    }

  public:
    ~DiscoveryProcess() { reset(); }
    void reset() {
        if (input_) {
            CloseHandle(input_);
            input_ = nullptr;
        }
        if (process_) {
            if (WaitForSingleObject(process_, 500) != WAIT_OBJECT_0) {
                TerminateProcess(process_, 0xee);
                WaitForSingleObject(process_, 500);
            }
            CloseHandle(process_);
            process_ = nullptr;
        }
        if (output_) {
            CloseHandle(output_);
            output_ = nullptr;
        }
        if (job_) {
            CloseHandle(job_);
            job_ = nullptr;
        }
        module_.clear();
        runtime_.clear();
    }
    Status discover(const std::wstring &module, const std::wstring &runtime,
                    std::vector<UcalJ2534PortV1> &ports, std::vector<UcalJ2534PortV2> &routes) {
        if (!start(module, runtime))
            return Status::NotFound;
        const unsigned char request = 1;
        DWORD written{};
        std::array<uint32_t, 3> header{};
        if (!WriteFile(input_, &request, 1, &written, nullptr) || written != 1 ||
            !read(header.data(), sizeof(header))) {
            reset();
            return Status::TransportError;
        }
        if (header[0] > 8 || header[1] > 64 || (header[2] != 1 && header[2] != 2)) {
            reset();
            return Status::TransportError;
        }
        if (header[0])
            return static_cast<Status>(header[0]);
        ports.resize(header[1]);
        routes.clear();
        if (header[2] == 2) {
            routes.resize(header[1]);
            if (!routes.empty() &&
                !read(routes.data(), static_cast<DWORD>(routes.size() * sizeof(routes[0])))) {
                reset();
                return Status::TransportError;
            }
            for (std::size_t i = 0; i < routes.size(); ++i)
                ports[i] = routes[i].port;
        } else if (!ports.empty() &&
                   !read(ports.data(), static_cast<DWORD>(ports.size() * sizeof(ports[0])))) {
            reset();
            return Status::TransportError;
        }
        return Status::Ok;
    }
};
} // namespace Ucal::j2534_backend
