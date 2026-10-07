#pragma once

#include "adapters/commonlib_capture.hpp"
#include "runtime/wait_recovery.hpp"
#include "adapters/native_fault_recorder.hpp"
#include <RE/M/MiddleHighProcessData.h>
#include <array>
#include <bit>
#include <mutex>
#include <random>

namespace synth::adapters::commonlib {

// Alias ownership survives engine interrupts; VM requests are serialized and contain only scalar identities.
class FlatWaitPackage final {
    struct Observation {
        std::uint32_t active{}, expected{};
        std::int32_t state{}, sit_state{};
        bool operator==(const Observation&) const = default;
    };
    struct Lease {
        RE::ActorHandle actor;
        std::uint32_t form{};
        core::CancellationToken cancellation;
        RE::NiPoint3 position;
        std::uint32_t cell{};
        std::uint64_t revision{};
        bool waiting{}, dirty{}, quarantined{};
        runtime::WaitHoldClock hold_clock;
        core::SnapshotClock::time_point next_verify{};
        bool verified{}, verification_reported{};
        std::optional<Observation> observation;
    };
    struct Job {
        runtime::WaitScriptReceipt receipt;
        std::size_t slot{};
        std::uint64_t revision{};
        bool reset{}, waiting{};
        core::SnapshotClock::time_point started;
        bool refresh{};
        std::optional<Observation> observation;
        float anchor_distance{};
    };
    inline static std::array<Lease, 256> leases_{};
    inline static std::mutex job_mutex_;
    inline static std::optional<Job> job_;
    inline static bool initialized_{};
    inline static std::uint64_t revision_{};
    inline static core::SnapshotClock::time_point next_sweep_{};
    inline static std::optional<core::SnapshotClock::time_point> paused_at_;
    inline static std::size_t refresh_cursor_{};

    // VM callbacks touch only copied job state. No game object lookup or mutation on a VM worker.
    static bool current(std::monostate, std::string_view ticket) {
        std::scoped_lock lock{job_mutex_};
        return job_ && job_->receipt.current(ticket);
    }
    static void finished(std::monostate, std::string_view ticket, bool success) {
        std::scoped_lock lock{job_mutex_};
        if (job_) job_->receipt.finish(ticket, success);
    }

    // Papyrus supplies copied diagnostics; only the game-thread completion path writes the log.
    static void observed(std::monostate, std::string_view ticket, std::int32_t active,
                         std::int32_t expected, std::int32_t state, std::int32_t sit_state, float distance) {
        std::scoped_lock lock{job_mutex_};
        if (job_ && job_->refresh && job_->receipt.current(ticket)) {
            job_->observation = Observation{std::bit_cast<std::uint32_t>(active),
                std::bit_cast<std::uint32_t>(expected), state, sit_state};
            job_->anchor_distance = distance;
        }
    }

    // Migration only: clear the old local-800 run-once slot by exact identity, never another owner's package.
    static void release_legacy(RE::Actor& actor) {
        const auto module = REX::FModule::GetExecutingModule();
        if (module.GetFileVersion().pack() != REL::Version{1, 11, 240, 0}.pack()) return;
        static REL::Relocation<std::uintptr_t> clear{REL::ID(2232344)};
        constexpr unsigned char anchor[]{0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xec,0x20};
        if (clear.address() != module.GetBaseAddress() + 0xD34620 ||
            std::memcmp(reinterpret_cast<const void*>(clear.address()), anchor, sizeof(anchor)) != 0) return;
        auto* data = RE::TESDataHandler::GetSingleton();
        auto* old = data ? data->LookupForm<RE::TESPackage>(0x800, "SYNTH.esp") : nullptr;
        if (!old || !actor.currentProcess || !actor.currentProcess->middleHigh) return;
        bool owned{};
        {
            auto& holder = actor.currentProcess->middleHigh->runOncePackage;
            const TrySpinLock lock{holder.packageLock};
            if (lock.owns_lock()) owned = holder.package == old;
        }
        if (owned) actor.currentProcess->SetRunOncePackage(nullptr, &actor);
    }

public:
    static bool bind(RE::BSScript::IVirtualMachine& vm) {
        bool bound = vm.BindNativeMethod(new RE::BSScript::NativeFunction("SYNTHWait", "Current", current, false));
        bound &= vm.BindNativeMethod(new RE::BSScript::NativeFunction("SYNTHWait", "Finished", finished, false));
        bound &= vm.BindNativeMethod(new RE::BSScript::NativeFunction("SYNTHWait", "Observed", observed, false));
        return bound;
    }

