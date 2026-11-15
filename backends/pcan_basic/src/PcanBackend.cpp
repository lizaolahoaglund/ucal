#include "NativeDiscoveryAbi.hpp"
#include "PcanBackend.hpp"
#include "ucal/CanTiming.hpp"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace Ucal::PcanBackend {
namespace {

// Minimal declarations of PEAK's public PCAN-Basic C ABI. The vendor header and
// runtime are not redistributed; the installed runtime is resolved dynamically.
using PcanHandle = WORD;
using PcanStatus = DWORD;
using PcanParameter = BYTE;
using PcanBaudrate = WORD;

constexpr PcanHandle pcanNoneBus = 0x00u;
constexpr PcanStatus pcanOk = 0x00000u;
constexpr PcanStatus pcanTransmitFull = 0x00001u;
constexpr PcanStatus pcanBusLight = 0x00004u;
constexpr PcanStatus pcanBusWarning = 0x00008u;
constexpr PcanStatus pcanBusOff = 0x00010u;
constexpr PcanStatus pcanReceiveEmpty = 0x00020u;
constexpr PcanStatus pcanTransmitQueueFull = 0x00080u;
constexpr PcanStatus pcanNoDriver = 0x00200u;
constexpr PcanStatus pcanHardwareInUse = 0x00400u;
constexpr PcanStatus pcanNetInUse = 0x00800u;
constexpr PcanStatus pcanIllegalHandle = 0x01c00u;
constexpr PcanStatus pcanResource = 0x02000u;
constexpr PcanStatus pcanInvalidParameterType = 0x04000u;
constexpr PcanStatus pcanInvalidParameterValue = 0x08000u;
constexpr PcanStatus pcanInvalidData = 0x20000u;
constexpr PcanStatus pcanBusPassive = 0x40000u;
constexpr PcanStatus pcanInvalidMode = 0x80000u;
constexpr PcanStatus pcanCaution = 0x2000000u;
constexpr PcanStatus pcanNotInitialized = 0x4000000u;
constexpr PcanStatus pcanInvalidOperation = 0x8000000u;
constexpr PcanStatus pcanAnyBusState = pcanBusLight | pcanBusWarning |
                                           pcanBusOff | pcanBusPassive;

constexpr PcanParameter pcanListenOnly = 0x08u;
constexpr PcanParameter pcanBusoffAutoreset = 0x07u;
constexpr PcanParameter pcanBusSpeedNominal = 0x1au;
constexpr PcanParameter pcanBusSpeedData = 0x1bu;
constexpr PcanParameter pcanAttachedChannelCount = 0x2au;
constexpr PcanParameter pcanAttachedChannels = 0x2bu;
constexpr PcanParameter pcanAllowEchoFrames = 0x2cu;
constexpr PcanParameter pcanDeviceGuid = 0x30u;

constexpr DWORD pcanChannelAvailable = 0x01u;
constexpr DWORD pcanChannelOccupied = 0x02u;
constexpr DWORD pcanChannelPcanView = 0x03u;
constexpr DWORD pcanFeatureFd = 0x01u;

constexpr BYTE pcanParameterOff = 0x00u;
constexpr BYTE pcanParameterOn = 0x01u;
constexpr BYTE pcanMessageRtr = 0x01u;
constexpr BYTE pcanMessageExtended = 0x02u;
constexpr BYTE pcanMessageFd = 0x04u;
constexpr BYTE pcanMessageBrs = 0x08u;
constexpr BYTE pcanMessageEsi = 0x10u;
constexpr BYTE pcanMessageEcho = 0x20u;
constexpr BYTE pcanMessageError = 0x40u;
constexpr BYTE pcanMessageStatus = 0x80u;

struct PcanMessage {
    DWORD id;
    BYTE messageType;
    BYTE length;
    BYTE data[8];
};

struct PcanTimestamp {
    DWORD milliseconds;
    WORD millisecondOverflow;
    WORD microseconds;
};

struct PcanMessageFd {
    DWORD id;
    BYTE messageType;
    BYTE dlc;
    BYTE data[64];
};

using NativeDiscoveryAbi::PcanChannelInformation;

static_assert(sizeof(PcanMessage) == 16, "PCAN classic message ABI changed");
static_assert(sizeof(PcanTimestamp) == 8, "PCAN timestamp ABI changed");
static_assert(sizeof(PcanMessageFd) == 72, "PCAN FD message ABI changed");
static_assert(sizeof(PcanChannelInformation) == 52, "PCAN channel ABI changed");

using Initialize = PcanStatus(__stdcall*)(PcanHandle, PcanBaudrate, BYTE, DWORD, WORD);
using InitializeFd = PcanStatus(__stdcall*)(PcanHandle, char*);
using Uninitialize = PcanStatus(__stdcall*)(PcanHandle);
using GetStatus = PcanStatus(__stdcall*)(PcanHandle);
using Read = PcanStatus(__stdcall*)(PcanHandle, PcanMessage*, PcanTimestamp*);
using ReadFd = PcanStatus(__stdcall*)(PcanHandle, PcanMessageFd*, std::uint64_t*);
using Write = PcanStatus(__stdcall*)(PcanHandle, PcanMessage*);
using WriteFd = PcanStatus(__stdcall*)(PcanHandle, PcanMessageFd*);
using GetValue = PcanStatus(__stdcall*)(PcanHandle, PcanParameter, void*, DWORD);
using SetValue = PcanStatus(__stdcall*)(PcanHandle, PcanParameter, void*, DWORD);

template <typename Function>
Function function(const HMODULE module, const char* name) {
    return module == nullptr ? nullptr :
        reinterpret_cast<Function>(GetProcAddress(module, name));
}

Status mapStatus(const PcanStatus status) {
    if (status == pcanOk) return Status::Ok;
    if ((status & (pcanTransmitFull | pcanTransmitQueueFull |
                   pcanHardwareInUse | pcanNetInUse | pcanCaution)) != 0u) {
        return Status::Busy;
    }
    if ((status & (pcanInvalidParameterType | pcanInvalidParameterValue |
                   pcanInvalidData | pcanInvalidMode)) != 0u) {
        return Status::InvalidArgument;
    }
    if ((status & pcanInvalidOperation) != 0u) return Status::Unsupported;
    if ((status & (pcanIllegalHandle | pcanNotInitialized)) != 0u) {
        return Status::NotFound;
    }
    if ((status & (pcanNoDriver | pcanResource)) != 0u) {
        return Status::TransportError;
    }
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
        (config.dataSamplePoint == 0u || config.dataSamplePoint > 100u)) {
        return false;
    }
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

bool echoFramesRequested() {
    std::array<char, 16> value{};
    const auto length = GetEnvironmentVariableA(
        "UCAL_PCAN_ALLOW_ECHO_FRAMES", value.data(), static_cast<DWORD>(value.size()));
    if (length == 0u || length >= value.size()) return false;
    std::string normalized(value.data(), length);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](const unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return normalized == "1" || normalized == "true" || normalized == "on" ||
           normalized == "yes";
}

std::uint8_t dlcToLength(const std::uint8_t dlc) {
    constexpr std::array<std::uint8_t, 16> lengths{
        0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64};
    return lengths[dlc & 0x0fu];
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

bool timingSolution(const std::uint32_t clock, const std::uint32_t bitrate,
                     const std::uint8_t targetSamplePoint,
                     const bool exactSamplePoint,
                     const std::uint32_t maximumBrp,
                     const std::uint32_t maximumTseg1,
                     const std::uint32_t maximumTseg2,
                     std::uint32_t& brp, std::uint32_t& tseg1,
                     std::uint32_t& tseg2) {
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

bool classicBaudrate(const CanChannelConfig& config, PcanBaudrate& output) {
    if (config.nominalTimingSpecified) {
        CanBitTiming timing{};
        if (!resolveCanBitTiming(config.nominalTiming, config.arbitrationBitrate,
                                8000000u, 64u, 16u, 8u, 4u, timing)) return false;
        const auto btr0 = (timing.prescaler - 1u) | ((timing.sjw - 1u) << 6u);
        const auto btr1 = (timing.tseg1 - 1u) | ((timing.tseg2 - 1u) << 4u) |
                          (timing.sampleCount == 3u ? 0x80u : 0u);
        output = static_cast<PcanBaudrate>((btr0 << 8u) | btr1);
        return true;
    }
    std::uint32_t brp = 0;
    std::uint32_t tseg1 = 0;
    std::uint32_t tseg2 = 0;
    const auto samplePoint = static_cast<std::uint8_t>(
        config.arbitrationSamplePointSpecified ?
            config.arbitrationSamplePoint : 75u);
    // PCAN-Basic's classic TPCANBaudrate value is the SJA1000 BTR0/BTR1
    // encoding. Its BRP divides the 16 MHz oscillator by 2, so the timing
    // solver must use the resulting 8 MHz time-quantum clock. Using 16 MHz
    // here produced a valid-looking BTR tuple at half the requested bitrate.
    if (!timingSolution(8000000u, config.arbitrationBitrate, samplePoint,
                         config.arbitrationSamplePointSpecified,
                         64u, 16u, 8u, brp, tseg1, tseg2)) return false;
    const auto btr0 = static_cast<std::uint8_t>(brp - 1u);
    const auto btr1 = static_cast<std::uint8_t>(((tseg2 - 1u) << 4u) | (tseg1 - 1u));
    output = static_cast<PcanBaudrate>((static_cast<std::uint16_t>(btr0) << 8u) | btr1);
    return true;
}

bool fdBitrate(const CanChannelConfig& config, std::string& output) {
    if (config.nominalTimingSpecified || config.dataTimingSpecified) {
        const auto clock = config.nominalTimingSpecified && config.nominalTiming.clockHz
            ? config.nominalTiming.clockHz : config.dataTimingSpecified && config.dataTiming.clockHz
            ? config.dataTiming.clockHz : 80000000u;
        if (clock != 80000000u && clock != 60000000u && clock != 40000000u &&
            clock != 30000000u && clock != 24000000u && clock != 20000000u) return false;
        auto phase = [&](bool data, CanBitTiming& t) {
            const auto specified = data ? config.dataTimingSpecified : config.nominalTimingSpecified;
            const auto rate = data ? config.dataBitrate : config.arbitrationBitrate;
            if (specified) return resolveCanBitTiming(data ? config.dataTiming : config.nominalTiming,
                rate, clock, 1024u, data ? 32u : 256u, data ? 16u : 128u, data ? 16u : 128u, t);
            const auto sample = data ? config.dataSamplePoint : config.arbitrationSamplePoint;
            const auto exact = data ? config.dataSamplePointSpecified : config.arbitrationSamplePointSpecified;
            t = {clock, 0u, 0u, 0u, 1u, 1u};
            return timingSolution(clock, rate, sample, exact, 1024u, data ? 32u : 256u,
                data ? 16u : 128u, t.prescaler, t.tseg1, t.tseg2);
        };
        CanBitTiming nominal{}, data{};
        if (!phase(false, nominal) || !phase(true, data)) return false;
        output = "f_clock=" + std::to_string(clock) + ",nom_brp=" + std::to_string(nominal.prescaler) +
            ",nom_tseg1=" + std::to_string(nominal.tseg1) + ",nom_tseg2=" + std::to_string(nominal.tseg2) +
            ",nom_sjw=" + std::to_string(nominal.sjw) + ",data_brp=" + std::to_string(data.prescaler) +
            ",data_tseg1=" + std::to_string(data.tseg1) + ",data_tseg2=" + std::to_string(data.tseg2) +
            ",data_sjw=" + std::to_string(data.sjw);
        return true;
    }
    std::uint32_t nominalBrp = 0;
    std::uint32_t nominalTseg1 = 0;
    std::uint32_t nominalTseg2 = 0;
    std::uint32_t dataBrp = 0;
    std::uint32_t dataTseg1 = 0;
    std::uint32_t dataTseg2 = 0;
    const auto nominalSample = static_cast<std::uint8_t>(
        config.arbitrationSamplePointSpecified ?
            config.arbitrationSamplePoint : 75u);
    const auto dataSample = static_cast<std::uint8_t>(
        config.dataSamplePointSpecified ? config.dataSamplePoint : 75u);
    if (!timingSolution(80000000u, config.arbitrationBitrate, nominalSample,
                         config.arbitrationSamplePointSpecified,
                         1024u, 256u, 128u,
                         nominalBrp, nominalTseg1, nominalTseg2) ||
        !timingSolution(80000000u, config.dataBitrate, dataSample,
                         config.dataSamplePointSpecified,
                         1024u, 32u, 16u, dataBrp, dataTseg1, dataTseg2)) {
        return false;
    }
    output = "f_clock=80000000,nom_brp=" + std::to_string(nominalBrp) +
             ",nom_tseg1=" + std::to_string(nominalTseg1) +
             ",nom_tseg2=" + std::to_string(nominalTseg2) +
             ",nom_sjw=1,data_brp=" + std::to_string(dataBrp) +
             ",data_tseg1=" + std::to_string(dataTseg1) +
             ",data_tseg2=" + std::to_string(dataTseg2) + ",data_sjw=1";
    return true;
}

ChannelStatus decodeStatus(const PcanStatus status) {
    ChannelStatus output{};
    output.busStateValid = true;
    output.errorCountersValid = false;
    output.busOff = (status & pcanBusOff) != 0u;
    output.errorPassive = (status & pcanBusPassive) != 0u;
    output.warning = (status & (pcanBusWarning | pcanBusLight)) != 0u;
    return output;
}

bool sameBusState(const ChannelStatus& left, const ChannelStatus& right) {
    return left.busStateValid == right.busStateValid &&
           left.busOff == right.busOff &&
           left.errorPassive == right.errorPassive &&
           left.warning == right.warning;
}

}  // namespace

struct Backend::Implementation {
    struct ChannelRecord {
        EndpointDescriptor descriptor{};
        PcanHandle handle{};
        DWORD condition{};
        bool fdCapable{};
    };

    struct Session {
        std::uint64_t token{};
        std::uint32_t ownerPid{};
        DeviceId id{};
        PcanHandle handle{};
        bool fd{};
        bool receiveEcho{};
        CanChannelConfig config{};
        bool lastStatusValid{};
        ChannelStatus lastStatus{};
    };

    HMODULE module{};
    Initialize initializeClassic{};
    InitializeFd initializeFd{};
    Uninitialize uninitialize{};
    GetStatus getStatus{};
    Read read{};
    ReadFd readFd{};
    Write write{};
    WriteFd writeFd{};
    GetValue getValue{};
    SetValue setValue{};
    bool loaded{};
    std::vector<ChannelRecord> channels;
    std::vector<std::shared_ptr<Session>> sessions;
    // Once a live catalog proves that a public DeviceGUID/controller tuple is
    // duplicated, keep using the handle-qualified degraded identity for this
    // Broker lifetime. Otherwise removing one duplicate would silently change
    // the surviving endpoint's UUID until its peer returned.
    std::vector<std::string> handleIdentityBases;
    std::mutex mutex;
    std::mutex runtimeMutex;
    std::condition_variable runtimeCondition;
    std::deque<std::function<void()>> runtimeTasks;
    bool runtimeStopping{};
    std::thread runtimeThread;

    Implementation() : runtimeThread([this] {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(runtimeMutex);
                runtimeCondition.wait(lock, [this] {
                    return runtimeStopping || !runtimeTasks.empty();
                });
                if (runtimeTasks.empty()) {
                    if (runtimeStopping) return;
                    continue;
                }
                task = std::move(runtimeTasks.front());
                runtimeTasks.pop_front();
            }
            task();
        }
    }) {}

