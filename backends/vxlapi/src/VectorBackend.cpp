#include "NativeDiscoveryAbi.hpp"
#include "VectorBackend.hpp"
#include "ucal/CanTiming.hpp"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Ucal::VectorBackend {
namespace {

constexpr std::size_t maxBufferedEvents = 8192u;

using XlStatus = std::int16_t;
using XlAccess = std::uint64_t;
using XlPortHandle = std::int32_t;

constexpr XlStatus xlSuccess = 0;
constexpr XlStatus xlQueueEmpty = 10;
constexpr XlStatus xlQueueFull = 11;
constexpr XlStatus xlTxNotPossible = 12;
constexpr XlStatus xlNoLicense = 14;
constexpr XlStatus xlWrongParameter = 101;
constexpr XlStatus xlInvalidChannel = 111;
constexpr XlStatus xlInvalidAccess = 112;
constexpr XlStatus xlPortOffline = 113;
constexpr XlStatus xlChannelOnline = 116;
constexpr XlStatus xlNotImplemented = 117;
constexpr XlStatus xlInvalidPort = 118;
constexpr XlStatus xlHardwareNotReady = 120;
constexpr XlStatus xlHardwareNotPresent = 129;
constexpr XlStatus xlNotFound = 139;
constexpr XlStatus xlRequestNotAccepted = 141;
constexpr XlStatus xlInternalError = 144;
constexpr XlStatus xlNoResources = 152;
constexpr XlStatus xlInitAccessMissing = 158;
constexpr XlStatus xlWrongVersion = 160;
constexpr XlStatus xlCannotOpenDriver = 201;
constexpr XlStatus xlWrongBusType = 202;
constexpr XlStatus xlDllNotFound = 203;
constexpr XlStatus xlInvalidChannelMask = 204;
constexpr XlStatus xlNotSupported = 205;
constexpr XlStatus xlConnectionBroken = 210;
constexpr XlStatus xlConnectionClosed = 211;
constexpr XlStatus xlConnectionFailed = 213;
constexpr XlStatus xlQueueOverrun = 216;
constexpr XlStatus xlInvalidDlc = 513;
constexpr XlStatus xlInvalidCanId = 514;
constexpr XlStatus xlInvalidFdFlag = 515;
constexpr XlStatus xlFdRtr = 516;
constexpr XlStatus xlFdNotSet = 517;
constexpr XlStatus xlUnknownFlag = 518;

constexpr std::uint32_t xlBusTypeCan = 1u;
constexpr std::uint8_t xlHardwareVirtual = 1u;
constexpr std::uint32_t xlBusCompatibleCan = 1u;
constexpr std::uint32_t xlBusActiveCan = 1u << 16;
constexpr std::uint32_t xlChannelCanfdIso = 0x80000000u;
constexpr std::uint32_t xlInterfaceVersionClassic = 3u;
constexpr std::uint32_t xlInterfaceVersionFd = 4u;
constexpr std::int8_t xlOutputSilent = 0;
constexpr std::int8_t xlOutputNormal = 1;
constexpr std::uint32_t xlAcceptStandard = 1u;
constexpr std::uint32_t xlAcceptExtended = 2u;
constexpr std::uint8_t xlReceiveMessage = 1u;
constexpr std::uint8_t xlChipState = 4u;
constexpr std::uint8_t xlTransmitMessage = 10u;
constexpr std::uint16_t xlCanRxOk = 0x0400u;
constexpr std::uint16_t xlCanRxError = 0x0401u;
constexpr std::uint16_t xlCanTxError = 0x0402u;
constexpr std::uint16_t xlCanTxOk = 0x0404u;
constexpr std::uint16_t xlCanChipState = 0x0409u;
constexpr std::uint16_t xlCanTxMessage = 0x0440u;
constexpr std::uint16_t xlClassicTransactionIdMask = 0x00ffu;
constexpr std::uint16_t xlFdTransactionIdMask = 0x0fffu;
constexpr std::uint32_t xlExtendedId = 0x80000000u;
constexpr std::uint16_t xlClassicRemote = 0x0010u;
constexpr std::uint16_t xlClassicTxCompleted = 0x0040u;
constexpr std::uint32_t xlFdEdl = 0x0001u;
constexpr std::uint32_t xlFdBrs = 0x0002u;
constexpr std::uint32_t xlFdEsi = 0x0004u;
constexpr std::uint32_t xlFdRtrFlag = 0x0010u;
constexpr std::uint8_t xlBusOff = 0x01u;
constexpr std::uint8_t xlErrorPassive = 0x02u;
constexpr std::uint8_t xlErrorWarning = 0x04u;

#pragma pack(push, 1)
using NativeDiscoveryAbi::XlBusData;
using NativeDiscoveryAbi::XlBusParams;
using NativeDiscoveryAbi::XlChannelConfig;
using NativeDiscoveryAbi::XlDriverConfig;

struct XlCanFdConfig {
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

struct XlChipParams {
    std::uint32_t bitrate;
    std::uint8_t sjw;
    std::uint8_t tseg1;
    std::uint8_t tseg2;
    std::uint8_t samplesPerBit;
};

struct XlCanMessage {
    std::uint32_t id;
    std::uint16_t flags;
    std::uint16_t dlc;
    std::uint64_t reserved1;
    std::uint8_t data[8];
    std::uint64_t reserved2;
};

struct XlEvent {
    std::uint8_t tag;
    std::uint8_t channelIndex;
    std::uint16_t transactionId;
    std::uint16_t portHandle;
    std::uint8_t flags;
    std::uint8_t reserved;
    std::uint64_t timestamp;
    XlCanMessage message;
};

struct XlCanTxMessage {
    std::uint32_t canId;
    std::uint32_t messageFlags;
    std::uint8_t dlc;
    std::uint8_t reserved[7];
    std::uint8_t data[64];
};

struct XlCanTxEvent {
    std::uint16_t tag;
    std::uint16_t transactionId;
    std::uint8_t channelIndex;
    std::uint8_t reserved[3];
    XlCanTxMessage message;
};

struct XlCanRxMessage {
    std::uint32_t canId;
    std::uint32_t messageFlags;
    std::uint32_t crc;
    std::uint8_t reserved1[12];
    std::uint16_t totalBitCount;
    std::uint8_t dlc;
    std::uint8_t reserved[5];
    std::uint8_t data[64];
};

struct XlCanRxEvent {
    std::uint32_t size;
    std::uint16_t tag;
    std::uint16_t channelIndex;
    std::uint32_t userHandle;
    std::uint16_t flagsChip;
    std::uint16_t reserved;
    std::uint64_t reserved1;
    std::uint64_t timestamp;
    union {
        std::uint8_t raw[96];
        XlCanRxMessage message;
    } data;
};
#pragma pack(pop)

static_assert(sizeof(XlChannelConfig) == 227, "XL channel ABI changed");
static_assert(sizeof(XlDriverConfig) == 14576, "XL driver ABI changed");
static_assert(sizeof(XlEvent) == 48, "XL classic event ABI changed");
static_assert(sizeof(XlCanTxEvent) == 88, "XL CAN FD Tx ABI changed");
static_assert(sizeof(XlCanRxEvent) == 128, "XL CAN FD Rx ABI changed");
static_assert(offsetof(XlCanRxEvent, timestamp) == 24, "XL CAN FD timestamp offset changed");
static_assert(sizeof(XlChipParams) == 8, "XL timing ABI changed");

using OpenDriver = XlStatus(__stdcall*)();
using CloseDriver = XlStatus(__stdcall*)();
using GetDriverConfig = XlStatus(__stdcall*)(XlDriverConfig*);
using OpenPort = XlStatus(__stdcall*)(XlPortHandle*, char*, XlAccess, XlAccess*,
                                      std::uint32_t, std::uint32_t, std::uint32_t);
using ClosePort = XlStatus(__stdcall*)(XlPortHandle);
using ActivateChannel = XlStatus(__stdcall*)(XlPortHandle, XlAccess, std::uint32_t,
                                              std::uint32_t);
using DeactivateChannel = XlStatus(__stdcall*)(XlPortHandle, XlAccess);
using SetChannelBitrate = XlStatus(__stdcall*)(XlPortHandle, XlAccess, std::uint32_t);
using SetChannelParams = XlStatus(__stdcall*)(XlPortHandle, XlAccess, const XlChipParams*);
using SetFdConfiguration = XlStatus(__stdcall*)(XlPortHandle, XlAccess,
                                                const XlCanFdConfig*);
using SetChannelOutput = XlStatus(__stdcall*)(XlPortHandle, XlAccess, std::int8_t);
using SetChannelMode = XlStatus(__stdcall*)(XlPortHandle, XlAccess, int, int);
using ResetAcceptance = XlStatus(__stdcall*)(XlPortHandle, XlAccess, std::uint32_t);
using Transmit = XlStatus(__stdcall*)(XlPortHandle, XlAccess, unsigned int*, XlEvent*);
using TransmitEx = XlStatus(__stdcall*)(XlPortHandle, XlAccess, unsigned int,
                                        unsigned int*, XlCanTxEvent*);
using Receive = XlStatus(__stdcall*)(XlPortHandle, unsigned int*, XlEvent*);
using ReceiveFd = XlStatus(__stdcall*)(XlPortHandle, XlCanRxEvent*);
using RequestChipState = XlStatus(__stdcall*)(XlPortHandle, XlAccess);

template <typename Function>
Function function(const HMODULE module, const char* name) {
    return module == nullptr ? nullptr :
        reinterpret_cast<Function>(GetProcAddress(module, name));
}

Status mapStatus(const XlStatus status) {
    if (status == xlSuccess) return Status::Ok;
    if (status == xlQueueFull || status == xlTxNotPossible ||
        status == xlInvalidAccess || status == xlChannelOnline ||
        status == xlRequestNotAccepted || status == xlInitAccessMissing) {
        return Status::Busy;
    }
    if (status == xlWrongParameter || status == xlInvalidChannel ||
        status == xlInvalidChannelMask || status == xlInvalidDlc ||
        status == xlInvalidCanId || status == xlInvalidFdFlag ||
        status == xlFdRtr || status == xlFdNotSet || status == xlUnknownFlag) {
        return Status::InvalidArgument;
    }
    if (status == xlNotImplemented || status == xlNotSupported ||
        status == xlWrongBusType) return Status::Unsupported;
    if (status == xlInvalidPort || status == xlHardwareNotPresent ||
        status == xlNotFound) return Status::NotFound;
    if (status == xlNoLicense) return Status::AccessDenied;
    if (status == xlHardwareNotReady || status == xlPortOffline ||
        status == xlCannotOpenDriver || status == xlDllNotFound ||
        status == xlConnectionBroken || status == xlConnectionClosed ||
        status == xlConnectionFailed || status == xlQueueOverrun) {
        return Status::TransportError;
    }
    if (status == xlWrongVersion) return Status::IncompatibleVersion;
    if (status == xlInternalError || status == xlNoResources) return Status::InternalError;
    return Status::InternalError;
}

bool validConfig(const CanChannelConfig& config) {
    if (!validCanAdvancedTiming(config)) return false;
    if (config.listenOnly && config.noAck) return false;
    if (config.arbitrationBitrate == 0u) return false;
    if (config.canFd && config.dataBitrate == 0u) return false;
    if (config.arbitrationSamplePointSpecified &&
        (config.arbitrationSamplePoint == 0u || config.arbitrationSamplePoint > 100u)) {
        return false;
    }
    if (config.dataSamplePointSpecified &&
        (config.dataSamplePoint == 0u || config.dataSamplePoint > 100u)) return false;
    if (config.acceptanceFilterCount > config.acceptanceFilters.size()) return false;
    for (std::uint8_t index = 0; index < config.acceptanceFilterCount; ++index) {
        const auto& filter = config.acceptanceFilters[index];
        const auto limit = filter.frameType == CanFilterFrameType::Extended ?
                               0x1fffffffu : 0x7ffu;
        if ((filter.idCode & ~limit) != 0u || (filter.idMask & ~limit) != 0u) return false;
    }
    return true;
}

bool acceptsFrame(const CanChannelConfig& config, const CanFrame& frame) {
    if (config.acceptanceFilterCount == 0u) return true;
    const auto type = (frame.flags & FrameExtended) != 0u ?
                          CanFilterFrameType::Extended : CanFilterFrameType::Standard;
    bool configured = false;
    for (std::uint8_t index = 0; index < config.acceptanceFilterCount; ++index) {
        const auto& filter = config.acceptanceFilters[index];
        if (filter.frameType != type) continue;
        configured = true;
        if (!filter.rejectAll &&
            (frame.id & filter.idMask) == (filter.idCode & filter.idMask)) return true;
    }
    return !configured;
}

std::uint8_t dlcToLength(const std::uint8_t dlc) {
    constexpr std::array<std::uint8_t, 16> lengths{
        0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64};
    return lengths[dlc & 0x0fu];
}

bool validFrame(const CanFrame& frame, const bool sessionFd) {
    if ((frame.id & ~((frame.flags & FrameExtended) != 0u ? 0x1fffffffu : 0x7ffu)) != 0u) {
        return false;
    }
    if (frame.dlc > 15u || frame.dataLength > frame.data.size()) return false;
    const auto isFd = (frame.flags & FrameFd) != 0u;
    if (isFd && !sessionFd) return false;
    if ((frame.flags & FrameBrs) != 0u && !isFd) return false;
    if ((frame.flags & FrameEsi) != 0u) return false;
    if (isFd && (frame.flags & FrameRemote) != 0u) return false;
    if (isFd) return frame.dataLength == dlcToLength(frame.dlc);
    if (frame.dlc > 8u) return false;
    return (frame.flags & FrameRemote) != 0u ? frame.dataLength == 0u :
                                               frame.dataLength == frame.dlc;
}

bool timingSolution(const std::uint32_t clock, const std::uint32_t bitrate,
                     const std::uint8_t targetSamplePoint, const bool exactSamplePoint,
                     const std::uint32_t maximumBrp, const std::uint32_t maximumTseg1,
                     const std::uint32_t maximumTseg2, std::uint32_t& brp,
                     std::uint32_t& tseg1, std::uint32_t& tseg2) {
    auto bestError = (std::numeric_limits<std::uint32_t>::max)();
    bool found = false;
    for (std::uint32_t candidateBrp = 1u; candidateBrp <= maximumBrp; ++candidateBrp) {
        const auto denominator = static_cast<std::uint64_t>(bitrate) * candidateBrp;
        if (denominator == 0u || clock % denominator != 0u) continue;
        const auto quanta = static_cast<std::uint32_t>(clock / denominator);
        if (quanta < 3u || quanta > 1u + maximumTseg1 + maximumTseg2) continue;
        for (std::uint32_t candidateTseg2 = 1u;
             candidateTseg2 <= maximumTseg2 && candidateTseg2 + 2u <= quanta;
             ++candidateTseg2) {
            const auto candidateTseg1 = quanta - 1u - candidateTseg2;
            if (candidateTseg1 == 0u || candidateTseg1 > maximumTseg1) continue;
            const auto sample = static_cast<std::uint32_t>(
                (100u * (1u + candidateTseg1) + quanta / 2u) / quanta);
            const auto error = sample > targetSamplePoint ?
                                   sample - targetSamplePoint : targetSamplePoint - sample;
            if (!found || error < bestError ||
                (error == bestError && candidateBrp < brp)) {
                found = true;
                bestError = error;
                brp = candidateBrp;
                tseg1 = candidateTseg1;
                tseg2 = candidateTseg2;
            }
        }
    }
    return found && (!exactSamplePoint || bestError == 0u);
}

bool classicTiming(const CanChannelConfig& config, XlChipParams& output) {
    if (config.nominalTimingSpecified) {
        const auto& t = config.nominalTiming;
        if (t.tseg1 > 16u || t.tseg2 > 8u || t.sjw > 4u) return false;
        output.bitrate = config.arbitrationBitrate;
        output.sjw = static_cast<std::uint8_t>(t.sjw);
        output.tseg1 = static_cast<std::uint8_t>(t.tseg1);
        output.tseg2 = static_cast<std::uint8_t>(t.tseg2);
        output.samplesPerBit = static_cast<std::uint8_t>(t.sampleCount);
        return true;
    }
    std::uint32_t brp = 0;
    std::uint32_t tseg1 = 0;
    std::uint32_t tseg2 = 0;
    const auto sample = static_cast<std::uint8_t>(
        config.arbitrationSamplePointSpecified ? config.arbitrationSamplePoint : 75u);
    if (!timingSolution(16000000u, config.arbitrationBitrate, sample,
                         config.arbitrationSamplePointSpecified,
                         64u, 16u, 8u, brp, tseg1, tseg2)) return false;
    output.bitrate = config.arbitrationBitrate;
    output.sjw = 1u;
    output.tseg1 = static_cast<std::uint8_t>(tseg1);
    output.tseg2 = static_cast<std::uint8_t>(tseg2);
    output.samplesPerBit = 1u;
    return true;
}

bool fdTiming(const CanChannelConfig& config, XlCanFdConfig& output) {
    std::uint32_t nominalBrp = 0;
    std::uint32_t nominalTseg1 = 0;
    std::uint32_t nominalTseg2 = 0;
    std::uint32_t dataBrp = 0;
    std::uint32_t dataTseg1 = 0;
    std::uint32_t dataTseg2 = 0;
    const auto nominalSample = static_cast<std::uint8_t>(
        config.arbitrationSamplePointSpecified ? config.arbitrationSamplePoint : 75u);
    const auto dataSample = static_cast<std::uint8_t>(
        config.dataSamplePointSpecified ? config.dataSamplePoint : 75u);
    if ((!config.nominalTimingSpecified && !timingSolution(80000000u, config.arbitrationBitrate, nominalSample,
                         config.arbitrationSamplePointSpecified,
                         1024u, 256u, 128u, nominalBrp, nominalTseg1, nominalTseg2)) ||
        (!config.dataTimingSpecified && !timingSolution(80000000u, config.dataBitrate, dataSample,
                         config.dataSamplePointSpecified,
                         1024u, 32u, 16u, dataBrp, dataTseg1, dataTseg2))) return false;
    output.arbitrationBitrate = config.arbitrationBitrate;
    output.arbitrationSjw = 1u;
    output.arbitrationTseg1 = nominalTseg1;
    output.arbitrationTseg2 = nominalTseg2;
    output.dataBitrate = config.dataBitrate;
    output.dataSjw = 1u;
    output.dataTseg1 = dataTseg1;
    output.dataTseg2 = dataTseg2;
    if (config.nominalTimingSpecified) {
        const auto& t = config.nominalTiming;
        if (t.tseg1 > 256u || t.tseg2 > 128u || t.sjw > 128u) return false;
        output.arbitrationTseg1 = t.tseg1;
        output.arbitrationTseg2 = t.tseg2;
        output.arbitrationSjw = t.sjw;
    }
    if (config.dataTimingSpecified) {
        const auto& t = config.dataTiming;
        if (t.tseg1 > 32u || t.tseg2 > 16u || t.sjw > 16u) return false;
        output.dataTseg1 = t.tseg1;
        output.dataTseg2 = t.tseg2;
        output.dataSjw = t.sjw;
    }
    return true;
}

std::string bounded(const char* text, const std::size_t capacity) {
    const auto end = static_cast<const char*>(std::memchr(text, 0, capacity));
    return std::string(text, end == nullptr ? capacity :
                                      static_cast<std::size_t>(end - text));
}

DeviceId hashId(const std::string& value, const std::uint8_t tag) {
    std::uint64_t first = 1469598103934665603ull;
    std::uint64_t second = 1099511628211ull ^ static_cast<std::uint64_t>(tag);
    for (const auto byte : value) {
        first ^= static_cast<std::uint8_t>(byte);
        first *= 1099511628211ull;
        second ^= static_cast<std::uint8_t>(byte);
        second *= 1469598103934665603ull;
    }
    DeviceId id{};
    for (std::size_t index = 0; index < 8u; ++index) {
        id[index] = static_cast<std::uint8_t>(first >> (index * 8u));
        id[index + 8u] = static_cast<std::uint8_t>(second >> (index * 8u));
    }
    id[15] ^= tag;
    return id;
}

std::uint64_t randomToken() {
    std::uint64_t token = 0;
    while (token == 0u) {
        if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&token), sizeof(token),
                            BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) return 0;
    }
    return token;
}

