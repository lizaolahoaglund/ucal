#pragma once

#include "ucal/Model.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Ucal::Protocol {

inline constexpr std::uint32_t magic = 0x49414355u;  // "UCAL" on little-endian hosts.
inline constexpr std::uint16_t majorVersion = 1;
inline constexpr std::uint16_t minorVersion = 11;
inline constexpr std::uint32_t maxPayloadBytes = 64u * 1024u;

enum class Operation : std::uint16_t {
    Ping = 1,
    Enumerate = 2,
    OpenDevice = 3,
    CloseDevice = 4,
    OpenCanChannel = 5,
    WriteCan = 6,
    ReadCan = 7,
    GetChannelStatus = 8,
    EnumerateEndpoints = 9,
    SubmitCan = 10,
    ReadCanEvents = 11,
    OpenLinChannel = 12,
    SubmitLin = 13,
    ReadLinEvents = 14,
    GetLinStatus = 15,
    ConfigureLinSlaveResponse = 16,
    EnumerateEndpointsV6 = 17,
    EnumerateBackendHealth = 18,
    SubmitCanBatch = 19,
    EnumerateBackendHealthV8 = 20,
    EnumerateEndpointsV9 = 21,
    EnumerateEndpointsV10 = 22,
    EnumerateBackendHealthV10 = 23,
    ConfigureBackendPlugins = 24,
    OpenCanChannelAdvanced = 25,
    EnumerateEndpointsV11 = 26,
    ShutdownForTest = 0x7fffu,
};

#pragma pack(push, 1)
struct MessageHeader {
    std::uint32_t magicValue;
    std::uint16_t major;
    std::uint16_t minor;
    std::uint16_t operation;
    std::uint16_t reserved;
    std::uint64_t requestId;
    std::uint32_t payloadBytes;
    std::int32_t status;
};

struct WireDeviceV1 {
    std::uint8_t id[16];
    std::uint32_t channelCount;
    std::uint32_t capabilities;
    std::uint32_t state;
    char name[64];
    char model[32];
    char serial[32];
};

struct WireEndpointV2 {
    std::uint8_t id[16];
    std::uint8_t adapterId[16];
    std::uint32_t busType;
    std::uint32_t physicalIndex;
    std::uint32_t capabilities;
    std::uint32_t state;
    char name[64];
    char model[32];
    char serial[32];
};

struct WireEndpointV6 {
    WireEndpointV2 endpoint;
    char backendFamily[32];
};

struct WireCanParameterAccessV9 {
    std::uint32_t readKnown, readSupported, writeKnown, writeSupported;
};
struct WireEndpointV9 {
    WireEndpointV6 endpoint;
    WireCanParameterAccessV9 classicCan, canFd;
    std::uint32_t bindingIsDefault;
};

struct WireEndpointV10 {
    WireEndpointV9 base;
    char backendInstanceId[128];
};

struct WireBackendHealthV6 {
    char backendFamily[32];
    std::int32_t status;
    std::uint32_t endpointCount;
};

struct WireBackendHealthV8 {
    WireBackendHealthV6 health;
    char runtimePath[1024];
    char fileVersion[64];
    char apiVersion[64];
    char sha256[65];
};

struct WireBackendHealthV10 {
    WireBackendHealthV8 base;
    char backendInstanceId[128];
};

struct OpenDeviceRequestV1 {
    std::uint8_t id[16];
};

struct OpenDeviceResponseV1 {
    std::uint64_t leaseToken;
};

struct CloseDeviceRequestV1 {
    std::uint64_t leaseToken;
};

struct OpenCanChannelRequestV1 {
    std::uint8_t id[16];
    std::uint32_t arbitrationBitrate;
    std::uint32_t dataBitrate;
    std::uint8_t arbitrationSamplePoint;
    std::uint8_t dataSamplePoint;
    std::uint8_t canFd;
    std::uint8_t listenOnly;
    std::uint8_t termination;
    std::uint8_t reserved[3];
};

struct WireCanAcceptanceFilterV4 {
    std::uint32_t idCode;
    std::uint32_t idMask;
    std::uint8_t frameType;
    std::uint8_t rejectAll;
    std::uint8_t reserved[2];
};

struct OpenCanChannelRequestV4 {
    std::uint8_t id[16];
    std::uint32_t arbitrationBitrate;
    std::uint32_t dataBitrate;
    std::uint8_t arbitrationSamplePoint;
    std::uint8_t dataSamplePoint;
    std::uint8_t samplePointFlags;
    std::uint8_t canFd;
    std::uint8_t listenOnly;
    std::uint8_t noAck;
    std::uint8_t termination;
    std::uint8_t acceptanceFilterCount;
    WireCanAcceptanceFilterV4 acceptanceFilters[2];
};