    ~Implementation() { stopRuntimeThread(); }

    bool onRuntimeThread() const {
        return runtimeThread.joinable() &&
               std::this_thread::get_id() == runtimeThread.get_id();
    }

    template <typename Function>
    auto invoke(Function&& function) -> decltype(function()) {
        if (onRuntimeThread()) return function();
        using Result = decltype(function());
        auto task = std::make_shared<std::packaged_task<Result()>>(
            std::forward<Function>(function));
        auto future = task->get_future();
        {
            std::lock_guard<std::mutex> lock(runtimeMutex);
            runtimeTasks.emplace_back([task] { (*task)(); });
        }
        runtimeCondition.notify_one();
        return future.get();
    }

    void stopRuntimeThread() {
        {
            std::lock_guard<std::mutex> lock(runtimeMutex);
            runtimeStopping = true;
        }
        runtimeCondition.notify_one();
        if (runtimeThread.joinable()) runtimeThread.join();
    }

    std::shared_ptr<Session> findSession(const std::uint64_t token,
                                          const std::uint32_t ownerPid) {
        const auto found = std::find_if(sessions.begin(), sessions.end(),
            [&](const auto& session) {
                return session->token == token && session->ownerPid == ownerPid;
            });
        return found == sessions.end() ? nullptr : *found;
    }
};

