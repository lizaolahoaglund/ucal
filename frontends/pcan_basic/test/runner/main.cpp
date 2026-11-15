#include "../../src/PcanAbi.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <iterator>
#include <string>
#include <unordered_set>
#include <vector>

namespace {
using GetAvailableHardware = Ucal::Pcan::Status (__stdcall*)(
    std::uint32_t, Ucal::Pcan::AvailableHardware*, std::uint32_t, std::uint32_t*);
using GetDiagnostics = Ucal::Pcan::Status (__stdcall*)(Ucal::Pcan::Diagnostics*);
using Initialize = Ucal::Pcan::Status (__stdcall*)(Ucal::Pcan::Handle,
    Ucal::Pcan::Baudrate, std::uint8_t, std::uint32_t, std::uint16_t);
using InitializeFd = Ucal::Pcan::Status (__stdcall*)(Ucal::Pcan::Handle, char*);
using Uninitialize = Ucal::Pcan::Status (__stdcall*)(Ucal::Pcan::Handle);
using Write = Ucal::Pcan::Status (__stdcall*)(Ucal::Pcan::Handle,
    Ucal::Pcan::ClassicMessage*);
using Read = Ucal::Pcan::Status (__stdcall*)(Ucal::Pcan::Handle,
    Ucal::Pcan::ClassicMessage*, Ucal::Pcan::ClassicTimestamp*);
using WriteFd = Ucal::Pcan::Status (__stdcall*)(Ucal::Pcan::Handle,
    Ucal::Pcan::FdMessage*);
using ReadFd = Ucal::Pcan::Status (__stdcall*)(Ucal::Pcan::Handle,
    Ucal::Pcan::FdMessage*, std::uint64_t*);
using GetValue = Ucal::Pcan::Status (__stdcall*)(Ucal::Pcan::Handle,
    Ucal::Pcan::Parameter, void*, std::uint32_t);
using GetStatus = Ucal::Pcan::Status (__stdcall*)(Ucal::Pcan::Handle);

Ucal::Pcan::Handle handleForOrdinal(const std::size_t ordinal) {
    if (ordinal < 1u || ordinal > Ucal::Pcan::maximumChannels) return 0u;
    return static_cast<Ucal::Pcan::Handle>(ordinal <= 8u ? 0x50u + ordinal
                                                         : 0x500u + ordinal);
}

int validateTopology(HMODULE module) {
    const auto get = reinterpret_cast<GetValue>(GetProcAddress(module, "CAN_GetValue"));
    const auto init = reinterpret_cast<Initialize>(GetProcAddress(module, "CAN_Initialize"));
    const auto close = reinterpret_cast<Uninitialize>(GetProcAddress(module, "CAN_Uninitialize"));
    const auto status = reinterpret_cast<GetStatus>(GetProcAddress(module, "CAN_GetStatus"));
    wchar_t path[32768]{};
    if (!get || !init || !close || !status || !GetEnvironmentVariableW(
        L"UCAL_PCAN_TEST_VISIBILITY_FILE", path, 32768)) return 40;
    auto visibility = [&](const char* value) {
        const auto file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        DWORD written{};
        const DWORD size = static_cast<DWORD>(std::strlen(value));
        const bool ok = WriteFile(file, value, size, &written, nullptr) && written == size;
        CloseHandle(file);
        return ok;
    };
    struct Cleanup { Uninitialize close; ~Cleanup() { close(0); } } cleanup{close};
    Ucal::Pcan::ChannelInformation original[2]{};
    if (get(0, Ucal::Pcan::attachedChannels, original, sizeof(original)) ||
        original[0].channelHandle != 0x51u || original[1].channelHandle != 0x52u) return 41;
    if (init(0x52u, 0x001cu, 0, 0, 0) || !visibility("second")) return 42;
    std::uint32_t count{};
    Ucal::Pcan::ChannelInformation remaining{};
    if (get(0, Ucal::Pcan::attachedChannelsCount, &count, sizeof(count)) || count != 1u ||
        get(0, Ucal::Pcan::attachedChannels, &remaining, sizeof(remaining)) ||
        remaining.channelHandle != 0x52u || remaining.deviceId != original[1].deviceId) return 43;
    std::uint32_t id{}, condition{};
    if (status(0x52u) || get(0x52u, Ucal::Pcan::deviceId, &id, sizeof(id)) ||
        id != original[1].deviceId || get(0x51u, Ucal::Pcan::channelCondition, &condition, sizeof(condition)) ||
        condition != Ucal::Pcan::channelUnavailable ||
        init(0x51u, 0x001cu, 0, 0, 0) != Ucal::Pcan::invalidHardware) return 44;
    if (!visibility("both") || get(0, Ucal::Pcan::attachedChannels, original, sizeof(original)) ||
        original[0].channelHandle != 0x51u || original[1].channelHandle != 0x52u ||
        original[1].deviceId != id) return 45;
    std::cout << "pcanStableHandles=true activeIdentity=true offlineTombstone=true\n";
    return 0;
}

int validateAdvancedTiming(HMODULE module, bool nominalOnly) {
    const auto init = reinterpret_cast<InitializeFd>(GetProcAddress(module, "CAN_InitializeFD"));
    const auto close = reinterpret_cast<Uninitialize>(GetProcAddress(module, "CAN_Uninitialize"));
    if (!init || !close) return 50;
    for (const auto sjw : {0u, 999u}) {
        auto invalid = std::string("f_clock=80000000,nom_brp=1,nom_tseg1=139,nom_tseg2=20,nom_sjw=") +
            std::to_string(sjw) + ",data_brp=1,data_tseg1=29,data_tseg2=10,data_sjw=4";
        if (init(0x51u, invalid.data()) != Ucal::Pcan::invalidParameter) return 51;
    }
    char precise[] = "f_clock=80000000,nom_brp=1,nom_tseg1=139,nom_tseg2=20,nom_sjw=8,data_brp=1,data_tseg1=29,data_tseg2=10,data_sjw=4";
    char nominal[] = "f_clock=80000000,nom_brp=1,nom_tseg1=139,nom_tseg2=20,nom_sjw=8";
    if (init(0x51u, nominalOnly ? nominal : precise)) return 52;
    if (close(0x51u)) return 53;
    std::cout << "advancedTiming=true exactNominalSample=87.5 sjw=8 nominalOnly=" << nominalOnly
              << " invalidSjwRejected=true\n";
    return 0;
}
}

