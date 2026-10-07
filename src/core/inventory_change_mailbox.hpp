#pragma once

#include "core/picked_reference_mailbox.hpp"

namespace synth::core {

// A coalesced refresh signal, not item history or a consumption/trade claim. No pointers or item data escape callbacks.
class InventoryChangeMailbox final {
public:
    void arm() noexcept { changes_.arm(); }
    void invalidate() noexcept { changes_.invalidate(); }
    [[nodiscard]] std::uint64_t stamp() const noexcept { return changes_.stamp(); }
    [[nodiscard]] std::optional<std::uint64_t> revision() const noexcept { return changes_.observed_stamp(); }
    [[nodiscard]] bool record(std::uint64_t owner, std::uint32_t old_container,
                              std::uint32_t new_container, std::uint32_t base_object) noexcept {
        if ((old_container != 0x14 && new_container != 0x14) || base_object == 0 || base_object == UINT32_MAX ||
            old_container == UINT32_MAX || new_container == UINT32_MAX) return false;
        return changes_.record(owner, 0); // One CAS; concurrent notifications coalesce, reconciliation remains the fallback.
    }
private:
    PickedReferenceMailbox changes_;
};

} // namespace synth::core