Backend::Backend() : implementation_(std::make_unique<Implementation>()) {}

Backend::~Backend() {
    auto* const implementation = implementation_.get();
    implementation->invoke([implementation] {
        std::lock_guard<std::mutex> lock(implementation->mutex);
        if (implementation->uninitialize != nullptr) {
            for (const auto& session : implementation->sessions) {
                implementation->uninitialize(session->handle);
            }
        }
        implementation->sessions.clear();
        if (implementation->module != nullptr) FreeLibrary(implementation->module);
        implementation->module = nullptr;
    });
    implementation->stopRuntimeThread();
}

Status Backend::initialize() {
    if (!implementation_->onRuntimeThread()) {
        return implementation_->invoke([this] { return initialize(); });
    }
    std::lock_guard<std::mutex> lock(implementation_->mutex);
    if (implementation_->loaded) return Status::Ok;
    std::array<wchar_t, 1024> configured{};
    const auto length = GetEnvironmentVariableW(L"UCAL_PCAN_BASIC_RUNTIME", configured.data(),
                                                 static_cast<DWORD>(configured.size()));
    if (length != 0u && length < configured.size()) {
        implementation_->module = LoadLibraryExW(configured.data(), nullptr,
                                                  LOAD_WITH_ALTERED_SEARCH_PATH);
    } else if (length == 0u) {
        implementation_->module = LoadLibraryExW(L"PCANBasic.dll", nullptr,
                                                  LOAD_LIBRARY_SEARCH_SYSTEM32);
    }
    if (implementation_->module == nullptr) return Status::NotFound;
    implementation_->initializeClassic = function<Initialize>(implementation_->module,
                                                                "CAN_Initialize");
    implementation_->initializeFd = function<InitializeFd>(implementation_->module,
                                                              "CAN_InitializeFD");
    implementation_->uninitialize = function<Uninitialize>(implementation_->module,
                                                             "CAN_Uninitialize");
    implementation_->getStatus = function<GetStatus>(implementation_->module, "CAN_GetStatus");
    implementation_->read = function<Read>(implementation_->module, "CAN_Read");
    implementation_->readFd = function<ReadFd>(implementation_->module, "CAN_ReadFD");
    implementation_->write = function<Write>(implementation_->module, "CAN_Write");
    implementation_->writeFd = function<WriteFd>(implementation_->module, "CAN_WriteFD");
    implementation_->getValue = function<GetValue>(implementation_->module, "CAN_GetValue");
    implementation_->setValue = function<SetValue>(implementation_->module, "CAN_SetValue");
    if (implementation_->initializeClassic == nullptr ||
        implementation_->initializeFd == nullptr ||
        implementation_->uninitialize == nullptr || implementation_->getStatus == nullptr ||
        implementation_->read == nullptr || implementation_->readFd == nullptr ||
        implementation_->write == nullptr || implementation_->writeFd == nullptr ||
        implementation_->getValue == nullptr || implementation_->setValue == nullptr) {
        FreeLibrary(implementation_->module);
        implementation_->module = nullptr;
        return Status::IncompatibleVersion;
    }
    implementation_->loaded = true;
    return Status::Ok;
}

