#include "PcanAbi.hpp"
#include "ucal/Client.hpp"
#include "ucal/Model.hpp"
#include "ucal/CanTiming.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

std::atomic_uint32_t enumerationCalls{0};
std::atomic_uint32_t lastChannelCount{0};
std::mutex channelMutex;

struct OpenChannel {
    Ucal::EndpointId endpointId{};
    std::uint64_t leaseToken{};
    Ucal::CanChannelConfig config{};
    Ucal::Pcan::Baudrate classicBitrate{};
    std::string fdBitrate;
};

std::map<Ucal::Pcan::Handle, OpenChannel> openChannels;
std::map<Ucal::Pcan::Handle, bool> pendingListenOnly;

void audit(const char* const event, const std::uint32_t deviceClass,
           const std::uint32_t capacity, const std::uint32_t count,
           const Ucal::Pcan::Status status) {
    wchar_t path[32768]{};
    auto length = GetEnvironmentVariableW(L"UCAL_PCAN_AUDIT_PATH", path,
                                          static_cast<DWORD>(std::size(path)));
    if (length == 0u) {
        const auto module = GetModuleHandleW(L"UcalPcanIntercept.dll");
        length = module == nullptr ? 0u : GetModuleFileNameW(
            module, path, static_cast<DWORD>(std::size(path)));
        if (length != 0u && length < std::size(path)) {
            wchar_t* separator = wcsrchr(path, L'\\');
            if (separator != nullptr) {
                const wchar_t suffix[] = L"\\ucal_pcan_call_audit.log";
                const auto prefix = static_cast<std::size_t>(separator - path);
                if (prefix + std::size(suffix) < std::size(path)) {
                    std::copy(std::begin(suffix), std::end(suffix), path + prefix);
                    length = static_cast<DWORD>(prefix + std::size(suffix) - 1u);
                }
            }
        }
    }
    if (length == 0u || length >= std::size(path)) return;
    const auto file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    char line[256]{};
    const auto used = std::snprintf(line, sizeof(line),
        "event=%s pid=%lu device=%u capacity=%u count=%u status=0x%08x\r\n",
        event, GetCurrentProcessId(), deviceClass, capacity, count, status);
    if (used > 0) {
        DWORD written = 0;
        WriteFile(file, line, static_cast<DWORD>((std::min)(used, static_cast<int>(sizeof(line)))),
                  &written, nullptr);
    }
    CloseHandle(file);
}

std::uint32_t stableHardwareId(const Ucal::EndpointId& endpointId) {
    std::uint32_t hash = 2166136261u;
    for (const auto value : endpointId) {
        hash ^= value;
        hash *= 16777619u;
    }
    // Keep the value in a UCAL-local synthetic range without reducing it to a
    // catalog-order counter. Endpoint-ID collisions are rejected below.
    return 0x80000000u | (hash & 0x7fffffffu);
}

struct ChannelCatalog {
    std::vector<Ucal::Pcan::AvailableHardware> channels;
    std::vector<Ucal::EndpointDescriptor> endpoints;
    bool identityCollision{};
};

std::mutex catalogMutex;
std::vector<Ucal::EndpointDescriptor> knownEndpoints;

ChannelCatalog makeChannels(std::vector<Ucal::EndpointDescriptor> endpoints,
                             const std::uint32_t requestedDeviceClass) {
    std::sort(endpoints.begin(), endpoints.end(), [](const auto& left, const auto& right) {
        if (left.backendFamily != right.backendFamily) {
            return left.backendFamily < right.backendFamily;
        }
        if (left.adapterId != right.adapterId) return left.adapterId < right.adapterId;
        if (left.physicalIndex != right.physicalIndex) {
            return left.physicalIndex < right.physicalIndex;
        }
        return left.id < right.id;
    });
    std::lock_guard<std::mutex> lock(catalogMutex);
    auto slots = knownEndpoints;
    for (auto& endpoint : slots) endpoint.state = Ucal::DeviceState::Offline;
    std::unordered_set<std::string> liveIds;
    ChannelCatalog catalog;
    for (const auto& endpoint : endpoints) {
        if (endpoint.busType != Ucal::BusType::Can ||
            (endpoint.capabilities & Ucal::CapabilityClassicCan) == 0u) continue;
        if (!liveIds.insert(Ucal::deviceIdToString(endpoint.id)).second) {
            catalog.identityCollision = true;
            return catalog;
        }
        const auto found = std::find_if(slots.begin(), slots.end(), [&](const auto& slot) {
            return slot.id == endpoint.id;
        });
        if (found != slots.end()) *found = endpoint;
        else if (slots.size() < Ucal::Pcan::maximumChannels) slots.push_back(endpoint);
    }
    std::unordered_set<std::uint32_t> identities;
    for (const auto& endpoint : slots) {
        Ucal::Pcan::AvailableHardware channel{};
        channel.deviceClass = Ucal::Pcan::deviceUsb;
        channel.type = 0;
        std::snprintf(channel.name, sizeof(channel.name), "UCAL %.27s",
                      endpoint.name.c_str());
        channel.hardwareId = stableHardwareId(endpoint.id);
        channel.channel = endpoint.physicalIndex + 1u;
        if (!identities.insert(channel.hardwareId).second) {
            catalog.identityCollision = true;
            catalog.channels.clear();
            catalog.endpoints.clear();
            return catalog;
        }
        if (endpoint.state != Ucal::DeviceState::Offline &&
            (requestedDeviceClass == Ucal::Pcan::deviceAll ||
             requestedDeviceClass == channel.deviceClass)) {
            catalog.channels.push_back(channel);
        }
    }
    // Keep all learned slots, including offline tombstones. Only the attached
    // list is filtered; a handle never changes its endpoint during this DLL life.
    knownEndpoints = slots;
    catalog.endpoints = std::move(slots);
    return catalog;
}