bool processAlive(const std::uint32_t pid) {
    const auto process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (process == nullptr) return GetLastError() == ERROR_ACCESS_DENIED;
    const auto alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    return alive;
}

}  // namespace

struct Backend::Implementation {
    struct ChannelRecord {
        EndpointDescriptor descriptor{};
        XlAccess access{};
        std::uint8_t channelIndex{};
        bool fdCapable{};
    };

    struct Session {
        std::uint64_t token{};
        std::uint32_t ownerPid{};
        DeviceId id{};
        XlPortHandle port{-1};
        XlAccess access{};
        bool fd{};
        CanChannelConfig config{};
        std::unordered_map<std::uint16_t, std::uint32_t> pendingTransactions;
        std::deque<CanEvent> bufferedEvents;
        std::uint32_t overrunCount{};
        ChannelStatus lastStatus{};
    };

    HMODULE module{};
    OpenDriver openDriver{};
    CloseDriver closeDriver{};
    GetDriverConfig getDriverConfig{};
    OpenPort openPort{};
    ClosePort closePort{};
    ActivateChannel activateChannel{};
    DeactivateChannel deactivateChannel{};
    SetChannelBitrate setChannelBitrate{};
    SetChannelParams setChannelParams{};
    SetFdConfiguration setFdConfiguration{};
    SetChannelOutput setChannelOutput{};
    SetChannelMode setChannelMode{};
    ResetAcceptance resetAcceptance{};
    Transmit transmit{};
    TransmitEx transmitEx{};
    Receive receive{};
    ReceiveFd receiveFd{};
    RequestChipState requestChipState{};
    bool loaded{};
    bool driverOpen{};
    std::vector<ChannelRecord> channels;
    std::vector<std::shared_ptr<Session>> sessions;
    std::mutex mutex;