Status Backend::enumerateEndpoints(std::vector<EndpointDescriptor>& endpoints) {
    if (!implementation_->onRuntimeThread()) {
        return implementation_->invoke([this, &endpoints] {
            return enumerateEndpoints(endpoints);
        });
    }
    endpoints.clear();
    const auto init = initialize();
    if (init != Status::Ok) return init;
    std::lock_guard<std::mutex> lock(implementation_->mutex);
    DWORD count = 0;
    auto raw = implementation_->getValue(pcanNoneBus, pcanAttachedChannelCount,
                                           &count, sizeof(count));
    if (raw != pcanOk) return mapStatus(raw);
    std::vector<PcanChannelInformation> attached(count);
    if (count != 0u) {
        const auto bytes = static_cast<std::uint64_t>(count) * sizeof(attached.front());
        if (bytes > (std::numeric_limits<DWORD>::max)()) return Status::InternalError;
        raw = implementation_->getValue(pcanNoneBus, pcanAttachedChannels,
                                          attached.data(), static_cast<DWORD>(bytes));
        if (raw != pcanOk) return mapStatus(raw);
    }
    struct ChannelIdentity {
        std::string guid;
        std::string name;
        std::string endpointBase;
        std::string adapterMaterial;
    };
    std::vector<ChannelIdentity> identities(attached.size());
    for (std::size_t index = 0; index < attached.size(); ++index) {
        const auto& channel = attached[index];
        if (channel.condition == 0u) continue;
        std::array<char, 256> guidBuffer{};
        const auto guidStatus = implementation_->getValue(
            channel.handle, pcanDeviceGuid, guidBuffer.data(),
            static_cast<DWORD>(guidBuffer.size()));
        auto& identity = identities[index];
        identity.guid = guidStatus == pcanOk ?
                            bounded(guidBuffer.data(), guidBuffer.size()) : std::string{};
        identity.name = bounded(channel.deviceName, sizeof(channel.deviceName));
        identity.endpointBase = identity.guid.empty() ?
            "pcan:endpoint:fallback:" + std::to_string(channel.deviceType) + ":" +
                std::to_string(channel.controllerNumber) + ":" + identity.name :
            "pcan:endpoint:" + identity.guid + ":" +
                std::to_string(channel.deviceType) + ":" +
                std::to_string(channel.controllerNumber);
        identity.adapterMaterial = identity.guid.empty() ? identity.endpointBase :
            "pcan:adapter:" + identity.guid + ":" + std::to_string(channel.deviceType);
    }
    for (const auto& identity : identities) {
        if (identity.endpointBase.empty()) continue;
        const auto collisionCount = std::count_if(
            identities.begin(), identities.end(), [&](const auto& candidate) {
                return candidate.endpointBase == identity.endpointBase;
            });
        if (collisionCount > 1 &&
            std::find(implementation_->handleIdentityBases.begin(),
                      implementation_->handleIdentityBases.end(),
                      identity.endpointBase) == implementation_->handleIdentityBases.end()) {
            implementation_->handleIdentityBases.push_back(identity.endpointBase);
        }
    }
    auto previousChannels = std::move(implementation_->channels);
    std::vector<Implementation::ChannelRecord> currentChannels;
    for (std::size_t index = 0; index < attached.size(); ++index) {
        const auto& channel = attached[index];
        const auto& identity = identities[index];
        if (channel.condition == 0u) continue;
        const auto collisionCount = std::count_if(
            identities.begin(), identities.end(), [&](const auto& candidate) {
                return !candidate.endpointBase.empty() &&
                       candidate.endpointBase == identity.endpointBase;
            });
        auto endpointMaterial = identity.endpointBase;
        auto physicalIndex = static_cast<std::uint32_t>(channel.controllerNumber);
        const auto rememberedCollision = std::find(
            implementation_->handleIdentityBases.begin(),
            implementation_->handleIdentityBases.end(), identity.endpointBase) !=
            implementation_->handleIdentityBases.end();
        if (collisionCount > 1 || rememberedCollision) {
            // A valid DeviceGUID/controller tuple should be unique. Some multi-channel
            // presentations expose duplicates, so use the public channel handle as an
            // explicit degraded fallback instead of silently collapsing endpoints. Keep
            // that choice for this Broker lifetime so hot-plug cannot compact identities.
            endpointMaterial += ":handle:" + std::to_string(channel.handle);
            physicalIndex = channel.handle;
        }
        Implementation::ChannelRecord record{};
        record.handle = channel.handle;
        record.condition = channel.condition;
        record.fdCapable = (channel.features & pcanFeatureFd) != 0u;
        record.descriptor.id = hashId(endpointMaterial, 0xe2u);
        record.descriptor.adapterId = hashId(identity.adapterMaterial, 0xa2u);
        record.descriptor.name = identity.name.empty() ? "PEAK PCAN" : identity.name;
        record.descriptor.model = "PEAK PCAN-Basic";
        record.descriptor.serial = identity.guid;
        record.descriptor.backendFamily = "pcan-basic";
        record.descriptor.busType = BusType::Can;
        record.descriptor.physicalIndex = physicalIndex;
        record.descriptor.capabilities = CapabilityClassicCan | CapabilityListenOnly | CapabilityCanAdvancedTiming;
        if (record.fdCapable) {
            record.descriptor.capabilities |= CapabilityCanFdIso | CapabilityBrs;
        }
        const auto owned = std::any_of(implementation_->sessions.begin(),
                                       implementation_->sessions.end(),
            [&](const auto& session) { return session->id == record.descriptor.id; });
        const auto externallyBusy = channel.condition == pcanChannelOccupied;
        record.descriptor.state = owned || externallyBusy ?
                                      DeviceState::Leased : DeviceState::Available;
        currentChannels.push_back(std::move(record));
    }
    implementation_->channels.clear();
    implementation_->channels.reserve(previousChannels.size() + currentChannels.size());
    // Reconcile by stable endpoint ID so a temporary removal neither compacts nor
    // reorders the catalog. Records learned during this Broker lifetime remain as
    // explicit offline tombstones until the Broker exits; newly discovered records
    // are appended deterministically after the known catalog.
    for (auto& previous : previousChannels) {
        const auto current = std::find_if(
            currentChannels.begin(), currentChannels.end(), [&](const auto& candidate) {
                return candidate.descriptor.id == previous.descriptor.id;
            });
        if (current != currentChannels.end()) {
            implementation_->channels.push_back(std::move(*current));
            currentChannels.erase(current);
        } else {
            previous.condition = 0u;
            previous.descriptor.state = DeviceState::Offline;
            implementation_->channels.push_back(std::move(previous));
        }
    }
    for (auto& current : currentChannels) {
        implementation_->channels.push_back(std::move(current));
    }
    for (const auto& channel : implementation_->channels) {
        endpoints.push_back(channel.descriptor);
    }
    return Status::Ok;
}

