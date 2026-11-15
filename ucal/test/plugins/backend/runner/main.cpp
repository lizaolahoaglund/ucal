#include "ucal/Client.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <future>
#include <set>
#include <stdexcept>
#include <windows.h>

namespace {
void need(bool value, const char *why) {
    if (!value)
        throw std::runtime_error(why);
}
} // namespace
int main(int argc, char **argv) {
    Ucal::Client client;
    std::vector<std::uint64_t> leases;
    try {
        const std::string scenario = argc > 1 ? argv[1] : "multiple";
        std::vector<Ucal::EndpointDescriptor> es;
        if (scenario == "invalid") {
            std::vector<Ucal::BackendHealth> health;
            need(client.enumerateBackendHealth(health) == Ucal::Status::Ok, "invalid health");
            bool rejected = false;
            for (const auto &h : health) {
                std::cout << "health " << h.backendInstanceId << " " << int(h.status) << std::endl;
                if (h.backendInstanceId == "bad" && h.status != Ucal::Status::Ok)
                    rejected = true;
            }
            need(rejected, "invalid plugin was admitted");
            return 0;
        }
        need(client.enumerateEndpoints(es) == Ucal::Status::Ok, "catalog");
        if (scenario == "empty") {
            need(es.empty(), "empty plugin catalog");
            return 0;
        }
        if (scenario == "inventory") {
            for (const auto &e : es)
                std::cout << "{\"instance\":\"" << e.backendInstanceId << "\",\"port\":" << e.physicalIndex
                          << ",\"id\":\"" << Ucal::deviceIdToString(e.id) << "\",\"adapter\":\""
                          << Ucal::deviceIdToString(e.adapterId) << "\"}" << std::endl;
            return 0;
        }
        if (scenario == "hold-endpoints") {
            need(argc >= 3, "explicit endpoint list required");
            std::string ids = argv[2];
            std::size_t offset = 0;
            while (offset < ids.size()) {
                const auto end = ids.find(';', offset);
                const auto text = ids.substr(offset, end - offset);
                const auto endpoint = std::find_if(es.begin(), es.end(), [&](const auto &e) {
                    return Ucal::deviceIdToString(e.id) == text;
                });
                need(endpoint != es.end() && endpoint->backendFamily == "j2534",
                     "explicit J2534 endpoint missing");
                Ucal::CanChannelConfig config{};
                config.canFd = true;
                config.arbitrationBitrate = 500000;
                config.dataBitrate = 8000000;
                config.termination = true;
                config.terminationSpecified = true;
                const auto opened = client.openCanChannel(endpoint->id, config);
                need(opened.status == Ucal::Status::Ok, "hold endpoint open");
                leases.push_back(opened.leaseToken);
                if (end == std::string::npos)
                    break;
                offset = end + 1;
            }
            need(!leases.empty(), "no hold endpoints");
            std::cout << "HOLD_READY " << leases.size() << std::endl;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
            unsigned received = 0;
            while (std::chrono::steady_clock::now() < deadline) {
                DWORD available{};
                if (PeekNamedPipe(GetStdHandle(STD_INPUT_HANDLE), nullptr, 0, nullptr, &available, nullptr) &&
                    available)
                    break;
                for (const auto lease : leases) {
                    std::vector<Ucal::CanEvent> events;
                    need(client.readCanEvents(lease, 256, events) == Ucal::Status::Ok,
                         "held endpoint read");
                    for (const auto &event : events)
                        if (event.kind == Ucal::CanEventKind::Received)
                            ++received;
                }
                Sleep(5);
            }
            for (const auto lease : leases)
                need(client.closeDevice(lease) == Ucal::Status::Ok, "held endpoint close");
            std::cout << "{\"status\":\"passed\",\"heldChannels\":" << leases.size()
                      << ",\"received\":" << received << "}" << std::endl;
            return 0;
        }
        if (scenario == "peer" || scenario == "peer-four") {
            for (const auto &e : es)
                if (e.backendInstanceId == "mock-peer" ||
                    (scenario == "peer" && e.backendInstanceId == "mock-four-port" && e.physicalIndex == 2)) {
                    Ucal::CanChannelConfig c{};
                    c.canFd = true;
                    c.arbitrationBitrate = 500000;
                    c.dataBitrate = 8000000;
                    c.termination = true;
                    c.terminationSpecified = true;
                    const auto o = client.openCanChannel(e.id, c);
                    need(o.status == Ucal::Status::Ok, "peer open");
                    if (e.backendInstanceId == "mock-peer")
                        leases.insert(leases.begin(), o.leaseToken);
                    else
                        leases.push_back(o.leaseToken);
                }
            need(leases.size() == (scenario == "peer" ? 2u : 1u), "peer and auxiliary ports");
            const auto quiet = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
            while (std::chrono::steady_clock::now() < quiet) {
                std::vector<Ucal::CanEvent> discarded;
                need(client.readCanEvents(leases[0], 256, discarded) == Ucal::Status::Ok, "peer drain");
                Sleep(2);
            }
            std::cout << "PEER_READY_8M_THREE_TERMINATIONS" << std::endl;
            unsigned count = 0, esiFrames = 0;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
            while (count < 1000 && std::chrono::steady_clock::now() < deadline) {
                std::vector<Ucal::CanEvent> events;
                need(client.readCanEvents(leases[0], 128, events) == Ucal::Status::Ok, "peer read");
                for (const auto &e : events)
                    if (e.kind == Ucal::CanEventKind::Received && e.frame.id == 0x5a5) {
                        if (e.frame.flags & Ucal::FrameEsi)
                            ++esiFrames;
                        need((e.frame.flags & ~Ucal::FrameEsi) == (Ucal::FrameFd | Ucal::FrameBrs) &&
                                 e.frame.dataLength == 64 && e.frame.timestampUs > 0,
                             "CANoe frame flags");
                        for (unsigned n = 0; n < 64; ++n) {
                            const auto expected =
                                n < 4 ? static_cast<std::uint8_t>(count >> (8u * n))
                                      : static_cast<std::uint8_t>(0x80u + ((count + n) & 0x3fu));
                            if (e.frame.data[n] != expected) {
                                std::cerr << "peer mismatch count=" << count << " byte=" << n
                                          << " actual=" << unsigned(e.frame.data[n])
                                          << " expected=" << unsigned(expected) << " flags=" << e.frame.flags
                                          << " first=";
                                for (unsigned k = 0; k < 8; ++k)
                                    std::cerr << unsigned(e.frame.data[k]) << ",";
                                std::cerr << std::endl;
                            }
                            need(e.frame.data[n] == expected, "CANoe payload sequence");
                        }
                        ++count;
                    }
                if (events.empty())
                    Sleep(1);
            }
            need(count == 1000, "CANoe transmit deadline");
            for (unsigned n = 0; n < 1000; ++n) {
                Ucal::CanFrame f{};
                f.id = 0x5a6;
                f.flags = Ucal::FrameFd | Ucal::FrameBrs;
                f.dlc = 15;
                f.dataLength = 64;
                for (unsigned j = 0; j < 64; ++j)
                    f.data[j] = j < 4 ? static_cast<std::uint8_t>(n >> (8u * j))
                                      : static_cast<std::uint8_t>(0x80u + ((n + j) & 0x3fu));
                need(client.submitCan(leases[0], f) == Ucal::Status::Ok, "peer transmit");
                Sleep(2);
            }
            for (auto token : leases)
                need(client.closeDevice(token) == Ucal::Status::Ok, "peer close");
            leases.clear();
            std::cout
                << "{\"status\":\"passed\",\"rate\":8000000,\"canoeTx\":1000,\"peerTx\":1000,\"esiFrames\":"
                << esiFrames << "}" << std::endl;
            return 0;
        }
        if (scenario == "physical") {
            need(es.size() == 5, "four-port adapter plus independent peer");
            std::set<Ucal::DeviceId> adapters;
            for (const auto &e : es)
                adapters.insert(e.adapterId);
            need(adapters.size() == 2, "physical adapter grouping");
            std::vector<std::size_t> wired;
            for (std::size_t i = 0; i < es.size(); ++i) {
                const auto &e = es[i];
                Ucal::CanChannelConfig c{};
                c.canFd = true;
                c.arbitrationBitrate = 500000;
                c.dataBitrate = 8000000;
                c.arbitrationSamplePoint = 75;
                c.dataSamplePoint = 80;
                const bool connected =
                    e.backendInstanceId == "mock-peer" || e.physicalIndex == 0 || e.physicalIndex == 2;
                c.termination = connected;
                c.terminationSpecified = true;
                auto o = client.openCanChannel(e.id, c);
                need(o.status == Ucal::Status::Ok, "five concurrent physical opens");
                leases.push_back(o.leaseToken);
                if (connected)
                    wired.push_back(i);
            }
            need(wired.size() == 3, "confirmed three wired ports");
            unsigned received = 0;
            const unsigned lengths[]{0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64};
            for (const auto source : wired)
                for (unsigned sequence = 0; sequence < 120; ++sequence) {
                    Ucal::CanFrame f{};
                    f.id = 0x410u + static_cast<std::uint32_t>(source);
                    const bool fd = sequence % 2 != 0;
                    f.flags = (sequence % 3 == 0 ? Ucal::FrameExtended : 0) |
                              (fd ? Ucal::FrameFd | Ucal::FrameBrs : 0);
                    f.dlc = fd ? static_cast<std::uint8_t>(sequence % 16) : 8;
                    f.dataLength = fd ? static_cast<std::uint8_t>(lengths[f.dlc]) : 8;
                    for (unsigned n = 0; n < f.dataLength; ++n)
                        f.data[n] = static_cast<std::uint8_t>(sequence + n + source);
                    need(client.submitCan(leases[source], f) == Ucal::Status::Ok, "physical submit");
                    std::set<std::size_t> got;
                    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
                    while (got.size() < 2 && std::chrono::steady_clock::now() < deadline) {
                        for (const auto target : wired) {
                            std::vector<Ucal::CanEvent> events;
                            need(client.readCanEvents(leases[target], 64, events) == Ucal::Status::Ok,
                                 "physical read");
                            for (const auto &e : events) {
                                need(e.kind != Ucal::CanEventKind::TransmitFailed &&
                                         e.kind != Ucal::CanEventKind::QueueOverrun,
                                     "physical error event");
                                if (e.kind == Ucal::CanEventKind::Received && target != source) {
                                    need(e.frame.id == f.id && e.frame.flags == f.flags &&
                                             e.frame.dlc == f.dlc && e.frame.dataLength == f.dataLength &&
                                             e.frame.timestampUs > 0 &&
                                             std::equal(f.data.begin(), f.data.begin() + f.dataLength,
                                                        e.frame.data.begin()),
                                         "physical payload flags timestamp");
                                    need(got.insert(target).second, "duplicate physical frame");
                                    ++received;
                                }
                            }
                        }
                        if (got.size() < 2)
                            Sleep(1);
                    }
                    need(got.size() == 2, "physical receive deadline");
                }
            for (const auto token : leases)
                need(client.closeDevice(token) == Ucal::Status::Ok, "physical close");
            leases.clear();
            std::cout << "{\"case\":\"physical\",\"status\":\"passed\",\"concurrentChannels\":5,"
                         "\"instances\":2,\"sent\":360,\"received\":"
                      << received << ",\"dataBitrate\":8000000}" << std::endl;
            return 0;
        }
        std::set<Ucal::DeviceId> ids;
        std::set<std::string> instances;
        for (const auto &e : es) {
            need(ids.insert(e.id).second, "duplicate endpoint identity");
            instances.insert(e.backendInstanceId);
        }
        if (scenario == "multiple" || scenario == "refresh" || scenario == "deferred")
            need(es.size() == 8 && instances.size() == 2, "two instances with four channels each");
        else if (scenario == "j2534")
            need(es.size() == 2 && instances.size() == 2, "two standard J2534 cores");
        else
            need(es.size() >= 2, "fault peer catalog");
        for (const auto &e : es) {
            if (scenario == "fault" && e.physicalIndex > 0)
                continue;
            Ucal::CanChannelConfig config{};
            config.canFd = true;
            const auto o = client.openCanChannel(e.id, config);
            need(o.status == Ucal::Status::Ok, "open separate channel");
            leases.push_back(o.leaseToken);
        }
        if (scenario == "deferred") {
            wchar_t path[32768]{}, value[32768]{};
            need(GetEnvironmentVariableW(L"UCAL_BACKEND_CATALOG", path, 32768) > 0, "catalog path");
            need(client.closeDevice(leases[1]) == Ucal::Status::Ok, "release one port");
            leases[1] = 0;
            GetPrivateProfileStringW(L"plugin.1", L"configuration", L"", value, 32768, path);
            WritePrivateProfileStringW(L"plugin.0", L"configuration", value, path);
            std::vector<Ucal::EndpointDescriptor> updated;
            need(client.enumerateEndpoints(updated) == Ucal::Status::Ok && updated.size() == 8,
                 "deferred catalog");
            Ucal::CanChannelConfig c{};
            c.canFd = true;
            need(client.openCanChannel(es[1].id, c).status == Ucal::Status::Busy,
                 "new open used superseded configuration");
        }
        if (scenario == "refresh") {
            wchar_t path[32768]{};
            need(GetEnvironmentVariableW(L"UCAL_BACKEND_CATALOG", path, 32768) > 0, "catalog path");
            wchar_t value[32768]{};
            for (const auto key : {L"family", L"mode", L"library", L"configuration", L"worker"}) {
                GetPrivateProfileStringW(L"plugin.0", key, L"", value, 32768, path);
                need(WritePrivateProfileStringW(L"plugin.2", key, value, path) != 0, "add plugin field");
            }
            WritePrivateProfileStringW(L"plugin.2", L"id", L"c", path);
            WritePrivateProfileStringW(L"catalog", L"count", L"3", path);
            std::vector<Ucal::EndpointDescriptor> updated;
            need(client.enumerateEndpoints(updated) == Ucal::Status::Ok && updated.size() == 12,
                 "live add backend");
            for (const auto &original : es)
                need(std::any_of(updated.begin(), updated.end(),
                                 [&](const auto &e) { return e.id == original.id; }),
                     "live add moved identity");
            WritePrivateProfileStringW(L"catalog", L"count", L"1", path);
            need(client.enumerateEndpoints(updated) == Ucal::Status::Ok && updated.size() == 12,
                 "retired endpoints lost");
            need(std::count_if(updated.begin(), updated.end(),
                               [](const auto &e) { return e.state == Ucal::DeviceState::Offline; }) == 8,
                 "offline tombstones");
            Ucal::CanChannelConfig cfg{};
            cfg.canFd = true;
            need(client.openCanChannel(es.back().id, cfg).status == Ucal::Status::NotFound,
                 "retired backend accepted new open");
        }
        std::future<Ucal::Status> fault;
        if (scenario == "fault") {
            fault = std::async(std::launch::async, [&]() {
                Ucal::Client other;
                std::vector<Ucal::CanEvent> events;
                return other.readCanEvents(leases.front(), 8u, events);
            });
            Sleep(100);
        }
        for (std::size_t i = scenario == "fault" ? 1u : 0u; i < leases.size(); ++i) {
            if (!leases[i])
                continue;
            const auto independentStart = std::chrono::steady_clock::now();
            Ucal::CanFrame f{};
            f.id = 0x321u + static_cast<std::uint32_t>(i);
            f.flags = Ucal::FrameFd | Ucal::FrameBrs;
            f.dlc = 15;
            f.dataLength = 64;
            f.data[63] = static_cast<std::uint8_t>(i + 31u);
            need(client.submitCan(leases[i], f) == Ucal::Status::Ok, "submit separate channel");
            std::vector<Ucal::CanEvent> events;
            const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (events.empty() && std::chrono::steady_clock::now() < end) {
                need(client.readCanEvents(leases[i], 8u, events) == Ucal::Status::Ok, "read channel");
                if (events.empty())
                    Sleep(1);
            }
            need(events.size() == 1 && events[0].kind == Ucal::CanEventKind::Received &&
                     events[0].frame.id == f.id && events[0].frame.data[63] == f.data[63],
                 "cross-channel contamination");
            if (scenario == "fault")
                need(std::chrono::steady_clock::now() - independentStart < std::chrono::milliseconds(700),
                     "another instance waited for the hung worker");
            need(client.closeDevice(leases[i]) == Ucal::Status::Ok, "close individual channel");
        }
        if (scenario == "fault") {
            need(fault.wait_for(std::chrono::seconds(5)) == std::future_status::ready &&
                     fault.get() == Ucal::Status::TransportError,
                 "fault containment deadline");
            wchar_t path[32768]{}, configuration[32768]{};
            need(GetEnvironmentVariableW(L"UCAL_BACKEND_CATALOG", path, 32768) > 0, "fault catalog path");
            GetPrivateProfileStringW(L"plugin.1", L"configuration", L"", configuration, 32768, path);
            WritePrivateProfileStringW(L"plugin.0", L"configuration", configuration, path);
            std::vector<Ucal::EndpointDescriptor> recovered;
            need(client.enumerateEndpoints(recovered) == Ucal::Status::Ok, "replace lost worker");
            Ucal::CanChannelConfig c{};
            c.canFd = true;
            auto opened = client.openCanChannel(es.front().id, c);
            need(opened.status == Ucal::Status::Ok, "stale worker leases blocked replacement");
            Ucal::CanFrame old{};
            old.id = 1;
            need(client.submitCan(leases.front(), old) == Ucal::Status::NotFound,
                 "old lease reached a new generation");
            need(client.closeDevice(opened.leaseToken) == Ucal::Status::Ok, "close recovered generation");
            client.closeDevice(leases.front());
        }
        leases.clear();
        std::vector<Ucal::EndpointDescriptor> after;
        need(client.enumerateEndpoints(after) == Ucal::Status::Ok &&
                 after.size() == (scenario == "refresh" ? 12u : es.size()),
             "catalog after close");
        for (std::size_t i = 0; i < es.size(); ++i)
            need(es[i].id == after[i].id, "unstable instance identity");
        std::cout << "{\"case\":\"" << scenario << "\",\"endpoints\":" << es.size()
                  << ",\"instances\":" << instances.size() << ",\"status\":\"passed\"}" << std::endl;
        return 0;
    } catch (const std::exception &e) {
        for (auto t : leases)
            client.closeDevice(t);
        std::cerr << e.what() << std::endl;
        return 1;
    }
}
