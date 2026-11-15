#include "../../src/PcanAbi.hpp"
#include <windows.h>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <vector>

extern "C" {
__declspec(dllimport) Ucal::Pcan::Status __stdcall CAN_GetValue(Ucal::Pcan::Handle, Ucal::Pcan::Parameter, void*, std::uint32_t);
__declspec(dllimport) Ucal::Pcan::Status __stdcall CAN_Initialize(Ucal::Pcan::Handle, Ucal::Pcan::Baudrate, std::uint8_t, std::uint32_t, std::uint16_t);
__declspec(dllimport) Ucal::Pcan::Status __stdcall CAN_InitializeFD(Ucal::Pcan::Handle, char*);
__declspec(dllimport) Ucal::Pcan::Status __stdcall CAN_Uninitialize(Ucal::Pcan::Handle);
__declspec(dllimport) Ucal::Pcan::Status __stdcall CAN_Write(Ucal::Pcan::Handle, Ucal::Pcan::ClassicMessage*);
__declspec(dllimport) Ucal::Pcan::Status __stdcall CAN_Read(Ucal::Pcan::Handle, Ucal::Pcan::ClassicMessage*, Ucal::Pcan::ClassicTimestamp*);
__declspec(dllimport) Ucal::Pcan::Status __stdcall CAN_WriteFD(Ucal::Pcan::Handle, Ucal::Pcan::FdMessage*);
__declspec(dllimport) Ucal::Pcan::Status __stdcall CAN_ReadFD(Ucal::Pcan::Handle, Ucal::Pcan::FdMessage*, std::uint64_t*);
}

int main(int argc, char** argv) {
    const bool expectFdUnsupported = argc == 2 && std::strcmp(argv[1], "--expect-fd-unsupported") == 0;
    std::uint32_t count{};
    if (CAN_GetValue(0, Ucal::Pcan::attachedChannelsCount, &count, sizeof(count)) != 0 || count == 0 || count > 16) return 1;
    std::vector<Ucal::Pcan::ChannelInformation> channels(count);
    if (CAN_GetValue(0, Ucal::Pcan::attachedChannels, channels.data(), count * sizeof(channels[0])) != 0) return 2;
    const auto found = std::find_if(channels.begin(), channels.end(), [](const auto& channel) {
        return (channel.deviceFeatures & Ucal::Pcan::featureFdCapable) != 0;
    });
    if (found == channels.end()) return 3;
    const auto handle = found->channelHandle;
    if (CAN_Initialize(handle, 0x001c, 0, 0, 0) != 0) return 4;
    Ucal::Pcan::ClassicMessage sent{};
    sent.id = 0x321; sent.length = 8;
    for (unsigned i = 0; i < 8; ++i) sent.data[i] = static_cast<std::uint8_t>(i + 0x50);
    if (CAN_Write(handle, &sent) != 0) return 5;
    Ucal::Pcan::ClassicMessage received{};
    Ucal::Pcan::ClassicTimestamp timestamp{};
    bool matched{};
    for (unsigned i = 0; i < 200 && !matched; ++i) {
        const auto status = CAN_Read(handle, &received, &timestamp);
        if (status != 0 && status != Ucal::Pcan::receiveEmpty) return 6;
        matched = status == 0 && received.id == sent.id && received.length == 8 &&
            std::equal(sent.data, sent.data + 8, received.data);
        if (!matched) Sleep(5);
    }
    if (!matched || CAN_Uninitialize(handle) != 0) return 7;
    char timing[] = "f_clock=80000000,nom_brp=1,nom_tseg1=119,nom_tseg2=40,nom_sjw=1,data_brp=1,data_tseg1=29,data_tseg2=10,data_sjw=1";
    const auto fdStatus = CAN_InitializeFD(handle, timing);
    if (expectFdUnsupported) {
        if (fdStatus != Ucal::Pcan::illegalOperation) return 12;
        wchar_t modulePath[32768]{};
        GetModuleFileNameW(GetModuleHandleW(L"PCANBasic.dll"), modulePath, 32768);
        std::wcout << L"module=" << modulePath << L'\n';
        std::wcout << L"api=pcan-basic classic=true fdSamplingRejected=true staticImport=true\n";
        return 0;
    }
    if (fdStatus != 0) return 8;
    Ucal::Pcan::FdMessage sentFd{};
    sentFd.id = 0x18ff4321; sentFd.type = 0x0e; sentFd.dlc = 15;
    for (unsigned i = 0; i < 64; ++i) sentFd.data[i] = static_cast<std::uint8_t>(i ^ 0xa5);
    if (CAN_WriteFD(handle, &sentFd) != 0) return 9;
    Ucal::Pcan::FdMessage receivedFd{};
    std::uint64_t stamp{};
    matched = false;
    for (unsigned i = 0; i < 200 && !matched; ++i) {
        const auto status = CAN_ReadFD(handle, &receivedFd, &stamp);
        if (status != 0 && status != Ucal::Pcan::receiveEmpty) return 10;
        matched = status == 0 && receivedFd.id == sentFd.id && receivedFd.type == sentFd.type && receivedFd.dlc == 15 &&
            std::equal(sentFd.data, sentFd.data + 64, receivedFd.data);
        if (!matched) Sleep(5);
    }
    if (!matched || CAN_Uninitialize(handle) != 0) return 11;
    wchar_t modulePath[32768]{};
    GetModuleFileNameW(GetModuleHandleW(L"PCANBasic.dll"), modulePath, 32768);
    std::wcout << L"module=" << modulePath << L'\n';
    std::wcout << L"api=pcan-basic classic=true fd=true brs=true staticImport=true channels=" << count << L'\n';
    return 0;
}