Status Backend::enumerate(std::vector<DeviceDescriptor>& devices) {
    if (!implementation_->onRuntimeThread()) {
        return implementation_->invoke([this, &devices] { return enumerate(devices); });
    }
    devices.clear();
    std::vector<EndpointDescriptor> endpoints;
    const auto status = enumerateEndpoints(endpoints);
    if (status != Status::Ok) return status;
    for (const auto& endpoint : endpoints) {
        DeviceDescriptor device{};
        device.id = endpoint.id;
        device.name = endpoint.name;
        device.model = endpoint.model;
        device.serial = endpoint.serial;
        device.channelCount = 1u;
        device.capabilities = endpoint.capabilities;
        device.state = endpoint.state;
        devices.push_back(std::move(device));
    }
    return Status::Ok;
}

OpenResult Backend::open(const DeviceId& id, const CanChannelConfig& config,
                         const std::uint32_t ownerPid) {
    if (!implementation_->onRuntimeThread()) {
        return implementation_->invoke([this, &id, &config, ownerPid] {
            return open(id, config, ownerPid);
        });
    }
    if (config.internalLoopback) return {Status::Unsupported, 0};
    if (!validConfig(config)) return {Status::InvalidArgument, 0};
    if (config.noAck || config.terminationSpecified) return {Status::Unsupported, 0};
    std::vector<EndpointDescriptor> ignored;
    const auto enumeration = enumerateEndpoints(ignored);
    if (enumeration != Status::Ok) return {enumeration, 0};
    std::lock_guard<std::mutex> lock(implementation_->mutex);
    const auto found = std::find_if(implementation_->channels.begin(),
                                    implementation_->channels.end(),
        [&](const auto& channel) { return channel.descriptor.id == id; });
    if (found == implementation_->channels.end()) return {Status::NotFound, 0};
    if (found->descriptor.state == DeviceState::Offline) return {Status::NotFound, 0};
    if (found->condition == pcanChannelOccupied ||
        std::any_of(implementation_->sessions.begin(), implementation_->sessions.end(),
                    [&](const auto& session) { return session->id == id; })) {
        return {Status::Busy, 0};
    }
    if (config.canFd && !found->fdCapable) return {Status::Unsupported, 0};
    BYTE listen = config.listenOnly ? pcanParameterOn : pcanParameterOff;
    const auto listenStatus = implementation_->setValue(
        found->handle, pcanListenOnly, &listen, sizeof(listen));
    if (listenStatus != pcanOk) return {mapStatus(listenStatus), 0};
    PcanStatus raw = pcanOk;
    if (config.canFd) {
        std::string timing;
        if (!fdBitrate(config, timing)) return {
            config.nominalTimingSpecified || config.dataTimingSpecified ? Status::Unsupported : Status::InvalidArgument, 0};
        raw = implementation_->initializeFd(found->handle, &timing[0]);
    } else {
        PcanBaudrate bitrate = 0;
        if (!classicBaudrate(config, bitrate)) return {
            config.nominalTimingSpecified ? Status::Unsupported : Status::InvalidArgument, 0};
        raw = implementation_->initializeClassic(found->handle, bitrate, 0u, 0u, 0u);
    }
    if (raw != pcanOk) return {mapStatus(raw), 0};
    BYTE autoReset = pcanParameterOff;
    const auto autoResetStatus = implementation_->setValue(
        found->handle, pcanBusoffAutoreset, &autoReset, sizeof(autoReset));
    if (autoResetStatus != pcanOk) {
        implementation_->uninitialize(found->handle);
        return {mapStatus(autoResetStatus), 0};
    }
    BYTE appliedAutoReset = pcanParameterOn;
    const auto appliedAutoResetStatus = implementation_->getValue(
        found->handle, pcanBusoffAutoreset, &appliedAutoReset,
        sizeof(appliedAutoReset));
    if (appliedAutoResetStatus != pcanOk || appliedAutoReset != autoReset) {
        implementation_->uninitialize(found->handle);
        return {appliedAutoResetStatus == pcanOk ? Status::InvalidArgument :
                                                       mapStatus(appliedAutoResetStatus), 0};
    }
    DWORD appliedNominal = 0;
    const auto nominalStatus = implementation_->getValue(
        found->handle, pcanBusSpeedNominal, &appliedNominal, sizeof(appliedNominal));
    if (nominalStatus != pcanOk || appliedNominal != config.arbitrationBitrate) {
        implementation_->uninitialize(found->handle);
        return {nominalStatus == pcanOk ? Status::InvalidArgument :
                                             mapStatus(nominalStatus),
                0};
    }
    if (config.canFd) {
        DWORD appliedData = 0;
        const auto dataStatus = implementation_->getValue(
            found->handle, pcanBusSpeedData, &appliedData, sizeof(appliedData));
        if (dataStatus != pcanOk || appliedData != config.dataBitrate) {
            implementation_->uninitialize(found->handle);
            return {dataStatus == pcanOk ? Status::InvalidArgument :
                                                mapStatus(dataStatus),
                    0};
        }
    }
    BYTE appliedListen = pcanParameterOff;
    const auto appliedListenStatus = implementation_->getValue(
        found->handle, pcanListenOnly, &appliedListen, sizeof(appliedListen));
    if (appliedListenStatus != pcanOk || appliedListen != listen) {
        implementation_->uninitialize(found->handle);
        return {appliedListenStatus == pcanOk ? Status::InvalidArgument :
                                                   mapStatus(appliedListenStatus), 0};
    }
    const auto receiveEcho = echoFramesRequested();
    if (receiveEcho) {
        BYTE enabled = pcanParameterOn;
        const auto echoStatus = implementation_->setValue(
            found->handle, pcanAllowEchoFrames, &enabled, sizeof(enabled));
        if (echoStatus != pcanOk) {
            implementation_->uninitialize(found->handle);
            return {mapStatus(echoStatus), 0};
        }
        BYTE appliedEcho = pcanParameterOff;
        const auto appliedEchoStatus = implementation_->getValue(
            found->handle, pcanAllowEchoFrames, &appliedEcho, sizeof(appliedEcho));
        if (appliedEchoStatus != pcanOk || appliedEcho != enabled) {
            implementation_->uninitialize(found->handle);
            return {appliedEchoStatus == pcanOk ? Status::InvalidArgument :
                                                     mapStatus(appliedEchoStatus), 0};
        }
    }
    const auto token = randomToken();
    if (token == 0u) {
        implementation_->uninitialize(found->handle);
        return {Status::InternalError, 0};
    }
    auto session = std::make_shared<Implementation::Session>();
    session->token = token;
    session->ownerPid = ownerPid;
    session->id = id;
    session->handle = found->handle;
    session->fd = config.canFd;
    session->receiveEcho = receiveEcho;
    session->config = config;
    implementation_->sessions.push_back(std::move(session));
    return {Status::Ok, token};
}