    // Reject saved/replayed stacks at the lifecycle boundary, then reset aliases in the next safe world.
    static void retire() {
        std::scoped_lock lock{job_mutex_};
        job_.reset();
        leases_ = {};
        initialized_ = false;
        next_sweep_ = {};
        paused_at_.reset();
    }

    static runtime::RuntimeActionResult apply(RE::Actor& actor, const runtime::RuntimeActionRequest& request) {
        using Status = runtime::RuntimeActionStatus;
        if (request.cancellation.is_cancelled() || core::SnapshotClock::now() >= request.deadline || menu_mode_active() ||
            actor.GetFormID() == 0x14 || actor.IsDeleted() || actor.IsDisabled() || actor.IsDead(false) || !actor.Get3D())
            return {Status::unavailable, "Wait requires the selected live, loaded NPC"};
        auto* data = RE::TESDataHandler::GetSingleton();
        if (!data || !data->LookupForm<RE::TESQuest>(0x801, "SYNTH.esp"))
            return {Status::unavailable, "Enable the updated SYNTH.esp wait controller"};
        const auto reference_handle = actor.GetHandle();
        RE::ActorHandle handle;
        static_assert(sizeof(handle) == sizeof(reference_handle));
        std::memcpy(static_cast<void*>(&handle), &reference_handle, sizeof(handle));
        if (!handle) return {Status::unavailable, "NPC reference handle is unavailable"};
        auto lease = std::ranges::find_if(leases_, [&](const auto& value) { return value.actor == handle; });
        const bool releasing = request.name == runtime::RuntimeActionName::release_wait;
        release_legacy(actor);
        if (lease == leases_.end()) {
            if (releasing) return {Status::succeeded, "No SYNTH wait command remains for this NPC"};
            lease = std::ranges::find_if(leases_, [](const auto& value) { return !value.actor && !value.quarantined; });
            if (lease == leases_.end()) return {Status::unavailable, "Wait capacity reached; release an existing wait first"};
        }
        if (lease->quarantined && !releasing) return {Status::unavailable, "Wait script timed out; reload before reusing this slot"};
        const auto* cell = actor.GetParentCell();
        if (!cell) return {Status::unavailable, "NPC cell is unavailable"};
        const bool quarantined = lease->quarantined;
        *lease = {handle, actor.GetFormID(), request.cancellation, actor.GetPosition(), cell->GetFormID(),
                  ++revision_, !releasing, true, false};
        lease->quarantined = quarantined;
        {
            std::scoped_lock lock{job_mutex_};
            if (job_ && !job_->reset && job_->slot == static_cast<std::size_t>(lease - leases_.begin()))
                job_->receipt.revoke_install(); // Drain the superseded job before moving this marker again.
        }
        return {Status::succeeded, releasing ? "Release Wait queued" : "Wait Here queued; applying NPC ownership"};
    }

