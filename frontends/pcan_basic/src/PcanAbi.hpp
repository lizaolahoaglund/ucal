#pragma once

#include <cstddef>
#include <cstdint>

namespace Ucal::Pcan {

using Status = std::uint32_t;
using Handle = std::uint16_t;
using Baudrate = std::uint16_t;
using MessageType = std::uint8_t;
using Parameter = std::uint8_t;

inline constexpr Status success = 0u;
inline constexpr Status transmitFull = 0x00001u;
inline constexpr Status busLight = 0x00004u;
inline constexpr Status busHeavy = 0x00008u;
inline constexpr Status busOff = 0x00010u;
inline constexpr Status receiveEmpty = 0x00020u;
inline constexpr Status receiveOverrun = 0x00040u;
inline constexpr Status transmitQueueFull = 0x00080u;
inline constexpr Status driverNotLoaded = 0x00200u;
inline constexpr Status hardwareInUse = 0x00400u;
inline constexpr Status invalidHardware = 0x01400u;
inline constexpr Status resourceError = 0x02000u;
inline constexpr Status invalidParameterType = 0x04000u;
inline constexpr Status invalidParameter = 0x08000u;
inline constexpr Status unknownError = 0x10000u;
inline constexpr Status invalidData = 0x20000u;
inline constexpr Status busPassive = 0x40000u;
inline constexpr Status invalidMode = 0x80000u;
inline constexpr Status notInitialized = 0x4000000u;
inline constexpr Status illegalOperation = 0x8000000u;

inline constexpr Handle noneBus = 0x00u;
inline constexpr Handle usbBus1 = 0x51u;
inline constexpr std::uint32_t maximumChannels = 16u;

inline constexpr MessageType messageStandard = 0x00u;
inline constexpr MessageType messageRtr = 0x01u;
inline constexpr MessageType messageExtended = 0x02u;
inline constexpr MessageType messageFd = 0x04u;
inline constexpr MessageType messageBrs = 0x08u;
inline constexpr MessageType messageEsi = 0x10u;

inline constexpr Parameter deviceId = 0x01u;
inline constexpr Parameter receiveEvent = 0x03u;
inline constexpr Parameter messageFilter = 0x04u;
inline constexpr Parameter apiVersion = 0x05u;
inline constexpr Parameter listenOnly = 0x08u;
inline constexpr Parameter channelCondition = 0x0du;
inline constexpr Parameter hardwareName = 0x0eu;
inline constexpr Parameter channelFeatures = 0x16u;
inline constexpr Parameter bitrateInfo = 0x18u;
inline constexpr Parameter bitrateInfoFd = 0x19u;
inline constexpr Parameter busSpeedNominal = 0x1au;
inline constexpr Parameter busSpeedData = 0x1bu;
inline constexpr Parameter attachedChannelsCount = 0x2au;
inline constexpr Parameter attachedChannels = 0x2bu;

inline constexpr std::uint32_t channelUnavailable = 0u;
inline constexpr std::uint32_t channelAvailable = 1u;
inline constexpr std::uint32_t channelOccupied = 2u;
inline constexpr std::uint32_t featureFdCapable = 1u;

inline constexpr std::uint32_t deviceAll = 0u;
inline constexpr std::uint32_t deviceUsb = 3u;
inline constexpr std::uint32_t lastKnownDeviceClass = 9u;

// Original, independently described ABI record used by PCAN_GetAvailableHardware.
// The names express the observed wire meaning and intentionally do not reproduce
// a vendor SDK header.
#pragma pack(push, 1)
struct AvailableHardware {
    std::uint32_t deviceClass;
    std::uint8_t type;
    char name[33];
    std::uint32_t hardwareId;
    std::uint32_t channel;
    std::uint32_t reserved;
};
#pragma pack(pop)

struct ClassicMessage {
    std::uint32_t id;
    MessageType type;
    std::uint8_t length;
    std::uint8_t data[8];
};

struct ClassicTimestamp {
    std::uint32_t milliseconds;
    std::uint16_t millisecondsOverflow;
    std::uint16_t microseconds;
};

struct FdMessage {
    std::uint32_t id;
    MessageType type;
    std::uint8_t dlc;
    std::uint8_t data[64];
};

struct ChannelInformation {
    Handle channelHandle;
    std::uint8_t deviceType;
    std::uint8_t controllerNumber;
    std::uint32_t deviceFeatures;
    char deviceName[33];
    std::uint32_t deviceId;
    std::uint32_t channelCondition;
};

struct Diagnostics {
    std::uint32_t size;
    std::uint32_t enumerationCalls;
    std::uint32_t lastChannelCount;
};

static_assert(sizeof(AvailableHardware) == 50, "PCAN available-hardware ABI changed");
static_assert(sizeof(ClassicMessage) == 16, "PCAN classic-message ABI changed");
static_assert(sizeof(ClassicTimestamp) == 8, "PCAN classic-timestamp ABI changed");
static_assert(sizeof(FdMessage) == 72, "PCAN FD-message ABI changed");
static_assert(sizeof(ChannelInformation) == 52, "PCAN channel-information ABI changed");
static_assert(offsetof(AvailableHardware, type) == 4, "PCAN type offset changed");
static_assert(offsetof(AvailableHardware, name) == 5, "PCAN name offset changed");
static_assert(offsetof(AvailableHardware, hardwareId) == 38, "PCAN hardware id offset changed");
static_assert(offsetof(AvailableHardware, channel) == 42, "PCAN channel offset changed");
static_assert(offsetof(AvailableHardware, reserved) == 46, "PCAN reserved offset changed");

}  // namespace Ucal::pcan
