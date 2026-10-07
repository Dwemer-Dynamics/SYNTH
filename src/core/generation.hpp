#pragma once

#include <atomic>
#include <compare>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace synth::core {

class RuntimeGeneration final {
public:
    using value_type = std::uint64_t;
    static constexpr value_type maximum_wire_value = 9'007'199'254'740'991ULL;

    constexpr RuntimeGeneration() noexcept = default;

    [[nodiscard]] static constexpr RuntimeGeneration initial() noexcept {
        return RuntimeGeneration{1};
    }

    [[nodiscard]] static constexpr RuntimeGeneration from_value(value_type value) {
        if (value == 0 || value > maximum_wire_value) {
            throw std::invalid_argument{"runtime generation is outside the wire-safe range"};
        }
        return RuntimeGeneration{value};
    }

    [[nodiscard]] constexpr value_type value() const noexcept { return value_; }
    [[nodiscard]] constexpr bool valid() const noexcept { return value_ != 0; }

    auto operator<=>(const RuntimeGeneration&) const = default;

private:
    explicit constexpr RuntimeGeneration(value_type value) noexcept : value_{value} {}

    value_type value_{0};
};

class GenerationClock final {
public:
    GenerationClock() noexcept = default;

    [[nodiscard]] RuntimeGeneration current() const noexcept {
        return RuntimeGeneration::from_value(value_.load(std::memory_order_acquire));
    }

    [[nodiscard]] bool is_current(RuntimeGeneration generation) const noexcept {
        return generation.valid() && generation.value() == value_.load(std::memory_order_acquire);
    }

    [[nodiscard]] RuntimeGeneration advance() {
        auto current_value = value_.load(std::memory_order_relaxed);
        for (;;) {
            if (current_value == RuntimeGeneration::maximum_wire_value) {
                throw std::overflow_error{"runtime generation exhausted"};
            }
            const auto next = current_value + 1;
            if (value_.compare_exchange_weak(
                    current_value, next, std::memory_order_acq_rel, std::memory_order_relaxed)) {
                return RuntimeGeneration::from_value(next);
            }
        }
    }

private:
    std::atomic<RuntimeGeneration::value_type> value_{RuntimeGeneration::initial().value()};
};

}  // namespace synth::core
