#pragma once
#include "ucal/Backend.hpp"
#include "ucal/Protocol.hpp"
#include <atomic>
#include <mutex>

namespace Ucal::Backend::Wire {
struct LegacyLease {
    DeviceId deviceId{};
    std::uint64_t token{};
    std::uint32_t ownerPid{};
};
// Shared request validation/translation for the Broker and original plugin SDK.
Status handleRequest(const Protocol::MessageHeader &request, const std::vector<std::byte> &input,
                      std::uint32_t ownerPid, std::vector<LegacyLease> &leases,
                      std::vector<std::byte> &output, BackendInterface *backend, std::mutex &mockMutex,
                      bool allowTestShutdown, std::atomic_bool &shutdown);
} // namespace Ucal::backend::wire