    std::shared_ptr<Session> findSession(const std::uint64_t token,
                                          const std::uint32_t ownerPid) {
        const auto found = std::find_if(sessions.begin(), sessions.end(),
            [&](const auto& session) {
                return session->token == token && session->ownerPid == ownerPid;
            });
        return found == sessions.end() ? nullptr : *found;
    }

    std::uint32_t completeTransaction(Session& session, const std::uint16_t shortId) {
        const auto found = session.pendingTransactions.find(shortId);
        if (found == session.pendingTransactions.end()) return shortId;
        const auto full = found->second;
        session.pendingTransactions.erase(found);
        return full;
    }

    bool decodeClassic(Session& session, const XlEvent& raw, CanEvent& event) {
        if (raw.tag == xlChipState) {
            const auto* state = reinterpret_cast<const std::uint8_t*>(&raw.message);
            event.kind = CanEventKind::ControllerState;
            event.channelStatus.busStateValid = true;
            event.channelStatus.errorCountersValid = true;
            event.channelStatus.busOff = (state[0] & xlBusOff) != 0u;
            event.channelStatus.errorPassive = (state[0] & xlErrorPassive) != 0u;
            event.channelStatus.warning = (state[0] & xlErrorWarning) != 0u;
            event.channelStatus.txErrorCounter = state[1];
            event.channelStatus.rxErrorCounter = state[2];
            session.lastStatus = event.channelStatus;
            return true;
        }
        if (raw.tag != xlReceiveMessage) return false;
        event.kind = (raw.message.flags & xlClassicTxCompleted) != 0u ?
                         CanEventKind::TransmitCompleted : CanEventKind::Received;
        event.frame.id = raw.message.id & 0x1fffffffu;
        if ((raw.message.id & xlExtendedId) != 0u) event.frame.flags |= FrameExtended;
        if ((raw.message.flags & xlClassicRemote) != 0u) event.frame.flags |= FrameRemote;
        if (event.kind == CanEventKind::TransmitCompleted) event.frame.flags |= FrameTxCompleted;
        event.frame.dlc = static_cast<std::uint8_t>(raw.message.dlc);
        event.frame.dataLength = (event.frame.flags & FrameRemote) != 0u ? 0u : event.frame.dlc;
        std::copy_n(raw.message.data, event.frame.dataLength, event.frame.data.begin());
        event.frame.timestampUs = raw.timestamp / 1000u;
        event.frame.transactionId = event.kind == CanEventKind::TransmitCompleted ?
            completeTransaction(session, raw.transactionId) : raw.transactionId;
        return event.kind != CanEventKind::Received || acceptsFrame(session.config, event.frame);
    }

