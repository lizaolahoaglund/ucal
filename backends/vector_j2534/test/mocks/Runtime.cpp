#include "../../src/NativeDiscoveryAbi.hpp"
#include <windows.h>
#include <cstring>
#include <algorithm>
#include <string>
using namespace Ucal::NativeDiscoveryAbi;
namespace {
uint32_t initialized{}, released{};
std::string mode() {
    char s[64]{};
    GetEnvironmentVariableA("UCAL_DISCOVERY_TEST_MODE", s, 64);
    return s;
}
} // namespace
extern "C" void __cdecl UcalTestDiscoveryCounts(uint32_t *init, uint32_t *close) {
    *init = initialized;
    *close = released;
}
extern "C" int16_t __stdcall xlOpenDriver() {
    ++initialized;
    return 0;
}
extern "C" int16_t __stdcall xlCloseDriver() {
    ++released;
    return 0;
}
extern "C" int16_t __stdcall xlGetDriverConfig(XlDriverConfig *output) {
    *output = {};
    output->channelCount = mode() == "oversized" ? 65 : 3;
    for (uint32_t i = 0; i < 3; ++i) {
        auto &c = output->channel[i];
        strcpy_s(c.name, "Original XL fixture");
        c.hardwareType = 55;
        c.hardwareChannel = static_cast<uint8_t>(i);
        c.serialNumber = 4242;
        c.channelCapabilities = 0x80000000;
        c.channelBusCapabilities = 0x10001;
    }
    return 0;
}
extern "C" int16_t __stdcall xlGetApplConfig(char *app, uint32_t slot, uint32_t *type, uint32_t *index,
                                             uint32_t *channel, uint32_t bus) {
    if (std::strcmp(app, "PassThruXLLibrary") || slot || bus != 1 || mode() == "unmapped")
        return 139;
    *type = 55;
    *index = 0;
    *channel = mode() == "mapping-changed" ? 1 : 0;
    return 0;
}
