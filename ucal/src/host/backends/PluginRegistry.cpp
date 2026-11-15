#include "backends/PluginRegistry.hpp"
#include "backends/CompositeBackend.hpp"
#include "backends/IsolatedBackend.hpp"
#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>
#include <set>
#include <windows.h>

namespace Ucal::Backend {
namespace {
std::wstring setting(const std::wstring &path, const std::wstring &section, const wchar_t *key) {
    std::vector<wchar_t> value(32768u);
    const auto size = GetPrivateProfileStringW(section.c_str(), key, L"", value.data(),
                                               static_cast<DWORD>(value.size()), path.c_str());
    return size < value.size() - 1u ? std::wstring(value.data(), size) : std::wstring{};
}
bool identifier(const std::wstring &s) {
    return !s.empty() && s.size() < 128u &&
           std::all_of(
               s.begin(), s.end(),
               [](wchar_t c) {
                   return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') ||
                          c == L'.' || c == L'_' || c == L'-';
               });
}
bool absolute(const std::wstring &p) {
    return p.size() > 2u && p[1] == L':' && (p[2] == L'/' || p[2] == L'\\') && p.find(L'"') == p.npos;
}
std::wstring catalogPath() {
    std::vector<wchar_t> s(32768u);
    const auto n = GetEnvironmentVariableW(L"UCAL_BACKEND_CATALOG", s.data(), static_cast<DWORD>(s.size()));
    return n > 0 && n < s.size() ? std::wstring(s.data(), n) : std::wstring{};
}
std::string contents(const std::wstring &path) {
    if (path.empty())
        return {};
    const auto f =
        CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return "missing";
    LARGE_INTEGER n{};
    std::string result;
    if (GetFileSizeEx(f, &n) && n.QuadPart >= 0 && n.QuadPart <= 262144) {
        result.resize(static_cast<std::size_t>(n.QuadPart));
        DWORD got{};
        if (!ReadFile(f, result.data(), static_cast<DWORD>(result.size()), &got, nullptr) ||
            got != result.size())
            result = "unreadable";
    } else
        result = "oversized";
    CloseHandle(f);
    return result;
}
class Node final : public BackendInterface {
  public:
    PluginSpec spec;
    std::string fingerprint;
    bool configured{true};
    bool deferred{};
    std::shared_ptr<IsolatedBackend> backend;
    std::vector<EndpointDescriptor> cached;
    std::chrono::steady_clock::time_point catalogValidUntil{};
    Node(PluginSpec value, std::string signature)
        : spec(std::move(value)), fingerprint(std::move(signature)),
          backend(std::make_shared<IsolatedBackend>(spec)) {}
    std::shared_ptr<IsolatedBackend> worker() const { return std::atomic_load(&backend); }
    bool active() const { return worker()->hasActiveSessions(); }
    void update(const PluginSpec &value, const std::string &signature) {
        if (!configured)
            catalogValidUntil = {};
        configured = true;
        const bool moduleChanged = spec.library != value.library ||
                                   spec.configuration != value.configuration || spec.worker != value.worker ||
                                   spec.family != value.family || spec.mode != value.mode || spec.preserveIdentity != value.preserveIdentity;
        if (moduleChanged || fingerprint != signature)
            catalogValidUntil = {};
        deferred = active() && (moduleChanged || (fingerprint != signature && spec.mode != "j2534"));
        if (!active() && (moduleChanged || (fingerprint != signature && spec.mode != "j2534"))) {
            spec = value;
            fingerprint = signature;
            std::atomic_store(&backend, std::make_shared<IsolatedBackend>(spec));
            deferred = false;
        } else if (!moduleChanged && spec.mode == "j2534") {
            // J2534 already reconciles channel additions/removals without moving
            // active entries. Runtime/profile changes return busy until idle.
            fingerprint = signature;
        }
    }
    Status initialize() override { return configured ? worker()->initialize() : Status::NotFound; }
    std::string backendFamily() const override { return spec.family; }
    Status enumerateEndpoints(std::vector<EndpointDescriptor> &es) override {
        if (configured && !active() && std::chrono::steady_clock::now() < catalogValidUntil) {
            es = cached;
            return Status::Ok;
        }
        const auto s = configured ? worker()->enumerateEndpoints(es) : Status::NotFound;
        if (s == Status::Ok) {
            cached = es;
            catalogValidUntil = active()
                                      ? std::chrono::steady_clock::time_point{}
                                      : std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
            return s;
        }
        es = cached;
        if (s != Status::Busy)
            for (auto &e : es)
                e.state = DeviceState::Offline;
        return es.empty() ? s : Status::Ok;
    }
    Status enumerate(std::vector<DeviceDescriptor> &ds) override {
        ds.clear();
        std::vector<EndpointDescriptor> es;
        const auto s = enumerateEndpoints(es);
        if (s != Status::Ok)
            return s;
        for (const auto &e : es)
            if (e.busType == BusType::Can)
                ds.push_back({e.id, e.name, e.model, e.serial, 1u, e.capabilities, e.state});
        return s;
    }
    void enumerateBackendHealth(std::vector<BackendHealth> &hs) override {
        if (configured)
            worker()->enumerateBackendHealth(hs);
        else
            hs.push_back({spec.family, Status::NotFound, 0u});
        for (auto &h : hs) {
            h.backendFamily = spec.family;
            h.backendInstanceId = spec.id;
            if (deferred)
                h.status = Status::Busy;
        }
    }
    OpenResult open(const DeviceId &id, const CanChannelConfig &c, std::uint32_t p) override {
        if (!configured)
            return {Status::NotFound, 0};
        if (deferred)
            return {Status::Busy, 0};
        catalogValidUntil = {};
        return worker()->open(id, c, p);
    }
    OpenResult openLin(const EndpointId &id, const LinChannelConfig &c, std::uint32_t p) override {
        if (!configured)
            return {Status::NotFound, 0};
        if (deferred)
            return {Status::Busy, 0};
        catalogValidUntil = {};
        return worker()->openLin(id, c, p);
    }
    Status close(std::uint64_t t, std::uint32_t p) override {
        catalogValidUntil = {};
        return worker()->close(t, p);
    }
    Status write(std::uint64_t t, std::uint32_t p, const CanFrame &f) override {
        return worker()->write(t, p, f);
    }
    Status submit(std::uint64_t t, std::uint32_t p, const CanFrame &f) override {
        return worker()->submit(t, p, f);
    }
    Status submitBatch(std::uint64_t t, std::uint32_t p, const std::vector<CanFrame> &f,
                        std::uint32_t &n) override {
        return worker()->submitBatch(t, p, f, n);
    }
    Status read(std::uint64_t t, std::uint32_t p, std::uint32_t n, std::vector<CanFrame> &f) override {
        return worker()->read(t, p, n, f);
    }
    Status readEvents(std::uint64_t t, std::uint32_t p, std::uint32_t n, std::vector<CanEvent> &f) override {
        return worker()->readEvents(t, p, n, f);
    }
    Status getStatus(std::uint64_t t, std::uint32_t p, ChannelStatus &s) override {
        return worker()->getStatus(t, p, s);
    }
    Status submitLin(std::uint64_t t, std::uint32_t p, const LinFrame &f) override {
        return worker()->submitLin(t, p, f);
    }
    Status readLinEvents(std::uint64_t t, std::uint32_t p, std::uint32_t n,
                           std::vector<LinEvent> &f) override {
        return worker()->readLinEvents(t, p, n, f);
    }
    Status getLinStatus(std::uint64_t t, std::uint32_t p, LinStatus &s) override {
        return worker()->getLinStatus(t, p, s);
    }
    Status configureLinSlaveResponse(std::uint64_t t, std::uint32_t p,
                                        const LinSlaveResponse &s) override {
        return worker()->configureLinSlaveResponse(t, p, s);
    }
    void pruneDeadSessions() override {
        const bool wasActive = active();
        worker()->pruneDeadSessions();
        if (wasActive && !active())
            catalogValidUntil = {};
    }
};
} // namespace
struct PluginRegistry::Implementation {
    std::mutex mutex;
    Composite routes{std::vector<std::unique_ptr<BackendInterface>>{}};
    std::wstring configuredCatalog;
    std::map<std::string, Node *> nodes;
    Status refresh() {
        const auto path = configuredCatalog.empty() ? catalogPath() : configuredCatalog;
        if (path.empty())
            return Status::Ok;
        if (!absolute(path))
            return Status::InvalidArgument;
        if (setting(path, L"catalog", L"version") != L"1")
            return Status::IncompatibleVersion;
        const auto text = setting(path, L"catalog", L"count");
        wchar_t *end{};
        const auto count = std::wcstoul(text.c_str(), &end, 10);
        if (text.empty() || *end || count > 32u)
            return Status::InvalidArgument;
        std::vector<PluginSpec> desired;
        std::set<std::string> ids;
        for (unsigned long i = 0; i < count; ++i) {
            const auto section = L"plugin." + std::to_wstring(i);
            const auto id = setting(path, section, L"id"), family = setting(path, section, L"family");
            PluginSpec spec;
            const auto identity = setting(path, section, L"preserve_identity");
            if (!identity.empty() && identity != L"0" && identity != L"1") return Status::InvalidArgument;
            spec.preserveIdentity = identity == L"1";
            spec.library = setting(path, section, L"library");
            spec.configuration = setting(path, section, L"configuration");
            spec.worker = setting(path, section, L"worker");
            if (!identifier(id) || !identifier(family) || !absolute(spec.library) ||
                (!spec.configuration.empty() && !absolute(spec.configuration)) ||
                (!spec.worker.empty() && !absolute(spec.worker)))
                return Status::InvalidArgument;
            for (const auto c : id)
                spec.id.push_back(static_cast<char>(c));
            for (const auto c : family)
                spec.family.push_back(static_cast<char>(c));
            const auto mode = setting(path, section, L"mode");
            if (!mode.empty() && mode != L"j2534" && mode != L"ucal")
                return Status::InvalidArgument;
            for (const auto c : mode)
                spec.mode.push_back(static_cast<char>(c));
            if (!ids.insert(spec.id).second)
                return Status::InvalidArgument;
            desired.push_back(std::move(spec));
        }
        for (auto &item : nodes)
            if (!ids.count(item.first)) {
                item.second->configured = false;
                item.second->catalogValidUntil = {};
            }
        for (const auto &spec : desired) {
            const auto signature = contents(spec.configuration);
            const auto found = nodes.find(spec.id);
            if (found == nodes.end()) {
                auto node = std::make_unique<Node>(spec, signature);
                nodes[spec.id] = node.get();
                routes.addBackend(std::move(node));
            } else
                found->second->update(spec, signature);
        }
        return Status::Ok;
    }
};
PluginRegistry::PluginRegistry() : implementation_(std::make_unique<Implementation>()) {}
PluginRegistry::~PluginRegistry() = default;
Status PluginRegistry::configurePlugins(const std::wstring &path) {
    std::lock_guard<std::mutex> l(implementation_->mutex);
    if (!absolute(path))
        return Status::InvalidArgument;
    const auto old = implementation_->configuredCatalog;
    implementation_->configuredCatalog = path;
    const auto status = implementation_->refresh();
    if (status != Status::Ok)
        implementation_->configuredCatalog = old;
    if (status == Status::Ok)
        for (auto &item : implementation_->nodes)
            item.second->catalogValidUntil = {};
    return status;
}
Status PluginRegistry::initialize() {
    std::lock_guard<std::mutex> l(implementation_->mutex);
    return implementation_->refresh();
}
Status PluginRegistry::enumerate(std::vector<DeviceDescriptor> &ds) {
    std::lock_guard<std::mutex> l(implementation_->mutex);
    const auto s = implementation_->refresh();
    if (s != Status::Ok)
        return s;
    if (implementation_->nodes.empty()) {
        ds.clear();
        return Status::Ok;
    }
    return implementation_->routes.enumerate(ds);
}
Status PluginRegistry::enumerateEndpoints(std::vector<EndpointDescriptor> &es) {
    std::lock_guard<std::mutex> l(implementation_->mutex);
    const auto s = implementation_->refresh();
    if (s != Status::Ok)
        return s;
    if (implementation_->nodes.empty()) {
        es.clear();
        return Status::Ok;
    }
    return implementation_->routes.enumerateEndpoints(es);
}
void PluginRegistry::enumerateBackendHealth(std::vector<BackendHealth> &hs) {
    std::lock_guard<std::mutex> l(implementation_->mutex);
    const auto s = implementation_->refresh();
    if (s != Status::Ok) {
        hs.push_back({"plugins", s, 0u});
        return;
    }
    implementation_->routes.enumerateBackendHealth(hs);
}
OpenResult PluginRegistry::open(const DeviceId &id, const CanChannelConfig &c, std::uint32_t p) {
    std::lock_guard<std::mutex> l(implementation_->mutex);
    const auto s = implementation_->refresh();
    if (s != Status::Ok)
        return {s, 0};
    return implementation_->routes.open(id, c, p);
}
OpenResult PluginRegistry::openLin(const EndpointId &id, const LinChannelConfig &c, std::uint32_t p) {
    std::lock_guard<std::mutex> l(implementation_->mutex);
    const auto s = implementation_->refresh();
    if (s != Status::Ok)
        return {s, 0};
    return implementation_->routes.openLin(id, c, p);
}
Status PluginRegistry::close(std::uint64_t t, std::uint32_t p) {
    std::lock_guard<std::mutex> l(implementation_->mutex);
    return implementation_->routes.close(t, p);
}
Status PluginRegistry::write(std::uint64_t t, std::uint32_t p, const CanFrame &f) {
    return implementation_->routes.write(t, p, f);
}
Status PluginRegistry::submit(std::uint64_t t, std::uint32_t p, const CanFrame &f) {
    return implementation_->routes.submit(t, p, f);
}
Status PluginRegistry::submitBatch(std::uint64_t t, std::uint32_t p, const std::vector<CanFrame> &f,
                                    std::uint32_t &n) {
    return implementation_->routes.submitBatch(t, p, f, n);
}
Status PluginRegistry::read(std::uint64_t t, std::uint32_t p, std::uint32_t n, std::vector<CanFrame> &f) {
    return implementation_->routes.read(t, p, n, f);
}
Status PluginRegistry::readEvents(std::uint64_t t, std::uint32_t p, std::uint32_t n,
                                   std::vector<CanEvent> &f) {
    return implementation_->routes.readEvents(t, p, n, f);
}
Status PluginRegistry::getStatus(std::uint64_t t, std::uint32_t p, ChannelStatus &s) {
    return implementation_->routes.getStatus(t, p, s);
}
Status PluginRegistry::submitLin(std::uint64_t t, std::uint32_t p, const LinFrame &f) {
    return implementation_->routes.submitLin(t, p, f);
}
Status PluginRegistry::readLinEvents(std::uint64_t t, std::uint32_t p, std::uint32_t n,
                                       std::vector<LinEvent> &f) {
    return implementation_->routes.readLinEvents(t, p, n, f);
}
Status PluginRegistry::getLinStatus(std::uint64_t t, std::uint32_t p, LinStatus &s) {
    return implementation_->routes.getLinStatus(t, p, s);
}
Status PluginRegistry::configureLinSlaveResponse(std::uint64_t t, std::uint32_t p,
                                                    const LinSlaveResponse &s) {
    return implementation_->routes.configureLinSlaveResponse(t, p, s);
}
void PluginRegistry::pruneDeadSessions() {
    std::unique_lock<std::mutex> l(implementation_->mutex, std::try_to_lock);
    if (!l.owns_lock())
        return;
    implementation_->routes.pruneDeadSessions();
}
} // namespace Ucal::backend
