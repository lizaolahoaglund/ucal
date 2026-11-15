#include "../../src/XlAbi.hpp"
#include <windows.h>
#include <algorithm>
#include <cstring>
#include <iostream>

#pragma pack(push, 1)
struct ClassicMessage {
    std::uint32_t id;
    std::uint16_t flags, dlc;
    std::uint64_t reserved1;
    std::uint8_t data[8];
    std::uint64_t reserved2;
};
struct ClassicEvent {
    std::uint8_t tag, channelIndex;
    std::uint16_t transactionId, portHandle;
    std::uint8_t flags, reserved;
    std::uint64_t timestamp;
    ClassicMessage message;
};
struct FdMessage {
    std::uint32_t id, flags;
    std::uint8_t dlc, reserved[7], data[64];
};
struct TxEvent {
    std::uint16_t tag, transactionId;
    std::uint8_t channelIndex, reserved[3];
    FdMessage message;
};
struct RxEvent {
    std::uint32_t size;
    std::uint16_t tag, channelIndex;
    std::uint32_t userHandle;
    std::uint16_t chipFlags, reserved0;
    std::uint64_t reserved1, timestamp;
    std::uint8_t data[96];
};
#pragma pack(pop)
static_assert(sizeof(ClassicEvent) == 48 && sizeof(TxEvent) == 88 && sizeof(RxEvent) == 128);

extern "C" {
__declspec(dllimport) Ucal::Xl::Status __stdcall xlOpenDriver();
__declspec(dllimport) Ucal::Xl::Status __stdcall xlCloseDriver();
__declspec(dllimport) Ucal::Xl::Status __stdcall xlGetDriverConfig(Ucal::Xl::DriverConfig*);
__declspec(dllimport) Ucal::Xl::Status __stdcall xlOpenPort(Ucal::Xl::PortHandle*, char*, Ucal::Xl::Access, Ucal::Xl::Access*, unsigned int, unsigned int, unsigned int);
__declspec(dllimport) Ucal::Xl::Status __stdcall xlClosePort(Ucal::Xl::PortHandle);
__declspec(dllimport) Ucal::Xl::Status __stdcall xlCanSetChannelBitrate(Ucal::Xl::PortHandle, Ucal::Xl::Access, unsigned int);
__declspec(dllimport) Ucal::Xl::Status __stdcall xlCanFdSetConfiguration(Ucal::Xl::PortHandle, Ucal::Xl::Access, Ucal::Xl::CanFdConfig*);
__declspec(dllimport) Ucal::Xl::Status __stdcall xlActivateChannel(Ucal::Xl::PortHandle, Ucal::Xl::Access, unsigned int, unsigned int);
__declspec(dllimport) Ucal::Xl::Status __stdcall xlDeactivateChannel(Ucal::Xl::PortHandle, Ucal::Xl::Access);
__declspec(dllimport) Ucal::Xl::Status __stdcall xlCanTransmit(Ucal::Xl::PortHandle, Ucal::Xl::Access, unsigned int*, ClassicEvent*);
__declspec(dllimport) Ucal::Xl::Status __stdcall xlReceive(Ucal::Xl::PortHandle, unsigned int*, ClassicEvent*);
__declspec(dllimport) Ucal::Xl::Status __stdcall xlCanTransmitEx(Ucal::Xl::PortHandle, Ucal::Xl::Access, unsigned int, unsigned int*, TxEvent*);
__declspec(dllimport) Ucal::Xl::Status __stdcall xlCanReceive(Ucal::Xl::PortHandle, RxEvent*);
}

