#pragma once

#include "adapters/commonlib_capture.hpp"
#include "runtime/fallout_runtime.hpp"
#include <RE/Fallout.h>
#include <array>
#include <cstring>
#include <limits>
#include <optional>

namespace synth::adapters {

// Flat 1.11.240 only, invoked exclusively through the runtime game-thread seam.
// Stop the actor's actual dialogue sound, without calling StopCurrentDialogue,
// changing scene/quest state, or changing a persistent actor speaking flag.
// Subsequent vanilla lines are stopped at the next owning game-thread pump.
class FlatSpeechExclusion final {
public:
    [[nodiscard]] static bool update(const std::optional<runtime::RuntimeSpeechOwner>& owner, bool discard) {
        if (discard || !owner) { forget(discard); return true; }
        if (!supported() || owner->cancellation.is_cancelled()) { forget(false); return false; }
        const auto epoch = FlatPickedReference::stamp();
        const auto* manager = RE::BGSSaveLoadManager::GetSingleton();
        if (!manager || manager->currentPlayerID == 0 || owner->speaker.form_id == 0 ||
            owner->speaker.form_id == 0x14 || owner->speaker.playthrough_id != commonlib::playthrough_id()) {
            forget(false);
            return false;
        }
        auto* actor = RE::TESForm::GetFormByID<RE::Actor>(owner->speaker.form_id);
        if (!actor || actor == RE::PlayerCharacter::GetSingleton() || actor->IsDeleted() ||
            actor->IsDisabled() || actor->IsDead(false) || !actor->Get3D() || !actor->currentProcess ||
            commonlib::origin_plugin(*actor) != owner->speaker.origin_plugin) {
            forget(false);
            return false;
        }
        if (owner_ && same_owner(*owner_, *owner)) {
            const auto resolved = handle_.get();
            if (!resolved || resolved.get() != actor) { forget(false); return false; }
        } else {
            forget(false);
            handle_ = actor->GetHandle();
            const auto resolved = handle_.get();
            if (!resolved || resolved.get() != actor) { forget(false); return false; }
            owner_ = owner;
            REX::INFO("SYNTH speech ownership: acquired speaker={:08X} utterance={}",
                      owner->speaker.form_id, owner->utterance_id);
        }
        if (epoch != FlatPickedReference::stamp() || owner->cancellation.is_cancelled()) {
            forget(true);
            return false;
        }
        // The audited helper returns the exact current dialogue handle at index 0.
        // Index 1 is not established as dialogue and is deliberately untouched.
        const auto* native = dialogue_handle()(actor->currentProcess, 0);
        if (!native) { forget(false); return false; }
        auto sound = *native;
        if (sound.soundID == std::numeric_limits<std::uint32_t>::max()) return true;
        // Query the manager, not the handle's optimistic cached state. Mutating
        // this local copy never modifies AIProcess, dialogue timers or callbacks.
        sound.assumeSuccess = false;
        sound.state = static_cast<std::uint8_t>(RE::BSSoundHandle::ASSUMED_STATE::kInitialized);
        if (!valid()(&sound)) return true;
        if (epoch != FlatPickedReference::stamp() || owner->cancellation.is_cancelled()) {
            forget(true);
            return false;
        }
        if (!stop()(&sound)) { forget(false); return false; }
        ++stop_requests_;
        if (last_sound_id_ != sound.soundID) {
            last_sound_id_ = sound.soundID;
            REX::INFO("SYNTH speech ownership: native-stop-requested speaker={:08X} utterance={} handle={}",
                      owner->speaker.form_id, owner->utterance_id, sound.soundID);
        }
        return true;
    }

private:
    [[nodiscard]] static bool same_owner(const runtime::RuntimeSpeechOwner& a, const runtime::RuntimeSpeechOwner& b) {
        return a.generation == b.generation && a.request_id == b.request_id && a.turn_id == b.turn_id &&
            a.utterance_id == b.utterance_id && a.speaker.form_id == b.speaker.form_id &&
            a.speaker.origin_plugin == b.speaker.origin_plugin && a.speaker.playthrough_id == b.speaker.playthrough_id;
    }

