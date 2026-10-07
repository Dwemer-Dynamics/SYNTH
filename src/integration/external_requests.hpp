#pragma once

#include "integration/synth_external_api.h"
#include "json/json.hpp"
#include "targeting/targeting.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>

namespace synth::integration {

// Session-mutex protected: persistent conversation ownership is distinct from outstanding work.
class ExternalConversationOwner final {
public:
    void begin(std::optional<std::string> actor) { owner_ = std::move(actor); idle(); }
    void idle() { work_.reset(); ambiguous_ = false; }
    void claim(std::optional<std::string> actor) {
        if (!actor || (work_ && *work_ != *actor)) ambiguous_ = true;
        if (!ambiguous_) work_ = std::move(actor);
    }
    void end(const std::string& actor) { if (owner_ && *owner_ == actor) owner_.reset(); }
    [[nodiscard]] const std::optional<std::string>& owner() const noexcept { return owner_; }
    [[nodiscard]] bool allows_ask(const std::string& actor, bool pipeline_idle) const {
        return (!owner_ || *owner_ == actor) &&
            (pipeline_idle || (!ambiguous_ && work_ && *work_ == actor));
    }
private:
    // Empty owner string represents a narrator/unresolved player turn, never an arbitrary NPC.
    std::optional<std::string> owner_, work_;
    bool ambiguous_{};
};

// Admission copies bounded native inputs only; the ordinary game pump owns execution.
class ExternalRequests final {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::size_t capacity = 16;
    static constexpr auto lifetime = std::chrono::seconds{2};

    [[nodiscard]] std::uint64_t get_epoch() const noexcept {
        const auto state = state_.load(std::memory_order_acquire);
        return state & 1 ? state : 0;
    }

    // One atomic word prevents reopening an old admission epoch across load/halt/menu gates.
    void set_available(bool available) noexcept {
        auto state = state_.load(std::memory_order_acquire);
        for (;;) {
            if (static_cast<bool>(state & 1) == available || state == std::numeric_limits<std::uint64_t>::max()-1) return;
            if (state_.compare_exchange_weak(state, state+1, std::memory_order_acq_rel)) return;
        }
    }

    [[nodiscard]] std::uint32_t submit(const SynthExternalRequestV1* input, Clock::time_point now = Clock::now()) noexcept {
        if (!input || input->size != sizeof(SynthExternalRequestV1) || input->reserved != 0 ||
            input->actor_form_id == 0 || input->actor_form_id == 0x14 ||
            input->kind < SYNTH_EXTERNAL_SPEAK_EXACT || input->kind > SYNTH_EXTERNAL_OPEN_PROMPT) return SYNTH_EXTERNAL_INVALID;
        const auto* end = static_cast<const char*>(std::memchr(input->text, 0, sizeof(input->text)));
        if (!end) return SYNTH_EXTERNAL_INVALID;
        std::string_view text{input->text, static_cast<std::size_t>(end-input->text)};
        const auto whitespace = [](char c) { return c==' ' || c=='\t' || c=='\r' || c=='\n'; };
        while (!text.empty() && whitespace(text.front())) text.remove_prefix(1);
        while (!text.empty() && whitespace(text.back())) text.remove_suffix(1);
        const auto no_text = input->kind == SYNTH_EXTERNAL_COMMENT || input->kind == SYNTH_EXTERNAL_OPEN_PROMPT;
        if (!json::detail::valid_utf8(text) || (no_text ? !text.empty() : text.empty())) return SYNTH_EXTERNAL_INVALID;
        const auto epoch = get_epoch();
        if (!epoch) return SYNTH_EXTERNAL_UNAVAILABLE;
        if (input->epoch != epoch) return SYNTH_EXTERNAL_STALE;
        std::unique_lock lock{mutex_, std::try_to_lock};
        if (!lock.owns_lock()) return SYNTH_EXTERNAL_BUSY;
        if (get_epoch() != epoch) return SYNTH_EXTERNAL_STALE;
        if (queue_epoch_ != epoch) { head_=count_=0; queue_epoch_=epoch; }
        if (count_ == capacity) return SYNTH_EXTERNAL_BUSY;
        auto& slot = queue_[(head_+count_)%capacity];
        slot = {};
        slot.request.size=sizeof(SynthExternalRequestV1);
        slot.request.kind=input->kind;
        slot.request.actor_form_id=input->actor_form_id;
        slot.request.epoch=epoch;
        std::memcpy(slot.request.text,text.data(),text.size());
        slot.deadline=now+lifetime;
        ++count_;
        return SYNTH_EXTERNAL_ACCEPTED;
    }

    [[nodiscard]] std::optional<SynthExternalRequestV1> take(Clock::time_point now = Clock::now()) noexcept {
        std::unique_lock lock{mutex_, std::try_to_lock};
        if (!lock.owns_lock()) return {};
        const auto epoch=get_epoch();
        if (!epoch || queue_epoch_!=epoch) { head_=count_=0; return {}; }
        while (count_) {
            const auto slot=queue_[head_];
            head_=(head_+1)%capacity; --count_;
            if (now < slot.deadline) return slot.request;
        }
        return {};
    }

private:
    struct Pending { SynthExternalRequestV1 request{}; Clock::time_point deadline{}; };
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    std::atomic<std::uint64_t> state_{0};
    std::mutex mutex_;
    std::array<Pending,capacity> queue_{};
    std::size_t head_{},count_{};
    std::uint64_t queue_epoch_{};
};

// External callers name an exact member of this fresh scene, never a crosshair/nearest substitute.
[[nodiscard]] inline const core::ActorSnapshot* external_actor(const core::RuntimeSnapshot& snapshot,
    std::uint32_t form_id, core::SnapshotClock::time_point now, double maximum_distance) {
    if (now < snapshot.captured_at() || now-snapshot.captured_at() > std::chrono::milliseconds{100} ||
        !std::isfinite(maximum_distance) || maximum_distance <= 0) return nullptr;
    auto origin=snapshot.player_pose().position();
    if (snapshot.variant()==core::RuntimeVariant::vr) {
        if (!snapshot.hmd_pose() || !snapshot.hmd_pose()->fresh_at(now) || snapshot.hmd_pose()->frame()!=snapshot.frame()) return nullptr;
        origin=snapshot.hmd_pose()->pose().position();
    }
    for (const auto& actor:snapshot.actors()) {
        if (actor.form_id()!=form_id) continue;
        if (form_id==snapshot.player().form_id() || actor.playthrough_id()!=snapshot.player().playthrough_id() ||
            !targeting::dialogue_actor_available(actor) || actor.talking_to_player() ||
            targeting::distance(origin,actor.position()) > maximum_distance) return nullptr;
        return &actor;
    }
    return nullptr;
}
}