    bool decodeFd(Session& session, const XlCanRxEvent& raw, CanEvent& event) {
        if (raw.tag == xlCanChipState) {
            event.kind = CanEventKind::ControllerState;
            event.channelStatus.busStateValid = true;
            event.channelStatus.errorCountersValid = true;
            event.channelStatus.busOff = (raw.data.raw[0] & xlBusOff) != 0u;
            event.channelStatus.errorPassive = (raw.data.raw[0] & xlErrorPassive) != 0u;
            event.channelStatus.warning = (raw.data.raw[0] & xlErrorWarning) != 0u;
            event.channelStatus.txErrorCounter = raw.data.raw[1];
            event.channelStatus.rxErrorCounter = raw.data.raw[2];
            session.lastStatus = event.channelStatus;
            return true;
        }
        if (raw.tag == xlCanTxError) {
            event.kind = CanEventKind::TransmitFailed;
            event.result = Status::TransportError;
            event.backendStatus = raw.data.raw[0];
            event.frame.flags = FrameTxFailed;
            event.frame.transactionId = completeTransaction(
                session, static_cast<std::uint16_t>(raw.userHandle & 0xffffu));
            event.frame.timestampUs = raw.timestamp / 1000u;
            return true;
        }
        if (raw.tag != xlCanRxOk && raw.tag != xlCanTxOk) {
            if (raw.tag == xlCanRxError) {
                event.kind = CanEventKind::ControllerState;
                event.result = Status::TransportError;
                event.backendStatus = raw.data.raw[0];
                return true;
            }
            return false;
        }
        event.kind = raw.tag == xlCanTxOk ?
                         CanEventKind::TransmitCompleted : CanEventKind::Received;
        const auto& message = raw.data.message;
        event.frame.id = message.canId & 0x1fffffffu;
        if ((message.canId & xlExtendedId) != 0u) event.frame.flags |= FrameExtended;
        if ((message.messageFlags & xlFdRtrFlag) != 0u) event.frame.flags |= FrameRemote;
        if ((message.messageFlags & xlFdEdl) != 0u) event.frame.flags |= FrameFd;
        if ((message.messageFlags & xlFdBrs) != 0u) event.frame.flags |= FrameBrs;
        if ((message.messageFlags & xlFdEsi) != 0u) event.frame.flags |= FrameEsi;
        if (event.kind == CanEventKind::TransmitCompleted) event.frame.flags |= FrameTxCompleted;
        event.frame.dlc = message.dlc;
        event.frame.dataLength = (event.frame.flags & FrameRemote) != 0u ?
                                      0u : dlcToLength(message.dlc);
        std::copy_n(message.data, event.frame.dataLength, event.frame.data.begin());
        event.frame.timestampUs = raw.timestamp / 1000u;
        const auto shortId = static_cast<std::uint16_t>(raw.userHandle & 0xffffu);
        event.frame.transactionId = event.kind == CanEventKind::TransmitCompleted ?
                                         completeTransaction(session, shortId) : shortId;
        return event.kind != CanEventKind::Received || acceptsFrame(session.config, event.frame);
    }

