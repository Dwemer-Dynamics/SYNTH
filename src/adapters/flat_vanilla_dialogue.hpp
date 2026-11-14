#pragma once

#if !defined(SYNTH_WITH_F4SE) || defined(SYNTH_WITH_F4SEVR)
#error "Vanilla dialogue observation requires the independently audited flat 1.11.240 ABI"
#endif

#include "adapters/commonlib_capture.hpp"
#include "adapters/flat_speech_exclusion.hpp"
#include "core/event_witness.hpp"
#include "json/json.hpp"

#include <array>
#include <cmath>
#include <cstring>
#include <optional>
#include <string_view>
#include <vector>

namespace synth::adapters {

// Read-only game-pump sampler of the NPC line Fallout itself selected for subtitle display. Audited
// 1.11.240 (docs/VANILLA-DIALOGUE-WITNESSES.md): selection RVA 107E520 picks one in-range priority
// entry, stores its handle in currentSpeaker and publishes that entry's text through the source
// display value (+30/+38, active +40, spin lock +48) under the manager write lock. Retained or
// out-of-range entries are never treated as spoken; only the entry matching both currentSpeaker
// and the source text, backed by an INFO and the speaker's dialogue sound the audio manager reports as
// playing (not proof of audibility), qualifies. Text scans are bounded under the native locks.
// SYNTH writes only the HUD sink, never this source. No hooks, writes, waits or engine pointers escape.
class FlatVanillaDialogue final {
public:
    // excluded: a positively read line whose speaker is the player or not an actor; it ends NPC residency.
    enum class Status : unsigned char { unavailable, absent, excluded, present };
    struct Sample final {
        Status status{Status::unavailable};
        bool eligible{}; // A present but ineligible line still occupies residency; it is never delivered.
        core::DialogueLine line;
    };
    struct Witnesses final {
        std::vector<core::ActorSnapshot> actors; // Speaker first, then witnesses.
        core::DialogueLine line;
        std::vector<core::WitnessIdentity> witnesses;
        core::SceneIdentity scene; // The speaker's physical cell as copied during collection.
        bool interior{};
    };

    [[nodiscard]] static bool supported() {
        if (attempted_) return verified_;
        attempted_ = true;
        const auto module = REX::FModule::GetExecutingModule();
        if (module.GetFileVersion().pack() != REL::Version{1, 11, 240, 0}.pack()) return false;
        const auto base = module.GetBaseAddress();
        // Selection entry: takes the manager lock and reads the array size; clear entry: the source display layout.
        constexpr std::array<unsigned char, 32> selection{
            0x41,0x55,0x48,0x83,0xEC,0x60,0x4C,0x8B,0xE9,0x48,0x8D,0x0D,0x68,0x50,0x29,0x02,
            0xE8,0x4B,0xAF,0x5D,0x00,0x41,0x83,0x7D,0x18,0x00,0xC6,0x44,0x24,0x70,0x01,0x0F};
        constexpr std::array<unsigned char, 32> clear{
            0x40,0x53,0x48,0x83,0xEC,0x20,0x48,0x8D,0x59,0x20,0x48,0x89,0x7C,0x24,0x38,0x48,
            0x8B,0xF9,0x48,0x8B,0xCB,0xE8,0x86,0xE7,0x1A,0xFF,0x80,0x7F,0x18,0x00,0x74,0x28};
        REL::Relocation<RE::SubtitleManager**> singleton{RE::ID::SubtitleManager::Singleton};
        REL::Relocation<RE::BSReadWriteLock*> lock{RE::ID::SubtitleManager::RWLock};
        if (singleton.address() != base + 0x30E8B08 || lock.address() != base + 0x3313598 ||
            std::memcmp(reinterpret_cast<const void*>(base + 0x107E520), selection.data(), selection.size()) != 0 ||
            std::memcmp(reinterpret_cast<const void*>(base + 0x107EF20), clear.data(), clear.size()) != 0) {
            REX::WARN("SYNTH vanilla dialogue unavailable: subtitle manager ABI differs from the audited 1.11.240 build");
            return false;
        }
        verified_ = true;
        return true;
    }

