#pragma once

#include <array>
#include <cstddef>

namespace synth::core {

struct ActorDiscoverySlice final {
    std::size_t begin{}, count{};
};

// Fair, bounded windows over four mutable process tiers. Only numeric cursors survive
// a capture; a multi-capture traversal never attests one complete world observation.
class ActorDiscoveryScan final {
public:
    static constexpr std::size_t budget = 256;
    [[nodiscard]] std::array<ActorDiscoverySlice, 4> next(const std::array<std::size_t, 4>& sizes) noexcept {
        std::array<ActorDiscoverySlice, 4> slices{};
        for (std::size_t tier = 0; tier < 4; ++tier)
            slices[tier].begin = sizes[tier] ? offsets_[tier] % sizes[tier] : 0;
        for (std::size_t read = 0; read < budget; ++read) {
            bool found{};
            for (std::size_t attempt = 0; attempt < 4; ++attempt) {
                const auto tier = next_tier_;
                next_tier_ = (next_tier_ + 1) % 4;
                if (slices[tier].count == sizes[tier]) continue;
                ++slices[tier].count;
                found = true;
                break;
            }
            if (!found) break;
        }
        for (std::size_t tier = 0; tier < 4; ++tier) {
            const auto [begin, count] = slices[tier];
            offsets_[tier] = count >= sizes[tier] - begin ? count - (sizes[tier] - begin) : begin + count;
        }
        return slices;
    }
private:
    std::array<std::size_t, 4> offsets_{};
    std::size_t next_tier_{};
};

} // namespace synth::core
