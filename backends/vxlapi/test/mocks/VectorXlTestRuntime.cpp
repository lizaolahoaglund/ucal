#include <windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>

namespace {

using XlStatus = std::int16_t;
using XlAccess = std::uint64_t;
using XlPortHandle = std::int32_t;

constexpr XlStatus success = 0;
constexpr XlStatus queueEmpty = 10;
constexpr XlStatus wrongParameter = 101;
constexpr XlStatus invalidAccess = 112;
constexpr XlStatus portOffline = 113;
constexpr XlStatus invalidPort = 118;
constexpr std::uint32_t busTypeCan = 1u;
constexpr std::uint32_t busCompatibleCan = 1u;
constexpr std::uint32_t busActiveCan = 1u << 16;
constexpr std::uint32_t channelCanfdIso = 0x80000000u;
constexpr std::uint8_t receiveMessage = 1u;
constexpr std::uint8_t chipState = 4u;
constexpr std::uint8_t transmitMessage = 10u;
constexpr std::uint16_t canRxOk = 0x0400u;
constexpr std::uint16_t canTxError = 0x0402u;
constexpr std::uint16_t canTxOk = 0x0404u;
constexpr std::uint16_t canChipState = 0x0409u;
constexpr std::uint16_t canTxMessage = 0x0440u;
constexpr std::uint16_t transactionIdMask = 0x0fffu;
constexpr std::uint16_t classicTxCompleted = 0x0040u;

#pragma pack(push, 1)
union XlBusData {
    std::uint8_t raw[28];
};
struct XlBusParams { std::uint32_t busType; XlBusData data; };
struct XlChannelConfig {
    char name[32];
    std::uint8_t hardwareType;
    std::uint8_t hardwareIndex;
    std::uint8_t hardwareChannel;
    std::uint16_t transceiverType;
    std::uint16_t transceiverState;
    std::uint16_t configError;
    std::uint8_t channelIndex;
    XlAccess channelMask;
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
    union { std::uint8_t raw[96]; XlCanRxMessage message; } data;
};
#pragma pack(pop)

static_assert(sizeof(XlDriverConfig) == 14576, "XL driver ABI changed");
static_assert(sizeof(XlEvent) == 48, "XL classic event ABI changed");
static_assert(sizeof(XlCanTxEvent) == 88, "XL CAN FD Tx ABI changed");
static_assert(sizeof(XlCanRxEvent) == 128, "XL CAN FD Rx ABI changed");
static_assert(offsetof(XlCanRxEvent, timestamp) == 24, "XL CAN FD timestamp offset changed");

struct PortState {
    XlAccess access{};
    std::uint8_t channelIndex{};
    bool fd{};
    bool active{};
    bool txReceipts{};
    std::int8_t outputMode{1};
    std::deque<XlEvent> classicEvents;
    std::deque<XlCanRxEvent> fdEvents;
};

std::mutex stateMutex;
bool driverOpen{};
XlPortHandle nextPort{100};
std::map<XlPortHandle, PortState> ports;
std::uint64_t timestampNs{1000000u};
std::uint32_t identityEnumeration{};

PortState* findPort(const XlPortHandle port) {
    const auto found = ports.find(port);
    return found == ports.end() ? nullptr : &found->second;
}

std::uint8_t channelForMask(const XlAccess mask) {
    return mask == 1u ? 0u : 1u;
}

}  // namespace

extern "C" __declspec(dllexport) XlStatus __stdcall xlOpenDriver() {
    std::lock_guard<std::mutex> lock(stateMutex);
    driverOpen = true;
    return success;
}

extern "C" __declspec(dllexport) XlStatus __stdcall xlCloseDriver() {
    std::lock_guard<std::mutex> lock(stateMutex);
    ports.clear();
    driverOpen = false;
    return success;
}

