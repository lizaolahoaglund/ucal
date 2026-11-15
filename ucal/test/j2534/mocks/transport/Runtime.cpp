#include "ucal/J2534Abi.hpp"
#include "ucal/ProviderQueries.h"
#include <array>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <windows.h>
#include "../../../ucal/include/ucal/J2534Discovery.h"
#include "Multichannel.hpp"

using namespace Ucal::J2534Backend::Abi;
using CanTimingV1 = UcalProviderCanTimingV1;
using PortV1 = UcalProviderPortV1;
namespace {
DWORD callingThread{};
bool opened{}, connected{};
U32 filterCount{};
U32 currentProtocol{};
std::map<U32, U32> config;
std::deque<Message> received;
std::string scenario() {
    std::array<char, 64> value{};
    GetEnvironmentVariableA("UCAL_J2534_TEST_CASE", value.data(), static_cast<DWORD>(value.size()));
    return value.data();
}
U32 deviceId() { return scenario() == "zero-handles" ? 0u : 101u; }
U32 channelId() { return scenario() == "zero-handles" ? 0u : 202u; }
bool correctThread() {
    if (!callingThread)
        callingThread = GetCurrentThreadId();
    return callingThread == GetCurrentThreadId();
}
} // namespace
extern "C" {
Result __stdcall PassThruOpen(void *selector, U32 *output) {
    if (Multi::active())
        return Multi::open(selector, output);
    const bool generic = scenario().find("standard-") == 0u;
    if (!correctThread() || !output ||
        (generic ? selector != nullptr : (!selector ||
         (std::strcmp(static_cast<char *>(selector), "fixture-device:0") &&
          std::strcmp(static_cast<char *>(selector), "000000000000000000000001:0")))))
        return 4;
    if (scenario() == "hang-open") {
        Sleep(10000);
        return 7;
    }
    if (scenario() == "busy")
        return 0xe;
    if (opened)
        return 0xe;
    opened = true;
    *output = deviceId();
    return 0;
}
Result __stdcall PassThruClose(U32 device) {
    if (Multi::active())
        return Multi::close(device);
    if (!correctThread() || device != deviceId() || !opened || connected || filterCount)
        return 7;
    opened = false;
    if (scenario() == "disconnected-close") return disconnected;
    return 0;
}
Result __stdcall PassThruConnect(U32 device, U32 protocol, U32 flags, U32 rate, U32 *output) {
    if (Multi::active())
        return Multi::connect(device, protocol, flags, rate, output);
    if (!correctThread() || device != deviceId() || !opened || connected || !output ||
        (protocol != gmCanFd && protocol != can) || flags != bothIds || rate == 0u)
        return 3;
    if (scenario() == "sae-only" || scenario() == "classic-only" || scenario() == "isotp-only")
        return 3;
    connected = true;
    currentProtocol = protocol;
    config = {
        {dataRate, rate}, {gmDataRate, rate * 10u}, {pins, 0u},           {gmType, 0u},
        {loopback, 0u},    {gmTermination, 0u},       {hsTermination, 0u}, {samplePoint, 80u}};
    *output = channelId();
    return 0;
}
Result __stdcall PassThruDisconnect(U32 channel) {
    if (Multi::active())
        return Multi::disconnect(channel);
    if (!correctThread() || channel != channelId() || !connected || filterCount)
        return 7;
    connected = false;
    received.clear();
    if (scenario() == "disconnected-close") return disconnected;
    return 0;
}
Result __stdcall PassThruReadVersion(U32 device, char *firmware, char *dll, char *api) {
    if (Multi::active())
        return Multi::version(device, firmware, dll, api);
    if (!correctThread() || !opened || device != deviceId())
        return 0x1a;
    strcpy_s(firmware, 80, "fixture");
    strcpy_s(dll, 80, "0.0.0.0");
    strcpy_s(api, 80, scenario() == "wrong-api" ? "05.00" : "04.04");
    return 0;
}
Result __stdcall PassThruGetLastError(char *output) {
    strcpy_s(output, 80, "fixture");
    return 0;
}
Result __stdcall PassThruIoctl(U32 channel, U32 operation, void *input, void *) {
    if (Multi::active())
        return Multi::ioctl(channel, operation, input);
    if (!correctThread() || channel != channelId() || !connected || !input)
        return 2;
    auto *list = static_cast<ConfigList *>(input);
    if (list->count != 1u || !list->items || !config.count(list->items->parameter))
        return 5;
    auto &c = *list->items;
    if (operation == getConfig && scenario() == "standard-set-only") return 1;
    if (operation == getConfig && scenario() == "standard-get-busy") return timeout;
    if (c.parameter == pins && scenario() == "standard-default-pins") return 1;
    if (operation == setConfig && c.parameter == samplePoint && scenario() == "standard-read-only") return 1;
    if (operation == setConfig && c.parameter == samplePoint && scenario() == "standard-invalid-value") return 5;
    if (operation == setConfig && c.parameter == gmTermination && scenario() == "standard-unsupported-termination") return 1;

    if (scenario() == "no-data-rate" && c.parameter == gmDataRate)
        return 1;
    if (operation == setConfig) {
        if (c.parameter == pins && (c.value != 0x060eu || config[gmDataRate] != 2000000u))
            return 5;
        if (c.parameter == pins && filterCount != 2u)
            return 5; // Activation must not acknowledge frames before filters exist.
        config[c.parameter] = c.value;
        return 0;
    }
    if (operation != getConfig)
        return 0xf;
    c.value = config[c.parameter];
    if (scenario() == "wrong-readback" && c.parameter == dataRate)
        ++c.value;
    return 0;
}
Result __stdcall PassThruStartMsgFilter(U32 channel, U32 type, Message *mask, Message *pattern,
                                        Message *flow, U32 *output) {
    if (Multi::active())
        return Multi::startFilter(channel, type, mask, pattern, flow, output);
    if (!correctThread() || channel != channelId() || !connected || type != 1u || !mask || !pattern ||
        flow || !output || mask->protocol != currentProtocol ||
        pattern->protocol != currentProtocol || mask->dataSize != 4u ||
        pattern->dataSize != 4u || mask->txFlags != pattern->txFlags ||
        (pattern->txFlags != 0u && pattern->txFlags != extended))
        return 0xa;
    if (scenario() == "filter-failure" && filterCount == 1u)
        return 0xc;
    *output = filterCount++;
    return 0;
}
Result __stdcall PassThruStopMsgFilter(U32 channel, U32 filter) {
    if (Multi::active())
        return Multi::stopFilter(channel, filter);
    if (!correctThread() || channel != channelId() || filter > 1u || !filterCount)
        return 0x16;
    --filterCount;
    if (scenario() == "disconnected-close") return disconnected;
    return 0;
}
Result __stdcall PassThruWriteMsgs(U32 channel, Message *messages, U32 *count, U32 timeoutMs) {
    if (Multi::active())
        return Multi::write(channel, messages, count, timeoutMs);
    if (!correctThread() || channel != channelId() || !connected || !messages || !count ||
        timeoutMs != 0u)
        return 4;
    const auto requested = *count;
    *count = 0u;
    if (scenario() == "disconnected-write") return disconnected;
    if (scenario() == "timeout-write") return timeout;
    if (scenario() == "failed-write") return 7;
    if (scenario() == "oversized-count") {
        *count = requested + 1u;
        return 0;
    }
    for (U32 i = 0; i < requested; ++i) {
        if (scenario() == "partial" && i == 1u)
            return full;
        auto m = messages[i];
        if (m.protocol != currentProtocol || m.dataSize < 4u || m.dataSize > 68u ||
            m.rxStatus != 0u || m.txFlags & ~(extended | fdBrs | fdFormat))
            return 0xa;
        m.rxStatus = m.txFlags;
        m.txFlags = 0u;
        m.timestamp = static_cast<U32>(100u + received.size());
        if (scenario() == "wrap")
            m.timestamp = received.empty() ? 0xfffffff0u : 0x10u;
        if (scenario() == "backward-clock")
            m.timestamp = received.empty() ? 100u : 90u;
        if (scenario() == "malformed-rx")
            m.dataSize = 69u;
        if (scenario() == "sae-flags")
            m.rxStatus = 0x100000u;
        if (scenario() == "echo")
            m.rxStatus |= txEcho;
        if (scenario() == "error-indication")
            m.rxStatus = errorIndication;
        if (scenario() == "esi")
            m.rxStatus |= fdEsi;
        received.push_back(m);
        ++*count;
    }
    return 0;
}
Result __stdcall PassThruReadMsgs(U32 channel, Message *messages, U32 *count, U32 timeoutMs) {
    if (Multi::active())
        return Multi::read(channel, messages, count, timeoutMs);
    if (!correctThread() || channel != channelId() || !connected || !messages || !count ||
        timeoutMs != 0u)
        return 4;
    if (scenario() == "hang-read")
        Sleep(10000u);
    if (scenario() == "crash-read")
        TerminateProcess(GetCurrentProcess(), 99u);
    const auto requested = *count;
    *count = 0u;
    if (scenario() == "disconnected-read") return disconnected;
    while (*count < requested && !received.empty()) {
        messages[*count] = received.front();
        received.pop_front();
        ++*count;
    }
    if (scenario() == "overflow-rx" || scenario() == "overflow-full")
        return overflow;
    return *count ? 0 : empty;
}
Result __stdcall PassThruStartPeriodicMsg(U32, Message*, U32*, U32) { return notSupported; }
Result __stdcall PassThruStopPeriodicMsg(U32, U32) { return notSupported; }
Result __stdcall PassThruSetProgrammingVoltage(U32, U32, U32) { return notSupported; }
Result __stdcall UcalFixtureGetCanTiming(U32 channel, CanTimingV1* output) {
    if (!correctThread() || !connected || channel != channelId() || !output ||
        output->size != sizeof(*output) || output->version != 1u) return 4;
    *output = {sizeof(*output), 1u, config[dataRate], config[gmDataRate], 75u,
               scenario() == "timing-mismatch" ? 70u : 80u};
    return 0;
}
Result __stdcall UcalFixtureEnumeratePorts(U32 size, U32 version, PortV1* output, U32* count) {
    if (!correctThread() || size != sizeof(PortV1) || version != 1u || !output || !count || *count < 1u) return 4;
    *count = 1u;
    *output = {sizeof(PortV1), 1u, "000000000000000000000001:0", 0u, 3u};
    if (scenario() == "ports-malformed") output->size = 0u;
    return 0;
}
}