    // One nonblocking manager read lease and one source spin-lock attempt. Contention is unavailable, never absence.
    [[nodiscard]] static Sample sample() {
        using Display = RE::BSTValueEventSource<RE::HUDSubtitleDisplayEvent>;
        static_assert(offsetof(RE::SubtitleManager, subtitlePriorityArray) == 0x08);
        static_assert(offsetof(RE::SubtitleManager, currentSpeaker) == 0x20);
        static_assert(offsetof(RE::SubtitleManager, subtitleDisplayData) == 0x28);
        static_assert(offsetof(Display, optionalValue) == 0x08 && offsetof(Display, dataLock) == 0x20);
        Sample result;
        if (!supported() || !commonlib::world_ready_for_capture()) return result;
        auto* manager = RE::SubtitleManager::GetSingleton();
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!manager || !player) return result;
        decltype(manager->currentSpeaker) handle;
        const RE::TESForm* info{};
        {
            const commonlib::TryReadLock lease{RE::SubtitleManager::GetRWLock()};
            if (!lease.owns_lock()) return result;
            const auto current = manager->currentSpeaker;
            commonlib::TrySpinLock display{manager->subtitleDisplayData.dataLock};
            if (!display.owns_lock()) return result;
            const auto& shown = manager->subtitleDisplayData.optionalValue;
            if (!current || !shown) {
                result.status = Status::absent;
                return result;
            }
            const auto text = bounded(shown->subtitleText.c_str());
            const auto& entries = manager->subtitlePriorityArray;
            if (!text || text->empty() || entries.size() > 32) return result; // Oversized is unavailable, never absence.
            const RE::SubtitleInfo* selected{};
            for (const auto& entry : entries) {
                if (entry.speaker != current || bounded(entry.subtitleText.c_str()) != text) continue;
                if (selected) return result; // Two candidates for one displayed line are ambiguous.
                selected = &entry;
            }
            // A stale source value after a settings change matches no current entry: unavailable, not absence.
            if (!selected || !selected->topicInfo) return result;
            info = reinterpret_cast<const RE::TESForm*>(selected->topicInfo);
            if (info->GetFormType() != RE::ENUM_FORM_ID::kINFO) return result;
            result.line.text.assign(*text);
            result.line.topic_info_form_id = info->GetFormID();
            handle = current; // Copied under the leases; native file and handle lookups run after both are released.
        }
        result.line.topic_info_plugin = commonlib::origin_plugin(*info);
        const RE::NiPointer<RE::TESObjectREFR> speaker = handle.get();
        if (!speaker) return result; // An unresolved handle is unknown, not a known non-NPC line.
        auto* actor = speaker->As<RE::Actor>();
        if (!actor || actor == player) {
            // Player-voiced or non-actor lines are outside this slice, but they are a positive transition.
            result.status = Status::excluded;
            result.line = {};
            return result;
        }
        result.line.speaker.form_id = actor->GetFormID();
        result.line.speaker.origin_plugin = commonlib::origin_plugin(*actor);
        result.line.speaker.display_name = commonlib::bounded_label(actor->GetDisplayFullName());
        try { result.line.speaker.playthrough_id = commonlib::playthrough_id(); }
        catch (const std::runtime_error&) { return Sample{}; }
        for (auto& character : result.line.text) if (static_cast<unsigned char>(character) < 0x20) character = ' ';
        if (!json::detail::valid_utf8(result.line.text) || !result.line.valid()) return Sample{};
        result.status = Status::present;
        const auto* cell = actor->GetParentCell();
        result.eligible = cell && cell == player->GetParentCell() && eligible(*actor) &&
            !iequals(result.line.topic_info_plugin, "SYNTH.esp") && result.line.speaker.form_id >> 24 != 0xFF &&
            FlatSpeechExclusion::owned_speaker() != result.line.speaker.form_id &&
            FlatSpeechExclusion::native_dialogue_playing(*actor);
        return result;
    }

