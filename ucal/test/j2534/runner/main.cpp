#include "backends/IsolatedBackend.hpp"
#include "ucal/Model.hpp"
#include <array>
#include <chrono>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
#include <windows.h>

using namespace Ucal;
namespace {
void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
void runMultichannel(const std::string &scenario) {
    Ucal::Backend::IsolatedBackend backend("j2534");
    std::vector<EndpointDescriptor> ports;
    const auto listed = backend.enumerateEndpoints(ports);
    if (scenario == "multi-malformed" || scenario == "multi-duplicate-route") {
        require(listed ==
                    (scenario == "multi-malformed" ? Status::TransportError : Status::InvalidArgument),
                "invalid v2 routing accepted");
        return;
    }
    require(listed == Status::Ok && ports.size() == 2, "two v2 ports");
    const auto first = ports[0].id, second = ports[1].id;
    const auto owner = GetCurrentProcessId();
    CanChannelConfig config{};
    config.canFd = scenario.find("classic") == std::string::npos;
    config.arbitrationSamplePointSpecified = !config.canFd;
    config.arbitrationSamplePoint = 75;
    auto a = backend.open(first, config, owner);
    require(a.status == Status::Ok, "first routed open");
    wchar_t path[2048]{};
    GetEnvironmentVariableW(L"UCAL_J2534_CONFIG", path, 2048);
    if (scenario == "multi-rollback" || scenario == "multi-filter-rollback") {
        const auto key = scenario == "multi-rollback" ? L"fail_connect" : L"fail_filter";
        require(WritePrivateProfileStringW(L"multi-test", key, L"1", path) != 0, "inject rollback");
        require(backend.open(second, config, owner).status == Status::Busy,
                "second open failure not reported");
    }
    if (scenario == "multi-named")
        require(WritePrivateProfileStringW(L"multi-test", L"hidden", L"1", path) != 0,
                "hide owned adapter from external SDK");
    auto other = config;
    other.arbitrationBitrate = 250000;
    other.dataBitrate = 4000000;
    auto b = backend.open(second, other, owner);
    require(b.status == Status::Ok, "second independent routed open");
    require(backend.open(first, config, owner).status == Status::Busy, "duplicate endpoint ownership");
    auto roundtrip = [&](const OpenResult &session, uint8_t tag) {
        CanFrame frame{};
        frame.id = 0x300 + tag;
        frame.flags = config.canFd ? FrameFd | FrameBrs : 0;
        frame.dlc = config.canFd ? 15 : 8;
        frame.dataLength = config.canFd ? 64 : 8;
        frame.data[0] = tag;
        frame.data[frame.dataLength - 1] = static_cast<uint8_t>(tag + 1);
        require(backend.submit(session.leaseToken, owner, frame) == Status::Ok,
                "peer lost after sibling operation");
        std::vector<CanEvent> events;
        require(backend.readEvents(session.leaseToken, owner, 4, events) == Status::Ok &&
                    events.size() == 1 && events[0].frame.id == frame.id && events[0].frame.data[0] == tag &&
                    events[0].frame.data[frame.dataLength - 1] == tag + 1,
                "cross-channel or protocol routing");
    };
    roundtrip(a, 1);
    roundtrip(b, 2);
    if (scenario == "multi-route-change") {
        require(WritePrivateProfileStringW(L"multi-test", L"changed", L"1", path) != 0,
                "change active route");
        require(backend.enumerateEndpoints(ports) == Status::Busy, "live route silently replaced");
        require(WritePrivateProfileStringW(L"multi-test", L"changed", L"0", path) != 0, "restore route");
        roundtrip(a, 3);
        roundtrip(b, 4);
    }
    require(backend.enumerateEndpoints(ports) == Status::Ok && ports.size() == 2 && ports[0].id == first &&
                ports[1].id == second,
            "active refresh moved endpoints");
    require(backend.close(a.leaseToken, owner) == Status::Ok, "first child close");
    roundtrip(b, 5);
    a = backend.open(first, config, owner);
    require(a.status == Status::Ok, "child reconnect with live peer");
    require(backend.close(b.leaseToken, owner) == Status::Ok, "reverse-order child close");
    roundtrip(a, 6);
    require(backend.close(a.leaseToken, owner) == Status::Ok, "last child/device close");
    if (scenario == "multi-named") {
        require(backend.enumerateEndpoints(ports) == Status::Ok && ports.empty(),
                "stale adapter survived last Close");
        require(WritePrivateProfileStringW(L"multi-test", L"hidden", L"0", path) != 0,
                "restore idle adapter");
    }
    a = backend.open(first, config, owner);
    require(a.status == Status::Ok, "device reopen after last child");
    require(backend.close(a.leaseToken, owner) == Status::Ok, "final cleanup");
}
void runDynamic() {
    wchar_t path[2048]{}; GetEnvironmentVariableW(L"UCAL_J2534_CONFIG", path, 2048);
    require(WritePrivateProfileStringW(L"discovery-test", L"count", L"1", path) != 0, "control");
    Ucal::Backend::IsolatedBackend backend("j2534"); std::vector<EndpointDescriptor> es;
    require(backend.enumerateEndpoints(es) == Status::Ok && es.size() == 1, "dynamic initial inventory");
    const auto id = es[0].id;
    CanChannelConfig config{}; config.canFd = true; const auto owner = GetCurrentProcessId();
    const auto opened = backend.open(id, config, owner); require(opened.status == Status::Ok, "dynamic open");
    require(WritePrivateProfileStringW(L"discovery-test", L"count", L"2", path) != 0, "add");
    require(backend.enumerateEndpoints(es) == Status::Ok && es.size() == 2 && es[0].id == id, "live addition or identity drift");
    require(WritePrivateProfileStringW(L"discovery-test", L"count", L"0", path) != 0, "remove");
    require(backend.enumerateEndpoints(es) == Status::Ok && es.size() == 2 && es[0].id == id,
            "owned adapter routes lost during external discovery");
    require(backend.close(opened.leaseToken, owner) == Status::Ok, "close");
    require(backend.enumerateEndpoints(es) == Status::Ok && es.empty(), "removed port restored from config");
    require(WritePrivateProfileStringW(L"discovery-test", L"count", L"3", path) != 0, "malformed");
    require(backend.enumerateEndpoints(es) == Status::TransportError, "oversized discovery result accepted");
}
void runGeneric(const std::string& scenario) {
    Ucal::Backend::IsolatedBackend backend("j2534");
    std::vector<EndpointDescriptor> endpoints;
    require(backend.enumerateEndpoints(endpoints) == Status::Ok && endpoints.size() == 1u, "standard catalog");
    require(endpoints[0].bindingIsDefault, "default binding metadata");
    const auto id = endpoints[0].id;
    const auto owner = GetCurrentProcessId();
    CanChannelConfig config{}; config.canFd = true;
    auto opened = backend.open(id, config, owner);
    require(opened.status == Status::Ok, "standard NULL/SET path");
    require(backend.enumerateEndpoints(endpoints) == Status::Ok, "capability refresh");
    const auto access = endpoints[0].configuration.canFd;
    const auto rates = CanConfigNominalBitrate | CanConfigDataBitrate;
    require((access.writeKnown & access.writeSupported & rates) == rates, "SET capability missing");
    if (scenario == "standard-set-only")
        require((access.readKnown & rates) == rates && !(access.readSupported & rates), "GET failure blocked SET or fabricated readback");
    if (scenario == "standard-get-busy")
        require(!(access.readKnown & rates), "transient GET failure became unsupported capability");
    require((access.writeKnown & CanConfigDataSample) && !(access.writeSupported & CanConfigDataSample), "invented FD sampling setter");
    CanFrame frame{}; frame.id = 0x321u; frame.flags = FrameFd | FrameBrs;
    frame.dlc = 15u; frame.dataLength = 64u; frame.data[63] = 0xabu;
    require(backend.submit(opened.leaseToken, owner, frame) == Status::Ok, "standard FD submit");
    std::vector<CanEvent> events;
    require(backend.readEvents(opened.leaseToken, owner, 4u, events) == Status::Ok && events.size() == 1u &&
        events[0].frame.dataLength == 64u && events[0].frame.data[63] == 0xabu, "standard FD receive");
    require(backend.close(opened.leaseToken, owner) == Status::Ok, "standard close");
    if (scenario == "standard-unsupported-termination") {
        config.terminationSpecified = true;
        require(backend.open(id, config, owner).status == Status::Unsupported, "unsupported requested termination");
        require(backend.enumerateEndpoints(endpoints) == Status::Ok, "termination capability");
        const auto& a = endpoints[0].configuration.canFd;
        require((a.writeKnown & CanConfigTermination) && !(a.writeSupported & CanConfigTermination), "termination support invented");
        config.terminationSpecified = false;
        opened = backend.open(id, config, owner);
        require(opened.status == Status::Ok, "optional termination prevented basic communication");
        require(backend.close(opened.leaseToken, owner) == Status::Ok, "optional close");
    }
    if (scenario == "standard-read-only" || scenario == "standard-invalid-value") {
        config.canFd = false; config.arbitrationSamplePointSpecified = true; config.arbitrationSamplePoint = 75u;
        const auto expected = scenario == "standard-read-only" ? Status::Unsupported : Status::InvalidArgument;
        require(backend.open(id, config, owner).status == expected, "sample setter classification");
        require(backend.enumerateEndpoints(endpoints) == Status::Ok, "sample capabilities");
        const auto& a = endpoints[0].configuration.classicCan;
        if (scenario == "standard-read-only")
            require((a.readSupported & CanConfigNominalSample) && !(a.writeSupported & CanConfigNominalSample), "read-only parameter conflated with write");
        else require(!(a.writeKnown & CanConfigNominalSample), "bad value permanently disabled a setter");
    }
}
void run(const std::string &scenario) {
    Ucal::Backend::IsolatedBackend backend("j2534");
    std::vector<EndpointDescriptor> endpoints;
    if (scenario == "ports-malformed") {
        require(backend.enumerateEndpoints(endpoints) == Status::TransportError,
                "invalid discovery record escaped containment");
        return;
    }
    if (scenario == "hang-open")
        SetEnvironmentVariableA("UCAL_J2534_TEST_CASE", "normal");
    require(backend.enumerateEndpoints(endpoints) == Status::Ok && endpoints.size() == 1u,
            "catalog");
    SetEnvironmentVariableA("UCAL_J2534_TEST_CASE", scenario.c_str());
    if (scenario == "wrong-api" || scenario == "busy")
        require(endpoints[0].state == DeviceState::Offline,
                "unavailable selector advertised online");
    require(endpoints[0].backendFamily == "j2534", "family");
    require((endpoints[0].capabilities & CapabilityTxCompletion) == 0u,
            "synthetic completion capability");
    const auto id = endpoints[0].id;
    const auto owner = GetCurrentProcessId();
    CanChannelConfig config{};
    config.canFd = true;
    config.terminationSpecified = true;
    if (scenario.find("fixed-") == 0u) {
        config.arbitrationSamplePointSpecified = true;
        config.dataSamplePointSpecified = true;
        config.arbitrationSamplePoint = scenario == "fixed-mismatch" ? 70u : 80u;
        config.dataSamplePoint = 75u;
        if (scenario == "fixed-rate-mismatch") config.dataBitrate = 8000000u;
    }
    if (scenario.find("timing-") == 0u || scenario == "ports") {
        config.arbitrationSamplePointSpecified = config.dataSamplePointSpecified = true;
        config.arbitrationSamplePoint = 75u;
        config.dataSamplePoint = 80u;
    }
    const auto started = std::chrono::steady_clock::now();
    auto opened = backend.open(id, config, owner);
    const std::map<std::string, Status> failures{{"wrong-api", Status::IncompatibleVersion},
                                                 {"sae-only", Status::Unsupported},
                                                 {"classic-only", Status::Unsupported},
                                                 {"isotp-only", Status::Unsupported},
                                                 {"wrong-readback", Status::Unsupported},
                                                 {"no-data-rate", Status::Unsupported},
                                                 {"filter-failure", Status::Busy},
                                                 {"busy", Status::Busy},
                                                 {"hang-open", Status::TransportError},
                                                 {"fixed-mismatch", Status::Unsupported},
                                                 {"fixed-version", Status::Unsupported},
                                                 {"fixed-rate-mismatch", Status::Unsupported},
                                                 {"timing-mismatch", Status::Unsupported}};
    const auto failure = failures.find(scenario);
    if (failure != failures.end()) {
        require(opened.status == failure->second && opened.leaseToken == 0u,
                "admission did not reject");
        require(std::chrono::steady_clock::now() - started < std::chrono::seconds(4),
                "open deadline");
        // Open failure must release the device/filter transaction and the worker.
        SetEnvironmentVariableA("UCAL_J2534_TEST_CASE", "normal");
        config.arbitrationSamplePointSpecified = false;
        config.dataSamplePointSpecified = false;
        config.dataBitrate = 2000000u;
        opened = backend.open(id, config, owner);
        require(opened.status == Status::Ok, "failed admission cleanup");
        require(backend.close(opened.leaseToken, owner) == Status::Ok, "cleanup close");
        return;
    }
    require(opened.status == Status::Ok, "open");
    const auto token = opened.leaseToken;
    require(backend.open(id, config, owner).status == Status::Busy, "exclusive lease");
    ChannelStatus state{};
    state.busStateValid = true;
    state.errorCountersValid = true;
    require(backend.getStatus(token, owner, state) == Status::Ok && !state.busStateValid &&
                !state.errorCountersValid,
            "state validity");
    require(backend.getStatus(token, owner + 1u, state) == Status::NotFound, "owner isolation");
    CanFrame frame{};
    frame.id = 0x18f00123u;
    frame.flags = FrameExtended | FrameFd | FrameBrs;
    frame.dlc = 15u;
    frame.dataLength = 64u;
    frame.transactionId = 99u;
    for (std::size_t i = 0; i < 64u; ++i)
        frame.data[i] = static_cast<std::uint8_t>(i ^ 0x5au);
    std::uint32_t accepted = 0;
    const auto submitted = backend.submitBatch(token, owner, {frame, frame}, accepted);
    if (scenario == "disconnected-write" || scenario == "timeout-write" || scenario == "failed-write") {
        const auto expected = scenario == "disconnected-write" ? Status::NotFound :
                              scenario == "timeout-write" ? Status::Busy : Status::InternalError;
        require(submitted == expected && accepted == 0u, "ordinary API failure misclassified as Worker fault");
        require(backend.getStatus(token, owner, state) ==
                    (scenario == "disconnected-write" ? Status::NotFound : Status::Ok),
                "endpoint availability after API failure");
        require(backend.close(token, owner) == Status::Ok, "ordinary failure invalidated Worker route");
        return;
    }
    if (scenario == "oversized-count") {
        require(submitted == Status::TransportError && accepted == 0u, "invalid accepted count");
        require(backend.close(token, owner) == Status::NotFound, "failed worker invalidation");
        return;
    }
    require(submitted == (scenario == "partial" ? Status::Busy : Status::Ok), "submit result");
    require(accepted == (scenario == "partial" ? 1u : 2u), "accepted prefix");
    std::vector<CanEvent> events;
    const auto readStart = std::chrono::steady_clock::now();
    const auto read =
        backend.readEvents(token, owner, scenario == "overflow-full" ? 2u : 32u, events);
    if (scenario == "disconnected-read") {
        require(read == Status::NotFound && events.empty(), "disconnect misclassified as Worker fault");
        require(backend.getStatus(token, owner, state) == Status::NotFound, "lost session remained healthy");
        require(backend.enumerateEndpoints(endpoints) == Status::Ok &&
                    endpoints[0].state == DeviceState::Offline, "lost session remained leased online");
        require(backend.close(token, owner) == Status::Ok, "disconnect invalidated Worker route");
        return;
    }
    if (scenario == "malformed-rx" || scenario == "sae-flags" || scenario == "hang-read" ||
        scenario == "crash-read") {
        require(read == Status::TransportError, "bad runtime containment");
        require(std::chrono::steady_clock::now() - readStart < std::chrono::seconds(2),
                "read deadline");
        require(backend.close(token, owner) == Status::NotFound,
                "stale token after worker failure");
        return;
    }
    require(read == Status::Ok, "read");
    if (scenario == "echo")
        require(events.empty(), "echo must not become Rx or Tx completion");
    else if (scenario == "error-indication") {
        require(events.size() == 2u && events[0].kind == CanEventKind::ControllerState &&
                    !events[0].channelStatus.busStateValid,
                "partial error state");
    } else {
        require(events.size() == accepted + (scenario == "overflow-rx" ? 1u : 0u),
                "received prefix");
        const auto &rx = events[0];
        require(rx.kind == CanEventKind::Received && rx.frame.id == frame.id &&
                    rx.frame.data == frame.data && rx.frame.dlc == 15u &&
                    rx.frame.dataLength == 64u && rx.frame.transactionId == 0u,
                "raw FD roundtrip");
        require(rx.frame.flags == (frame.flags | (scenario == "esi" ? FrameEsi : 0u)), "GM flags");
        if (scenario == "wrap")
            require(events[1].frame.timestampUs == 0x100000010ull, "timestamp wrap");
        if (scenario == "backward-clock")
            require(events[1].frame.timestampUs == 0u, "ambiguous clock must be invalid");
        if (scenario == "overflow-rx")
            require(events.back().kind == CanEventKind::QueueOverrun, "overflow event");
        if (scenario == "overflow-full") {
            require(backend.readEvents(token, owner, 1u, events) == Status::Ok &&
                        events.size() == 1u && events[0].kind == CanEventKind::QueueOverrun,
                    "deferred overflow must not be lost");
        }
    }
    if (scenario == "hot-refresh") {
        wchar_t path[32768]{};
        require(GetEnvironmentVariableW(L"UCAL_J2534_CONFIG", path, 32768) > 0, "config path");
        require(WritePrivateProfileStringW(L"provider", L"endpoint_count", L"2", path) != 0, "append count");
        const std::pair<const wchar_t*, const wchar_t*> values[] = {
            {L"selector",L"fixture-device:1"}, {L"adapter_identity",L"fixture-device"},
            {L"physical_port",L"1"}, {L"name",L"Added port"}, {L"brs",L"1"}, {L"termination",L"1"}};
        for (const auto& item : values)
            require(WritePrivateProfileStringW(L"endpoint.1", item.first, item.second, path) != 0, "append port");
        require(backend.enumerateEndpoints(endpoints) == Status::Ok && endpoints.size() == 2u &&
                    endpoints[0].id == id && endpoints[0].state == DeviceState::Leased,
                "hot discovery moved the active mapping");
        require(backend.submit(token, owner, frame) == Status::Ok, "hot refresh interrupted traffic");
    }
    require(backend.close(token, owner) == Status::Ok, "close");
    require(backend.readEvents(token, owner, 1u, events) == Status::NotFound, "stale token");
    if (scenario != "normal")
        return;
    for (int i = 0; i < 5; ++i) {
        opened = backend.open(id, config, owner);
        require(opened.status == Status::Ok, "reopen");
        require(backend.close(opened.leaseToken, owner) == Status::Ok, "reclose");
    }
    config.arbitrationSamplePointSpecified = true;
    require(backend.open(id, config, owner).status == Status::Unsupported,
            "unsupported exact sample point");
    config.arbitrationSamplePointSpecified = false;
    config.noAck = true;
    require(backend.open(id, config, owner).status == Status::Unsupported, "unsupported noack");
    config.noAck = false;
    config.internalLoopback = true;
    require(backend.open(id, config, owner).status == Status::Unsupported,
            "echo is not internal loopback");
    config.internalLoopback = false;
    config.listenOnly = true;
    require(backend.open(id, config, owner).status == Status::Unsupported,
            "unsupported listenonly");
    config.listenOnly = false;
    config.canFd = false;
    config.dataBitrate = 0u;
    config.arbitrationSamplePointSpecified = true;
    config.arbitrationSamplePoint = 75u;
    opened = backend.open(id, config, owner);
    require(opened.status == Status::Ok, "Classic on admitted FD port");
    frame.flags = FrameRemote;
    frame.id = 0x123u;
    frame.dlc = 8u;
    frame.dataLength = 8u;
    require(backend.submit(opened.leaseToken, owner, frame) == Status::Unsupported,
            "RTR unsupported");
    frame.flags = 0u;
    require(backend.submit(opened.leaseToken, owner, frame) == Status::Ok, "Classic submit");
    require(backend.readEvents(opened.leaseToken, owner, 4u, events) == Status::Ok &&
                events.size() == 1u && events[0].frame.flags == 0u &&
                events[0].frame.dataLength == 8u && events[0].frame.id == 0x123u,
            "Classic protocol roundtrip");
    require(backend.close(opened.leaseToken, owner) == Status::Ok, "Classic close");
}
} // namespace
#include "Discovery.hpp"
int main(int argc, char **argv) {
    const std::string scenario = argc > 1 ? argv[1] : "normal";
    SetEnvironmentVariableA("UCAL_J2534_TEST_CASE", scenario.c_str());
    try {
        if (scenario.find("public-") == 0u)
            runPublicDiscovery(scenario);
        else if (scenario.find("multi-") == 0u)
            runMultichannel(scenario);
        else if (scenario == "dynamic")
            runDynamic();
        else if (scenario.find("standard-") == 0u) runGeneric(scenario); else run(scenario);
        std::cout << "{\"case\":\"" << scenario << "\",\"status\":\"passed\"}\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << scenario << ": " << e.what() << "\n";
        return 1;
    }
}