extern "C" int32_t __cdecl UcalJ2534DiscoverV1(const uint16_t* path, UcalJ2534PortV1* ports, uint32_t capacity, uint32_t* count) {
    if (!path || !ports || !count || capacity < 2) return 1;
    wchar_t control[2048]{};
    GetEnvironmentVariableW(L"UCAL_J2534_CONFIG", control, 2048);
    const auto n = GetPrivateProfileIntW(L"discovery-test", L"count", 1, control);
    if (n > 2) { *count = 65; return 0; }
    *count = n;
    for (unsigned i = 0; i < n; ++i) {
        auto& p = ports[i]; p = {}; p.size = sizeof(p); p.version = 1; p.physicalPort = i; p.capabilities = 7;
        strcpy_s(p.identity, "fixture-device"); strcpy_s(p.adapterIdentity, "physical-fixture");
        strcpy_s(p.selector, i ? "fixture-device:1" : "fixture-device:0");
        strcpy_s(p.name, i ? "Dynamic CAN 2" : "Dynamic CAN 1"); strcpy_s(p.model, "Dynamic fixture");
    }
    return 0;
}

extern "C" int32_t __cdecl UcalJ2534DiscoverV2(const uint16_t *path, UcalJ2534PortV2 *ports,
                                                  uint32_t capacity, uint32_t *count) {
    if (!path || !ports || !count || capacity > 64)
        return 1;
    if (Multi::active())
        return Multi::discover(ports, capacity, count);
    std::array<UcalJ2534PortV1, 64> legacy{};
    const auto status = UcalJ2534DiscoverV1(path, legacy.data(), capacity, count);
    if (status || *count > capacity)
        return status;
    for (U32 i = 0; i < *count; ++i) {
        auto &p = ports[i];
        p = {};
        p.port = legacy[i];
        p.port.size = sizeof(p);
        p.port.version = 2;
        p.connectionModel = UCAL_J2534_OPEN_PER_PORT;
        p.classicProtocol = can;
        p.fdProtocol = gmCanFd;
    }
    return 0;
}
