#pragma once
#include "runtime/fallout_runtime.hpp"
#include <mutex>

namespace synth::client {
// Worker callbacks register copied identities. Only the game-thread runtime may apply the holds.
class ConversationHolds final {
    struct Entry { runtime::RuntimeConversationHold hold; core::SnapshotClock::time_point expires; };
    std::mutex mutex_;
    std::vector<Entry> entries_;
    bool suspended_{};
public:
    void clear() { std::scoped_lock lock{mutex_}; entries_.clear(); suspended_=false; }
    void suspend(const core::CancellationToken& token) {
        std::scoped_lock lock{mutex_}; if (!token.is_cancelled()) { suspended_=true; entries_.clear(); }
    }
    void add(runtime::RuntimeActorIdentity actor, core::CancellationToken token,
             core::SnapshotClock::time_point now=core::SnapshotClock::now()) {
        std::scoped_lock lock{mutex_};
        if (suspended_ || token.is_cancelled() || actor.form_id==0 || actor.form_id==0x14 ||
            actor.origin_plugin.empty() || actor.playthrough_id.empty()) return;
        std::erase_if(entries_,[](const auto& entry){ return entry.hold.cancellation.is_cancelled(); });
        for (auto& entry:entries_) if (entry.hold.actor.form_id==actor.form_id &&
            entry.hold.actor.origin_plugin==actor.origin_plugin && entry.hold.actor.playthrough_id==actor.playthrough_id) return;
        if (entries_.size()<16) entries_.push_back({{std::move(actor),token},now+std::chrono::seconds{90}});
    }
    [[nodiscard]] std::vector<runtime::RuntimeConversationHold> frame(bool busy, core::RuntimeGeneration generation,
            core::SnapshotClock::time_point now=core::SnapshotClock::now()) {
        std::scoped_lock lock{mutex_};
        if (!busy) { entries_.clear(); return {}; }
        std::erase_if(entries_,[&](const auto& entry){return entry.hold.cancellation.is_cancelled() ||
            entry.hold.cancellation.generation()!=generation || now>=entry.expires;});
        std::vector<runtime::RuntimeConversationHold> result;
        if (!suspended_) for (const auto& entry:entries_) result.push_back(entry.hold);
        return result;
    }
};
} // namespace synth::client
