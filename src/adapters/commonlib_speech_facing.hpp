#pragma once

#include "adapters/commonlib_actions.hpp"
#include "presentation/speech_facing.hpp"

namespace synth::adapters::commonlib {

// Game-thread-only one-shot yaw; no AI package, headtracking, camera or player rotation is acquired.
[[nodiscard]] inline runtime::RuntimeActionResult face_speech_listener(const runtime::RuntimeFacingRequest& request) {
    using Status = runtime::RuntimeActionStatus;
    if (request.cancellation.is_cancelled() || request.speaker.form_id == 0 || request.speaker.form_id == 0x14 ||
        request.listener.form_id == 0 || request.listener.form_id == request.speaker.form_id || menu_mode_active())
        return {Status::unavailable, "speech facing skipped: owner, target or menu state"};
    const auto* manager = RE::BGSSaveLoadManager::GetSingleton();
    if (!manager || manager->currentPlayerID == 0 || request.speaker.playthrough_id != playthrough_id() ||
        request.listener.playthrough_id != request.speaker.playthrough_id)
        return {Status::unavailable, "speech facing skipped: playthrough changed"};
#if defined(SYNTH_WITH_F4SEVR)
    // v1.13.1's reviewed addrlib.csv maps the VR header's ID 1049748. Never use flat ID 2201134.
    if (!REL::IDDB::get().IsVRAddressLibraryAtLeastVersion("1.13.1", false))
        return {Status::unsupported_runtime, "speech facing needs VR Address Library 1.13.1 or newer"};
#else
    const auto epoch = FlatPickedReference::stamp();
#endif
    auto* speaker = RE::TESForm::GetFormByID<RE::Actor>(request.speaker.form_id);
    auto* listener = RE::TESForm::GetFormByID<RE::Actor>(request.listener.form_id);
    if (!speaker || !listener || speaker == RE::PlayerCharacter::GetSingleton() ||
        !plugin_name_equal(origin_plugin(*speaker), request.speaker.origin_plugin))
        return {Status::unavailable, "speech facing skipped: live speaker identity changed"};
    auto listener_plugin = origin_plugin(*listener);
    if (listener_plugin.empty() && listener->GetFormID() == 0x14) listener_plugin = "Fallout4.esm";
    if (!plugin_name_equal(listener_plugin, request.listener.origin_plugin) ||
        speaker->IsDeleted() || speaker->IsDisabled() || speaker->IsDead(false) || !speaker->Get3D() ||
        listener->IsDeleted() || listener->IsDisabled() || listener->IsDead(false) ||
        (listener->GetFormID() != 0x14 && !listener->Get3D()))
        return {Status::unavailable, "speech facing skipped: actor unavailable or listener identity changed"};
    auto* source_cell = speaker->GetParentCell();
    auto* target_cell = listener->GetParentCell();
    if (!source_cell || !target_cell || (source_cell != target_cell &&
        (source_cell->IsInterior() || target_cell->IsInterior() || !source_cell->worldSpace ||
         source_cell->worldSpace != target_cell->worldSpace)))
        return {Status::unavailable, "speech facing skipped: actors no longer share a spatial context"};
    const auto speed = speaker->DoGetCurrentSpeed();
    const auto scene_flags = static_cast<std::uint32_t>(RE::Actor::BOOL_FLAGS::kScenePackage) |
        static_cast<std::uint32_t>(RE::Actor::BOOL_FLAGS::kSceneHeadtrackRotation) |
        static_cast<std::uint32_t>(RE::Actor::BOOL_FLAGS::kIsInKillMove);
    if (!speaker->currentProcess || speaker->DoGetSitSleepState() != RE::SIT_SLEEP_STATE::kNormal ||
        speaker->lifeState != 0 || speaker->knockState != 0 || speaker->flyState != 0 ||
        speaker->meleeAttackState != 0 || speaker->staggered || speaker->inSyncAnim || speaker->talkingToPlayer ||
        speaker->interactingState != RE::INTERACTING_STATE::kNotInteracting || speaker->IsInCombat() ||
        (speaker->niFlags.flags & scene_flags) != 0 || !std::isfinite(speed) || std::abs(speed) > 0.1F ||
        speaker->DoGetSprinting() ||
        (speaker->weaponState != RE::WEAPON_STATE::kSheathed && speaker->weaponState != RE::WEAPON_STATE::kDrawn))
        return {Status::rejected, "speech facing skipped: actor moving, busy or in a scene"};
    auto target = point(listener->GetPosition());
#if defined(SYNTH_WITH_F4SEVR)
    if (listener->GetFormID() == 0x14) {
        const auto* camera = RE::PlayerCamera::GetSingleton();
        if (!camera || !camera->cameraRoot) return {Status::unavailable, "speech facing skipped: live HMD pose unavailable"};
        target = point(camera->cameraRoot->world.translate);
    }
#endif
    const auto yaw = presentation::facing_yaw(point(speaker->GetPosition()), target);
    auto angles = speaker->data.angle;
    if (!yaw || !std::isfinite(angles.x) || !std::isfinite(angles.y) || !std::isfinite(angles.z))
        return {Status::unavailable, "speech facing skipped: heading unavailable"};
    if (request.cancellation.is_cancelled()) return {Status::unavailable, "speech facing cancelled before mutation"};
#if !defined(SYNTH_WITH_F4SEVR)
    if (epoch != FlatPickedReference::stamp()) return {Status::unavailable, "speech facing crossed a native load boundary"};
#endif
    angles.z = *yaw;
    speaker->SetAngleOnReference(angles);
    const auto error = std::remainder(speaker->data.angle.z - *yaw, 2.0F * std::numbers::pi_v<float>);
    return std::isfinite(error) && std::abs(error) <= std::numbers::pi_v<float>/90.0F
        ? runtime::RuntimeActionResult{Status::succeeded, "speech facing applied and heading verified"}
        : runtime::RuntimeActionResult{Status::failed, "speech facing heading not confirmed by runtime"};
}

}  // namespace synth::adapters::commonlib