    Status receiveOne(Session& session, CanEvent& event, bool& present) {
        present = false;
        for (std::uint32_t attempt = 0; attempt < 64u; ++attempt) {
            if (session.fd) {
                XlCanRxEvent raw{};
                raw.size = sizeof(raw);
                const auto status = receiveFd(session.port, &raw);
                if (status == xlQueueEmpty) return Status::Ok;
                if (status != xlSuccess) return mapStatus(status);
                present = decodeFd(session, raw, event);
            } else {
                XlEvent raw{};
                unsigned int count = 1u;
                const auto status = receive(session.port, &count, &raw);
                if (status == xlQueueEmpty || count == 0u) return Status::Ok;
                if (status != xlSuccess) return mapStatus(status);
                present = decodeClassic(session, raw, event);
            }
            if (present) return Status::Ok;
        }
        return Status::Ok;
    }
};

Backend::Backend() : implementation_(std::make_unique<Implementation>()) {}

Backend::~Backend() {
    std::lock_guard<std::mutex> lock(implementation_->mutex);
    for (const auto& session : implementation_->sessions) {
        if (implementation_->deactivateChannel != nullptr) {
            implementation_->deactivateChannel(session->port, session->access);
        }
        if (implementation_->closePort != nullptr) implementation_->closePort(session->port);
    }
    implementation_->sessions.clear();
    if (implementation_->driverOpen && implementation_->closeDriver != nullptr) {
        implementation_->closeDriver();
    }
    if (implementation_->module != nullptr) FreeLibrary(implementation_->module);
}

