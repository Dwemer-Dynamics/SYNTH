#pragma once

#include "runtime/fallout_runtime.hpp"
#include "adapters/commonlib_capture.hpp"
#include "audio/speech_animation.hpp"
#include <F4SE/F4SE.h>
#include <RE/Fallout.h>
#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

namespace synth::adapters {

// Independently verified on 1.11.240. No Skyrim or VR ABI is used here.
// See docs/LIPSYNC-NATIVE-AUDIT.md for executable identity and instruction evidence.
struct FlatSpeechFaceView final {
    std::byte base[0x18];
    float final_expression[54];
    float speech_expression[54];
    float emotion_expression[54];
    std::byte emotion_state[0x14];
    RE::BSSpinLock lock;
    std::byte padding[4];
    void* native_lip;
    std::byte sound_and_archetype[0x10];
    bool dirty, force_update, skip_update, dead;
    std::uint32_t gender, lip_state;
};
static_assert(offsetof(FlatSpeechFaceView, speech_expression) == 0xF0);
static_assert(offsetof(FlatSpeechFaceView, emotion_expression) == 0x1C8);
static_assert(offsetof(FlatSpeechFaceView, lock) == 0x2B4);
static_assert(offsetof(FlatSpeechFaceView, native_lip) == 0x2C0);
static_assert(offsetof(FlatSpeechFaceView, dirty) == 0x2D8);
static_assert(offsetof(FlatSpeechFaceView, lip_state) == 0x2E0);

class FlatSpeechAnimation final {
public:
    // Called exclusively by RuntimeBase's game-thread speech callback.
    static void update(const std::optional<runtime::RuntimeSpeechFrame>& frame, bool discard) {
        if (discard) { forget(); return; }
        // No native hook installation during idle/startup/save admission; wait for admitted speech.
        if (!installed_ && (!frame || !install())) return;
        if (owner_ && !reported_ && permit_ && permit_->merged.load(std::memory_order_relaxed) != 0) {
            REX::INFO("SYNTH lipsync: native-applied speaker={:08X} utterance={} game-thread writes={} engine merges={}",
                      owner_->actor_id, owner_->utterance_id, applied_frames_, permit_->merged.load(std::memory_order_relaxed));
            reported_ = true;
        }
        if (!frame || (face_ && !owner_) || (owner_ && (owner_->generation != frame->generation ||
            owner_->actor_id != frame->actor_id || owner_->utterance_id != frame->utterance_id))) {
            if (!release()) return;
        }
        if (!frame) return;
        auto* actor = RE::TESForm::GetFormByID<RE::Actor>(frame->actor_id);
        if (!actor || actor->IsDeleted() || actor->IsDisabled() || !actor->Get3D() ||
            !actor->race || actor->race->GetFormID() != 0x13746 ||
            !actor->currentProcess || !actor->currentProcess->middleHigh) {
            if (unavailable_utterance_ != frame->utterance_id) {
                REX::INFO("SYNTH lipsync unavailable: speaker={:08X} utterance={} needs a live supported human face",
                          frame->actor_id, frame->utterance_id);
                unavailable_utterance_ = frame->utterance_id;
            }
            (void)release();
            return;
        }
        auto* face = view(actor);
        if (!face) { (void)release(); return; }
        if (face_ && face != face_) forget(); // 3D replacement never inherits old expression backups.
        if (!owner_) {
            handle_ = actor->GetHandle();
            face_ = face;
            permit_ = std::make_shared<MergePermit>(reinterpret_cast<std::uintptr_t>(face), frame->cancellation);
            published_permit_.store(permit_, std::memory_order_release);
            REX::INFO("SYNTH lipsync: armed speaker={:08X} utterance={}", frame->actor_id, frame->utterance_id);
        }
        owner_ = frame;
        // The engine owns this same lock during its worker update. Never use the pinned
        // BSSpinLock::try_lock (wrong RW-lock relocation); use the verified flat adapter.
        const commonlib::TrySpinLock lock{face->lock};
        if (!lock.owns_lock()) return;
        if (face->dead || face->skip_update || frame->cancellation.is_cancelled()) {
            permit_->revoke();
            restore(face);
            return;
        }
        if (face->native_lip && (face->lip_state == 1 || face->lip_state == 2)) {
            permit_->revoke();
            applied_ = false; last_ = {};
            return; // Native speech owns the mouth during overlap.
        }
        const auto now = std::chrono::steady_clock::now();
        const auto delta = last_write_ == decltype(last_write_){} ? 1.0F / 60.0F :
            std::chrono::duration<float>(now - last_write_).count();
        apply_mouth(face, delta);
        last_write_ = now;
        permit_->publish(now);
    }

private:
    // Only speech-related human face controls; brows, eyes, cheeks and expressions remain native.
    static constexpr std::array<unsigned, 13> channels_{2, 8, 31, 11, 34, 21, 44, 22, 23, 25, 45, 46, 47};
    using LipUpdate = bool (*)(FlatSpeechFaceView*, float);