Status Backend::close(const std::uint64_t token, const std::uint32_t ownerPid) {
    if (!implementation_->onRuntimeThread()) {
        return implementation_->invoke([this, token, ownerPid] {
            return close(token, ownerPid);
        });
    }
    std::lock_guard<std::mutex> lock(implementation_->mutex);
    const auto found = std::find_if(implementation_->sessions.begin(),
                                    implementation_->sessions.end(),
        [&](const auto& session) {
            return session->token == token && session->ownerPid == ownerPid;
        });
    if (found == implementation_->sessions.end()) return Status::NotFound;
    const auto raw = implementation_->uninitialize((*found)->handle);
    implementation_->sessions.erase(found);
    return raw == pcanOk ? Status::Ok : mapStatus(raw);
}

Status Backend::write(const std::uint64_t token, const std::uint32_t ownerPid,
                      const CanFrame& frame) {
    if (!implementation_->onRuntimeThread()) {
        return implementation_->invoke([this, token, ownerPid, &frame] {
            return write(token, ownerPid, frame);
        });
    }
    std::lock_guard<std::mutex> lock(implementation_->mutex);
    const auto session = implementation_->findSession(token, ownerPid);
    if (session == nullptr) return Status::NotFound;
    const auto extended = (frame.flags & FrameExtended) != 0u;
    const auto fdFrame = (frame.flags & FrameFd) != 0u;
    const auto remoteFrame = (frame.flags & FrameRemote) != 0u;
    const auto maximumId = extended ? 0x1fffffffu : 0x7ffu;
    if (frame.id > maximumId || frame.dlc > 15u ||
        frame.dataLength != (remoteFrame ? 0u : dlcToLength(frame.dlc)) ||
        (!session->fd && (fdFrame || frame.dlc > 8u)) ||
        ((frame.flags & (FrameBrs | FrameEsi)) != 0u && !fdFrame) ||
        (fdFrame && remoteFrame) || (remoteFrame && frame.dlc > 8u)) {
        return Status::InvalidArgument;
    }
    BYTE type = extended ? pcanMessageExtended : 0u;
    if ((frame.flags & FrameRemote) != 0u) type |= pcanMessageRtr;
    if (fdFrame) type |= pcanMessageFd;
    if ((frame.flags & FrameBrs) != 0u) type |= pcanMessageBrs;
    if ((frame.flags & FrameEsi) != 0u) type |= pcanMessageEsi;
    PcanStatus raw = pcanOk;
    if (session->fd) {
        PcanMessageFd message{};
        message.id = frame.id;
        message.messageType = type;
        message.dlc = frame.dlc;
        std::memcpy(message.data, frame.data.data(), frame.dataLength);
        raw = implementation_->writeFd(session->handle, &message);
    } else {
        PcanMessage message{};
        message.id = frame.id;
        message.messageType = type;
        message.length = frame.dlc;
        std::memcpy(message.data, frame.data.data(), frame.dataLength);
        raw = implementation_->write(session->handle, &message);
    }
    // PCAN-Basic confirms queue acceptance only. Do not create a physical
    // transmit-completion event here.
    return raw == pcanOk ? Status::Ok : mapStatus(raw);
}

