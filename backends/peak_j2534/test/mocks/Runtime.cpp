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
extern "C" uint32_t __stdcall CAN_GetValue(uint16_t handle, uint8_t parameter, void *output,
                                           uint32_t capacity) {
    if (parameter == 0x2a && !handle) {
        *static_cast<uint32_t *>(output) = mode() == "oversized" ? 5000 : 2;
        return 0;
    }
    if (parameter == 0x2b && !handle) {
        if (capacity < 2 * sizeof(PcanChannelInformation))
            return 0x8000;
        auto *list = static_cast<PcanChannelInformation *>(output);
        for (uint32_t i = 0; i < 2; ++i) {
            auto &c = list[i];
            c = {};
            c.handle = static_cast<uint16_t>(0x51 + i);
            c.deviceType = 5;
            c.controllerNumber = mode() == "duplicate-identity" ? 0 : static_cast<uint8_t>(i);
            c.features = 1;
            strcpy_s(c.deviceName, "Original PCAN fixture");
            c.condition = 1;
        }
        return 0;
    }
    if (parameter == 0x30 && mode() != "no-identity") {
        constexpr char guid[] = "{11111111-2222-3333-4444-555555555555}";
        if (capacity < sizeof(guid))
            return 0x8000;
        std::memcpy(output, guid, sizeof(guid));
        return 0;
    }
    return 0x4000;
}