    using MergePermit = audio::SpeechMergePermit;

    static bool install() {
        if (attempted_) return false;
        attempted_ = true;
        const auto module = REX::FModule::GetExecutingModule();
        if (module.GetFileVersion().pack() != REL::Version{1,11,240,0}.pack()) {
            REX::WARN("SYNTH lipsync unavailable: unverified runtime");
            return false;
        }
        const auto base = module.GetBaseAddress();
        // Exact native call plus merge instruction: refuse patched or mismatched code.
        constexpr std::array<unsigned char, 5> call{0xE8,0x00,0xF1,0xFF,0xFF};
        constexpr std::array<unsigned char, 9> merge{0xF3,0x0F,0x5F,0x84,0x8F,0xF0,0,0,0};
        constexpr std::array<unsigned char, 5> original_entry{0x48,0x89,0x5C,0x24,0x08};
        if (std::memcmp(reinterpret_cast<void*>(base + 0x6D177B), call.data(), call.size()) != 0 ||
            std::memcmp(reinterpret_cast<void*>(base + 0x6D0880), original_entry.data(), original_entry.size()) != 0 ||
            std::memcmp(reinterpret_cast<void*>(base + 0x6D17B1), merge.data(), merge.size()) != 0) {
            REX::WARN("SYNTH lipsync unavailable: native call or expression layout signature differs");
            return false;
        }
        expected_vtable_ = base + 0x2506020;
        // Publish the original before patching: a concurrent native worker may immediately enter the hook.
        original_.store(reinterpret_cast<LipUpdate>(base + 0x6D0880), std::memory_order_release);
        const auto previous = REL::GetTrampoline().write_call<5>(base + 0x6D177B, lip_update);
        installed_ = previous == base + 0x6D0880;
        REX::INFO("SYNTH lipsync: verified flat speech-layer hook installed={}", installed_);
        return installed_;
    }

    static FlatSpeechFaceView* view(RE::Actor* actor) {
        if (!actor || !actor->currentProcess || !actor->currentProcess->middleHigh) return nullptr;
        auto* face = actor->currentProcess->middleHigh->faceAnimationData;
        if (!face || *reinterpret_cast<const std::uintptr_t*>(face) != expected_vtable_) return nullptr;
        return reinterpret_cast<FlatSpeechFaceView*>(face);
    }

    // Fallout may invoke this on an animation worker. SYNTH performs NO native object
    // reads/writes here: only original engine processing and a copied merge permission.
    static bool lip_update(FlatSpeechFaceView* face, float delta) {
        const auto native_result = original_.load(std::memory_order_acquire)(face, delta);
        const auto permit = published_permit_.load(std::memory_order_acquire);
        if (!permit || !permit->allows(reinterpret_cast<std::uintptr_t>(face), std::chrono::steady_clock::now()))
            return native_result;
        permit->merged.fetch_add(1, std::memory_order_relaxed);
        return true; // Engine merges the speech values already written by the game-thread pump.
    }