Ucal::Pcan::Handle handleForOrdinal(const std::uint32_t ordinal) {
    if (ordinal < 1u || ordinal > Ucal::Pcan::maximumChannels) return 0u;
    return static_cast<Ucal::Pcan::Handle>(ordinal <= 8u ? 0x50u + ordinal
                                                         : 0x500u + ordinal);
}

std::uint32_t ordinalForHandle(const Ucal::Pcan::Handle handle) {
    if (handle >= 0x51u && handle <= 0x58u) return handle - 0x50u;
    if (handle >= 0x509u && handle <= 0x510u) return handle - 0x500u;
    return 0u;
}

Ucal::Pcan::Status mapStatus(const Ucal::Status status) {
    switch (status) {
    case Ucal::Status::Ok: return Ucal::Pcan::success;
    case Ucal::Status::InvalidArgument: return Ucal::Pcan::invalidParameter;
    case Ucal::Status::Unsupported:
    case Ucal::Status::IncompatibleVersion: return Ucal::Pcan::illegalOperation;
    case Ucal::Status::NotFound: return Ucal::Pcan::invalidHardware;
    case Ucal::Status::Busy:
    case Ucal::Status::AccessDenied: return Ucal::Pcan::hardwareInUse;
    case Ucal::Status::TransportError: return Ucal::Pcan::driverNotLoaded;
    case Ucal::Status::InternalError: return Ucal::Pcan::unknownError;
    }
    return Ucal::Pcan::unknownError;
}

bool catalog(ChannelCatalog& result) {
    std::vector<Ucal::EndpointDescriptor> endpoints;
    if (Ucal::Client{}.enumerateEndpoints(endpoints) != Ucal::Status::Ok) return false;
    result = makeChannels(std::move(endpoints), Ucal::Pcan::deviceAll);
    return !result.identityCollision;
}

bool classicBitrate(const Ucal::Pcan::Baudrate encoded, std::uint32_t& bitrate) {
    switch (encoded) {
    case 0x0014u: bitrate = 1000000u; return true;
    case 0x0016u: bitrate = 800000u; return true;
    case 0x001cu: bitrate = 500000u; return true;
    case 0x011cu: bitrate = 250000u; return true;
    case 0x031cu: bitrate = 125000u; return true;
    case 0x432fu: bitrate = 100000u; return true;
    case 0xc34eu: bitrate = 95238u; return true;
    case 0x852bu: bitrate = 83333u; return true;
    case 0x472fu: bitrate = 50000u; return true;
    case 0x1414u: bitrate = 47619u; return true;
    case 0x8b2fu: bitrate = 33333u; return true;
    case 0x532fu: bitrate = 20000u; return true;
    case 0x672fu: bitrate = 10000u; return true;
    case 0x7f7fu: bitrate = 5000u; return true;
    default: return false;
    }
}

bool parseUnsigned(const std::string& value, std::uint32_t& parsed) {
    if (value.empty()) return false;
    std::uint64_t result = 0u;
    for (const char character : value) {
        if (character < '0' || character > '9') return false;
        result = result * 10u + static_cast<unsigned>(character - '0');
        if (result > std::numeric_limits<std::uint32_t>::max()) return false;
    }
    parsed = static_cast<std::uint32_t>(result);
    return true;
}