    [[nodiscard]] static bool supported() {
        if (attempted_) return verified_;
        attempted_ = true;
        const auto module = REX::FModule::GetExecutingModule();
        if (module.GetFileVersion().pack() != REL::Version{1, 11, 240, 0}.pack()) return false;
        const auto base = module.GetBaseAddress();
        constexpr std::array<unsigned char, 24> getter{
            0x4C,0x8B,0x41,0x10,0x4D,0x85,0xC0,0x74,0x0F,0x48,0x63,0xC2,
            0x48,0x8D,0x04,0xC5,0xCC,0x03,0x00,0x00,0x49,0x03,0xC0,0xC3};
        constexpr std::array<unsigned char, 32> stop_entry{
            0x40,0x53,0x48,0x83,0xEC,0x20,0x8B,0x19,0x83,0xFB,0xFF,0x74,0x1B,0xC6,0x41,0x05,
            0x02,0xE8,0x0A,0x18,0x00,0x00,0x48,0x8B,0xC8,0x8B,0xD3,0xE8,0x20,0x32,0x00,0x00};
        constexpr std::array<unsigned char, 32> valid_entry{
            0x40,0x53,0x48,0x83,0xEC,0x20,0x8B,0x19,0x83,0xFB,0xFF,0x74,0x14,0xE8,0x2E,0x17,
            0x00,0x00,0x48,0x8B,0xC8,0x8B,0xD3,0x48,0x83,0xC4,0x20,0x5B,0xE9,0xCF,0x3F,0x00};
        if (dialogue_handle().address() != base + 0xD02750 || stop().address() != base + 0x162DE20 ||
            valid().address() != base + 0x162DF00 ||
            std::memcmp(reinterpret_cast<const void*>(dialogue_handle().address()), getter.data(), getter.size()) != 0 ||
            std::memcmp(reinterpret_cast<const void*>(stop().address()), stop_entry.data(), stop_entry.size()) != 0 ||
            std::memcmp(reinterpret_cast<const void*>(valid().address()), valid_entry.data(), valid_entry.size()) != 0) {
            REX::WARN("SYNTH speech ownership unavailable: native dialogue sound ABI differs");
            return false;
        }
        verified_ = true;
        return true;
    }

    // No engine writes or native object resolution are needed on release/load.
    static void forget(bool discard) {
        if (owner_) REX::INFO("SYNTH speech ownership: released speaker={:08X} utterance={} stops={} discard={}",
                             owner_->speaker.form_id, owner_->utterance_id, stop_requests_, discard);
        owner_.reset();
        handle_ = {};
        stop_requests_ = 0;
        last_sound_id_ = std::numeric_limits<std::uint32_t>::max();
    }

    static REL::Relocation<RE::BSSoundHandle* (*)(RE::AIProcess*, std::int32_t)>& dialogue_handle() {
        static REL::Relocation<RE::BSSoundHandle* (*)(RE::AIProcess*, std::int32_t)> value{REL::ID(2231884)};
        return value;
    }
    static REL::Relocation<bool (*)(RE::BSSoundHandle*)>& stop() {
        static REL::Relocation<bool (*)(RE::BSSoundHandle*)> value{REL::ID(2267045)};
        return value;
    }
    static REL::Relocation<bool (*)(RE::BSSoundHandle*)>& valid() {
        static REL::Relocation<bool (*)(RE::BSSoundHandle*)> value{REL::ID(2267048)};
        return value;
    }
    static inline std::optional<runtime::RuntimeSpeechOwner> owner_;
    static inline RE::ObjectRefHandle handle_;
    static inline std::uint64_t stop_requests_{};
    static inline std::uint32_t last_sound_id_{std::numeric_limits<std::uint32_t>::max()};
    static inline bool attempted_{}, verified_{};
};

} // namespace synth::adapters
