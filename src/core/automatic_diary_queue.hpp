#pragma once

#include "core/rest_event_mailbox.hpp"
#include "core/snapshot.hpp"

#include <chrono>
#include <deque>
#include <memory>

namespace synth::core {

// Owned copied frames wait behind existing diary work; callers serialize access with the session diary mutex.
class AutomaticDiaryQueue final {
public:
    using Snapshot = std::shared_ptr<const RuntimeSnapshot>;
    struct Item final { Snapshot snapshot; RestStart reason; SnapshotClock::time_point expires; };
    static constexpr std::size_t capacity = 16;
    static constexpr auto lifetime = std::chrono::minutes{2};

    explicit AutomaticDiaryQueue(RuntimeGeneration generation) : generation_{generation} {}

    [[nodiscard]] bool push(Snapshot snapshot, RestStart reason, SnapshotClock::time_point now) {
        if (!snapshot || snapshot->generation() != generation_ || snapshot->captured_at() > now ||
            now >= snapshot->captured_at() + lifetime ||
            (reason != RestStart::sleep && reason != RestStart::wait) || items_.size() == capacity) return false;
        const auto expires = snapshot->captured_at() + lifetime;
        items_.push_back({std::move(snapshot), reason, expires});
        return true;
    }

    [[nodiscard]] std::size_t expire(SnapshotClock::time_point now) {
        return std::erase_if(items_, [now](const Item& item) { return now >= item.expires; });
    }
    [[nodiscard]] const Item* front() const noexcept { return items_.empty() ? nullptr : &items_.front(); }
    void pop() { if (!items_.empty()) items_.pop_front(); }
    void clear() noexcept { items_.clear(); }
    [[nodiscard]] std::size_t size() const noexcept { return items_.size(); }

private:
    RuntimeGeneration generation_;
    std::deque<Item> items_;
};

} // namespace synth::core