Status Backend::initialize() {
    std::lock_guard<std::mutex> lock(implementation_->mutex);
    if (implementation_->loaded) return Status::Ok;
    std::array<wchar_t, 1024> configured{};
    const auto length = GetEnvironmentVariableW(L"UCAL_VECTOR_XL_RUNTIME", configured.data(),
                                                 static_cast<DWORD>(configured.size()));
    if (length != 0u && length < configured.size()) {
        implementation_->module = LoadLibraryExW(configured.data(), nullptr,
                                                  LOAD_WITH_ALTERED_SEARCH_PATH);
    } else if (length == 0u) {
#if defined(_WIN64)
        implementation_->module = LoadLibraryExW(L"vxlapi64.dll", nullptr,
                                                  LOAD_LIBRARY_SEARCH_SYSTEM32);
#else
        implementation_->module = LoadLibraryExW(L"vxlapi.dll", nullptr,
                                                  LOAD_LIBRARY_SEARCH_SYSTEM32);
#endif
    }
    if (implementation_->module == nullptr) return Status::NotFound;
#define UCAL_XL_LOAD(member, type, name) \
    implementation_->member = function<type>(implementation_->module, name)
    UCAL_XL_LOAD(openDriver, OpenDriver, "xlOpenDriver");
    UCAL_XL_LOAD(closeDriver, CloseDriver, "xlCloseDriver");
    UCAL_XL_LOAD(getDriverConfig, GetDriverConfig, "xlGetDriverConfig");
    UCAL_XL_LOAD(openPort, OpenPort, "xlOpenPort");
    UCAL_XL_LOAD(closePort, ClosePort, "xlClosePort");
    UCAL_XL_LOAD(activateChannel, ActivateChannel, "xlActivateChannel");
    UCAL_XL_LOAD(deactivateChannel, DeactivateChannel, "xlDeactivateChannel");
    UCAL_XL_LOAD(setChannelBitrate, SetChannelBitrate, "xlCanSetChannelBitrate");
    UCAL_XL_LOAD(setChannelParams, SetChannelParams, "xlCanSetChannelParams");
    UCAL_XL_LOAD(setFdConfiguration, SetFdConfiguration, "xlCanFdSetConfiguration");
    UCAL_XL_LOAD(setChannelOutput, SetChannelOutput, "xlCanSetChannelOutput");
    UCAL_XL_LOAD(setChannelMode, SetChannelMode, "xlCanSetChannelMode");
    UCAL_XL_LOAD(resetAcceptance, ResetAcceptance, "xlCanResetAcceptance");
    UCAL_XL_LOAD(transmit, Transmit, "xlCanTransmit");
    UCAL_XL_LOAD(transmitEx, TransmitEx, "xlCanTransmitEx");
    UCAL_XL_LOAD(receive, Receive, "xlReceive");
    UCAL_XL_LOAD(receiveFd, ReceiveFd, "xlCanReceive");
    UCAL_XL_LOAD(requestChipState, RequestChipState, "xlCanRequestChipState");
#undef UCAL_XL_LOAD
    if (implementation_->openDriver == nullptr || implementation_->closeDriver == nullptr ||
        implementation_->getDriverConfig == nullptr || implementation_->openPort == nullptr ||
        implementation_->closePort == nullptr || implementation_->activateChannel == nullptr ||
        implementation_->deactivateChannel == nullptr ||
        implementation_->setChannelBitrate == nullptr ||
        implementation_->setChannelParams == nullptr ||
        implementation_->setFdConfiguration == nullptr ||
        implementation_->setChannelOutput == nullptr ||
        implementation_->setChannelMode == nullptr ||
        implementation_->resetAcceptance == nullptr || implementation_->transmit == nullptr ||
        implementation_->transmitEx == nullptr || implementation_->receive == nullptr ||
        implementation_->receiveFd == nullptr ||
        implementation_->requestChipState == nullptr) {
        FreeLibrary(implementation_->module);
        implementation_->module = nullptr;
        return Status::IncompatibleVersion;
    }
    const auto status = implementation_->openDriver();
    if (status != xlSuccess) {
        FreeLibrary(implementation_->module);
        implementation_->module = nullptr;
        return mapStatus(status);
    }
    implementation_->driverOpen = true;
    implementation_->loaded = true;
    return Status::Ok;
}

Status Backend::enumerateEndpoints(std::vector<EndpointDescriptor>& endpoints) {
    endpoints.clear();
    const auto init = initialize();
    if (init != Status::Ok) return init;
    std::lock_guard<std::mutex> lock(implementation_->mutex);
    XlDriverConfig config{};
    const auto raw = implementation_->getDriverConfig(&config);
    if (raw != xlSuccess) return mapStatus(raw);
    implementation_->channels.clear();
    const auto count = (std::min)(config.channelCount, 64u);
    for (std::uint32_t index = 0; index < count; ++index) {
        const auto& channel = config.channel[index];
        if ((channel.channelBusCapabilities &
             (xlBusCompatibleCan | xlBusActiveCan)) == 0u ||
            channel.channelMask == 0u) continue;
        const auto name = bounded(channel.name, sizeof(channel.name));
        const auto serial = channel.serialNumber == 0u ? std::string{} :
                                                        std::to_string(channel.serialNumber);
        // XL indices/access masks and display names belong to the current
        // inventory, not to the physical identity persisted by Switch.
        // Virtual channels have a runtime-local adapter/port identity. Their
        // global XL index and user-visible name also change with enumeration.
        // Anonymous physical devices retain an enumeration-based fallback.
        const bool physicalIdentity = channel.hardwareType != xlHardwareVirtual &&
                                       channel.serialNumber != 0u;
        const bool virtualIdentity = channel.hardwareType == xlHardwareVirtual;
        const auto adapterMaterial = physicalIdentity
            ? "vector:adapter:" + std::to_string(channel.hardwareType) + ":serial:" + serial
            : "vector:adapter:" + std::to_string(channel.hardwareType) + ":" +
                  std::to_string(channel.hardwareIndex) + ":" + serial;
        const auto endpointMaterial = physicalIdentity || virtualIdentity
            ? adapterMaterial + ":can:" + std::to_string(channel.hardwareChannel)
            : adapterMaterial + ":" + std::to_string(channel.hardwareChannel) + ":" +
                  std::to_string(channel.channelIndex) + ":" + name;
        Implementation::ChannelRecord record{};
        record.access = channel.channelMask;
        record.channelIndex = channel.channelIndex;
        record.fdCapable = (channel.channelCapabilities & xlChannelCanfdIso) != 0u;
        record.descriptor.id = hashId(endpointMaterial, 0xe3u);
        record.descriptor.adapterId = hashId(adapterMaterial, 0xa3u);
        record.descriptor.name = name.empty() ? "Vector XL CAN" : name;
        record.descriptor.model = "Vector XL Driver";
        record.descriptor.serial = serial;
        record.descriptor.backendFamily = "vector-xl";
        record.descriptor.busType = BusType::Can;
        record.descriptor.physicalIndex = channel.hardwareChannel;
        record.descriptor.capabilities = CapabilityClassicCan | CapabilityListenOnly |
                                         CapabilityTxCompletion | CapabilityCanAdvancedTiming;
        if (record.fdCapable) {
            record.descriptor.capabilities |= CapabilityCanFdIso | CapabilityBrs;
        }
        const auto owned = std::any_of(implementation_->sessions.begin(),
                                       implementation_->sessions.end(),
            [&](const auto& session) { return session->id == record.descriptor.id; });
        record.descriptor.state = owned ? DeviceState::Leased : DeviceState::Available;
        endpoints.push_back(record.descriptor);
        implementation_->channels.push_back(std::move(record));
    }
    return Status::Ok;
}