struct OpenCanChannelRequestV5 {
    std::uint8_t id[16];
    std::uint32_t arbitrationBitrate;
    std::uint32_t dataBitrate;
    std::uint8_t arbitrationSamplePoint;
    std::uint8_t dataSamplePoint;
    std::uint8_t samplePointFlags;
    std::uint8_t canFd;
    std::uint8_t listenOnly;
    std::uint8_t noAck;
    std::uint8_t termination;
    std::uint8_t acceptanceFilterCount;
    WireCanAcceptanceFilterV4 acceptanceFilters[2];
    std::uint8_t configurationFlags;
    std::uint8_t reserved[3];
};

struct WireCanBitTimingV11 {
    std::uint32_t clockHz, prescaler, tseg1, tseg2, sjw, sampleCount;
};
struct OpenCanChannelRequestV11 {
    OpenCanChannelRequestV5 base;
    std::uint32_t timingFlags;
    WireCanBitTimingV11 nominal, data;
    std::uint32_t reserved[2];
};

inline constexpr std::uint8_t canConfigTerminationSpecified = 1u << 0;
inline constexpr std::uint8_t CanConfigInternalLoopback = 1u << 1;

struct WriteCanRequestV1 {
    std::uint64_t leaseToken;
    std::uint32_t id;
    std::uint32_t flags;
    std::uint8_t dlc;
    std::uint8_t dataLength;
    std::uint8_t reserved[2];
    std::uint8_t data[64];
    std::uint32_t transactionId;
};

struct ReadCanRequestV1 {
    std::uint64_t leaseToken;
    std::uint32_t capacity;
};

struct WireCanFrameV1 {
    std::uint32_t id;
    std::uint32_t flags;
    std::uint8_t dlc;
    std::uint8_t dataLength;
    std::uint8_t reserved[2];
    std::uint8_t data[64];
    std::uint64_t timestampUs;
    std::uint32_t transactionId;
};

struct SubmitCanBatchRequestV7 {
    std::uint64_t leaseToken;
    std::uint32_t count;
    std::uint32_t reserved;
};

struct SubmitCanBatchResponseV7 {
    std::uint32_t acceptedCount;
    std::int32_t itemStatus;
};

struct ChannelStatusRequestV1 {
    std::uint64_t leaseToken;
};

struct ChannelStatusResponseV1 {
    std::uint8_t busOff;
    std::uint8_t errorPassive;
    std::uint8_t warning;
    std::uint8_t txErrorCounter;
    std::uint8_t rxErrorCounter;
    std::uint8_t reserved[3];
};

inline constexpr std::uint8_t channelStatusBusStateValid = 1u << 0;
inline constexpr std::uint8_t channelStatusErrorCountersValid = 1u << 1;

struct WireCanEventV2 {
    std::uint32_t kind;
    std::int32_t result;
    std::uint32_t backendStatus;
    std::uint32_t reserved;
    WireCanFrameV1 frame;
    ChannelStatusResponseV1 channelStatus;
};

struct OpenLinChannelRequestV3 {
    std::uint8_t id[16];
    std::uint32_t bitrate;
    std::uint8_t role;
    std::uint8_t protocolVersion;
    std::uint8_t checksum;
    std::uint8_t pullup;
    std::uint8_t power;
    std::uint8_t reserved[3];
};

struct WriteLinRequestV3 {
    std::uint64_t leaseToken;
    std::uint32_t flags;
    std::uint32_t transactionId;
    std::uint8_t id;
    std::uint8_t dlc;
    std::uint8_t checksum;
    std::uint8_t reserved;
    std::uint8_t data[8];
};

struct LinStatusResponseV3 {
    std::uint8_t active;
    std::uint8_t asleep;
    std::uint8_t errorFlags;
    std::uint8_t reserved[5];
};

struct WireLinEventV3 {
    std::uint32_t kind;
    std::int32_t result;
    std::uint32_t backendStatus;
    std::uint32_t flags;
    std::uint64_t timestampUs;
    std::uint32_t transactionId;
    std::uint8_t id;
    std::uint8_t dlc;
    std::uint8_t checksum;
    std::uint8_t errorFlags;
    std::uint8_t data[8];
    LinStatusResponseV3 channelStatus;
};

struct ConfigureLinSlaveResponseRequestV3 {
    std::uint64_t leaseToken;
    std::uint8_t id;
    std::uint8_t dlc;
    std::uint8_t enabled;
    std::uint8_t checksum;
    std::uint8_t checksumValue;
    std::uint8_t reserved[3];
    std::uint8_t data[8];
};
#pragma pack(pop)

