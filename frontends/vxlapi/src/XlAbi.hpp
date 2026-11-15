#pragma once

#include <cstdint>

namespace Ucal::Xl {

using Status = std::int16_t;
using Access = std::uint64_t;
using PortHandle = std::int32_t;

inline constexpr Status success = 0;
inline constexpr Status queueEmpty = 10;
inline constexpr Status queueFull = 11;
inline constexpr Status wrongParameter = 101;
inline constexpr Status notImplemented = 117;
inline constexpr Status invalidPort = 118;
inline constexpr Status cannotOpenDriver = 201;

inline constexpr std::uint32_t busTypeNone = 0u;
inline constexpr std::uint32_t busTypeCan = 1u;
inline constexpr std::uint32_t busTypeLin = 2u;
inline constexpr std::uint8_t hardwareTypeVirtual = 1u;
inline constexpr std::uint32_t busCompatibleCan = 1u;
inline constexpr std::uint32_t busActiveCan = 1u << 16;
inline constexpr std::uint32_t busCompatibleLin = 2u;
inline constexpr std::uint32_t busActiveLin = 2u << 16;
inline constexpr std::uint32_t channelCanfdIso = 0x80000000u;

#pragma pack(push, 1)
union BusData {
    struct {
        std::uint32_t bitrate;
        std::uint8_t sjw;
        std::uint8_t tseg1;
        std::uint8_t tseg2;
        std::uint8_t sam;
        std::uint8_t outputMode;
        std::uint8_t reserved[7];
        std::uint8_t canOperationMode;
    } can;
    struct {
        std::uint32_t arbitrationBitrate;
        std::uint8_t arbitrationSjw;
        std::uint8_t arbitrationTseg1;
        std::uint8_t arbitrationTseg2;
        std::uint8_t arbitrationSam;
        std::uint8_t outputMode;
        std::uint8_t dataSjw;
        std::uint8_t dataTseg1;
        std::uint8_t dataTseg2;
        std::uint32_t dataBitrate;
        std::uint8_t canOperationMode;
    } canFd;
    std::uint8_t raw[28];
};

struct BusParams {
    std::uint32_t busType;
    BusData data;
};

struct ChannelConfig {
    char name[32];
    std::uint8_t hardwareType;
    std::uint8_t hardwareIndex;
    std::uint8_t hardwareChannel;
    std::uint16_t transceiverType;
    std::uint16_t transceiverState;
    std::uint16_t configError;
    std::uint8_t channelIndex;
    Access channelMask;
    std::uint32_t channelCapabilities;
    std::uint32_t channelBusCapabilities;
    std::uint8_t isOnBus;
    std::uint32_t connectedBusType;
    BusParams busParams;
    std::uint32_t compatibilityPadding;
    std::uint32_t driverVersion;
    std::uint32_t interfaceVersion;
    std::uint32_t rawData[10];
    std::uint32_t serialNumber;
    std::uint32_t articleNumber;
    char transceiverName[32];
    std::uint32_t specialCabFlags;
    std::uint32_t dominantTimeout;
    std::uint8_t dominantRecessiveDelay;
    std::uint8_t recessiveDominantDelay;
    std::uint8_t connectionInfo;
    std::uint8_t currentlyAvailableTimestamps;
    std::uint16_t minimalSupplyVoltage;
    std::uint16_t maximalSupplyVoltage;
    std::uint32_t maximalBaudrate;
    std::uint8_t fpgaCoreCapabilities;
    std::uint8_t specialDeviceStatus;
    std::uint16_t channelBusActiveCapabilities;
    std::uint16_t breakOffset;
    std::uint16_t delimiterOffset;
    std::uint32_t reserved[3];
};

struct DriverConfig {
    std::uint32_t dllVersion;
    std::uint32_t channelCount;
    std::uint32_t reserved[10];
    ChannelConfig channel[64];
};

struct CanFdConfig {
    std::uint32_t arbitrationBitrate;
    std::uint32_t arbitrationSjw;
    std::uint32_t arbitrationTseg1;
    std::uint32_t arbitrationTseg2;
    std::uint32_t dataBitrate;
    std::uint32_t dataSjw;
    std::uint32_t dataTseg1;
    std::uint32_t dataTseg2;
    std::uint32_t reserved[2];
};
#pragma pack(pop)

struct Diagnostics {
    std::uint32_t size;
    std::uint32_t openDriverCalls;
    std::uint32_t getDriverConfigCalls;
    std::int32_t lastChannelCount;
};

static_assert(sizeof(BusParams) == 32, "XL bus parameters ABI changed");
static_assert(sizeof(ChannelConfig) == 227, "XL channel configuration ABI changed");
static_assert(sizeof(DriverConfig) == 14576, "XL driver configuration ABI changed");

}  // namespace Ucal::xl