    // Game thread only, under the verified native try-lock, before publishing merge permission.
    static void apply_mouth(FlatSpeechFaceView* face, float delta) {
        if (!applied_) {
            for (std::size_t i = 0; i < channels_.size(); ++i) baseline_[i] = face->speech_expression[channels_[i]];
            applied_ = true;
        }
        const auto& m = owner_->mouth;
        const float open=m[0], wide=m[1], closed=m[2], teeth=m[3], round=m[4], funnel=m[5], tongue=m[6];
        const std::array<float,13> targets{
            open*.65F + wide*.25F + teeth*.12F + round*.35F + funnel*.18F + tongue*.22F,
            wide*.45F, wide*.45F, open*.20F + wide*.15F, open*.20F + wide*.15F,
            teeth*.20F + tongue*.10F, teeth*.20F + tongue*.10F,
            round*.45F + funnel*.55F, teeth*.35F, round*.45F + funnel*.65F,
            closed*.65F, round*.45F + funnel*.55F, closed*.25F};
        for (std::size_t i = 0; i < channels_.size(); ++i) {
            const auto target = std::clamp(std::isfinite(targets[i]) ? targets[i] : 0.0F, 0.0F, 0.82F);
            const auto blend = std::clamp(std::isfinite(delta) ? delta * 40.0F : 1.0F, 0.0F, 1.0F);
            last_[i] += (target - last_[i]) * blend;
            face->speech_expression[channels_[i]] = last_[i];
        }
        // Select native speech+emotion merge even when native audio is absent or shape is unchanged.
        ++applied_frames_;
    }

    // Caller owns the native lock. Only restore channels not taken over by native/other animation.
    static void restore(FlatSpeechFaceView* face) {
        if (!applied_) return;
        if (!(face->native_lip && (face->lip_state == 1 || face->lip_state == 2))) {
            for (std::size_t i = 0; i < channels_.size(); ++i) {
                const auto channel = channels_[i];
                if (face->speech_expression[channel] == last_[i]) {
                    const auto saved = std::isfinite(baseline_[i]) ? baseline_[i] : 0.0F;
                    face->speech_expression[channel] = saved;
                    const auto emotion = face->emotion_expression[channel];
                    face->final_expression[channel] = std::clamp(std::max(saved, std::isfinite(emotion) ? emotion : 0.0F),0.0F,1.0F);
                }
            }
            face->dirty = true;
        }
        applied_ = false;
    }

    // Ordinary completion restores only still-owned channels on the same live handle/FaceGen object.
    // A contended native lock is retried next pump, never waited on by the game thread.
    static bool release() {
        if (permit_) permit_->revoke();
        published_permit_.store(nullptr, std::memory_order_release);
        if (owner_) owner_.reset();
        if (applied_ && face_) {
            const auto actor = handle_.get();
            auto* face = view(actor ? actor->As<RE::Actor>() : nullptr);
            if (face == face_) {
                const commonlib::TrySpinLock lock{face->lock};
                if (!lock.owns_lock()) return false;
                restore(face);
            }
        }
        if (face_) REX::INFO("SYNTH lipsync: released; game-thread writes={} engine merges={}",
                            applied_frames_, permit_ ? permit_->merged.load(std::memory_order_relaxed) : 0);
        forget();
        return true;
    }

    // Save/load invalidation deliberately performs no actor resolution, lock acquisition or native write.
    static void forget() noexcept {
        owner_.reset(); face_ = nullptr; handle_ = {}; applied_ = false; last_ = {};
        if (permit_) permit_->revoke();
        published_permit_.store(nullptr, std::memory_order_release);
        permit_.reset(); last_write_ = {};
        applied_frames_ = 0; reported_ = false;
    }

    static inline std::optional<runtime::RuntimeSpeechFrame> owner_;
    static inline RE::ObjectRefHandle handle_;
    static inline FlatSpeechFaceView* face_{}; // Comparison key only; never dereferenced after re-resolution.
    static inline std::array<float,13> baseline_{}, last_{};
    static inline std::chrono::steady_clock::time_point last_write_{};
    static inline std::shared_ptr<MergePermit> permit_;
    static inline std::atomic<std::shared_ptr<MergePermit>> published_permit_;
    static inline std::uintptr_t expected_vtable_{};
    static inline std::atomic<LipUpdate> original_{};
    static inline bool attempted_{}, installed_{}, applied_{};
    static inline std::uint64_t applied_frames_{};
    static inline bool reported_{};
    static inline std::string unavailable_utterance_;
};

} // namespace synth::adapters