static_assert(sizeof(MessageHeader) == 28, "Wire header ABI changed");
static_assert(sizeof(WireDeviceV1) == 156, "Wire device ABI changed");
static_assert(sizeof(WireEndpointV2) == 176, "Wire endpoint ABI changed");
static_assert(sizeof(WireEndpointV6) == 208, "Wire endpoint v6 ABI changed");
static_assert(sizeof(WireEndpointV10) == 372, "Wire endpoint v10 ABI changed");
static_assert(sizeof(WireBackendHealthV10) == 1385, "Wire backend health v10 ABI changed");
static_assert(sizeof(WireEndpointV9) == 244, "Wire endpoint v9 ABI changed");
static_assert(sizeof(WireBackendHealthV6) == 40, "Wire backend health ABI changed");
static_assert(sizeof(WireBackendHealthV8) == 1257, "Wire runtime health ABI changed");
static_assert(sizeof(WireCanFrameV1) == 88, "Wire CAN frame ABI changed");
static_assert(sizeof(SubmitCanBatchRequestV7) == 16, "Wire CAN batch request ABI changed");
static_assert(sizeof(SubmitCanBatchResponseV7) == 8, "Wire CAN batch response ABI changed");
static_assert(sizeof(WireCanEventV2) == 112, "Wire CAN event ABI changed");
static_assert(sizeof(OpenCanChannelRequestV5) == 60, "Wire CAN open v5 ABI changed");
static_assert(sizeof(OpenCanChannelRequestV11) == 120, "Wire advanced CAN open ABI changed");
static_assert(sizeof(OpenLinChannelRequestV3) == 28, "Wire LIN open ABI changed");
static_assert(sizeof(WriteLinRequestV3) == 28, "Wire LIN write ABI changed");
static_assert(sizeof(WireLinEventV3) == 48, "Wire LIN event ABI changed");
static_assert(sizeof(ConfigureLinSlaveResponseRequestV3) == 24,
              "Wire LIN slave response ABI changed");

std::wstring defaultPipeName();
std::wstring currentUserSidString();
MessageHeader makeRequest(Operation operation, std::uint64_t requestId, std::uint32_t payloadBytes);
bool validRequestHeader(const MessageHeader& header);
bool validResponseHeader(const MessageHeader& header);
bool validStatusValue(std::int32_t value);
std::vector<std::byte> encodeDevices(const std::vector<DeviceDescriptor>& devices);
Status decodeDevices(const std::vector<std::byte>& payload, std::vector<DeviceDescriptor>& devices);
std::vector<std::byte> encodeEndpoints(const std::vector<EndpointDescriptor>& endpoints);
Status decodeEndpoints(const std::vector<std::byte>& payload,
                        std::vector<EndpointDescriptor>& endpoints);
std::vector<std::byte> encodeEndpointsV10(const std::vector<EndpointDescriptor>& endpoints);
Status decodeEndpointsV10(const std::vector<std::byte>& payload, std::vector<EndpointDescriptor>& endpoints);
std::vector<std::byte> encodeBackendHealthV10(const std::vector<BackendHealth>& health);
Status decodeBackendHealthV10(const std::vector<std::byte>& payload, std::vector<BackendHealth>& health);
std::vector<std::byte> encodeEndpointsV9(const std::vector<EndpointDescriptor>& endpoints);
Status decodeEndpointsV9(const std::vector<std::byte>& payload,
                           std::vector<EndpointDescriptor>& endpoints);
std::vector<std::byte> encodeEndpointsV6(
    const std::vector<EndpointDescriptor>& endpoints);
Status decodeEndpointsV6(const std::vector<std::byte>& payload,
                           std::vector<EndpointDescriptor>& endpoints);
std::vector<std::byte> encodeBackendHealth(const std::vector<BackendHealth>& health);
std::vector<std::byte> encodeBackendHealthV8(const std::vector<BackendHealth>& health);
Status decodeBackendHealthV8(const std::vector<std::byte>& payload,
                               std::vector<BackendHealth>& health);
Status decodeBackendHealth(const std::vector<std::byte>& payload,
                              std::vector<BackendHealth>& health);
Status decodeCanFrames(const std::vector<std::byte>& payload,
                         std::vector<CanFrame>& frames);
Status decodeCanEvents(const std::vector<std::byte>& payload,
                         std::vector<CanEvent>& events);
Status decodeLinEvents(const std::vector<std::byte>& payload,
                         std::vector<LinEvent>& events);
Status decodeChannelStatus(const ChannelStatusResponseV1& wire,
                             ChannelStatus& status);
Status decodeLinStatus(const LinStatusResponseV3& wire, LinStatus& status);

}  // namespace Ucal::protocol
