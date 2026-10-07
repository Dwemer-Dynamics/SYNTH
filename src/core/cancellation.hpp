#pragma once

#include "generation.hpp"

#include <atomic>
#include <memory>

namespace synth::core {

namespace detail {
struct CancellationState final {
    explicit CancellationState(RuntimeGeneration generation_value) noexcept
        : generation{generation_value} {}

    RuntimeGeneration generation;
    std::atomic_bool cancelled{false};
    std::shared_ptr<const CancellationState> parent;
};
}  // namespace detail

class CancellationToken final {
public:
    CancellationToken() noexcept = default;

    [[nodiscard]] RuntimeGeneration generation() const noexcept {
        return state_ ? state_->generation : RuntimeGeneration{};
    }

    [[nodiscard]] bool is_cancelled() const noexcept {
        if (!state_) return true;
        for (auto state = state_; state; state = state->parent) {
            if (state->cancelled.load(std::memory_order_acquire)) return true;
        }
        return false;
    }

    [[nodiscard]] bool is_current(const GenerationClock& clock) const noexcept {
        return !is_cancelled() && clock.is_current(generation());
    }

private:
    friend class CancellationSource;
    explicit CancellationToken(std::shared_ptr<const detail::CancellationState> state) noexcept
        : state_{std::move(state)} {}

    std::shared_ptr<const detail::CancellationState> state_;
};

class CancellationSource final {
public:
    explicit CancellationSource(RuntimeGeneration generation)
        : state_{std::make_shared<detail::CancellationState>(generation)} {
        if (!generation.valid()) {
            throw std::invalid_argument{"cancellation source requires a valid generation"};
        }
    }

    // A turn can be cancelled independently; session cancellation invalidates all its turns.
    explicit CancellationSource(const CancellationToken& parent)
        : CancellationSource{parent.generation()} {
        state_->parent = parent.state_;
    }

    CancellationSource(const CancellationSource&) = default;
    CancellationSource& operator=(const CancellationSource&) = default;
    CancellationSource(CancellationSource&&) = delete;
    CancellationSource& operator=(CancellationSource&&) = delete;

    [[nodiscard]] CancellationToken token() const noexcept { return CancellationToken{state_}; }

    void cancel() noexcept { state_->cancelled.store(true, std::memory_order_release); }

    // Late worker failures must not cancel a replacement turn in the same runtime generation.
    [[nodiscard]] bool cancel_if_owner(const CancellationToken& owner) noexcept {
        if (state_ != owner.state_) return false;
        cancel();
        return true;
    }

    [[nodiscard]] bool is_cancelled() const noexcept {
        return token().is_cancelled();
    }

private:
    std::shared_ptr<detail::CancellationState> state_;
};

}  // namespace synth::core
