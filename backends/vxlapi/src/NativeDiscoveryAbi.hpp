#pragma once
#include <cstdint>
// Original declarations of this plugin's public read-only discovery ABI.
namespace Ucal::NativeDiscoveryAbi {
#pragma pack(push, 1)
union XlBusData {
    struct {
        std::uint32_t bitrate;
        std::uint8_t sjw;
        std::uint8_t tseg1;
        std::uint8_t tseg2;
        std::uint8_t sam;
        std::uint8_t outputMode;
        std::uint8_t reserved[7];
        std::uint8_t operationMode;
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
        std::uint8_t operationMode;
    } canFd;
    std::uint8_t raw[28];
};

struct XlBusParams {
    std::uint32_t busType;
    XlBusData data;
};

struct XlChannelConfig {
    char name[32];
    std::uint8_t hardwareType;
    std::uint8_t hardwareIndex;
    std::uint8_t hardwareChannel;
    std::uint16_t transceiverType;
    std::uint16_t transceiverState;
    std::uint16_t configError;
    std::uint8_t channelIndex;
    std::uint64_t channelMask;
    std::uint32_t channelCapabilities;
    std::uint32_t channelBusCapabilities;
    std::uint8_t isOnBus;
    std::uint32_t connectedBusType;
    XlBusParams busParams;
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

struct XlDriverConfig {
    std::uint32_t dllVersion;
    std::uint32_t channelCount;
    std::uint32_t reserved[10];
    XlChannelConfig channel[64];
};
#pragma pack(pop)
static_assert(sizeof(XlChannelConfig) == 227 && sizeof(XlDriverConfig) == 14576, "XL discovery ABI");
}