    static void tick(core::SnapshotClock::time_point now) {
        if (menu_mode_active()) {
            if (!paused_at_) paused_at_ = now;
            return;
        }
        if (paused_at_) {
            std::scoped_lock lock{job_mutex_};
            if (job_) job_->started += now - *paused_at_;
            for (auto& lease : leases_) lease.hold_clock.pause(now - *paused_at_);
            paused_at_.reset();
        }
        if (now < next_sweep_) return;
        next_sweep_ = now + std::chrono::milliseconds{250};
        if (REX::FModule::GetExecutingModule().GetFileVersion().pack() != REL::Version{1, 11, 240, 0}.pack()) return;
        // Expiry revokes even an in-flight verification before its VM completion is consumed.
        for (std::size_t i = 0; i < leases_.size(); ++i) {
            auto& lease = leases_[i];
            if (lease.waiting && lease.hold_clock.expired(now)) {
                lease.waiting = false; lease.dirty = true; ++lease.revision;
                std::scoped_lock lock{job_mutex_};
                if (job_ && !job_->reset && job_->slot == i) job_->receipt.revoke_install();
                REX::INFO("SYNTH wait: actor {:08X} reached 90 seconds; releasing hold", lease.form);
            }
        }
        std::optional<Job> completed;
        {
            std::scoped_lock lock{job_mutex_};
            if (job_) {
                if (!job_->receipt.done() && now - job_->started < std::chrono::seconds{30}) return;
                completed = std::move(job_);
                job_.reset(); // A late completion or saved stack can no longer authorize any work.
            }
        }
        if (completed) {
            bool ok = completed->receipt.success();
            // A completed VM call is not proof of a stopped actor. Verify the engine state
            // on the game thread, independently of whichever workshop package is selected.
            if (!completed->reset && completed->waiting) {
                const auto actor = leases_[completed->slot].actor.get();
                ok = ok && actor && static_cast<RE::ACTOR_LIFE_STATE>(actor->lifeState) == RE::ACTOR_LIFE_STATE::kRestrained;
            }
            if (completed->reset) {
                initialized_ = ok;
                if (!ok) next_sweep_ = now + std::chrono::seconds{10};
                REX::INFO("SYNTH wait: alias controller reset {}", ok ? "complete" : "failed; retry deferred");
            } else if (completed->refresh) {
                auto& lease = leases_[completed->slot];
                if (lease.revision == completed->revision && lease.waiting) {
                    if (completed->observation && completed->observation != lease.observation) {
                        const auto& value = *completed->observation;
                        REX::INFO("SYNTH wait evidence: actor {:08X}, active {:08X}, expected {:08X}, "
                                  "state {:02X} (owner=1,running=2,alias=4,hold=8,anchor=16,sameCell=32,nearAnchor=64,ownedRestraint=128), "
                                  "sitState {}, anchorDistance {:.1f}", lease.form, value.active, value.expected,
                                  value.state, value.sit_state, completed->anchor_distance);
                        lease.observation = completed->observation;
                    }
                    if (!lease.verification_reported || ok != lease.verified)
                        REX::INFO("SYNTH wait: actor {:08X} movement restraint {}", lease.form,
                                  ok ? "verified" : "not active; restraint retry requested");
                    lease.verified = ok;
                    lease.verification_reported = true;
                    lease.next_verify = now + std::chrono::seconds{1};
                }
            } else {
                auto& lease = leases_[completed->slot];
                REX::INFO("SYNTH wait: slot {}, actor {:08X}, alias {} {}", completed->slot, lease.form,
                    completed->waiting ? "assignment and movement restraint" : "release", ok ? "confirmed" : "failed");
                if (!completed->receipt.done()) {
                    // A timed-out latent MoveTo may still finish. Do not reuse its anchor this generation.
                    // Revoke the old job, clear its alias, but never reuse a marker that may still move.
                    lease.quarantined = true; lease.waiting = false; lease.dirty = true;
                } else if (lease.revision == completed->revision) {
                    if (completed->waiting && ok && !lease.hold_clock.started()) lease.hold_clock.start(now);
                    if (!completed->waiting && ok) {
                        const bool quarantined = lease.quarantined;
                        lease = {}; lease.quarantined = quarantined;
                    }
                    else if (!ok && completed->waiting) { lease.waiting = false; lease.dirty = true; }
                    else if (!ok) { lease.dirty = true; next_sweep_ = now + std::chrono::seconds{5}; }
                }
            }
            if (!initialized_ || now < next_sweep_ - std::chrono::milliseconds{250}) return;
        }
        // Legacy migration is command-scoped in apply(). Do not sweep process
        // lists or mutate unrelated NPC packages during save-load recovery.
        std::size_t slot{};
        if (initialized_) {
            for (; slot < leases_.size(); ++slot) {
                auto& lease = leases_[slot];
                const auto actor = lease.actor.get();
                if (lease.actor && lease.waiting && (lease.cancellation.is_cancelled() || !actor ||
                    actor->IsDeleted() || actor->IsDead(false) || actor->IsDisabled())) {
                    lease.waiting = false; lease.dirty = true; ++lease.revision;
                }
                if (lease.actor && lease.dirty && (!lease.quarantined || !lease.waiting)) break;
            }
            if (slot == leases_.size()) {
                // Commands/releases take priority; round-robin checks cannot starve another NPC.
                for (std::size_t offset = 0; offset < leases_.size(); ++offset) {
                    const auto candidate = (refresh_cursor_ + offset) % leases_.size();
                    const auto& lease = leases_[candidate];
                    if (lease.actor && lease.waiting && !lease.quarantined && now >= lease.next_verify) {
                        slot = candidate; refresh_cursor_ = (candidate + 1) % leases_.size(); break;
                    }
                }
                if (slot == leases_.size()) return;
            }
        }
        auto* game = RE::GameVM::GetSingleton();
        const auto vm = game ? game->GetVM() : nullptr;
        if (!vm) return;
        std::random_device entropy;
        std::string ticket;
        for (int part = 0; part < 4; ++part) ticket += std::to_string(entropy()) + "-";
        const bool reset = !initialized_;
        auto& lease = leases_[slot];
        const bool refresh = !reset && lease.waiting && !lease.dirty;
        auto* data = RE::TESDataHandler::GetSingleton();
        auto* owner = data ? data->LookupForm<RE::TESQuest>(0x801, "SYNTH.esp") : nullptr;
        auto* anchor = data ? data->LookupForm<RE::TESObjectREFR>(0xA00 + static_cast<std::uint32_t>(slot), "SYNTH.esp") : nullptr;
        const auto actor = lease.actor.get();
        if (!owner || (!reset && lease.waiting && !actor)) return;
        auto* cell = actor ? actor->GetParentCell() : nullptr;
        if (!reset && lease.waiting && (!cell || cell->GetFormID() != lease.cell)) {
            lease.waiting = false; lease.dirty = true; ++lease.revision;
            return;
        }
        {
            std::scoped_lock lock{job_mutex_};
            job_ = Job{runtime::WaitScriptReceipt{ticket, lease.cancellation, !reset && lease.waiting},
                       slot, lease.revision, reset, !reset && lease.waiting, now, refresh};
        }
        const RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback;
        NativeFaultRecorder::phase(reset ? "wait Reset dispatch" : "wait Apply dispatch");
        // Do not marshal RE form pointers here: the pinned CommonLib CreateObject
        // overload uses the wrong VM slot on 1.11.240. Resolve IDs inside Papyrus.
        // bit_cast preserves dynamic/high-bit FormIDs in Papyrus's signed Int.
        const auto owner_id = std::bit_cast<std::int32_t>(owner->GetFormID());
        const auto actor_id = actor ? std::bit_cast<std::int32_t>(actor->GetFormID()) : 0;
        const auto anchor_id = anchor ? std::bit_cast<std::int32_t>(anchor->GetFormID()) : 0;
        const auto cell_id = cell ? std::bit_cast<std::int32_t>(cell->GetFormID()) : 0;
        bool queued{};
        if (reset) queued = vm->DispatchStaticCall("SYNTHWait", "ResetByFormId", callback, ticket, owner_id);
        else if (refresh) queued = vm->DispatchStaticCall("SYNTHWait", "RefreshByFormIds", callback, ticket,
                                    owner_id, static_cast<std::int32_t>(slot), actor_id);
        else queued = vm->DispatchStaticCall("SYNTHWait", "ApplyByFormIds", callback, ticket, static_cast<std::int32_t>(slot),
                                    owner_id, actor_id, anchor_id, cell_id,
                                    lease.position.x, lease.position.y, lease.position.z, lease.waiting);
        NativeFaultRecorder::phase("wait dispatch returned");
        if (queued && !refresh) REX::INFO("SYNTH wait: {} scalar dispatch accepted", reset ? "Reset" : "Apply");
        if (queued && !reset) lease.dirty = false;
        if (!queued) {
            finished({}, ticket, false);
            next_sweep_ = now + std::chrono::seconds{5};
            REX::WARN("SYNTH wait: script dispatch rejected; check SYNTHWait.pex installation");
        }
    }
};
}