Status Backend::submit(const std::uint64_t token, const std::uint32_t ownerPid,
                       const CanFrame& frame) {
    if (!implementation_->onRuntimeThread()) {
        return implementation_->invoke([this, token, ownerPid, &frame] {
            return submit(token, ownerPid, frame);
        });
    }
    return write(token, ownerPid, frame);
}

Status Backend::readEvents(const std::uint64_t token, const std::uint32_t ownerPid,
                            const std::uint32_t capacity,
                            std::vector<CanEvent>& events) {
    if (!implementation_->onRuntimeThread()) {
        return implementation_->invoke([this, token, ownerPid, capacity, &events] {
            return readEvents(token, ownerPid, capacity, events);
        });
    }
    events.clear();
    std::lock_guard<std::mutex> lock(implementation_->mutex);
    const auto session = implementation_->findSession(token, ownerPid);
    if (session == nullptr) return Status::NotFound;
    const auto limit = (std::min)(capacity, 512u);
    while (events.size() < limit) {
        CanFrame frame{};
        BYTE type = 0u;
        PcanStatus raw = pcanOk;
        if (session->fd) {
            PcanMessageFd message{};
            std::uint64_t timestamp = 0;
            raw = implementation_->readFd(session->handle, &message, &timestamp);
            if (raw == pcanOk) {
                frame.id = message.id;
                frame.dlc = message.dlc;
                type = message.messageType;
                frame.dataLength = (type & pcanMessageRtr) != 0u ? 0u :
                                                                     dlcToLength(message.dlc);
                frame.timestampUs = timestamp;
                std::memcpy(frame.data.data(), message.data, frame.dataLength);
            }
        } else {
            PcanMessage message{};
            PcanTimestamp timestamp{};
            raw = implementation_->read(session->handle, &message, &timestamp);
            if (raw == pcanOk) {
                frame.id = message.id;
                frame.dlc = message.length;
                type = message.messageType;
                frame.dataLength = (type & pcanMessageRtr) != 0u ? 0u : message.length;
                frame.timestampUs = static_cast<std::uint64_t>(timestamp.microseconds) +
                    1000ull * timestamp.milliseconds +
                    0x100000000ull * 1000ull * timestamp.millisecondOverflow;
                std::memcpy(frame.data.data(), message.data, frame.dataLength);
            }
        }
        if (raw == pcanReceiveEmpty) break;
        if (raw != pcanOk) {
            if ((raw & pcanAnyBusState) != 0u) break;
            return mapStatus(raw);
        }
        if ((type & (pcanMessageError | pcanMessageStatus)) != 0u) {
            continue;
        }
        const auto selfReceived = (type & pcanMessageEcho) != 0u;
        if (selfReceived && !session->receiveEcho) continue;
        if ((type & pcanMessageExtended) != 0u) frame.flags |= FrameExtended;
        if ((type & pcanMessageRtr) != 0u) frame.flags |= FrameRemote;
        if ((type & pcanMessageFd) != 0u) frame.flags |= FrameFd;
        if ((type & pcanMessageBrs) != 0u) frame.flags |= FrameBrs;
        if ((type & pcanMessageEsi) != 0u) frame.flags |= FrameEsi;
        if (selfReceived) frame.flags |= FrameSelfReceived;
        if (!acceptsFrame(session->config, frame)) continue;
        CanEvent event{};
        event.kind = CanEventKind::Received;
        event.frame = frame;
        events.push_back(std::move(event));
    }
    const auto rawStatus = implementation_->getStatus(session->handle);
    const auto nonBusError = rawStatus & ~pcanAnyBusState;
    if (nonBusError != pcanOk) return mapStatus(nonBusError);
    const auto current = decodeStatus(rawStatus);
    const auto changed = session->lastStatusValid &&
                         !sameBusState(session->lastStatus, current);
    const auto initialError = !session->lastStatusValid &&
                               (current.busOff || current.errorPassive || current.warning);
    session->lastStatus = current;
    session->lastStatusValid = true;
    if (events.size() < limit && (changed || initialError)) {
        CanEvent event{};
        event.kind = CanEventKind::ControllerState;
        event.backendStatus = rawStatus;
        event.channelStatus = current;
        events.push_back(std::move(event));
    }
    return Status::Ok;
}