Status Backend::enumerate(std::vector<DeviceDescriptor>& devices) {
    devices.clear();
    std::vector<EndpointDescriptor> endpoints;
    const auto status = enumerateEndpoints(endpoints);
    if (status != Status::Ok) return status;
    for (const auto& endpoint : endpoints) {
        auto found = std::find_if(devices.begin(), devices.end(),
            [&](const auto& device) { return device.id == endpoint.adapterId; });
        if (found == devices.end()) {
            DeviceDescriptor device{};
            device.id = endpoint.adapterId;
            device.name = endpoint.name;
            device.model = endpoint.model;
            device.serial = endpoint.serial;
            device.channelCount = 1u;
            device.capabilities = endpoint.capabilities;
            device.state = endpoint.state;
            devices.push_back(std::move(device));
        } else {
            ++found->channelCount;
            found->capabilities |= endpoint.capabilities;
            if (endpoint.state == DeviceState::Leased) found->state = DeviceState::Leased;
        }
    }
    return Status::Ok;
}

OpenResult Backend::open(const DeviceId& id, const CanChannelConfig& config,
                         const std::uint32_t ownerPid) {
    if (config.internalLoopback) return {Status::Unsupported, 0};
    if (!validConfig(config)) return {Status::InvalidArgument, 0};
    if (config.noAck || config.terminationSpecified) return {Status::Unsupported, 0};
    std::vector<EndpointDescriptor> ignored;
    const auto refresh = enumerateEndpoints(ignored);
    if (refresh != Status::Ok) return {refresh, 0};
    std::lock_guard<std::mutex> lock(implementation_->mutex);
    const auto found = std::find_if(implementation_->channels.begin(),
                                    implementation_->channels.end(),
        [&](const auto& channel) { return channel.descriptor.id == id; });
    if (found == implementation_->channels.end()) return {Status::NotFound, 0};
    if (config.canFd && !found->fdCapable) return {Status::Unsupported, 0};
    if (std::any_of(implementation_->sessions.begin(), implementation_->sessions.end(),
                    [&](const auto& session) { return session->id == id; })) {
        return {Status::Busy, 0};
    }
    XlPortHandle port = -1;
    auto permission = found->access;
    char application[] = "UCAL";
    auto raw = implementation_->openPort(
        &port, application, found->access, &permission, 8192u,
        config.canFd ? xlInterfaceVersionFd : xlInterfaceVersionClassic,
        xlBusTypeCan);
    if (raw != xlSuccess) return {mapStatus(raw), 0};
    auto cleanup = [&]() {
        implementation_->deactivateChannel(port, found->access);
        implementation_->closePort(port);
    };
    if ((permission & found->access) != found->access) {
        cleanup();
        return {Status::Busy, 0};
    }
    if (config.canFd) {
        XlCanFdConfig timing{};
        if (!fdTiming(config, timing)) {
            cleanup();
            return {config.nominalTimingSpecified || config.dataTimingSpecified ?
                Status::Unsupported : Status::InvalidArgument, 0};
        }
        raw = implementation_->setFdConfiguration(port, found->access, &timing);
    } else if (config.arbitrationSamplePointSpecified || config.nominalTimingSpecified) {
        XlChipParams timing{};
        if (!classicTiming(config, timing)) {
            cleanup();
            return {config.nominalTimingSpecified ? Status::Unsupported : Status::InvalidArgument, 0};
        }
        raw = implementation_->setChannelParams(port, found->access, &timing);
    } else {
        raw = implementation_->setChannelBitrate(port, found->access,
                                                   config.arbitrationBitrate);
    }
    if (raw != xlSuccess) {
        cleanup();
        return {mapStatus(raw), 0};
    }
    raw = implementation_->setChannelOutput(
        port, found->access, config.listenOnly ? xlOutputSilent : xlOutputNormal);
    if (raw != xlSuccess) {
        cleanup();
        return {mapStatus(raw), 0};
    }
    raw = implementation_->setChannelMode(port, found->access, 1, 0);
    if (raw != xlSuccess) {
        cleanup();
        return {mapStatus(raw), 0};
    }
    for (const auto filterType : {xlAcceptStandard, xlAcceptExtended}) {
        raw = implementation_->resetAcceptance(port, found->access, filterType);
        if (raw != xlSuccess && raw != xlNotImplemented && raw != xlNotSupported) {
            cleanup();
            return {mapStatus(raw), 0};
        }
    }
    raw = implementation_->activateChannel(port, found->access, xlBusTypeCan, 0u);
    if (raw != xlSuccess) {
        cleanup();
        return {mapStatus(raw), 0};
    }
    auto session = std::make_shared<Implementation::Session>();
    session->token = randomToken();
    if (session->token == 0u) {
        cleanup();
        return {Status::InternalError, 0};
    }
    session->ownerPid = ownerPid;
    session->id = id;
    session->port = port;
    session->access = found->access;
    session->fd = config.canFd;
    session->config = config;
    implementation_->sessions.push_back(session);
    return {Status::Ok, session->token};
}

Status Backend::close(const std::uint64_t token, const std::uint32_t ownerPid) {
    std::lock_guard<std::mutex> lock(implementation_->mutex);
    const auto found = std::find_if(implementation_->sessions.begin(),
                                    implementation_->sessions.end(),
        [&](const auto& session) {
            return session->token == token && session->ownerPid == ownerPid;
        });
    if (found == implementation_->sessions.end()) return Status::NotFound;
    const auto session = *found;
    const auto deactivate = implementation_->deactivateChannel(session->port, session->access);
    const auto closed = implementation_->closePort(session->port);
    implementation_->sessions.erase(found);
    if (deactivate != xlSuccess) return mapStatus(deactivate);
    return mapStatus(closed);
}

