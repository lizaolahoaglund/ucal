#include "ucal/Backend.hpp"
#include "ucal/CanTiming.hpp"
#include <deque>
#include <map>
#include <memory>
#include <windows.h>

namespace {
// Original virtual fixture: receive echo only, never physical Tx completion.
class Fixture final : public Ucal::Backend::BackendInterface {
    using Status = Ucal::Status;
    struct Session {
        std::uint32_t owner{}, port{};
        std::deque<Ucal::CanEvent> queue;
        Ucal::CanChannelConfig config{};
    };
    std::map<std::uint64_t, Session> sessions;
    std::uint32_t count{4};
    std::uint64_t next{1};
    bool fd{true};
    bool advancedTiming{};
    std::wstring fault;
    Ucal::DeviceId id(std::uint32_t port) const {
        Ucal::DeviceId value{};
        value[0] = 0xa1;
        value[15] = static_cast<std::uint8_t>(port + 1);
        return value;
    }
    Session *session(std::uint64_t token, std::uint32_t pid) {
        auto i = sessions.find(token);
        return i != sessions.end() && i->second.owner == pid ? &i->second : nullptr;
    }

  public:
    explicit Fixture(const std::wstring &path) {
        if (!path.empty()) {
            count = GetPrivateProfileIntW(L"fixture", L"channels", 4, path.c_str());
            fd = GetPrivateProfileIntW(L"fixture", L"can_fd", 1, path.c_str()) != 0;
            advancedTiming = GetPrivateProfileIntW(L"fixture", L"advanced_timing", 0, path.c_str()) != 0;
            wchar_t text[64]{};
            GetPrivateProfileStringW(L"fixture", L"fault", L"", text, 64, path.c_str());
            fault = text;
        }
    }
    Status initialize() override { return count > 0 && count <= 16 ? Status::Ok : Status::InvalidArgument; }
    std::string backendFamily() const override { return "sdk-fixture"; }
    Status enumerateEndpoints(std::vector<Ucal::EndpointDescriptor> &endpoints) override {
        endpoints.clear();
        if (initialize() != Status::Ok)
            return initialize();
        for (std::uint32_t p = 0; p < count; ++p) {
            Ucal::EndpointDescriptor e{};
            e.id = id(p);
            e.adapterId = id(0);
            e.name = "Original SDK fixture CAN " + std::to_string(p + 1);
            e.model = "Virtual deterministic fixture";
            e.serial = "fixture";
            e.backendFamily = "sdk-fixture";
            e.physicalIndex = p;
            e.capabilities = Ucal::CapabilityClassicCan |
                             (fd ? (Ucal::CapabilityCanFdIso | Ucal::CapabilityBrs) : 0u) |
                             (advancedTiming ? Ucal::CapabilityCanAdvancedTiming : 0u);
            for (const auto &s : sessions)
                if (s.second.port == p)
                    e.state = Ucal::DeviceState::Leased;
            endpoints.push_back(e);
        }
        return Status::Ok;
    }
    Status enumerate(std::vector<Ucal::DeviceDescriptor> &out) override {
        out.clear();
        std::vector<Ucal::EndpointDescriptor> es;
        const auto s = enumerateEndpoints(es);
        for (const auto &e : es)
            out.push_back({e.id, e.name, e.model, e.serial, 1u, e.capabilities, e.state});
        return s;
    }
    Ucal::OpenResult open(const Ucal::DeviceId &target, const Ucal::CanChannelConfig &config,
                          std::uint32_t pid) override {
        if (!pid || !config.arbitrationBitrate || !Ucal::validCanAdvancedTiming(config))
            return {Status::InvalidArgument, 0};
        if ((config.nominalTimingSpecified || config.dataTimingSpecified) && !advancedTiming)
            return {Status::Unsupported, 0};
        if (!fd || config.listenOnly || config.noAck || config.internalLoopback)
            return {Status::Unsupported, 0};
        for (std::uint32_t p = 0; p < count; ++p)
            if (target == id(p)) {
                for (const auto &s : sessions)
                    if (s.second.port == p)
                        return {Status::Busy, 0};
                const auto token = next++;
                sessions.emplace(token, Session{pid, p, {}, config});
                return {Status::Ok, token};
            }
        return {Status::NotFound, 0};
    }
    Status close(std::uint64_t t, std::uint32_t p) override {
        if (!session(t, p))
            return Status::NotFound;
        sessions.erase(t);
        return Status::Ok;
    }
    Status write(std::uint64_t t, std::uint32_t p, const Ucal::CanFrame &f) override {
        return submit(t, p, f);
    }
    Status submit(std::uint64_t t, std::uint32_t p, const Ucal::CanFrame &f) override {
        auto *s = session(t, p);
        if (!s)
            return Status::NotFound;
        if (s->queue.size() >= 256)
            return Status::Busy;
        Ucal::CanEvent e{};
        e.kind = Ucal::CanEventKind::Received;
        e.frame = f;
        e.frame.timestampUs = GetTickCount64() * 1000u + 1u;
        s->queue.push_back(e);
        return Status::Ok;
    }
    Status readEvents(std::uint64_t t, std::uint32_t p, std::uint32_t n,
                       std::vector<Ucal::CanEvent> &out) override {
        if (fault == L"crash-read")
            TerminateProcess(GetCurrentProcess(), 0xe1);
        if (fault == L"hang-read")
            Sleep(10000);
        out.clear();
        auto *s = session(t, p);
        if (!s)
            return Status::NotFound;
        while (n-- && !s->queue.empty()) {
            out.push_back(s->queue.front());
            s->queue.pop_front();
        }
        return Status::Ok;
    }
    Status read(std::uint64_t t, std::uint32_t p, std::uint32_t n,
                std::vector<Ucal::CanFrame> &out) override {
        out.clear();
        std::vector<Ucal::CanEvent> es;
        const auto s = readEvents(t, p, n, es);
        for (const auto &e : es)
            out.push_back(e.frame);
        return s;
    }
    Status getStatus(std::uint64_t t, std::uint32_t p, Ucal::ChannelStatus &s) override {
        s = {};
        return session(t, p) ? Status::Ok : Status::NotFound;
    }
    void pruneDeadSessions() override {
        for (auto i = sessions.begin(); i != sessions.end();) {
            const auto p = OpenProcess(SYNCHRONIZE, FALSE, i->second.owner);
            const bool alive =
                p ? WaitForSingleObject(p, 0) == WAIT_TIMEOUT : GetLastError() == ERROR_ACCESS_DENIED;
            if (p)
                CloseHandle(p);
            if (!alive)
                i = sessions.erase(i);
            else
                ++i;
        }
    }
};
} // namespace
std::unique_ptr<Ucal::Backend::BackendInterface> makeUcalPlugin(const std::wstring &path) {
    return std::make_unique<Fixture>(path);
}
