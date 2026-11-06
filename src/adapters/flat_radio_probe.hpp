#pragma once

#include "adapters/commonlib_capture.hpp"
#include "radio/diagnostic.hpp"
#include <RE/B/BGSSceneActionRadio.h>
#include <RE/E/ExtraRadioData.h>
#include <RE/R/RadioManager.h>
#include <array>
#include <cmath>
#include <string>

namespace synth::adapters::commonlib {

// Flat 1.11.240 only. Called exclusively by the IFalloutRuntime game-thread seam.
// Never edits DCR, descriptor filenames, scene action fields, or cached resources.
class FlatRadioProbe final {
    static constexpr const char* plugin = "SYNTHRadioDiagnostics.esp";
    struct Records {
        RE::TESQuest* quest{};
        RE::TESObjectREFR* transmitter{};
        RE::TESObjectREFR* receiver{};
        std::array<RE::BGSScene*, 2> scenes{};
        std::array<RE::BGSSceneActionRadio*, 2> actions{};
        std::array<std::uint32_t, 2> sound_ids{};
        bool complete{};
        std::string failure_detail{"diagnostic_records_unavailable"};
    };
    [[nodiscard]] static bool supported() {
        return REX::FModule::GetExecutingModule().GetFileVersion().pack() == REL::Version{1, 11, 240, 0}.pack();
    }
    [[nodiscard]] static Records records() {
        Records value;
        const auto fail = [&value](std::string detail) {
            value.failure_detail = detail.substr(0, 128);
            return value;
        };
        if (!supported()) return fail("unsupported_runtime");
        auto* data = RE::TESDataHandler::GetSingleton();
        if (!data) return fail("diagnostic_data_handler_unavailable");
        value.quest = data->LookupForm<RE::TESQuest>(0x800, plugin);
        value.transmitter = data->LookupForm<RE::TESObjectREFR>(0x803, plugin);
        value.receiver = data->LookupForm<RE::TESObjectREFR>(0x805, plugin);
        if (!value.quest) return fail("diagnostic_quest_missing");
        if (!value.transmitter) return fail("diagnostic_transmitter_missing");
        if (!value.receiver) return fail("diagnostic_receiver_missing");
        auto* transmitter_base = data->LookupForm<RE::TESObjectACTI>(0x801, plugin);
        auto* receiver_base = data->LookupForm<RE::TESObjectACTI>(0x804, plugin);
        if (!transmitter_base || value.transmitter->GetObjectReference() != transmitter_base)
            return fail("diagnostic_transmitter_base_mismatch");
        if (!receiver_base || value.receiver->GetObjectReference() != receiver_base)
            return fail("diagnostic_receiver_base_mismatch");
        const auto* radio_data = value.transmitter->extraList ?
            value.transmitter->extraList->GetByType<RE::ExtraRadioData>() : nullptr;
        if (!radio_data || !std::isfinite(radio_data->data.radioStation) ||
            std::abs(radio_data->data.radioStation - 106.9F) > 0.001F)
            return fail("diagnostic_transmitter_frequency_mismatch");
        for (std::size_t i = 0; i < 2; ++i) {
            const std::string slot = i ? "diagnostic_slot_b_" : "diagnostic_slot_a_";
            const auto local_scene_id = 0x806 + static_cast<std::uint32_t>(i);
            const auto expected_scene_id = data->LookupFormID(local_scene_id, plugin);
            auto* raw_scene = data->LookupForm(local_scene_id, plugin);
            RE::BGSScene* scene{};
            if (raw_scene) {
                if (!raw_scene->Is(RE::BGSScene::FORM_ID))
                    return fail(slot + "scene_type_mismatch:type=" +
                                std::to_string(static_cast<std::int32_t>(raw_scene->GetFormType())));
                scene = raw_scene->As<RE::BGSScene>();
                if (!scene) return fail(slot + "scene_cast_failed");
            } else {
                // Quest children may be held by their owning quest before the global form map
                // exposes them. Accept only the exact owned ID, not a different scene or quest.
                if (!expected_scene_id) return fail(slot + "scene_expected_id_unavailable");
                constexpr std::size_t maximum_quest_scenes = 256;
                const auto quest_scene_count = value.quest->scenes.size();
                if (quest_scene_count > maximum_quest_scenes)
                    return fail(slot + "quest_scene_limit:count=" + std::to_string(quest_scene_count));
                for (auto* candidate : value.quest->scenes) {
                    if (!candidate || candidate->GetFormID() != expected_scene_id) continue;
                    if (!candidate->Is(RE::BGSScene::FORM_ID))
                        return fail(slot + "quest_scene_type_mismatch");
                    if (candidate->parentQuest != value.quest)
                        return fail(slot + "quest_mismatch");
                    if (scene) return fail(slot + "quest_scene_ambiguous");
                    scene = candidate;
                }
                if (!scene) {
                    // One vanilla scene is a read-only control for the shared map/type path.
                    const auto* dcr = data->LookupForm(0x304F0, "Fallout4.esm");
                    return fail(slot + "scene_missing:global=0,qscenes=" + std::to_string(quest_scene_count) +
                                ",qfiles=" + std::to_string(value.quest->fileOffsets.size()) +
                                ",promoting=" + (value.quest->promoteTask ? "1" : "0") +
                                ",dcrtype=" + (dcr ? std::to_string(static_cast<std::int32_t>(dcr->GetFormType())) : "absent"));
                }
            }
            auto* sound = data->LookupForm<RE::BGSSoundDescriptorForm>(0x808 + static_cast<std::uint32_t>(i), plugin);
            if (scene->GetFormID() != expected_scene_id)
                return fail(slot + "scene_id_mismatch");
            if (scene->parentQuest != value.quest)
                return fail(i ? "diagnostic_slot_b_quest_mismatch" : "diagnostic_slot_a_quest_mismatch");
            if (scene->actions.size() != 1)
                return fail(i ? "diagnostic_slot_b_action_count" : "diagnostic_slot_a_action_count");
            if (!sound) return fail(i ? "diagnostic_slot_b_sound_missing" : "diagnostic_slot_a_sound_missing");
            const auto expected_sound_id = data->LookupFormID(0x808 + static_cast<std::uint32_t>(i), plugin);
            if (!expected_sound_id || sound->GetFormID() != expected_sound_id)
                return fail(slot + "sound_id_mismatch");
            auto* action = scene->actions[0];
            if (!action) return fail(i ? "diagnostic_slot_b_action_missing" : "diagnostic_slot_a_action_missing");
            if (action->GetActionType() != RE::SCENE_ACTION_TYPE::kRadio)
                return fail(i ? "diagnostic_slot_b_action_type" : "diagnostic_slot_a_action_type");
            if (action->actorID != 0)
                return fail(i ? "diagnostic_slot_b_actor_alias" : "diagnostic_slot_a_actor_alias");
            if (action->startPhase != 0 || action->endPhase != 0)
                return fail(i ? "diagnostic_slot_b_phase" : "diagnostic_slot_a_phase");
            auto* radio = static_cast<RE::BGSSceneActionRadio*>(action);
            if (radio->sound != sound)
                return fail(i ? "diagnostic_slot_b_sound_mismatch" : "diagnostic_slot_a_sound_mismatch");
            value.scenes[i] = scene;
            value.actions[i] = radio;
            value.sound_ids[i] = expected_sound_id;
        }
        value.complete = true;
        return value;
    }
    [[nodiscard]] static bool active(const RE::BGSScene& scene) {
        return (scene.niFlags.flags & static_cast<std::uint32_t>(RE::BGSScene::BOOL_BITS::kActive)) != 0;
    }
    // These static, locked radio-manager entry points are audited for flat 1.11.240.
    // The key is the exact owned scene and transmitter. A descriptor FormID is
    // never passed to BSSoundHandle: only the engine reads its actual listener handles.
    [[nodiscard]] static std::uint32_t native_descriptor(std::uint32_t scene_id, RE::TESObjectREFR* transmitter) {
        static REL::Relocation<std::uint32_t (*)(std::uint32_t, RE::TESObjectREFR*)> query{REL::ID(2227682)};
        return query(scene_id, transmitter);
    }
    [[nodiscard]] static radio::SlotObservation observe(RE::BGSScene& scene, RE::BGSSceneActionRadio& action,
                                                         RE::TESObjectREFR& transmitter, std::uint32_t expected_sound_id) {
        static_assert(offsetof(RE::BGSSceneAction, status) == 0x14);
        static_assert(offsetof(RE::BGSSceneActionRadio, soundID) == 0x4C);
        static_assert(offsetof(RE::BGSSceneActionRadio, soundDone) == 0x50);
        radio::SlotObservation result;
        result.available = true;
        result.expected_descriptor_id = expected_sound_id;
        result.scene_active = active(scene);
        result.phase = scene.currentActivePhase;
        result.action_status = static_cast<std::uint32_t>(action.status.underlying());
        result.sound_done = action.soundDone;
        result.sound_id = action.soundID;
        result.action_duration = action.soundDuration;
        const auto scene_id = scene.GetFormID();
        const auto descriptor_before = native_descriptor(scene_id, &transmitter);
        static REL::Relocation<void (*)(std::uint32_t, RE::TESObjectREFR*, bool*, bool*)> query_state{REL::ID(2227681)};
        query_state(scene_id, &transmitter, &result.sound_busy, &result.sound_paused);
        static REL::Relocation<std::uint64_t (*)(std::uint32_t, RE::TESObjectREFR*)> query_duration{REL::ID(2227683)};
        result.handle_duration = query_duration(scene_id, &transmitter);
        result.native_descriptor_id = native_descriptor(scene_id, &transmitter);
        result.sound_valid = result.native_descriptor_id != 0xFFFFFFFFU;
        // Audio state can change between individually locked reads. Never use
        // an inconsistent transition as an idle authorization.
        result.sound_state_available = descriptor_before == result.native_descriptor_id;
        return result;
    }
    [[nodiscard]] static radio::ProbeResult validate_audio(Records& value, bool require_idle) {
        for (std::size_t i = 0; i < 2; ++i) {
            const auto observation = observe(*value.scenes[i], *value.actions[i], *value.transmitter, value.sound_ids[i]);
            const auto state = radio::slot_readiness(observation, value.sound_ids[i]);
            if (state == radio::SlotReadiness::state_unavailable) return {false, "diagnostic_radio_state_unavailable"};
            if (state == radio::SlotReadiness::descriptor_mismatch) return {false, "diagnostic_sound_ownership_mismatch"};
            if (require_idle && state == radio::SlotReadiness::busy) return {false, "owned_audio_busy"};
        }
        return {true, "owned_audio_validated"};
    }
    static void stop(Records& value) {
        static REL::Relocation<void (*)(std::uint32_t)> stop_scene{REL::ID(2227678)};
        for (std::size_t i = 0; i < 2; ++i) {
            if (active(*value.scenes[i])) value.scenes[i]->SetSceneActive(false);
            // Release every actual listener handle for only this owned scene key,
            // including entries lingering after EndAction. Release is asynchronous.
            stop_scene(value.scenes[i]->GetFormID());
        }
    }
public:
    [[nodiscard]] static radio::ProbeObservation capture(core::RuntimeGeneration generation) {
        radio::ProbeObservation result{generation};
        result.supported = supported();
        auto value = records();
        result.records_available = value.complete;
        if (!result.supported || !value.complete) return result;
        result.radio_on = RE::RadioManager::QPlayerRadioEnabled();
        result.frequency = RE::RadioManager::QCurrentPlayerFreq();
        for (std::size_t i = 0; i < 2; ++i)
            result.slots[i] = observe(*value.scenes[i], *value.actions[i], *value.transmitter, value.sound_ids[i]);
        constexpr std::array<std::uint32_t, 5> dcr{0x304F0, 0x87768, 0x144F6A, 0x176902, 0x225342};
        auto* data = RE::TESDataHandler::GetSingleton();
        for (std::size_t i = 0; i < dcr.size(); ++i) {
            result.dcr_scenes[i].local_form_id = dcr[i];
            if (auto* scene = data->LookupForm<RE::BGSScene>(dcr[i], "Fallout4.esm")) {
                result.dcr_scenes[i].available = true;
                result.dcr_scenes[i].active = active(*scene);
                result.dcr_scenes[i].phase = scene->currentActivePhase;
            }
        }
        return result;
    }
    [[nodiscard]] static radio::ProbeResult execute(const radio::ProbeRequest& request) {
        if (!request.valid() || !supported()) return {};
        auto value = records();
        if (!value.complete) return {false, value.failure_detail};
        using Op = radio::ProbeOperation;
        if (request.operation == Op::observe) return {true, "observed"};
        if (request.operation == Op::stop || request.operation == Op::disable) {
            const auto validation = validate_audio(value, false);
            if (!validation.succeeded) return validation;
            stop(value);
            if (request.operation == Op::disable) {
                value.transmitter->Disable();
                value.receiver->Disable();
            }
            return {true, request.operation == Op::stop ? "stop_requested" : "disabled"};
        }
        RE::ObjectRefHandle alias;
        value.quest->GetAliasedRef(&alias, 0);
        const auto reference = alias.get();
        if (!reference || reference.get() != value.transmitter) return {false, "start_diagnostic_quest_first"};
        if (request.operation == Op::enable) {
            value.transmitter->Enable(false);
            value.receiver->Enable(false);
            return {true, "enabled"};
        }
        if (value.transmitter->IsDisabled()) return {false, "transmitter_disabled"};
        const auto validation = validate_audio(value, true);
        if (!validation.succeeded) return validation;
        value.scenes[request.slot]->ResetAllSceneActions();
        value.scenes[request.slot]->SetSceneActive(true);
        return {true, "play_requested"};
    }
};
} // namespace synth::adapters::commonlib