extern "C" __declspec(dllexport) XlStatus __stdcall xlGetDriverConfig(XlDriverConfig* config) {
    if (config == nullptr) return wrongParameter;
    std::lock_guard<std::mutex> lock(stateMutex);
    if (!driverOpen) return portOffline;
    *config = {};
    config->dllVersion = 0x0907001au;
    config->channelCount = 2u;
    char identitySequence[2]{};
    const bool identityTest = GetEnvironmentVariableA(
        "UCAL_VECTOR_TEST_IDENTITY_SEQUENCE", identitySequence,
        static_cast<DWORD>(sizeof(identitySequence))) == 1u && identitySequence[0] == '1';
    const auto generation = identityTest ? identityEnumeration++ : 0u;
    for (std::uint8_t index = 0; index < 2u; ++index) {
        auto& channel = config->channel[index];
        const char* name = index == 0u ? "Vector Virtual Test 1" : "Vector Virtual Test 2";
        std::memcpy(channel.name, name, std::strlen(name));
        channel.hardwareType = 1u;
        channel.hardwareIndex = 0u;
        channel.hardwareChannel = index;
        channel.channelIndex = index;
        channel.channelMask = XlAccess{1} << index;
        channel.channelCapabilities = channelCanfdIso;
        channel.channelBusCapabilities = busCompatibleCan | busActiveCan;
        channel.connectedBusType = busTypeCan;
        channel.busParams.busType = busTypeCan;
        channel.driverVersion = 0x0907001au;
        channel.interfaceVersion = 4u;
        channel.serialNumber = 4242u;
        channel.maximalBaudrate = 8000000u;
        channel.channelBusActiveCapabilities = static_cast<std::uint16_t>(busTypeCan);
        if (identityTest && generation >= 1u && generation <= 5u) {
            channel.hardwareType = 55u;
            channel.serialNumber = generation == 3u ? 4243u :
                                    generation >= 4u ? 0u : 4242u;
            if (generation == 2u) {
                // Same physical ports after another adapter changed the XL
                // enumeration indices and the user renamed the channels.
                channel.hardwareIndex = 7u;
                channel.channelIndex = static_cast<std::uint8_t>(index + 5u);
                channel.channelMask = XlAccess{1} << channel.channelIndex;
                std::memset(channel.name, 0, sizeof(channel.name));
                const char* renamed = index == 0u ? "Renamed CAN A" : "Renamed CAN B";
                std::memcpy(channel.name, renamed, std::strlen(renamed));
            } else if (generation == 5u) {
                channel.hardwareIndex = 1u;
            }
        } else if (identityTest && generation == 6u) {
            // Attaching a two-port physical adapter moves Virtual CAN's XL
            // global indices. A user rename must not change its port identity.
            channel.channelIndex = static_cast<std::uint8_t>(index + 2u);
            channel.channelMask = XlAccess{1} << channel.channelIndex;
            std::memset(channel.name, 0, sizeof(channel.name));
            const char* renamed = index == 0u ? "Renamed Virtual A" : "Renamed Virtual B";
            std::memcpy(channel.name, renamed, std::strlen(renamed));
        }
    }
    return success;
}

extern "C" __declspec(dllexport) XlStatus __stdcall xlOpenPort(
    XlPortHandle* port, char*, const XlAccess access, XlAccess* permission,
    std::uint32_t, const std::uint32_t interfaceVersion, const std::uint32_t busType) {
    if (port == nullptr || permission == nullptr || (access != 1u && access != 2u) ||
        busType != busTypeCan || (interfaceVersion != 3u && interfaceVersion != 4u)) {
        return wrongParameter;
    }
    std::lock_guard<std::mutex> lock(stateMutex);
    if (!driverOpen) return portOffline;
    if (std::any_of(ports.begin(), ports.end(),
                    [&](const auto& entry) { return entry.second.access == access; })) {
        return invalidAccess;
    }
    *port = nextPort++;
    *permission = access;
    PortState state{};
    state.access = access;
    state.channelIndex = channelForMask(access);
    state.fd = interfaceVersion == 4u;
    ports.emplace(*port, std::move(state));
    return success;
}

extern "C" __declspec(dllexport) XlStatus __stdcall xlClosePort(const XlPortHandle port) {
    std::lock_guard<std::mutex> lock(stateMutex);
    return ports.erase(port) == 1u ? success : invalidPort;
}

extern "C" __declspec(dllexport) XlStatus __stdcall xlActivateChannel(
    const XlPortHandle port, const XlAccess access, const std::uint32_t busType,
    std::uint32_t) {
    std::lock_guard<std::mutex> lock(stateMutex);
    auto* state = findPort(port);
    if (state == nullptr) return invalidPort;
    if (state->access != access || busType != busTypeCan) return wrongParameter;
    state->active = true;
    return success;
}

extern "C" __declspec(dllexport) XlStatus __stdcall xlDeactivateChannel(
    const XlPortHandle port, const XlAccess access) {
    std::lock_guard<std::mutex> lock(stateMutex);
    auto* state = findPort(port);
    if (state == nullptr) return invalidPort;
    if (state->access != access) return wrongParameter;
    state->active = false;
    return success;
}

