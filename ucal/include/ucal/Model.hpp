#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace Ucal {

enum class Status : std::int32_t {
    Ok = 0,
    InvalidArgument = 1,
    IncompatibleVersion = 2,
    Unsupported = 3,
    NotFound = 4,
    Busy = 5,
    AccessDenied = 6,
    TransportError = 7,
    InternalError = 8,
};

enum Capability : std::uint32_t {
    CapabilityClassicCan = 1u << 0,
    CapabilityCanFdIso = 1u << 1,
    CapabilityBrs = 1u << 2,
    CapabilityLin = 1u << 3,
    CapabilityLinMaster = 1u << 4,
    CapabilityLinSlave = 1u << 5,
    CapabilityListenOnly = 1u << 6,
    CapabilityTermination = 1u << 7,
    CapabilityNoAck = 1u << 8,
    CapabilityTxCompletion = 1u << 9,
    CapabilityInternalLoopback = 1u << 10,
    CapabilityCanAdvancedTiming = 1u << 11,
    CapabilityDiscoveryOnly = 1u << 31,
};

enum FrameFlag : std::uint32_t {
    FrameExtended = 1u << 0,
    FrameRemote = 1u << 1,
    FrameFd = 1u << 2,
    FrameBrs = 1u << 3,
    FrameEsi = 1u << 4,
    FrameTxCompleted = 1u << 5,
    FrameTxFailed = 1u << 6,
    FrameSelfReceived = 1u << 7,
};

enum class BusType : std::uint32_t {
    Can = 1,
    Lin = 2,
};

enum class DeviceState : std::uint32_t {
    Available = 0,
    Leased = 1,
    Offline = 2,
};

using DeviceId = std::array<std::uint8_t, 16>;
using AdapterId = DeviceId;
using EndpointId = DeviceId;

// Frozen namespace bytes for version-1 endpoint identity hashing. These bytes are
// protocol data, not the current product name. Changing them invalidates bindings.
inline constexpr std::array<char, 4> endpointIdentityNamespaceV1{{0x75, 0x63, 0x61, 0x69}};


struct DeviceDescriptor {
    DeviceId id{};
    std::string name;
    std::string model;
    std::string serial;
    std::uint32_t channelCount{};
    std::uint32_t capabilities{};
    DeviceState state{DeviceState::Available};
};

// A physical adapter may expose multiple independently leased bus endpoints.
// DeviceDescriptor remains the v1 compatibility projection used by existing
// CAN-only clients; new frontends should consume EndpointDescriptor.
// Reading and setting are independent. Zero masks mean unknown, including
// catalogs received from older Brokers. These are neutral flags, not IOCTL IDs.
enum CanConfigurationParameter : std::uint32_t {
    CanConfigNominalBitrate = 1u << 0,
    CanConfigDataBitrate = 1u << 1,
    CanConfigNominalSample = 1u << 2,
    CanConfigDataSample = 1u << 3,
    CanConfigTermination = 1u << 4,
    CanConfigListenOnly = 1u << 5,
    CanConfigNoAck = 1u << 6,
    CanConfigInternalLoopback = 1u << 7,
    CanConfigReceiveFilter = 1u << 8,
};
inline constexpr std::uint32_t canConfigParameterMask = (1u << 9) - 1u;
struct CanParameterAccess {
    std::uint32_t readKnown{}, readSupported{}, writeKnown{}, writeSupported{};
};
struct CanConfigurationCapabilities {
    CanParameterAccess classicCan{}, canFd{};
};

struct EndpointDescriptor {
    EndpointId id{};
    AdapterId adapterId{};
    std::string name;
    std::string model;
    std::string serial;
    // Stable UCAL routing key supplied by the backend. It is never inferred
    // from display strings.
    std::string backendFamily;
    BusType busType{BusType::Can};
    std::uint32_t physicalIndex{};
    std::uint32_t capabilities{};
    DeviceState state{DeviceState::Available};
    CanConfigurationCapabilities configuration{};
    bool bindingIsDefault{};
    std::string backendInstanceId;
};

// CAN FD is the baseline CAN device contract. Optional features (including
// BRS and programmable termination) remain independent capability bits.
inline bool endpointMeetsDeviceContract(const EndpointDescriptor& endpoint) {
    return endpoint.busType == BusType::Lin ||
           (endpoint.busType == BusType::Can &&
            (endpoint.capabilities & CapabilityCanFdIso) != 0u &&
            (endpoint.capabilities & CapabilityDiscoveryOnly) == 0u);
}

struct BackendRuntimeIdentity {
    std::string path;
    std::string fileVersion;
    std::string apiVersion;
    std::string sha256;
};

struct BackendHealth {
    std::string backendFamily;
    Status status{Status::InternalError};
    std::uint32_t endpointCount{};
    BackendRuntimeIdentity runtime;
    std::string backendInstanceId;
};

