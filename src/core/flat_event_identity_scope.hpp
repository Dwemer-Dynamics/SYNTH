#pragma once

#include <cstdint>
#include <optional>

namespace synth::core {

// Exact flat 1.11.240 ordinary-form VM handles only. Inventory-instance and other handle kinds are not FormIDs.
[[nodiscard]] inline std::optional<std::uint32_t> flat_vm_actor_form_id(std::uint32_t type, std::uint64_t handle) noexcept {
    constexpr std::uint64_t plain_form = 0x0000FFFF00000000ULL;
    const auto form = static_cast<std::uint32_t>(handle);
    if (type != 0x41 || (handle & 0xFFFFFFFF00000000ULL) != plain_form || form == 0 || form == UINT32_MAX)
        return std::nullopt;
    return form;
}

// Stack-only correlation during original native forwarding. Address tokens are compared, never dereferenced or queued.
// A future adapter must observe the native policy's EXISTING return, not call it again or move pointers to a worker.
class FlatEventIdentityScope final {
public:
    struct Identities final {
        std::uint32_t actor{}, other{}; // Zero other means the original event had no secondary actor.
        bool operator==(const Identities&) const = default;
    };

    FlatEventIdentityScope(std::uint64_t epoch, std::uintptr_t actor, std::uintptr_t other = 0) noexcept
        : parent_{current_}, epoch_{epoch}, actor_address_{actor}, other_address_{other} {
        current_ = this;
    }
    FlatEventIdentityScope(const FlatEventIdentityScope&) = delete;
    FlatEventIdentityScope& operator=(const FlatEventIdentityScope&) = delete;
    ~FlatEventIdentityScope() { current_ = parent_; }

    // Called after an unchanged original GetHandleForObject invocation returns on the same callback stack.
    static void observe(std::uint32_t type, std::uintptr_t address, std::uint64_t handle) noexcept {
        auto* scope = current_;
        if (!scope || address == 0 || !(scope->epoch_ & 1)) return;
        const bool actor = address == scope->actor_address_;
        const bool other = address == scope->other_address_;
        if (!actor && !other) return;
        const auto identity = flat_vm_actor_form_id(type, handle);
        if (!identity) { scope->invalid_ = true; return; }
        if (actor) {
            if (scope->actor_ != 0 && scope->actor_ != *identity) scope->invalid_ = true;
            scope->actor_ = *identity;
        }
        if (other) {
            if (scope->other_ != 0 && scope->other_ != *identity) scope->invalid_ = true;
            scope->other_ = *identity;
        }
    }

    // The caller rechecks its mailbox epoch after native return and queues these scalars only.
    [[nodiscard]] std::optional<Identities> result(std::uint64_t current_epoch) const noexcept {
        if (current_ != this || current_epoch != epoch_ || !(epoch_ & 1) || invalid_ || actor_ == 0 ||
            (other_address_ != 0 && other_ == 0)) return std::nullopt;
        return Identities{actor_, other_};
    }

private:
    static inline thread_local FlatEventIdentityScope* current_{};
    FlatEventIdentityScope* parent_{};
    std::uint64_t epoch_{};
    std::uintptr_t actor_address_{}, other_address_{};
    std::uint32_t actor_{}, other_{};
    bool invalid_{};
};

} // namespace synth::core