int main(int argc, char** argv) {
    const bool ratesOnly = argc == 2 && std::strcmp(argv[1], "--rates-only") == 0;
    if (xlOpenDriver() != 0) return 1;
    Ucal::Xl::DriverConfig config{};
    if (xlGetDriverConfig(&config) != 0 || config.channelCount > 64) return 2;
    Ucal::Xl::Access mask{};
    for (unsigned i = 0; i < config.channelCount; ++i) {
        if ((config.channel[i].channelCapabilities & Ucal::Xl::channelCanfdIso) != 0) { mask = config.channel[i].channelMask; break; }
    }
    if (!mask) return 3;
    Ucal::Xl::Access permission = mask;
    Ucal::Xl::PortHandle port = -1;
    char name[] = "Original static XL API client";
    if (xlOpenPort(&port, name, mask, &permission, 256, 3, 1) != 0 || permission != mask) return 4;
    if (xlCanSetChannelBitrate(port, mask, 500000) != 0 || xlActivateChannel(port, mask, 1, 0) != 0) return 5;
    ClassicEvent sent{};
    sent.tag = 10; sent.message.id = 0x321; sent.message.dlc = 8;
    for (unsigned i = 0; i < 8; ++i) sent.message.data[i] = static_cast<std::uint8_t>(i + 0x50);
    unsigned int count = 1;
    if (xlCanTransmit(port, mask, &count, &sent) != 0 || count != 1) return 6;
    bool matched{};
    for (unsigned i = 0; i < 200 && !matched; ++i) {
        ClassicEvent received{}; count = 1;
        const auto status = xlReceive(port, &count, &received);
        if (status != 0 && status != Ucal::Xl::queueEmpty) return 7;
        matched = status == 0 && count == 1 && received.tag == 1 && received.message.id == sent.message.id &&
            received.message.dlc == 8 && std::equal(sent.message.data, sent.message.data + 8, received.message.data);
        if (!matched) Sleep(5);
    }
    if (!matched || xlDeactivateChannel(port, mask) != 0 || xlClosePort(port) != 0) return 8;
    permission = mask;
    if (xlOpenPort(&port, name, mask, &permission, 256, 4, 1) != 0) return 9;
    Ucal::Xl::CanFdConfig timing{500000, 1, 119, 40, 2000000, 1, 29, 10, {0,0}};
    if (ratesOnly) {
        timing.arbitrationSjw = timing.arbitrationTseg1 = timing.arbitrationTseg2 = 0;
        timing.dataSjw = timing.dataTseg1 = timing.dataTseg2 = 0;
    }
    if (xlCanFdSetConfiguration(port, mask, &timing) != 0 || xlActivateChannel(port, mask, 1, 0) != 0) return 10;
    TxEvent sentFd{};
    sentFd.tag = 0x440; sentFd.message.id = 0x98ff4321; sentFd.message.flags = 3; sentFd.message.dlc = 15;
    for (unsigned i = 0; i < 64; ++i) sentFd.message.data[i] = static_cast<std::uint8_t>(i ^ 0xa5);
    count = 0;
    if (xlCanTransmitEx(port, mask, 1, &count, &sentFd) != 0 || count != 1) return 11;
    matched = false;
    for (unsigned i = 0; i < 200 && !matched; ++i) {
        RxEvent received{};
        const auto status = xlCanReceive(port, &received);
        if (status != 0 && status != Ucal::Xl::queueEmpty) return 12;
        if (status == 0 && received.tag == 0x400) {
            std::uint32_t id{}; std::memcpy(&id, received.data, sizeof(id));
            matched = id == sentFd.message.id && received.data[26] == 15 &&
                std::equal(sentFd.message.data, sentFd.message.data + 64, received.data + 32);
        }
        if (!matched) Sleep(5);
    }
    if (!matched || xlDeactivateChannel(port, mask) != 0 || xlClosePort(port) != 0 || xlCloseDriver() != 0) return 13;
    wchar_t modulePath[32768]{};
#if defined(_WIN64)
    GetModuleFileNameW(GetModuleHandleW(L"vxlapi64.dll"), modulePath, 32768);
#else
    GetModuleFileNameW(GetModuleHandleW(L"vxlapi.dll"), modulePath, 32768);
#endif
    std::wcout << L"module=" << modulePath << L'\n';
    std::wcout << L"api=vxlapi classic=true fd=true brs=true staticImport=true channels=" << config.channelCount << L'\n';
    return 0;
}