struct OpenResult {
    Status status{Status::InternalError};
    std::uint64_t leaseToken{};
};

enum class CanFilterFrameType : std::uint8_t {
    Standard = 0,
    Extended = 1,
};

struct CanAcceptanceFilter {
    CanFilterFrameType frameType{CanFilterFrameType::Standard};
    std::uint32_t idCode{};
    std::uint32_t idMask{};
    bool rejectAll{};
};

// Reference clock/prescaler describe the requested time quantum. Both may be
// zero when an API supplies bitrate and quanta directly. Backends may use an
// equivalent clock only when sample position and SJW duration remain exact.
struct CanBitTiming {
    std::uint32_t clockHz{}, prescaler{}, tseg1{}, tseg2{}, sjw{}, sampleCount{1};
};

struct CanChannelConfig {
    std::uint32_t arbitrationBitrate{500000};
    std::uint32_t dataBitrate{2000000};
    std::uint8_t arbitrationSamplePoint{75};
    std::uint8_t dataSamplePoint{75};
    bool arbitrationSamplePointSpecified{false};
    bool dataSamplePointSpecified{false};
    bool nominalTimingSpecified{false};
    bool dataTimingSpecified{false};
    CanBitTiming nominalTiming{}, dataTiming{};
    bool canFd{false};
    bool listenOnly{false};
    bool noAck{false};
    bool internalLoopback{false};
    bool termination{true};
    bool terminationSpecified{false};
    std::array<CanAcceptanceFilter, 2> acceptanceFilters{};
    std::uint8_t acceptanceFilterCount{};
};

struct CanFrame {
    std::uint32_t id{};
    std::uint32_t flags{};
    std::uint8_t dlc{};
    std::uint8_t dataLength{};
    std::array<std::uint8_t, 64> data{};
    std::uint64_t timestampUs{};
    std::uint32_t transactionId{};
};

struct ChannelStatus {
    bool busStateValid{};
    bool errorCountersValid{};
    bool busOff{};
    bool errorPassive{};
    bool warning{};
    std::uint8_t txErrorCounter{};
    std::uint8_t rxErrorCounter{};
};

enum class LinRole : std::uint8_t {
    Master = 1,
    Slave = 2,
    ListenOnly = 3,
};

enum class LinChecksum : std::uint8_t {
    Automatic = 0,
    Classic = 1,
    Enhanced = 2,
    Manual = 3,
};

enum LinFrameFlag : std::uint32_t {
    LinTransmit = 1u << 0,
    LinWakeup = 1u << 1,
    LinSleep = 1u << 2,
    LinEnhancedChecksum = 1u << 3,
    LinUserChecksum = 1u << 4,
    LinSyncBreak = 1u << 5,
    LinTxCompleted = 1u << 6,
    LinTxFailed = 1u << 7,
};

struct LinChannelConfig {
    std::uint32_t bitrate{19200};
    LinRole role{LinRole::ListenOnly};
    std::uint8_t protocolVersion{0x22};
    LinChecksum checksum{LinChecksum::Automatic};
    bool pullup{};
    bool power{};
};

struct LinFrame {
    std::uint8_t id{};
    std::uint8_t dlc{};
    std::uint8_t checksum{};
    std::uint8_t errorFlags{};
    std::uint32_t flags{};
    std::array<std::uint8_t, 8> data{};
    std::uint64_t timestampUs{};
    std::uint32_t transactionId{};
};

struct LinSlaveResponse {
    std::uint8_t id{};
    std::uint8_t dlc{};
    bool enabled{true};
    LinChecksum checksum{LinChecksum::Automatic};
    std::uint8_t checksumValue{};
    std::array<std::uint8_t, 8> data{};
};

struct LinStatus {
    bool active{};
    bool asleep{};
    std::uint8_t errorFlags{};
};

enum class LinEventKind : std::uint32_t {
    Received = 0,
    TransmitCompleted = 1,
    TransmitFailed = 2,
    ControllerState = 3,
    QueueOverrun = 4,
};

struct LinEvent {
    LinEventKind kind{LinEventKind::Received};
    Status result{Status::Ok};
    std::uint32_t backendStatus{};
    LinFrame frame{};
    LinStatus channelStatus{};
};

enum class CanEventKind : std::uint32_t {
    Received = 0,
    TransmitCompleted = 1,
    TransmitFailed = 2,
    ControllerState = 3,
    QueueOverrun = 4,
};

struct CanEvent {
    CanEventKind kind{CanEventKind::Received};
    Status result{Status::Ok};
    std::uint32_t backendStatus{};
    CanFrame frame{};
    ChannelStatus channelStatus{};
};

const std::vector<DeviceDescriptor>& mockCatalog();
std::string deviceIdToString(const DeviceId& id);

}  // namespace ucal
