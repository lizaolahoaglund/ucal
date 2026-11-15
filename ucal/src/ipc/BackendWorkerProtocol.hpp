#pragma once

#include "ucal/Protocol.hpp"

#include <cstdint>

namespace Ucal::Backend::Worker {

inline constexpr std::uint32_t magic = 0x574D4255u;  // "UBMW" on little-endian hosts.
inline constexpr std::uint16_t version = 1u;

#pragma pack(push, 1)
struct RequestHeader {
    std::uint32_t magicValue;
    std::uint16_t versionValue;
    std::uint16_t reserved;
    std::uint32_t ownerPid;
    Protocol::MessageHeader request;
};
#pragma pack(pop)

static_assert(sizeof(RequestHeader) == 40, "Backend worker header ABI changed");

inline bool validRequest(const RequestHeader& header) {
    return header.magicValue == magic && header.versionValue == version &&
           header.reserved == 0u && header.ownerPid != 0u &&
           Protocol::validRequestHeader(header.request);
}

}  // namespace Ucal::backend::worker