extern "C" __declspec(dllexport) XlStatus __stdcall xlCanSetChannelBitrate(
    const XlPortHandle port, const XlAccess access, const std::uint32_t bitrate) {
    std::lock_guard<std::mutex> lock(stateMutex);
    auto* state = findPort(port);
    return state == nullptr ? invalidPort :
           state->access != access || bitrate == 0u ? wrongParameter : success;
}

extern "C" __declspec(dllexport) XlStatus __stdcall xlCanSetChannelParams(
    const XlPortHandle port, const XlAccess access, const XlChipParams* params) {
    std::lock_guard<std::mutex> lock(stateMutex);
    auto* state = findPort(port);
    return state == nullptr ? invalidPort :
           state->access != access || params == nullptr || params->bitrate == 0u ?
               wrongParameter : success;
}

extern "C" __declspec(dllexport) XlStatus __stdcall xlCanFdSetConfiguration(
    const XlPortHandle port, const XlAccess access, const XlCanFdConfig* config) {
    std::lock_guard<std::mutex> lock(stateMutex);
    auto* state = findPort(port);
    char expected[32]{};
    GetEnvironmentVariableA("UCAL_TEST_EXPECT_TIMING", expected, 32);
    if ((std::string(expected) == "precise" || std::string(expected) == "nominal-only") &&
        (config == nullptr || config->arbitrationTseg1 != 139u || config->arbitrationTseg2 != 20u ||
         config->arbitrationSjw != 8u || (std::string(expected) == "precise" &&
         (config->dataTseg1 != 29u || config->dataTseg2 != 10u || config->dataSjw != 4u)) ||
         (std::string(expected) == "nominal-only" && config->dataBitrate != 500000u)))
        return wrongParameter;
    return state == nullptr ? invalidPort :
           state->access != access || config == nullptr ||
           config->arbitrationBitrate == 0u || config->dataBitrate == 0u ?
               wrongParameter : success;
}

extern "C" __declspec(dllexport) XlStatus __stdcall xlCanSetChannelOutput(
    const XlPortHandle port, const XlAccess access, const std::int8_t mode) {
    std::lock_guard<std::mutex> lock(stateMutex);
    auto* state = findPort(port);
    if (state == nullptr) return invalidPort;
    if (state->access != access || (mode != 0 && mode != 1)) return wrongParameter;
    state->outputMode = mode;
    return success;
}

extern "C" __declspec(dllexport) XlStatus __stdcall xlCanSetChannelMode(
    const XlPortHandle port, const XlAccess access, const int txReceipts, int) {
    std::lock_guard<std::mutex> lock(stateMutex);
    auto* state = findPort(port);
    if (state == nullptr) return invalidPort;
    if (state->access != access) return wrongParameter;
    state->txReceipts = txReceipts != 0;
    return success;
}

extern "C" __declspec(dllexport) XlStatus __stdcall xlCanResetAcceptance(
    const XlPortHandle port, const XlAccess access, const std::uint32_t filterType) {
    std::lock_guard<std::mutex> lock(stateMutex);
    auto* state = findPort(port);
    return state == nullptr ? invalidPort :
           state->access != access || (filterType != 1u && filterType != 2u) ?
               wrongParameter : success;
}

extern "C" __declspec(dllexport) XlStatus __stdcall xlCanTransmit(
    const XlPortHandle port, const XlAccess access, unsigned int* count, XlEvent* events) {
    if (count == nullptr || events == nullptr || *count == 0u) return wrongParameter;
    std::lock_guard<std::mutex> lock(stateMutex);
    auto* state = findPort(port);
    if (state == nullptr) return invalidPort;
    if (!state->active || state->access != access) return portOffline;
    if (events[0].tag != transmitMessage || events[0].message.dlc > 8u) return wrongParameter;
    if ((events[0].message.id & 0x1fffffffu) == 0x6aeu) Sleep(100u);
    XlEvent received = events[0];
    received.tag = receiveMessage;
    received.channelIndex = state->channelIndex;
    received.timestamp = timestampNs += 1000000u;
    received.message.flags &= ~classicTxCompleted;
    state->classicEvents.push_back(received);
    if (state->txReceipts) {
        auto completed = received;
        // VN1610 Classic CAN receipts retain only the low eight transId bits.
        completed.transactionId &= 0x00ffu;
        completed.message.flags |= classicTxCompleted;
        completed.timestamp = timestampNs += 1000000u;
        state->classicEvents.push_back(completed);
    }
    *count = 1u;
    return success;
}

