#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace Ucal::Timing {

// Neutral endpoints do not promise a common hardware clock, even when they
// share an adapter. Translate each clock into measurement time independently.
// After the first observation, preserve hardware intervals rather than host
// arrival jitter. This translation does not claim hardware synchronization.
class ChannelClockMapper {
    struct Origin {
        bool valid{};
        std::uint64_t hardwareUs{};
        std::uint64_t measurementUs{};
    };
    std::array<Origin, 64> origins_{};

public:
    void reset() noexcept { origins_ = {}; }
    bool hasOrigin(const std::size_t channel) const noexcept {
        return channel < origins_.size() && origins_[channel].valid;
    }
    std::uint64_t translate(const std::size_t channel, const std::uint64_t hardwareUs,
                            const std::uint64_t observedMeasurementUs) noexcept {
        if (!hardwareUs || channel >= origins_.size()) return 0;
        auto& origin = origins_[channel];
        if (!origin.valid || hardwareUs < origin.hardwareUs) {
            origin = {true, hardwareUs, observedMeasurementUs};
        }
        const auto delta = hardwareUs - origin.hardwareUs;
        if (delta > (std::numeric_limits<std::uint64_t>::max)() - origin.measurementUs) return 0;
        return origin.measurementUs + delta;
    }
};

}  // namespace Ucal::timing
