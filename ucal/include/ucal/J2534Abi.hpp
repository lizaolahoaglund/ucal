#pragma once

#include <cstddef>
#include <cstdint>

// Original minimal declarations of the public Windows J2534 04.04 C ABI and
// GMW17753 raw ISO CAN FD namespace. No vendor header is included or shipped.
namespace Ucal::J2534Backend::Abi {
using U32 = std::uint32_t;
using Result = std::int32_t;
inline constexpr U32 can = 5u;
inline constexpr U32 gmCanFd = 0x1000du;
inline constexpr U32 extended = 0x100u;
inline constexpr U32 bothIds = 0x800u;
inline constexpr U32 fdBrs = 0x1000000u;
inline constexpr U32 fdFormat = 0x2000000u;
inline constexpr U32 fdEsi = 0x4000000u;
inline constexpr U32 truncated = 0x8000000u;
inline constexpr U32 txEcho = 1u;
inline constexpr U32 txIndication = 8u;
inline constexpr U32 errorIndication = 0x10u;
inline constexpr U32 getConfig = 1u, setConfig = 2u;
inline constexpr U32 dataRate = 1u, loopback = 3u, samplePoint = 0x17u;
inline constexpr U32 pins = 0x8001u, gmDataRate = 0x10010u;
inline constexpr U32 gmTermination = 0x10012u, gmType = 0x10013u;
inline constexpr U32 hsTermination = 0x805eu;
inline constexpr Result ok = 0, notSupported = 1, disconnected = 8;
inline constexpr Result timeout = 9, empty = 0x10, full = 0x11, overflow = 0x12;

#pragma pack(push, 1)
struct Message {
    U32 protocol, rxStatus, txFlags, timestamp, dataSize, extraDataIndex;
    std::uint8_t data[4128];
};
struct Config {
    U32 parameter, value;
};
struct ConfigList {
    U32 count;
    Config *items;
};
#pragma pack(pop)
// Some 04.04 x64 drivers use the platform layout for the pointer list.
struct NativeConfigList {
    U32 count;
    Config *items;
};
static_assert(offsetof(NativeConfigList, items) == sizeof(void *), "native configuration pointer offset");
static_assert(sizeof(Message) == 4152, "04.04 message ABI");
static_assert(sizeof(Config) == 8, "04.04 config ABI");
static_assert(sizeof(ConfigList) == 4 + sizeof(void *), "04.04 packed pointer list ABI");

using Open = Result(__stdcall *)(void *, U32 *);
using Close = Result(__stdcall *)(U32);
using Connect = Result(__stdcall *)(U32, U32, U32, U32, U32 *);
using Disconnect = Result(__stdcall *)(U32);
using Messages = Result(__stdcall *)(U32, Message *, U32 *, U32);
using StartFilter = Result(__stdcall *)(U32, U32, Message *, Message *, Message *, U32 *);
using StopFilter = Result(__stdcall *)(U32, U32);
using ReadVersion = Result(__stdcall *)(U32, char *, char *, char *);
using LastError = Result(__stdcall *)(char *);
using Ioctl = Result(__stdcall *)(U32, U32, void *, void *);
} // namespace Ucal::j2534_backend::abi