extern "C" __declspec(dllexport) XlStatus __stdcall xlCanTransmitEx(
    const XlPortHandle port, const XlAccess access, const unsigned int count,
    unsigned int* sent, XlCanTxEvent* events) {
    if (sent == nullptr || events == nullptr || count == 0u) return wrongParameter;
    std::lock_guard<std::mutex> lock(stateMutex);
    auto* state = findPort(port);
    if (state == nullptr) return invalidPort;
    if (!state->active || state->access != access) return portOffline;
    if (events[0].tag != canTxMessage) return wrongParameter;
    // Deterministic slow-vendor path used only by the Provider queue-pressure
    // regression. Production backends and ordinary mock traffic are unchanged.
    if ((events[0].message.canId & 0x1fffffffu) == 0x6aeu) Sleep(100u);
    XlCanRxEvent received{};
    received.size = sizeof(received);
    received.tag = canRxOk;
    received.channelIndex = state->channelIndex;
    // The installed XL runtime returns only the low twelve transId bits in
    // userHandle. Model that constraint so full-width UCAL transaction IDs
    // cannot pass tests through a more permissive fake.
    received.userHandle = events[0].transactionId & transactionIdMask;
    received.timestamp = timestampNs += 1000000u;
    received.data.message.canId = events[0].message.canId;
    received.data.message.messageFlags = events[0].message.messageFlags;
    received.data.message.dlc = events[0].message.dlc;
    std::copy_n(events[0].message.data, 64u, received.data.message.data);
    if ((events[0].message.canId & 0x1fffffffu) == 0x6afu) {
        received.tag = canTxError;
        received.data.raw[0] = 7u;
        state->fdEvents.push_back(received);
        *sent = 1u;
        return success;
    }
    state->fdEvents.push_back(received);
    if (state->txReceipts) {
        auto completed = received;
        completed.tag = canTxOk;
        completed.timestamp = timestampNs += 1000000u;
        state->fdEvents.push_back(completed);
    }
    *sent = 1u;
    return success;
}

extern "C" __declspec(dllexport) XlStatus __stdcall xlReceive(
    const XlPortHandle port, unsigned int* count, XlEvent* event) {
    if (count == nullptr || event == nullptr || *count == 0u) return wrongParameter;
    std::lock_guard<std::mutex> lock(stateMutex);
    auto* state = findPort(port);
    if (state == nullptr) return invalidPort;
    if (state->classicEvents.empty()) { *count = 0u; return queueEmpty; }
    *event = state->classicEvents.front();
    state->classicEvents.pop_front();
    *count = 1u;
    return success;
}

extern "C" __declspec(dllexport) XlStatus __stdcall xlCanReceive(
    const XlPortHandle port, XlCanRxEvent* event) {
    if (event == nullptr) return wrongParameter;
    std::lock_guard<std::mutex> lock(stateMutex);
    auto* state = findPort(port);
    if (state == nullptr) return invalidPort;
    if (state->fdEvents.empty()) return queueEmpty;
    *event = state->fdEvents.front();
    state->fdEvents.pop_front();
    return success;
}

extern "C" __declspec(dllexport) XlStatus __stdcall xlCanRequestChipState(
    const XlPortHandle port, const XlAccess access) {
    std::lock_guard<std::mutex> lock(stateMutex);
    auto* state = findPort(port);
    if (state == nullptr) return invalidPort;
    if (state->access != access) return wrongParameter;
    if (state->fd) {
        XlCanRxEvent event{};
        event.size = sizeof(event);
        event.tag = canChipState;
        event.channelIndex = state->channelIndex;
        event.timestamp = timestampNs += 1000000u;
        event.data.raw[0] = 0u;
        event.data.raw[1] = 3u;
        event.data.raw[2] = 4u;
        state->fdEvents.push_back(event);
    } else {
        XlEvent event{};
        event.tag = chipState;
        event.channelIndex = state->channelIndex;
        event.timestamp = timestampNs += 1000000u;
        auto* bytes = reinterpret_cast<std::uint8_t*>(&event.message);
        bytes[0] = 0u;
        bytes[1] = 3u;
        bytes[2] = 4u;
        state->classicEvents.push_back(event);
    }
    return success;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