bool parseFdBitrate(const char* text, Ucal::CanChannelConfig& config) {
    if (text == nullptr) return false;
    std::map<std::string, std::uint32_t> values;
    std::string input(text);
    std::size_t offset = 0u;
    while (offset < input.size()) {
        const auto comma = input.find(',', offset);
        std::string item = input.substr(offset, comma == std::string::npos
                                                   ? std::string::npos : comma - offset);
        const auto equal = item.find('=');
        if (equal == std::string::npos) return false;
        auto key = item.substr(0u, equal);
        auto value = item.substr(equal + 1u);
        key.erase(std::remove_if(key.begin(), key.end(), [](const unsigned char c) {
            return std::isspace(c) != 0;
        }), key.end());
        value.erase(std::remove_if(value.begin(), value.end(), [](const unsigned char c) {
            return std::isspace(c) != 0;
        }), value.end());
        std::transform(key.begin(), key.end(), key.begin(), [](const unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        std::uint32_t parsed = 0u;
        if (!parseUnsigned(value, parsed) || !values.emplace(key, parsed).second) return false;
        if (comma == std::string::npos) break;
        offset = comma + 1u;
    }
    std::uint32_t clock = 0u;
    if (values.count("f_clock") + values.count("f_clock_mhz") != 1u) return false;
    const std::unordered_set<std::string> supported{
        "f_clock", "f_clock_mhz", "nom_brp", "nom_tseg1", "nom_tseg2", "nom_sjw",
        "data_brp", "data_tseg1", "data_tseg2", "data_sjw"};
    for (const auto& value : values) if (!supported.count(value.first)) return false;
    if (const auto found = values.find("f_clock"); found != values.end()) {
        clock = found->second;
    } else if (const auto foundMhz = values.find("f_clock_mhz");
               foundMhz != values.end() &&
               foundMhz->second <= std::numeric_limits<std::uint32_t>::max() / 1000000u) {
        clock = foundMhz->second * 1000000u;
    }
    const auto required = [&values](const char* name, std::uint32_t& value) {
        const auto found = values.find(name);
        if (found == values.end() || found->second == 0u) return false;
        value = found->second;
        return true;
    };
    if (clock != 80000000u && clock != 60000000u && clock != 40000000u &&
        clock != 30000000u && clock != 24000000u && clock != 20000000u) return false;
    std::uint32_t nomBrp{}, nomTseg1{}, nomTseg2{}, nomSjw{}, dataBrp{}, dataTseg1{}, dataTseg2{}, dataSjw{};
    if (clock == 0u || !required("nom_brp", nomBrp) ||
        !required("nom_tseg1", nomTseg1) || !required("nom_tseg2", nomTseg2) ||
        !required("nom_sjw", nomSjw) || nomBrp > 1024u || nomTseg1 > 256u ||
        nomTseg2 > 128u || nomSjw > nomTseg2) return false;
    const bool hasData = values.count("data_brp") || values.count("data_tseg1") ||
                         values.count("data_tseg2") || values.count("data_sjw");
    if (hasData && (!required("data_brp", dataBrp) || !required("data_tseg1", dataTseg1) ||
        !required("data_tseg2", dataTseg2) || !required("data_sjw", dataSjw) ||
        dataBrp > 1024u || dataTseg1 > 32u || dataTseg2 > 16u || dataSjw > dataTseg2)) return false;
    if (!hasData) { dataBrp = nomBrp; dataTseg1 = nomTseg1; dataTseg2 = nomTseg2; }
    const std::uint64_t nominalDivisor = static_cast<std::uint64_t>(nomBrp) *
        (1u + nomTseg1 + nomTseg2);
    const std::uint64_t dataDivisor = static_cast<std::uint64_t>(dataBrp) *
        (1u + dataTseg1 + dataTseg2);
    if (nominalDivisor == 0u || dataDivisor == 0u ||
        clock % nominalDivisor != 0u || clock % dataDivisor != 0u) return false;
    config.arbitrationBitrate = static_cast<std::uint32_t>(clock / nominalDivisor);
    config.dataBitrate = static_cast<std::uint32_t>(clock / dataDivisor);
    config.arbitrationSamplePoint = static_cast<std::uint8_t>(
        (((1u + nomTseg1) * 100u) + (1u + nomTseg1 + nomTseg2) / 2u) /
        (1u + nomTseg1 + nomTseg2));
    config.dataSamplePoint = static_cast<std::uint8_t>(
        (((1u + dataTseg1) * 100u) + (1u + dataTseg1 + dataTseg2) / 2u) /
        (1u + dataTseg1 + dataTseg2));
    config.arbitrationSamplePointSpecified = true;
    config.dataSamplePointSpecified = hasData;
    config.nominalTimingSpecified = true;
    config.dataTimingSpecified = hasData;
    config.nominalTiming = {clock, nomBrp, nomTseg1, nomTseg2, nomSjw, 1u};
    if (hasData) config.dataTiming = {clock, dataBrp, dataTseg1, dataTseg2, dataSjw, 1u};
    config.canFd = true;
    return validCanAdvancedTiming(config);
}

std::uint8_t lengthFromDlc(const std::uint8_t dlc) {
    static constexpr std::uint8_t lengths[]{0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u,
                                            8u, 12u, 16u, 20u, 24u, 32u, 48u, 64u};
    return dlc < std::size(lengths) ? lengths[dlc] : 0xffu;
}

Ucal::CanFrame makeFrame(const std::uint32_t id, const Ucal::Pcan::MessageType type,
                          const std::uint8_t dlc, const std::uint8_t* data,
                          const bool fdCall) {
    Ucal::CanFrame frame{};
    frame.id = id;
    frame.dlc = dlc;
    frame.dataLength = fdCall ? lengthFromDlc(dlc) : dlc;
    if ((type & Ucal::Pcan::messageExtended) != 0u) frame.flags |= Ucal::FrameExtended;
    if ((type & Ucal::Pcan::messageRtr) != 0u) frame.flags |= Ucal::FrameRemote;
    if ((type & Ucal::Pcan::messageFd) != 0u) frame.flags |= Ucal::FrameFd;
    if ((type & Ucal::Pcan::messageBrs) != 0u) frame.flags |= Ucal::FrameBrs;
    if ((type & Ucal::Pcan::messageEsi) != 0u) frame.flags |= Ucal::FrameEsi;
    if (data != nullptr && frame.dataLength <= frame.data.size()) {
        std::copy_n(data, frame.dataLength, frame.data.begin());
    }
    return frame;
}

bool validFrame(const Ucal::CanFrame& frame) {
    const bool extended = (frame.flags & Ucal::FrameExtended) != 0u;
    return frame.dataLength <= frame.data.size() && frame.dlc <= 15u &&
           frame.id <= (extended ? 0x1fffffffu : 0x7ffu) &&
           ((frame.flags & Ucal::FrameBrs) == 0u || (frame.flags & Ucal::FrameFd) != 0u) &&
           ((frame.flags & Ucal::FrameRemote) == 0u || (frame.flags & Ucal::FrameFd) == 0u);
}

Ucal::Pcan::MessageType messageType(const Ucal::CanFrame& frame) {
    Ucal::Pcan::MessageType type = Ucal::Pcan::messageStandard;
    if ((frame.flags & Ucal::FrameExtended) != 0u) type |= Ucal::Pcan::messageExtended;
    if ((frame.flags & Ucal::FrameRemote) != 0u) type |= Ucal::Pcan::messageRtr;
    if ((frame.flags & Ucal::FrameFd) != 0u) type |= Ucal::Pcan::messageFd;
    if ((frame.flags & Ucal::FrameBrs) != 0u) type |= Ucal::Pcan::messageBrs;
    if ((frame.flags & Ucal::FrameEsi) != 0u) type |= Ucal::Pcan::messageEsi;
    return type;
}

Ucal::Pcan::Status initializeChannel(const Ucal::Pcan::Handle handle,
                                      Ucal::CanChannelConfig config,
                                      const Ucal::Pcan::Baudrate classic,
                                      std::string fdBitrate) {
    const auto ordinal = ordinalForHandle(handle);
    if (ordinal == 0u) return Ucal::Pcan::invalidHardware;
    std::lock_guard<std::mutex> lock(channelMutex);
    if (openChannels.find(handle) != openChannels.end()) return Ucal::Pcan::notInitialized;
    ChannelCatalog available;
    if (!catalog(available)) return Ucal::Pcan::driverNotLoaded;
    if (ordinal > available.endpoints.size()) return Ucal::Pcan::invalidHardware;
    config.listenOnly = pendingListenOnly[handle];
    const auto& endpoint = available.endpoints[ordinal - 1u];
    if (endpoint.state == Ucal::DeviceState::Offline) return Ucal::Pcan::invalidHardware;
    if (config.canFd && (endpoint.capabilities & Ucal::CapabilityCanFdIso) == 0u) {
        return Ucal::Pcan::invalidMode;
    }
    if ((config.nominalTimingSpecified || config.dataTimingSpecified) &&
        !(endpoint.capabilities & Ucal::CapabilityCanAdvancedTiming)) return Ucal::Pcan::illegalOperation;
    const auto opened = Ucal::Client{}.openCanChannel(endpoint.id, config);
    if (opened.status != Ucal::Status::Ok) return mapStatus(opened.status);
    openChannels.emplace(handle, OpenChannel{endpoint.id, opened.leaseToken, config,
                                              classic, std::move(fdBitrate)});
    return Ucal::Pcan::success;
}

template <typename Value>
Ucal::Pcan::Status writeValue(void* buffer, const std::uint32_t bytes, const Value& value) {
    if (buffer == nullptr || bytes < sizeof(Value)) return Ucal::Pcan::invalidParameter;
    std::memcpy(buffer, &value, sizeof(Value));
    return Ucal::Pcan::success;
}

Ucal::Pcan::Status writeString(void* buffer, const std::uint32_t bytes,
                                const std::string& value) {
    if (buffer == nullptr || bytes <= value.size()) return Ucal::Pcan::invalidParameter;
    std::memcpy(buffer, value.c_str(), value.size() + 1u);
    return Ucal::Pcan::success;
}

}  // namespace

extern "C" {

__declspec(dllexport) Ucal::Pcan::Status __stdcall CAN_Initialize(
    const Ucal::Pcan::Handle channel, const Ucal::Pcan::Baudrate bitrate,
    std::uint8_t, std::uint32_t, std::uint16_t) {
    std::uint32_t bitsPerSecond = 0u;
    if (!classicBitrate(bitrate, bitsPerSecond)) return Ucal::Pcan::invalidParameter;
    Ucal::CanChannelConfig config{};
    config.arbitrationBitrate = bitsPerSecond;
    config.dataBitrate = bitsPerSecond;
    config.canFd = false;
    return initializeChannel(channel, config, bitrate, {});
}

__declspec(dllexport) Ucal::Pcan::Status __stdcall CAN_InitializeFD(
    const Ucal::Pcan::Handle channel, char* const bitrate) {
    Ucal::CanChannelConfig config{};
    if (!parseFdBitrate(bitrate, config)) return Ucal::Pcan::invalidParameter;
    return initializeChannel(channel, config, 0u, bitrate);
}

__declspec(dllexport) Ucal::Pcan::Status __stdcall CAN_Uninitialize(
    const Ucal::Pcan::Handle channel) {
    std::lock_guard<std::mutex> lock(channelMutex);
    if (channel == Ucal::Pcan::noneBus) {
        Ucal::Pcan::Status result = Ucal::Pcan::success;
        for (const auto& entry : openChannels) {
            const auto status = mapStatus(Ucal::Client{}.closeDevice(entry.second.leaseToken));
            if (result == Ucal::Pcan::success && status != Ucal::Pcan::success) result = status;
        }
        openChannels.clear();
        return result;
    }
    const auto found = openChannels.find(channel);
    if (found == openChannels.end()) return Ucal::Pcan::notInitialized;
    const auto status = mapStatus(Ucal::Client{}.closeDevice(found->second.leaseToken));
    if (status == Ucal::Pcan::success || status == Ucal::Pcan::invalidHardware ||
        status == Ucal::Pcan::driverNotLoaded) {
        openChannels.erase(found);
    }
    return status;
}

__declspec(dllexport) Ucal::Pcan::Status __stdcall CAN_Reset(
    const Ucal::Pcan::Handle channel) {
    std::lock_guard<std::mutex> lock(channelMutex);
    const auto found = openChannels.find(channel);
    if (found == openChannels.end()) return Ucal::Pcan::notInitialized;
    for (unsigned pass = 0; pass < 64u; ++pass) {
        std::vector<Ucal::CanFrame> frames;
        const auto status = Ucal::Client{}.readCan(found->second.leaseToken, 256u, frames);
        if (status != Ucal::Status::Ok) return mapStatus(status);
        if (frames.empty()) break;
    }
    return Ucal::Pcan::success;
}

__declspec(dllexport) Ucal::Pcan::Status __stdcall CAN_GetStatus(
    const Ucal::Pcan::Handle channel) {
    std::lock_guard<std::mutex> lock(channelMutex);
    const auto found = openChannels.find(channel);
    if (found == openChannels.end()) return Ucal::Pcan::notInitialized;
    Ucal::ChannelStatus state{};
    const auto result = Ucal::Client{}.getChannelStatus(found->second.leaseToken, state);
    if (result != Ucal::Status::Ok) return mapStatus(result);
    Ucal::Pcan::Status status = Ucal::Pcan::success;
    if (state.busStateValid) {
        if (state.busOff) status |= Ucal::Pcan::busOff;
        if (state.errorPassive) status |= Ucal::Pcan::busPassive;
        if (state.warning) status |= Ucal::Pcan::busHeavy;
    }
    return status;
}

__declspec(dllexport) Ucal::Pcan::Status __stdcall CAN_Read(
    const Ucal::Pcan::Handle channel, Ucal::Pcan::ClassicMessage* const message,
    Ucal::Pcan::ClassicTimestamp* const timestamp) {
    if (message == nullptr) return Ucal::Pcan::invalidParameter;
    std::lock_guard<std::mutex> lock(channelMutex);
    const auto found = openChannels.find(channel);
    if (found == openChannels.end()) return Ucal::Pcan::notInitialized;
    if (found->second.config.canFd) return Ucal::Pcan::invalidMode;
    std::vector<Ucal::CanFrame> frames;
    const auto result = Ucal::Client{}.readCan(found->second.leaseToken, 1u, frames);
    if (result != Ucal::Status::Ok) return mapStatus(result);
    if (frames.empty()) return Ucal::Pcan::receiveEmpty;
    const auto& frame = frames.front();
    if (!validFrame(frame) || (frame.flags & Ucal::FrameFd) != 0u || frame.dataLength > 8u) {
        return Ucal::Pcan::invalidData;
    }
    *message = {};
    message->id = frame.id;
    message->type = messageType(frame);
    message->length = frame.dataLength;
    std::copy_n(frame.data.begin(), frame.dataLength, message->data);
    if (timestamp != nullptr) {
        const std::uint64_t milliseconds = frame.timestampUs / 1000u;
        timestamp->milliseconds = static_cast<std::uint32_t>(milliseconds);
        timestamp->millisecondsOverflow = static_cast<std::uint16_t>(milliseconds >> 32u);
        timestamp->microseconds = static_cast<std::uint16_t>(frame.timestampUs % 1000u);
    }
    return Ucal::Pcan::success;
}

__declspec(dllexport) Ucal::Pcan::Status __stdcall CAN_ReadFD(
    const Ucal::Pcan::Handle channel, Ucal::Pcan::FdMessage* const message,
    std::uint64_t* const timestamp) {
    if (message == nullptr) return Ucal::Pcan::invalidParameter;
    std::lock_guard<std::mutex> lock(channelMutex);
    const auto found = openChannels.find(channel);
    if (found == openChannels.end()) return Ucal::Pcan::notInitialized;
    if (!found->second.config.canFd) return Ucal::Pcan::invalidMode;
    std::vector<Ucal::CanFrame> frames;
    const auto result = Ucal::Client{}.readCan(found->second.leaseToken, 1u, frames);
    if (result != Ucal::Status::Ok) return mapStatus(result);
    if (frames.empty()) return Ucal::Pcan::receiveEmpty;
    const auto& frame = frames.front();
    if (!validFrame(frame)) return Ucal::Pcan::invalidData;
    *message = {};
    message->id = frame.id;
    message->type = messageType(frame);
    message->dlc = frame.dlc;
    std::copy_n(frame.data.begin(), frame.dataLength, message->data);
    if (timestamp != nullptr) *timestamp = frame.timestampUs;
    return Ucal::Pcan::success;
}

__declspec(dllexport) Ucal::Pcan::Status __stdcall CAN_Write(
    const Ucal::Pcan::Handle channel, Ucal::Pcan::ClassicMessage* const message) {
    if (message == nullptr || message->length > 8u ||
        (message->type & ~(Ucal::Pcan::messageRtr | Ucal::Pcan::messageExtended)) != 0u) {
        return Ucal::Pcan::invalidData;
    }
    std::lock_guard<std::mutex> lock(channelMutex);
    const auto found = openChannels.find(channel);
    if (found == openChannels.end()) return Ucal::Pcan::notInitialized;
    if (found->second.config.canFd) return Ucal::Pcan::invalidMode;
    const auto frame = makeFrame(message->id, message->type, message->length,
                                  message->data, false);
    if (!validFrame(frame)) return Ucal::Pcan::invalidData;
    return mapStatus(Ucal::Client{}.submitCan(found->second.leaseToken, frame));
}

__declspec(dllexport) Ucal::Pcan::Status __stdcall CAN_WriteFD(
    const Ucal::Pcan::Handle channel, Ucal::Pcan::FdMessage* const message) {
    if (message == nullptr || message->dlc > 15u ||
        (message->type & ~(Ucal::Pcan::messageRtr | Ucal::Pcan::messageExtended |
                           Ucal::Pcan::messageFd | Ucal::Pcan::messageBrs |
                           Ucal::Pcan::messageEsi)) != 0u) {
        return Ucal::Pcan::invalidData;
    }
    std::lock_guard<std::mutex> lock(channelMutex);
    const auto found = openChannels.find(channel);
    if (found == openChannels.end()) return Ucal::Pcan::notInitialized;
    if (!found->second.config.canFd) return Ucal::Pcan::invalidMode;
    const auto frame = makeFrame(message->id, message->type, message->dlc,
                                  message->data, true);
    if (!validFrame(frame)) return Ucal::Pcan::invalidData;
    return mapStatus(Ucal::Client{}.submitCan(found->second.leaseToken, frame));
}

__declspec(dllexport) Ucal::Pcan::Status __stdcall CAN_FilterMessages(
    Ucal::Pcan::Handle, std::uint32_t, std::uint32_t, std::uint8_t) {
    // Arbitrary inclusive ranges cannot be represented truthfully by UCAL's
    // mask filters. Do not pretend a filter was applied.
    return Ucal::Pcan::illegalOperation;
}

__declspec(dllexport) Ucal::Pcan::Status __stdcall CAN_GetValue(
    const Ucal::Pcan::Handle channel, const Ucal::Pcan::Parameter parameter,
    void* const buffer, const std::uint32_t bufferBytes) {
    ChannelCatalog available;
    if (!catalog(available)) return Ucal::Pcan::driverNotLoaded;
    const auto count = static_cast<std::uint32_t>(available.channels.size());
    if (parameter == Ucal::Pcan::attachedChannelsCount) {
        return writeValue(buffer, bufferBytes, count);
    }
    if (parameter == Ucal::Pcan::attachedChannels) {
        if (buffer == nullptr || bufferBytes < count * sizeof(Ucal::Pcan::ChannelInformation)) {
            return Ucal::Pcan::invalidParameter;
        }
        auto* output = static_cast<Ucal::Pcan::ChannelInformation*>(buffer);
        std::lock_guard<std::mutex> lock(channelMutex);
        std::size_t attachedIndex = 0u;
        for (std::size_t index = 0u; index < available.endpoints.size(); ++index) {
            if (available.endpoints[index].state == Ucal::DeviceState::Offline) continue;
            auto& information = output[attachedIndex++];
            information = {};
            information.channelHandle = handleForOrdinal(static_cast<std::uint32_t>(index + 1u));
            information.deviceType = 0x05u;
            information.controllerNumber = static_cast<std::uint8_t>(index);
            information.deviceFeatures =
                (available.endpoints[index].capabilities & Ucal::CapabilityCanFdIso) != 0u
                    ? Ucal::Pcan::featureFdCapable : 0u;
            std::snprintf(information.deviceName, sizeof(information.deviceName),
                          "UCAL %.27s", available.endpoints[index].name.c_str());
            information.deviceId = stableHardwareId(available.endpoints[index].id);
            information.channelCondition =
                openChannels.find(information.channelHandle) != openChannels.end() ||
                available.endpoints[index].state == Ucal::DeviceState::Leased
                    ? Ucal::Pcan::channelOccupied : Ucal::Pcan::channelAvailable;
        }
        return Ucal::Pcan::success;
    }
    if (parameter == Ucal::Pcan::apiVersion) {
        return writeString(buffer, bufferBytes, "UCAL PCAN-Basic 1.0");
    }
    const auto ordinal = ordinalForHandle(channel);
    if (ordinal == 0u || ordinal > available.endpoints.size()) return Ucal::Pcan::invalidHardware;
    const auto& endpoint = available.endpoints[ordinal - 1u];
    if (parameter == Ucal::Pcan::channelCondition) {
        std::lock_guard<std::mutex> lock(channelMutex);
        const std::uint32_t condition = endpoint.state == Ucal::DeviceState::Offline
            ? Ucal::Pcan::channelUnavailable :
            openChannels.find(channel) != openChannels.end() ||
            endpoint.state == Ucal::DeviceState::Leased
                ? Ucal::Pcan::channelOccupied : Ucal::Pcan::channelAvailable;
        return writeValue(buffer, bufferBytes, condition);
    }
    if (parameter == Ucal::Pcan::channelFeatures) {
        const std::uint32_t features =
            (endpoint.capabilities & Ucal::CapabilityCanFdIso) != 0u
                ? Ucal::Pcan::featureFdCapable : 0u;
        return writeValue(buffer, bufferBytes, features);
    }
    if (parameter == Ucal::Pcan::hardwareName) {
        return writeString(buffer, bufferBytes, "UCAL " + endpoint.name);
    }
    if (parameter == Ucal::Pcan::deviceId) {
        const auto id = stableHardwareId(endpoint.id);
        return writeValue(buffer, bufferBytes, id);
    }
    std::lock_guard<std::mutex> lock(channelMutex);
    const auto found = openChannels.find(channel);
    if (found == openChannels.end()) return Ucal::Pcan::notInitialized;
    if (parameter == Ucal::Pcan::listenOnly) {
        const std::uint32_t enabled = found->second.config.listenOnly ? 1u : 0u;
        return writeValue(buffer, bufferBytes, enabled);
    }
    if (parameter == Ucal::Pcan::bitrateInfo) {
        return found->second.config.canFd ? Ucal::Pcan::invalidMode
                                          : writeValue(buffer, bufferBytes,
                                                        found->second.classicBitrate);
    }
    if (parameter == Ucal::Pcan::bitrateInfoFd) {
        return !found->second.config.canFd ? Ucal::Pcan::invalidMode
                                            : writeString(buffer, bufferBytes,
                                                           found->second.fdBitrate);
    }
    if (parameter == Ucal::Pcan::busSpeedNominal) {
        return writeValue(buffer, bufferBytes, found->second.config.arbitrationBitrate);
    }
    if (parameter == Ucal::Pcan::busSpeedData) {
        return writeValue(buffer, bufferBytes, found->second.config.dataBitrate);
    }
    return Ucal::Pcan::invalidParameterType;
}

__declspec(dllexport) Ucal::Pcan::Status __stdcall CAN_SetValue(
    const Ucal::Pcan::Handle channel, const Ucal::Pcan::Parameter parameter,
    void* const buffer, const std::uint32_t bufferBytes) {
    if (ordinalForHandle(channel) == 0u || buffer == nullptr) {
        return Ucal::Pcan::invalidParameter;
    }
    if (parameter == Ucal::Pcan::listenOnly) {
        if (bufferBytes < sizeof(std::uint32_t)) return Ucal::Pcan::invalidParameter;
        std::uint32_t value = 0u;
        std::memcpy(&value, buffer, sizeof(value));
        if (value > 1u) return Ucal::Pcan::invalidParameter;
        std::lock_guard<std::mutex> lock(channelMutex);
        if (openChannels.find(channel) != openChannels.end()) return Ucal::Pcan::invalidMode;
        pendingListenOnly[channel] = value != 0u;
        return Ucal::Pcan::success;
    }
    if (parameter == Ucal::Pcan::receiveEvent) {
        return Ucal::Pcan::illegalOperation;
    }
    return Ucal::Pcan::invalidParameterType;
}

__declspec(dllexport) Ucal::Pcan::Status __stdcall CAN_GetErrorText(
    const Ucal::Pcan::Status error, std::uint16_t, char* const buffer) {
    if (buffer == nullptr) return Ucal::Pcan::invalidParameter;
    const char* text = error == Ucal::Pcan::success ? "No error" :
        error == Ucal::Pcan::receiveEmpty ? "Receive queue is empty" :
        error == Ucal::Pcan::driverNotLoaded ? "UCAL Broker is unavailable" :
        error == Ucal::Pcan::invalidHardware ? "Invalid channel handle" :
        error == Ucal::Pcan::invalidParameter ? "Invalid parameter value" :
        error == Ucal::Pcan::invalidData ? "Invalid CAN frame" :
        error == Ucal::Pcan::invalidMode ? "Channel mode does not match the operation" :
        error == Ucal::Pcan::notInitialized ? "Channel is not initialized" :
        error == Ucal::Pcan::hardwareInUse ? "Channel is already in use" :
        error == Ucal::Pcan::illegalOperation ? "Operation is unsupported" :
        "UCAL PCAN frontend error";
    std::snprintf(buffer, 256u, "%s", text);
    return Ucal::Pcan::success;
}

__declspec(dllexport) Ucal::Pcan::Status __stdcall PCAN_GetAvailableHardware(
    const std::uint32_t deviceClass,
    Ucal::Pcan::AvailableHardware* const output,
    const std::uint32_t capacity,
    std::uint32_t* const returnedCount) {
    enumerationCalls.fetch_add(1u);
    if (returnedCount == nullptr || deviceClass > Ucal::Pcan::lastKnownDeviceClass ||
        (output != nullptr && capacity == 0u)) {
        audit("enumerate", deviceClass, capacity, 0u, Ucal::Pcan::invalidParameter);
        return Ucal::Pcan::invalidParameter;
    }
    *returnedCount = 0u;
    std::vector<Ucal::EndpointDescriptor> endpoints;
    if (Ucal::Client{}.enumerateEndpoints(endpoints) != Ucal::Status::Ok) {
        lastChannelCount.store(0u);
        audit("enumerate", deviceClass, capacity, 0u, Ucal::Pcan::driverNotLoaded);
        return Ucal::Pcan::driverNotLoaded;
    }
    const auto catalog = makeChannels(std::move(endpoints), deviceClass);
    if (catalog.identityCollision) {
        lastChannelCount.store(0u);
        audit("identity-collision", deviceClass, capacity, 0u,
              Ucal::Pcan::driverNotLoaded);
        return Ucal::Pcan::driverNotLoaded;
    }
    const auto& channels = catalog.channels;
    lastChannelCount.store(static_cast<std::uint32_t>(channels.size()));
    if (output == nullptr) {
        *returnedCount = static_cast<std::uint32_t>(channels.size());
        audit("enumerate", deviceClass, capacity, *returnedCount, Ucal::Pcan::success);
        return Ucal::Pcan::success;
    }
    const auto copyCount = (std::min)(capacity, static_cast<std::uint32_t>(channels.size()));
    std::copy_n(channels.begin(), copyCount, output);
    *returnedCount = copyCount;
    audit("enumerate", deviceClass, capacity, *returnedCount, Ucal::Pcan::success);
    return Ucal::Pcan::success;
}

__declspec(dllexport) Ucal::Pcan::Status __stdcall UcalPcanGetDiagnostics(
    Ucal::Pcan::Diagnostics* const diagnostics) {
    if (diagnostics == nullptr || diagnostics->size != sizeof(*diagnostics)) {
        return Ucal::Pcan::invalidParameter;
    }
    diagnostics->enumerationCalls = enumerationCalls.load();
    diagnostics->lastChannelCount = lastChannelCount.load();
    return Ucal::Pcan::success;
}

}  // extern "C"

BOOL WINAPI DllMain(HMODULE module, const DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(module);
    return TRUE;
}