int wmain(const int argc, wchar_t** argv) {
    if (argc != 2 && argc != 3) {
        std::cerr << "usage: UcalPcanHarness <PcanApi DLL>"
                     " [forwarding-proxy|data-plane|peer-data-plane]\n";
        return 2;
    }
    const auto module = LoadLibraryW(argv[1]);
    if (module == nullptr) return 3;
    if (argc == 3 && (std::wstring(argv[2]) == L"topology" || std::wstring(argv[2]) == L"advanced-timing" ||
                     std::wstring(argv[2]) == L"nominal-only")) {
        const auto result = std::wstring(argv[2]) == L"topology" ? validateTopology(module) :
            validateAdvancedTiming(module, std::wstring(argv[2]) == L"nominal-only");
        FreeLibrary(module);
        return result;
    }
    const auto enumerate = reinterpret_cast<GetAvailableHardware>(
        GetProcAddress(module, "PCAN_GetAvailableHardware"));
    const auto diagnostics = reinterpret_cast<GetDiagnostics>(
        GetProcAddress(module, "UcalPcanGetDiagnostics"));
    if (enumerate == nullptr || (argc == 2 && diagnostics == nullptr)) return 4;

    std::uint32_t count = 0;
    if (enumerate(Ucal::Pcan::deviceAll, nullptr, 0, &count) != Ucal::Pcan::success || count == 0) {
        return 5;
    }
    std::vector<Ucal::Pcan::AvailableHardware> channels(count);
    std::uint32_t returned = 0;
    if (enumerate(Ucal::Pcan::deviceAll, channels.data(), count, &returned) != Ucal::Pcan::success ||
        returned != count) {
        return 6;
    }
    std::cout << "channels=" << returned << '\n';
    for (const auto& channel : channels) {
        std::cout << channel.name << " device=" << channel.deviceClass
                  << " hardware=0x" << std::hex << channel.hardwareId << std::dec
                  << " channel=" << channel.channel << '\n';
    }
    std::unordered_set<std::uint32_t> identities;
    for (const auto& channel : channels) {
        if (channel.hardwareId == 0u || !identities.insert(channel.hardwareId).second ||
            channel.channel == 0u) return 9;
    }
    std::vector<Ucal::Pcan::AvailableHardware> repeated(count);
    std::uint32_t repeatedCount = 0;
    if (enumerate(Ucal::Pcan::deviceAll, repeated.data(), count, &repeatedCount) !=
            Ucal::Pcan::success || repeatedCount != count) return 10;
    for (std::size_t index = 0; index < channels.size(); ++index) {
        if (channels[index].hardwareId != repeated[index].hardwareId ||
            channels[index].channel != repeated[index].channel) return 11;
    }
    std::uint32_t invalidCount = 0;
    if (enumerate(Ucal::Pcan::lastKnownDeviceClass + 1u, nullptr, 0, &invalidCount) !=
        Ucal::Pcan::invalidParameter) {
        return 7;
    }
    if (diagnostics != nullptr) {
        Ucal::Pcan::Diagnostics snapshot{sizeof(snapshot)};
        if (diagnostics(&snapshot) != Ucal::Pcan::success || snapshot.enumerationCalls != 4u ||
            snapshot.lastChannelCount != count) {
            return 8;
        }
    }
    if (argc == 3 && (std::wstring(argv[2]) == L"data-plane" ||
                      std::wstring(argv[2]) == L"peer-data-plane")) {
        const bool peerMode = std::wstring(argv[2]) == L"peer-data-plane";
        const auto initialize = reinterpret_cast<Initialize>(
            GetProcAddress(module, "CAN_Initialize"));
        const auto initializeFd = reinterpret_cast<InitializeFd>(
            GetProcAddress(module, "CAN_InitializeFD"));
        const auto uninitialize = reinterpret_cast<Uninitialize>(
            GetProcAddress(module, "CAN_Uninitialize"));
        const auto write = reinterpret_cast<Write>(GetProcAddress(module, "CAN_Write"));
        const auto read = reinterpret_cast<Read>(GetProcAddress(module, "CAN_Read"));
        const auto writeFd = reinterpret_cast<WriteFd>(GetProcAddress(module, "CAN_WriteFD"));
        const auto readFd = reinterpret_cast<ReadFd>(GetProcAddress(module, "CAN_ReadFD"));
        const auto getValue = reinterpret_cast<GetValue>(GetProcAddress(module, "CAN_GetValue"));
        const auto getStatus = reinterpret_cast<GetStatus>(GetProcAddress(module, "CAN_GetStatus"));
        if (initialize == nullptr || initializeFd == nullptr || uninitialize == nullptr ||
            write == nullptr || read == nullptr || writeFd == nullptr || readFd == nullptr ||
            getValue == nullptr || getStatus == nullptr) return 12;
        std::uint32_t attached = 0u;
        if (getValue(Ucal::Pcan::noneBus, Ucal::Pcan::attachedChannelsCount,
                      &attached, sizeof(attached)) != Ucal::Pcan::success || attached == 0u) {
            return 13;
        }
        auto handle = Ucal::Pcan::usbBus1;
        auto receiveHandle = handle;
        if (peerMode) {
            bool foundPair = false;
            for (std::size_t left = 0u; left < channels.size() && !foundPair; ++left) {
                for (std::size_t right = left + 1u; right < channels.size(); ++right) {
                    if (channels[left].channel == channels[right].channel &&
                        channels[left].hardwareId != channels[right].hardwareId &&
                        std::string(channels[left].name).find("Virtual") == std::string::npos &&
                        std::string(channels[right].name).find("Virtual") == std::string::npos) {
                        handle = handleForOrdinal(left + 1u);
                        receiveHandle = handleForOrdinal(right + 1u);
                        foundPair = handle != 0u && receiveHandle != 0u;
                        break;
                    }
                }
            }
            if (!foundPair) {
                for (std::size_t left = 0u; left < channels.size() && !foundPair; ++left) {
                    for (std::size_t right = left + 1u; right < channels.size(); ++right) {
                        if (channels[left].hardwareId != channels[right].hardwareId &&
                            std::string(channels[left].name).find("Virtual") == std::string::npos &&
                            std::string(channels[right].name).find("Virtual") == std::string::npos) {
                            handle = handleForOrdinal(left + 1u);
                            receiveHandle = handleForOrdinal(right + 1u);
                            foundPair = handle != 0u && receiveHandle != 0u;
                            break;
                        }
                    }
                }
            }
            if (!foundPair) return 20;
        }
        if (initialize(handle, 0x001cu, 0u, 0u, 0u) != Ucal::Pcan::success) return 14;
        if (peerMode && initialize(receiveHandle, 0x001cu, 0u, 0u, 0u) !=
                             Ucal::Pcan::success) return 21;
        Ucal::Pcan::ClassicMessage classic{};
        classic.id = 0x321u;
        classic.length = 8u;
        for (std::uint8_t index = 0u; index < classic.length; ++index) {
            classic.data[index] = static_cast<std::uint8_t>(0x40u + index);
        }
        if (write(handle, &classic) != Ucal::Pcan::success) return 15;
        Ucal::Pcan::ClassicMessage received{};
        Ucal::Pcan::ClassicTimestamp classicTimestamp{};
        Ucal::Pcan::Status readStatus = Ucal::Pcan::receiveEmpty;
        for (unsigned attempt = 0u; attempt < 100u && readStatus == Ucal::Pcan::receiveEmpty;
             ++attempt) {
            Sleep(5u);
            readStatus = read(receiveHandle, &received, &classicTimestamp);
        }
        if (readStatus != Ucal::Pcan::success || received.id != classic.id ||
            received.length != classic.length ||
            !std::equal(std::begin(classic.data), std::end(classic.data),
                        std::begin(received.data)) ||
            getStatus(handle) != Ucal::Pcan::success ||
            getStatus(receiveHandle) != Ucal::Pcan::success ||
            uninitialize(handle) != Ucal::Pcan::success ||
            (peerMode && uninitialize(receiveHandle) != Ucal::Pcan::success)) return 16;

        char fdBitrate[] =
            "f_clock=80000000,nom_brp=1,nom_tseg1=119,nom_tseg2=40,nom_sjw=1,"
            "data_brp=1,data_tseg1=29,data_tseg2=10,data_sjw=1";
        const auto fdInitializeStatus = initializeFd(handle, fdBitrate);
        if (fdInitializeStatus != Ucal::Pcan::success) {
            std::cerr << "CAN_InitializeFD failed: 0x" << std::hex
                      << fdInitializeStatus << std::dec << '\n';
            return 17;
        }
        if (peerMode && initializeFd(receiveHandle, fdBitrate) != Ucal::Pcan::success) {
            return 22;
        }
        Ucal::Pcan::FdMessage fd{};
        fd.id = 0x18ff50e5u;
        fd.type = Ucal::Pcan::messageExtended | Ucal::Pcan::messageFd |
                  Ucal::Pcan::messageBrs;
        fd.dlc = 15u;
        for (std::uint8_t index = 0u; index < 64u; ++index) fd.data[index] = index;
        if (writeFd(handle, &fd) != Ucal::Pcan::success) return 18;
        Ucal::Pcan::FdMessage receivedFd{};
        std::uint64_t fdTimestamp = 0u;
        readStatus = Ucal::Pcan::receiveEmpty;
        for (unsigned attempt = 0u; attempt < 100u && readStatus == Ucal::Pcan::receiveEmpty;
             ++attempt) {
            Sleep(5u);
            readStatus = readFd(receiveHandle, &receivedFd, &fdTimestamp);
        }
        if (readStatus != Ucal::Pcan::success || receivedFd.id != fd.id ||
            receivedFd.type != fd.type || receivedFd.dlc != fd.dlc ||
            !std::equal(std::begin(fd.data), std::end(fd.data),
                        std::begin(receivedFd.data)) ||
            uninitialize(handle) != Ucal::Pcan::success ||
            (peerMode && uninitialize(receiveHandle) != Ucal::Pcan::success)) return 19;
        std::cout << "pcanBasicDataPlane=true classic=true fd=true brs=true peer="
                  << (peerMode ? "true" : "false") << '\n';
    }
    FreeLibrary(module);
    return 0;
}
