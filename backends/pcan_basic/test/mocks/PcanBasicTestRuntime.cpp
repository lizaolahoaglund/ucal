#include <windows.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <string>

namespace {

using PcanHandle = WORD;
using PcanStatus = DWORD;

constexpr PcanHandle testHandle = 0x51u;
constexpr PcanHandle secondTestHandle = 0x52u;
constexpr PcanStatus statusOk = 0x00000u;
constexpr PcanStatus statusReceiveEmpty = 0x00020u;
constexpr PcanStatus statusInvalidParameterType = 0x04000u;
constexpr PcanStatus statusInvalidParameterValue = 0x08000u;
constexpr PcanStatus statusInvalidData = 0x20000u;
constexpr PcanStatus statusNotInitialized = 0x4000000u;
constexpr PcanStatus statusInvalidOperation = 0x8000000u;

constexpr BYTE parameterListenOnly = 0x08u;
constexpr BYTE parameterBusoffAutoreset = 0x07u;
constexpr BYTE parameterBusSpeedNominal = 0x1au;
constexpr BYTE parameterBusSpeedData = 0x1bu;
constexpr BYTE parameterAttachedCount = 0x2au;
constexpr BYTE parameterAttachedChannels = 0x2bu;
constexpr BYTE parameterAllowEchoFrames = 0x2cu;
constexpr BYTE parameterDeviceGuid = 0x30u;
constexpr BYTE messageEcho = 0x20u;

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

struct PcanChannelInformation {
    PcanHandle handle;
    BYTE deviceType;
    BYTE controllerNumber;
    DWORD features;
    char deviceName[33];
    DWORD deviceId;
    DWORD condition;
};

bool initialized{};
bool fdMode{};
BYTE listenOnly{};
BYTE busoffAutoreset{1u};
BYTE allowEchoFrames{};
bool pending{};
PcanMessage pendingClassic{};
PcanMessageFd pendingFd{};
volatile LONG runtimeThreadId{};

PcanStatus requireRuntimeThread() {
    const auto current = static_cast<LONG>(GetCurrentThreadId());
    const auto expected = InterlockedCompareExchange(&runtimeThreadId, current, 0);
    return expected == 0 || expected == current ? statusOk : statusInvalidOperation;
}

std::string timingReadbackMode() {
    std::array<char, 64> value{};
    const auto length = GetEnvironmentVariableA(
        "UCAL_PCAN_TEST_TIMING_READBACK", value.data(),
        static_cast<DWORD>(value.size()));
    if (length == 0u || length >= value.size()) return {};
    return std::string(value.data(), length);
}

std::string autoresetMode() {
    std::array<char, 64> value{};
    const auto length = GetEnvironmentVariableA(
        "UCAL_PCAN_TEST_AUTORESET", value.data(),
        static_cast<DWORD>(value.size()));
    if (length == 0u || length >= value.size()) return {};
    return std::string(value.data(), length);
}

DWORD visibleChannelMask() {
    std::array<char, 1024> path{};
    const auto length = GetEnvironmentVariableA(
        "UCAL_PCAN_TEST_VISIBILITY_FILE", path.data(),
        static_cast<DWORD>(path.size()));
    if (length == 0u || length >= path.size()) return 0x03u;
    const auto file = CreateFileA(path.data(), GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return 0x03u;
    std::array<char, 32> value{};
    DWORD read = 0u;
    const auto ok = ReadFile(file, value.data(), static_cast<DWORD>(value.size() - 1u),
                             &read, nullptr);
    CloseHandle(file);
    if (!ok) return 0x03u;
    const std::string mode(value.data(), read);
    if (mode.find("none") == 0u) return 0x00u;
    if (mode.find("first") == 0u) return 0x01u;
    if (mode.find("second") == 0u) return 0x02u;
    return 0x03u;
}

bool validHandle(const PcanHandle handle) {
    return handle == testHandle || handle == secondTestHandle;
}

template <typename Value>
PcanStatus copyValue(void* const buffer, const DWORD bufferLength, const Value& value) {
    if (buffer == nullptr || bufferLength < sizeof(value)) return statusInvalidParameterValue;
    std::memcpy(buffer, &value, sizeof(value));
    return statusOk;
}

}  // namespace

extern "C" PcanStatus __stdcall CAN_Initialize(const PcanHandle handle,
                                                const WORD bitrate,
                                                BYTE, DWORD, WORD) {
    if (const auto thread = requireRuntimeThread(); thread != statusOk) return thread;
    // Accept the public 500 kbit/s preset (87.5% sample point) and the exact
    // SJA1000 BTR0/BTR1 encoding for 500 kbit/s at a 75% sample point. A
    // solver that forgets the SJA1000 divide-by-two emits 0x013a and must fail.
    if (!validHandle(handle) ||
        (bitrate != 0x001cu && bitrate != 0x003au) || listenOnly != 0u) {
        return statusInvalidParameterValue;
    }
    if (timingReadbackMode() == "default75" && bitrate != 0x003au)
        return statusInvalidParameterValue;
    initialized = true;
    fdMode = false;
    pending = false;
    return statusOk;
}

extern "C" PcanStatus __stdcall CAN_InitializeFD(const PcanHandle handle,
                                                  char* const bitrate) {
    if (const auto thread = requireRuntimeThread(); thread != statusOk) return thread;
    if (!validHandle(handle) || bitrate == nullptr) return statusInvalidParameterValue;
    const std::string value(bitrate);
    char expected[32]{};
    const bool precise = GetEnvironmentVariableA("UCAL_TEST_EXPECT_TIMING", expected, 32) &&
        (std::string(expected) == "precise" || std::string(expected) == "nominal-only");
    if (precise) {
        const char* fields[]{"f_clock=80000000", "nom_brp=1", "nom_tseg1=139", "nom_tseg2=20",
            "nom_sjw=8", "data_brp=1", "data_tseg1=29", "data_tseg2=10", "data_sjw=4"};
        const auto count = std::string(expected) == "nominal-only" ? 5u : 9u;
        for (unsigned index = 0u; index < count; ++index)
            if (value.find(fields[index]) == std::string::npos) return statusInvalidParameterValue;
        initialized = true; fdMode = true; pending = false;
        return statusOk;
    }
    if (value.find("f_clock=80000000") == std::string::npos ||
        value.find("nom_brp=1") == std::string::npos ||
        !((value.find("nom_tseg1=119") != std::string::npos &&
           value.find("nom_tseg2=40") != std::string::npos) ||
          (value.find("nom_tseg1=111") != std::string::npos &&
           value.find("nom_tseg2=48") != std::string::npos)) ||
        value.find("data_brp=1") == std::string::npos ||
        !((value.find("data_tseg1=29") != std::string::npos &&
           value.find("data_tseg2=10") != std::string::npos) ||
          (value.find("data_tseg1=27") != std::string::npos &&
           value.find("data_tseg2=12") != std::string::npos))) {
        return statusInvalidParameterValue;
    }
    initialized = true;
    fdMode = true;
    pending = false;
    return statusOk;
}

extern "C" PcanStatus __stdcall CAN_Uninitialize(const PcanHandle handle) {
    if (const auto thread = requireRuntimeThread(); thread != statusOk) return thread;
    if (!validHandle(handle)) return statusInvalidParameterValue;
    initialized = false;
    fdMode = false;
    busoffAutoreset = 1u;
    allowEchoFrames = 0u;
    pending = false;
    return statusOk;
}

extern "C" PcanStatus __stdcall CAN_GetStatus(const PcanHandle handle) {
    if (const auto thread = requireRuntimeThread(); thread != statusOk) return thread;
    if (!validHandle(handle)) return statusInvalidParameterValue;
    return initialized ? statusOk : statusNotInitialized;
}

extern "C" PcanStatus __stdcall CAN_Read(const PcanHandle handle,
                                          PcanMessage* const message,
                                          PcanTimestamp* const timestamp) {
    if (const auto thread = requireRuntimeThread(); thread != statusOk) return thread;
    if (!validHandle(handle) || !initialized || fdMode) return statusNotInitialized;
    if (!pending) return statusReceiveEmpty;
    if (message == nullptr) return statusInvalidParameterValue;
    *message = pendingClassic;
    if (timestamp != nullptr) {
        timestamp->milliseconds = 123u;
        timestamp->millisecondOverflow = 0u;
        timestamp->microseconds = 456u;
    }
    pending = false;
    return statusOk;
}

extern "C" PcanStatus __stdcall CAN_ReadFD(const PcanHandle handle,
                                            PcanMessageFd* const message,
                                            std::uint64_t* const timestamp) {
    if (const auto thread = requireRuntimeThread(); thread != statusOk) return thread;
    if (!validHandle(handle) || !initialized || !fdMode) return statusNotInitialized;
    if (!pending) return statusReceiveEmpty;
    if (message == nullptr) return statusInvalidParameterValue;
    *message = pendingFd;
    if (timestamp != nullptr) *timestamp = 123456u;
    pending = false;
    return statusOk;
}

extern "C" PcanStatus __stdcall CAN_Write(const PcanHandle handle,
                                           PcanMessage* const message) {
    if (const auto thread = requireRuntimeThread(); thread != statusOk) return thread;
    if (!validHandle(handle) || !initialized || fdMode) return statusNotInitialized;
    if (message == nullptr || message->length > 8u || busoffAutoreset != 0u) {
        return statusInvalidData;
    }
    pendingClassic = *message;
    if (allowEchoFrames != 0u) pendingClassic.messageType |= messageEcho;
    pending = true;
    return statusOk;
}

extern "C" PcanStatus __stdcall CAN_WriteFD(const PcanHandle handle,
                                             PcanMessageFd* const message) {
    if (const auto thread = requireRuntimeThread(); thread != statusOk) return thread;
    if (!validHandle(handle) || !initialized || !fdMode) return statusNotInitialized;
    if (message == nullptr || message->dlc > 15u || busoffAutoreset != 0u) {
        return statusInvalidData;
    }
    pendingFd = *message;
    if (allowEchoFrames != 0u) pendingFd.messageType |= messageEcho;
    pending = true;
    return statusOk;
}

extern "C" PcanStatus __stdcall CAN_GetValue(const PcanHandle handle,
                                              const BYTE parameter,
                                              void* const buffer,
                                              const DWORD bufferLength) {
    if (const auto thread = requireRuntimeThread(); thread != statusOk) return thread;
    if (parameter == parameterAttachedCount && handle == 0u) {
        const auto mask = visibleChannelMask();
        const DWORD count = ((mask & 0x01u) != 0u ? 1u : 0u) +
                            ((mask & 0x02u) != 0u ? 1u : 0u);
        return copyValue(buffer, bufferLength, count);
    }
    if (parameter == parameterAttachedChannels && handle == 0u) {
        std::array<PcanChannelInformation, 2> info{};
        const char name[] = "UCAL PCAN Test Channel";
        const auto mask = visibleChannelMask();
        std::size_t count = 0u;
        for (std::size_t source = 0u; source < 2u; ++source) {
            if ((mask & (1u << source)) == 0u) continue;
            auto& channel = info[count++];
            channel.handle = source == 0u ? testHandle : secondTestHandle;
            channel.deviceType = 0x05u;
            channel.controllerNumber = 0u;
            channel.features = 0x01u;
            std::memcpy(channel.deviceName, name, sizeof(name));
            channel.deviceId = 0xffffffffu;
            channel.condition = 0x01u;
        }
        const auto bytes = static_cast<DWORD>(count * sizeof(info.front()));
        if (buffer == nullptr || bufferLength < bytes) return statusInvalidParameterValue;
        if (bytes != 0u) std::memcpy(buffer, info.data(), bytes);
        return statusOk;
    }
    if (!validHandle(handle)) return statusInvalidParameterValue;
    if (parameter == parameterDeviceGuid) {
        constexpr char guid[] = "{11111111-2222-3333-4444-555555555555}";
        if (buffer == nullptr || bufferLength < sizeof(guid)) return statusInvalidParameterValue;
        std::memcpy(buffer, guid, sizeof(guid));
        return statusOk;
    }
    if (parameter == parameterBusSpeedNominal) {
        const auto mode = timingReadbackMode();
        if (mode == "nominal-unavailable") return statusInvalidParameterType;
        const DWORD bitrate = mode == "nominal-mismatch" ? 250000u : 500000u;
        return copyValue(buffer, bufferLength, bitrate);
    }
    if (parameter == parameterBusSpeedData) {
        const auto mode = timingReadbackMode();
        if (mode == "data-unavailable") return statusInvalidParameterType;
        char expected[32]{};
        GetEnvironmentVariableA("UCAL_TEST_EXPECT_TIMING", expected, 32);
        const DWORD bitrate = mode == "data-mismatch" ? 1000000u :
            std::string(expected) == "nominal-only" ? 500000u : 2000000u;
        return copyValue(buffer, bufferLength, bitrate);
    }
    if (parameter == parameterListenOnly) {
        return copyValue(buffer, bufferLength, listenOnly);
    }
    if (parameter == parameterBusoffAutoreset) {
        const auto mode = autoresetMode();
        if (mode == "read-unavailable") return statusInvalidParameterType;
        if (mode == "read-mismatch") {
            const BYTE enabled = 1u;
            return copyValue(buffer, bufferLength, enabled);
        }
        return copyValue(buffer, bufferLength, busoffAutoreset);
    }
    if (parameter == parameterAllowEchoFrames && initialized) {
        return copyValue(buffer, bufferLength, allowEchoFrames);
    }
    return statusInvalidParameterType;
}

extern "C" PcanStatus __stdcall CAN_SetValue(const PcanHandle handle,
                                              const BYTE parameter,
                                              void* const buffer,
                                              const DWORD bufferLength) {
    if (const auto thread = requireRuntimeThread(); thread != statusOk) return thread;
    if (!validHandle(handle) || buffer == nullptr || bufferLength < sizeof(BYTE)) {
        return statusInvalidParameterValue;
    }
    if (parameter == parameterListenOnly) {
        listenOnly = *static_cast<BYTE*>(buffer);
        return statusOk;
    }
    if (parameter == parameterBusoffAutoreset) {
        if (autoresetMode() == "set-unavailable") return statusInvalidParameterType;
        busoffAutoreset = *static_cast<BYTE*>(buffer);
        return statusOk;
    }
    if (parameter == parameterAllowEchoFrames && initialized) {
        allowEchoFrames = *static_cast<BYTE*>(buffer);
        return statusOk;
    }
    return statusInvalidParameterValue;
}