Status Backend::submit(const std::uint64_t token, const std::uint32_t ownerPid,
                       const CanFrame& frame) {
    std::lock_guard<std::mutex> lock(implementation_->mutex);
    const auto session = implementation_->findSession(token, ownerPid);
    if (session == nullptr) return Status::NotFound;
    if (!validFrame(frame, session->fd)) return Status::InvalidArgument;
    // Classic receipts on VN1610 preserve eight transId bits; FD receipts
    // preserve twelve. Correlate with the bits that survive the runtime, while
    // retaining the caller's full transaction ID in the pending map. Colliding
    // in-flight keys must be rejected until their real receipt arrives.
    const auto shortId = static_cast<std::uint16_t>(
        frame.transactionId & (session->fd ? xlFdTransactionIdMask :
                                             xlClassicTransactionIdMask));
    if (session->pendingTransactions.find(shortId) != session->pendingTransactions.end()) {
        return Status::Busy;
    }
    session->pendingTransactions.emplace(shortId, frame.transactionId);
    XlStatus raw = xlSuccess;
    if (session->fd) {
        XlCanTxEvent event{};
        event.tag = xlCanTxMessage;
        event.transactionId = shortId;
        event.message.canId = frame.id;
        if ((frame.flags & FrameExtended) != 0u) event.message.canId |= xlExtendedId;
        if ((frame.flags & FrameFd) != 0u) event.message.messageFlags |= xlFdEdl;
        if ((frame.flags & FrameBrs) != 0u) event.message.messageFlags |= xlFdBrs;
        if ((frame.flags & FrameRemote) != 0u) event.message.messageFlags |= xlFdRtrFlag;
        event.message.dlc = frame.dlc;
        std::copy_n(frame.data.begin(), frame.dataLength, event.message.data);
        unsigned int sent = 0u;
        raw = implementation_->transmitEx(session->port, session->access, 1u, &sent, &event);
        if (raw == xlSuccess && sent != 1u) raw = xlQueueFull;
    } else {
        XlEvent event{};
        event.tag = xlTransmitMessage;
        event.transactionId = shortId;
        event.message.id = frame.id;
        if ((frame.flags & FrameExtended) != 0u) event.message.id |= xlExtendedId;
        if ((frame.flags & FrameRemote) != 0u) event.message.flags |= xlClassicRemote;
        event.message.dlc = frame.dlc;
        std::copy_n(frame.data.begin(), frame.dataLength, event.message.data);
        unsigned int count = 1u;
        raw = implementation_->transmit(session->port, session->access, &count, &event);
        if (raw == xlSuccess && count != 1u) raw = xlQueueFull;
    }
    if (raw != xlSuccess) session->pendingTransactions.erase(shortId);
    return mapStatus(raw);
}

Status Backend::write(const std::uint64_t token, const std::uint32_t ownerPid,
                      const CanFrame& frame) {
    return submit(token, ownerPid, frame);
}

Status Backend::readEvents(const std::uint64_t token, const std::uint32_t ownerPid,
                            const std::uint32_t capacity, std::vector<CanEvent>& events) {
    events.clear();
    std::lock_guard<std::mutex> lock(implementation_->mutex);
    const auto session = implementation_->findSession(token, ownerPid);
    if (session == nullptr) return Status::NotFound;
    if (capacity != 0u && session->overrunCount != 0u) {
        CanEvent overrun{};
        overrun.kind = CanEventKind::QueueOverrun;
        overrun.backendStatus = session->overrunCount;
        events.push_back(std::move(overrun));
        session->overrunCount = 0u;
    }
    while (events.size() < capacity && !session->bufferedEvents.empty()) {
        events.push_back(std::move(session->bufferedEvents.front()));
        session->bufferedEvents.pop_front();
    }
    while (events.size() < capacity) {
        CanEvent event{};
        bool present = false;
        const auto status = implementation_->receiveOne(*session, event, present);
        if (status != Status::Ok) return status;
        if (!present) break;
        events.push_back(std::move(event));
    }
    return Status::Ok;
}

Status Backend::read(const std::uint64_t token, const std::uint32_t ownerPid,
                     const std::uint32_t capacity, std::vector<CanFrame>& frames) {
    frames.clear();
    std::vector<CanEvent> events;
    const auto status = readEvents(token, ownerPid, capacity, events);
    if (status != Status::Ok) return status;
    for (const auto& event : events) {
        if (event.kind == CanEventKind::Received) frames.push_back(event.frame);
    }
    return Status::Ok;
}

Status Backend::getStatus(const std::uint64_t token, const std::uint32_t ownerPid,
                           ChannelStatus& status) {
    std::lock_guard<std::mutex> lock(implementation_->mutex);
    const auto session = implementation_->findSession(token, ownerPid);
    if (session == nullptr) return Status::NotFound;
    const auto request = implementation_->requestChipState(session->port, session->access);
    if (request != xlSuccess && request != xlNotImplemented && request != xlNotSupported) {
        return mapStatus(request);
    }
    if (request == xlSuccess) {
        for (std::uint32_t attempt = 0; attempt < 64u; ++attempt) {
            CanEvent event{};
            bool present = false;
            const auto received = implementation_->receiveOne(*session, event, present);
            if (received != Status::Ok) return received;
            if (!present) break;
            if (event.kind == CanEventKind::ControllerState &&
                event.channelStatus.busStateValid) break;
            if (session->bufferedEvents.size() >= maxBufferedEvents) {
                session->bufferedEvents.pop_front();
                ++session->overrunCount;
            }
            session->bufferedEvents.push_back(std::move(event));
        }
    }
    status = session->lastStatus;
    return Status::Ok;
}

void Backend::pruneDeadSessions() {
    std::lock_guard<std::mutex> lock(implementation_->mutex);
    auto current = implementation_->sessions.begin();
    while (current != implementation_->sessions.end()) {
        if (processAlive((*current)->ownerPid)) {
            ++current;
            continue;
        }
        implementation_->deactivateChannel((*current)->port, (*current)->access);
        implementation_->closePort((*current)->port);
        current = implementation_->sessions.erase(current);
    }
}

}  // namespace Ucal::vector_backend