Status Backend::read(const std::uint64_t token, const std::uint32_t ownerPid,
                     const std::uint32_t capacity, std::vector<CanFrame>& frames) {
    if (!implementation_->onRuntimeThread()) {
        return implementation_->invoke([this, token, ownerPid, capacity, &frames] {
            return read(token, ownerPid, capacity, frames);
        });
    }
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
    if (!implementation_->onRuntimeThread()) {
        return implementation_->invoke([this, token, ownerPid, &status] {
            return getStatus(token, ownerPid, status);
        });
    }
    std::lock_guard<std::mutex> lock(implementation_->mutex);
    const auto session = implementation_->findSession(token, ownerPid);
    if (session == nullptr) return Status::NotFound;
    const auto raw = implementation_->getStatus(session->handle);
    const auto nonBusError = raw & ~pcanAnyBusState;
    if (nonBusError != pcanOk) return mapStatus(nonBusError);
    status = decodeStatus(raw);
    return Status::Ok;
}

void Backend::pruneDeadSessions() {
    if (!implementation_->onRuntimeThread()) {
        implementation_->invoke([this] { pruneDeadSessions(); });
        return;
    }
    std::lock_guard<std::mutex> lock(implementation_->mutex);
    const auto firstDead = std::remove_if(implementation_->sessions.begin(),
                                           implementation_->sessions.end(),
        [&](const auto& session) {
            if (processAlive(session->ownerPid)) return false;
            implementation_->uninitialize(session->handle);
            return true;
        });
    implementation_->sessions.erase(firstDead, implementation_->sessions.end());
}

}  // namespace Ucal::pcan_backend