    // Only on a new eligible observation, in the same pump. Exact speaker cell, live NPCs within the policy
    // radius of the actual speaker; an over-budget or unresolvable scene grants nothing.
    [[nodiscard]] static std::optional<Witnesses> witnesses(const core::DialogueLine& line) {
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* speaker = RE::TESForm::GetFormByID<RE::Actor>(line.speaker.form_id);
        auto* lists = RE::ProcessLists::GetSingleton();
        if (!player || !speaker || !lists || speaker == player || !eligible(*speaker) ||
            commonlib::origin_plugin(*speaker) != line.speaker.origin_plugin) return std::nullopt;
        const auto* cell = speaker->GetParentCell();
        if (!cell || cell != player->GetParentCell() || lists->highActorHandles.size() > 64) return std::nullopt;
        const auto interior = cell->IsInterior();
        const auto* space = !interior ? cell->worldSpace : nullptr;
        const auto* cell_file = cell->GetFile(0);
        const auto* space_file = space ? space->GetFile(0) : nullptr;
        core::SceneIdentity scene{cell->GetFormID(), space ? space->GetFormID() : 0,
            cell_file ? std::string{cell_file->GetFilename()} : std::string{},
            space_file ? std::string{space_file->GetFilename()} : std::string{}};
        if (!scene.valid(interior)) return std::nullopt;
        const auto origin = commonlib::point(speaker->GetPosition());
        Witnesses result;
        try {
            const auto& playthrough = line.speaker.playthrough_id;
            result.actors.push_back(commonlib::actor_snapshot(*speaker, playthrough, player, false, false));
            for (const auto& handle : lists->highActorHandles) {
                const auto actor = handle.get();
                if (!actor || actor.get() == speaker || actor.get() == player || !eligible(*actor) ||
                    actor->GetParentCell() != cell || actor->GetFormID() >> 24 == 0xFF) continue;
                const auto position = commonlib::point(actor->GetPosition());
                const auto dx = position.x - origin.x, dy = position.y - origin.y, dz = position.z - origin.z;
                const auto distance = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (!std::isfinite(distance) || distance > core::EventWitnessSet::radius) continue;
                if (result.actors.size() > core::EventWitnessSet::capacity) return std::nullopt;
                result.actors.push_back(commonlib::actor_snapshot(*actor, playthrough, player, false, false));
            }
        } catch (const std::exception&) {
            return std::nullopt;
        }
        const auto identity = [](const core::ActorSnapshot& actor) {
            return core::WitnessIdentity{actor.form_id(), actor.origin_plugin(), actor.playthrough_id(), actor.name()};
        };
        result.scene = std::move(scene);
        result.interior = interior;
        result.line = line;
        result.line.speaker = identity(result.actors.front());
        if (result.line.speaker.form_id != line.speaker.form_id || result.line.speaker.origin_plugin != line.speaker.origin_plugin)
            return std::nullopt;
        for (std::size_t i = 1; i < result.actors.size(); ++i) result.witnesses.push_back(identity(result.actors[i]));
        return result;
    }

private:
    // Reads at most maximum_text_bytes + 1 bytes; nullopt when longer. Nothing is allocated here.
    [[nodiscard]] static std::optional<std::string_view> bounded(const char* text) noexcept {
        if (!text) return std::string_view{};
        const auto size = ::strnlen(text, core::DialogueLine::maximum_text_bytes + 1);
        if (size > core::DialogueLine::maximum_text_bytes) return std::nullopt;
        return std::string_view{text, size};
    }
    [[nodiscard]] static bool eligible(RE::Actor& actor) {
        return !actor.IsDead(false) && !actor.IsDisabled() && !actor.IsDeleted() && actor.Get3D() && actor.currentProcess;
    }
    [[nodiscard]] static bool iequals(std::string_view a, std::string_view b) noexcept {
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) {
            return (x >= 'A' && x <= 'Z' ? x + 32 : x) == (y >= 'A' && y <= 'Z' ? y + 32 : y);
        });
    }
    static inline bool attempted_{}, verified_{};
};

} // namespace synth::adapters
