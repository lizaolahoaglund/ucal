#pragma once
#include "ucal/Model.hpp"

namespace Ucal {
inline bool validCanBitTiming(const CanBitTiming& timing, const std::uint32_t bitrate) {
    if (!bitrate || !timing.tseg1 || timing.tseg1 > 65535u || !timing.tseg2 ||
        timing.tseg2 > 65535u || !timing.sjw || timing.sjw > timing.tseg2 ||
        (timing.sampleCount != 1u && timing.sampleCount != 3u) ||
        (timing.clockHz == 0u) != (timing.prescaler == 0u)) return false;
    if (!timing.clockHz) return true;
    const auto divisor = std::uint64_t(timing.prescaler) * (1u + timing.tseg1 + timing.tseg2);
    return timing.clockHz % divisor == 0u && timing.clockHz / divisor == bitrate;
}

inline bool validCanAdvancedTiming(const CanChannelConfig& config) {
    return (!config.nominalTimingSpecified ||
               (validCanBitTiming(config.nominalTiming, config.arbitrationBitrate) &&
                (!config.canFd || config.nominalTiming.sampleCount == 1u))) &&
           (!config.dataTimingSpecified || (config.canFd &&
               validCanBitTiming(config.dataTiming, config.dataBitrate) &&
               config.dataTiming.sampleCount == 1u));
}

// Convert to a backend clock without rounding any requested physical interval.
inline bool resolveCanBitTiming(const CanBitTiming& requested, std::uint32_t bitrate,
    std::uint32_t clockHz, std::uint32_t maxPrescaler, std::uint32_t maxTseg1,
    std::uint32_t maxTseg2, std::uint32_t maxSjw, CanBitTiming& resolved) {
    if (!validCanBitTiming(requested, bitrate)) return false;
    if (requested.clockHz == clockHz && requested.prescaler <= maxPrescaler &&
        requested.tseg1 <= maxTseg1 && requested.tseg2 <= maxTseg2 && requested.sjw <= maxSjw) {
        resolved = requested;
        return true;
    }
    const std::uint64_t originalQuanta = 1u + requested.tseg1 + requested.tseg2;
    for (std::uint32_t prescaler = 1u; prescaler <= maxPrescaler; ++prescaler) {
        const auto divisor = std::uint64_t(bitrate) * prescaler;
        if (clockHz % divisor) continue;
        const std::uint64_t quanta = clockHz / divisor;
        if (quanta < 3u || quanta > 1u + maxTseg1 + maxTseg2 ||
            quanta * requested.tseg2 % originalQuanta ||
            quanta * requested.sjw % originalQuanta) continue;
        const auto tseg2 = quanta * requested.tseg2 / originalQuanta;
        const auto sjw = quanta * requested.sjw / originalQuanta;
        const auto tseg1 = quanta - 1u - tseg2;
        if (!tseg1 || tseg1 > maxTseg1 || !tseg2 || tseg2 > maxTseg2 || !sjw || sjw > maxSjw) continue;
        resolved = {clockHz, prescaler, static_cast<std::uint32_t>(tseg1),
            static_cast<std::uint32_t>(tseg2), static_cast<std::uint32_t>(sjw), requested.sampleCount};
        return true;
    }
    return false;
}
} // namespace Ucal
