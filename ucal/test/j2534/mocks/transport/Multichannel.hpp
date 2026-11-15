#pragma once
#include <set>
// Original runtime with real independent handles, queues, timing and resources.
// It models both public connection shapes; it is not evidence of vendor support.
namespace Multi {
using namespace Ucal::J2534Backend::Abi;
struct Channel {
    U32 device{}, protocol{};
    std::map<U32, U32> config;
    std::set<U32> filters;
    std::deque<Message> rx;
};
inline std::map<U32, std::string> devices;
inline std::map<U32, Channel> channels;
inline U32 nextDevice = 100, nextChannel = 200;
inline DWORD thread{};
inline std::string scenario() {
    char s[80]{};
    GetEnvironmentVariableA("UCAL_J2534_TEST_CASE", s, 80);
    return s;
}
inline bool active() { return scenario().find("multi-") == 0; }
inline bool named() { return scenario().find("named") != std::string::npos; }
inline bool onThread() {
    if (!thread)
        thread = GetCurrentThreadId();
    return thread == GetCurrentThreadId();
}
inline std::wstring control() {
    wchar_t p[2048]{};
    GetEnvironmentVariableW(L"UCAL_J2534_CONFIG", p, 2048);
    return p;
}
inline Result open(void *selector, U32 *output) {
    if (!onThread() || !output)
        return 4;
    const auto text = selector ? std::string(static_cast<char *>(selector)) : std::string{};
    if (named() ? text.find("fixture-multi:") != 0 : selector != nullptr)
        return 4;
    for (const auto &d : devices)
        if (d.second == text)
            return 0xe;
    const auto id = scenario().find("zero") != std::string::npos && devices.empty() ? 0u : ++nextDevice;
    devices.emplace(id, text);
    *output = id;
    return 0;
}
inline Result close(U32 device) {
    if (!onThread() || !devices.count(device))
        return 0x1a;
    for (const auto &c : channels)
        if (c.second.device == device)
            return 7;
    devices.erase(device);
    return 0;
}
inline Result connect(U32 device, U32 protocol, U32 flags, U32 rate, U32 *output) {
    if (!onThread() || !devices.count(device) || !output || flags != bothIds || !rate ||
        (protocol != gmCanFd && protocol != can && (protocol < 0x9000 || protocol > 0x907f)))
        return 3;
    const auto path = control();
    if (GetPrivateProfileIntW(L"multi-test", L"fail_connect", 0, path.c_str())) {
        WritePrivateProfileStringW(L"multi-test", L"fail_connect", L"0", path.c_str());
        return 0xc;
    }
    const auto id = ++nextChannel;
    Channel c{};
    c.device = device;
    c.protocol = protocol;
    c.config = {{dataRate, rate}, {gmDataRate, 2000000}, {pins, 0},           {gmType, 0},
                {loopback, 0},     {gmTermination, 0},     {hsTermination, 0}, {samplePoint, 75}};
    channels.emplace(id, std::move(c));
    *output = id;
    return 0;
}
inline Result disconnect(U32 channel) {
    const auto it = channels.find(channel);
    if (!onThread() || it == channels.end() || !it->second.filters.empty())
        return 2;
    channels.erase(it);
    return 0;
}
inline Result version(U32 device, char *firmware, char *dll, char *api) {
    if (!onThread() || !devices.count(device))
        return 0x1a;
    strcpy_s(firmware, 80, "original multichannel fixture");
    strcpy_s(dll, 80, "0.0.0.0");
    strcpy_s(api, 80, "04.04");
    return 0;
}
inline Result ioctl(U32 channel, U32 operation, void *input) {
    const auto it = channels.find(channel);
    if (!onThread() || it == channels.end() || !input)
        return 2;
    auto *list = static_cast<ConfigList *>(input);
    if (list->count != 1 || !list->items)
        return 4;
    auto &c = it->second;
    auto &p = *list->items;
    if (!c.config.count(p.parameter))
        return 5;
    if (operation == getConfig) {
        p.value = c.config[p.parameter];
        return 0;
    }
    if (operation != setConfig)
        return 0xf;
    if (p.parameter == pins) {
        if (c.filters.size() != 2)
            return 5;
        for (const auto &peer : channels)
            if (peer.first != channel && peer.second.device == c.device &&
                peer.second.config.at(pins) == p.value)
                return 0x14;
    }
    c.config[p.parameter] = p.value;
    return 0;
}
inline Result startFilter(U32 channel, U32 type, Message *mask, Message *pattern, Message *flow,
                           U32 *output) {
    const auto it = channels.find(channel);
    if (!onThread() || it == channels.end() || type != 1 || !mask || !pattern || flow || !output ||
        mask->protocol != it->second.protocol || pattern->protocol != it->second.protocol ||
        it->second.filters.size() >= 2)
        return 0xa;
    if (it->second.filters.size() == 1 &&
        GetPrivateProfileIntW(L"multi-test", L"fail_filter", 0, control().c_str())) {
        WritePrivateProfileStringW(L"multi-test", L"fail_filter", L"0", control().c_str());
        return 0xc;
    }
    const auto id = static_cast<U32>(it->second.filters.size());
    it->second.filters.insert(id);
    *output = id;
    return 0;
}
inline Result stopFilter(U32 channel, U32 filter) {
    const auto it = channels.find(channel);
    if (!onThread() || it == channels.end() || !it->second.filters.erase(filter))
        return 0x16;
    return 0;
}
inline Result write(U32 channel, Message *messages, U32 *count, U32 timeoutMs) {
    const auto it = channels.find(channel);
    if (!onThread() || it == channels.end() || !messages || !count || timeoutMs)
        return 4;
    auto &c = it->second;
    const auto n = *count;
    *count = 0;
    for (U32 i = 0; i < n; ++i) {
        auto m = messages[i];
        if (m.protocol != c.protocol || m.dataSize < 4 || m.dataSize > 68)
            return 0xa;
        m.rxStatus = m.txFlags;
        m.txFlags = 0;
        m.timestamp = static_cast<U32>(100 + c.rx.size());
        c.rx.push_back(m);
        ++*count;
    }
    return 0;
}
inline Result read(U32 channel, Message *messages, U32 *count, U32 timeoutMs) {
    const auto it = channels.find(channel);
    if (!onThread() || it == channels.end() || !messages || !count || timeoutMs)
        return 4;
    auto &c = it->second;
    const auto n = *count;
    *count = 0;
    while (*count < n && !c.rx.empty()) {
        messages[(*count)++] = c.rx.front();
        c.rx.pop_front();
    }
    return *count ? 0 : empty;
}
inline int32_t discover(UcalJ2534PortV2 *output, uint32_t capacity, uint32_t *count) {
    if (capacity < 2)
        return 1;
    *count = 2;
    if (GetPrivateProfileIntW(L"multi-test", L"hidden", 0, control().c_str())) {
        *count = 0;
        return 0;
    }
    const bool changed = GetPrivateProfileIntW(L"multi-test", L"changed", 0, control().c_str()) != 0;
    for (U32 i = 0; i < 2; ++i) {
        auto &p = output[i];
        p = {};
        p.port.size = sizeof(p);
        p.port.version = 2;
        p.port.physicalPort = i;
        p.port.capabilities = 7;
        strcpy_s(p.port.identity, "multi-adapter");
        strcpy_s(p.port.adapterIdentity, "multi-adapter");
        strcpy_s(p.port.name, i ? "Multi CAN 2" : "Multi CAN 1");
        strcpy_s(p.port.model, "Original fixture");
        p.connectionModel = named() ? UCAL_J2534_OPEN_PER_PORT : UCAL_J2534_OPEN_SHARED;
        if (named())
            strcpy_s(p.port.selector, i ? "fixture-multi:1" : "fixture-multi:0");
        else
            strcpy_s(p.deviceKey, "shared-device");
        p.classicProtocol = named() ? can : 0x9000 + i;
        p.fdProtocol = gmCanFd;
        p.pinAssignment = i ? 0x030bu : 0x060eu;
        if (changed && !i)
            p.pinAssignment = 0x0109;
    }
    if (scenario() == "multi-malformed")
        output[1].reserved[0] = 1;
    if (scenario() == "multi-duplicate-route")
        output[1].pinAssignment = output[0].pinAssignment;
    return 0;
}
} // namespace multi
