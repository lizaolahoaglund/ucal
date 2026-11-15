#pragma once
#include <cstdint>
// Original declarations of this plugin's public read-only discovery ABI.
namespace Ucal::NativeDiscoveryAbi {
#pragma pack(push, 4)
struct PcanChannelInformation {
    std::uint16_t handle;
    std::uint8_t deviceType;
    std::uint8_t controllerNumber;
    std::uint32_t features;
    char deviceName[33];
    std::uint32_t deviceId;
    std::uint32_t condition;
};
#pragma pack(pop)
static_assert(sizeof(PcanChannelInformation) == 52, "Public discovery ABI");
}
