#pragma once

#include "protocol_native/v1_codec.hpp"
#include <array>
#include <atomic>
#include <mutex>

namespace synth::client {

// One session/generation's copied captures. The game pump only attempts admission; workers prepare and finish.
class ActorEventDelivery final {
public:
    static constexpr std::size_t capacity=4;
    static constexpr std::size_t actor_capacity=16;
    using Capture=std::shared_ptr<const BoundActorEventScene>;
    struct Prepared final {
        Capture capture;
        std::uint64_t serial{};
        std::vector<context::ActorIdentity> actors;

        // Resolve only inside the retained immutable scene, without hearing filters or any live engine lookup.
        [[nodiscard]] std::vector<const core::ActorSnapshot*> resolve_actor_states() const {
            if (!capture || !capture->scene || actors.empty() || actors.size()>actor_capacity ||
                actors.front()!=context::identity_of(capture->scene->player()))
                throw std::invalid_argument{"actor fragment state owner is missing"};
            std::vector<const core::ActorSnapshot*> result;
            for (const auto& required:actors) {
                const auto& scene=*capture->scene;
                const core::ActorSnapshot* resolved=scene.player().form_id()==required.form().form_id() ? &scene.player() : nullptr;
                for (const auto& actor:scene.actors()) if (actor.form_id()==required.form().form_id()) {
                    if (resolved) throw std::invalid_argument{"ambiguous actor event state"};
                    resolved=&actor;
                }
                if (!resolved || resolved->disabled() || context::identity_of(*resolved)!=required ||
                    std::ranges::find(result,resolved)!=result.end()) throw std::invalid_argument{"actor event state is unavailable"};
                result.push_back(resolved);
            }
            return result;
        }

        [[nodiscard]] protocol_native::NativeActorBatch packet(core::SnapshotClock::time_point now) const {
            if (now<capture->scene->captured_at()) throw std::invalid_argument{"actor delivery clock moved backwards"};
            const auto age=std::chrono::duration_cast<std::chrono::milliseconds>(now-capture->scene->captured_at()).count();
            return {serial,static_cast<std::uint64_t>(age),capture};
        }
    };

    ActorEventDelivery(std::string session,core::RuntimeGeneration generation)
        : session_{std::move(session)},generation_{generation} {
        if (session_.empty() || !generation_.valid()) throw std::invalid_argument{"invalid actor delivery owner"};
    }
    void stop() noexcept { stopped_.store(true,std::memory_order_release); }
    [[nodiscard]] bool pending() const noexcept { return pending_.load(std::memory_order_acquire)!=0; }
    [[nodiscard]] bool full() const noexcept { return pending_.load(std::memory_order_acquire)>=capacity; }
    [[nodiscard]] std::uint64_t dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }

    // No allocations or waits here; the binder has already copied and validated the scene on the safe pump.
    [[nodiscard]] bool enqueue(Capture capture) {
        if (!capture || !capture->scene || capture->session_id!=session_ || !(capture->native_epoch&1) ||
            capture->scene->generation()!=generation_ || capture->scene->variant()!=core::RuntimeVariant::flat ||
            capture->events.empty() || capture->events.size()>core::ActorEventMailbox::capacity ||
            stopped_.load(std::memory_order_acquire)) return false;
        std::unique_lock lock{mutex_,std::try_to_lock};
        if (!lock.owns_lock() || pending_.load(std::memory_order_relaxed)==capacity) {
            dropped_.fetch_add(capture->events.size(),std::memory_order_relaxed);return false;
        }
        if (stopped_.load(std::memory_order_acquire)) return false;
        if (latest_ && (capture==latest_ || capture->scene->player().form_id()!=latest_->scene->player().form_id() ||
            capture->scene->player().origin_plugin()!=latest_->scene->player().origin_plugin() ||
            capture->scene->player().playthrough_id()!=latest_->scene->player().playthrough_id() ||
            capture->native_epoch<latest_->native_epoch || capture->scene->frame()<latest_->scene->frame() ||
            capture->scene->captured_at()<latest_->scene->captured_at() ||
            capture->scene->game_time_ticks()<latest_->scene->game_time_ticks())) return false;
        slots_[(head_+pending_.load(std::memory_order_relaxed))%capacity]=capture;
        latest_=std::move(capture);
        pending_.fetch_add(1,std::memory_order_release);
        return true;
    }

    // Split only between records to fit the existing 16-state contract; never discard a resolved secondary identity.
    [[nodiscard]] std::shared_ptr<const Prepared> prepare() {
        std::scoped_lock lock{mutex_};
        if (stopped_.load(std::memory_order_acquire) || !pending_.load(std::memory_order_relaxed)) return {};
        if (prepared_) return prepared_;
        const auto& source=slots_[head_];
        if (serial_==core::RuntimeGeneration::maximum_wire_value) {
            stopped_.store(true,std::memory_order_release);return {};
        }
        auto part=std::make_shared<BoundActorEventScene>();
        part->session_id=source->session_id;part->native_epoch=source->native_epoch;part->scene=source->scene;
        auto result=std::make_shared<Prepared>();
        result->actors.push_back(context::identity_of(source->scene->player()));
        for (auto index=offset_;index<source->events.size();++index) {
            const auto& event=source->events[index];
            auto actors=result->actors;
            for (const auto* identity:{&event.subject,event.other ? &*event.other : nullptr}) {
                if (identity && std::ranges::find(actors,*identity)==actors.end()) actors.push_back(*identity);
            }
            if (actors.size()>actor_capacity) break;
            result->actors=std::move(actors);
            part->events.push_back(event);
        }
        result->capture=std::move(part);result->serial=++serial_;
        prepared_=std::move(result);
        return stopped_.load(std::memory_order_acquire) ? nullptr : prepared_;
    }

    // Ambiguous transport outcomes keep the exact fragment. Only its terminal receipt advances the ordered source.
    [[nodiscard]] bool finish(const std::shared_ptr<const Prepared>& batch,bool acknowledged) {
        std::scoped_lock lock{mutex_};
        if (stopped_.load(std::memory_order_acquire) || !batch || batch!=prepared_) return false;
        const auto count=batch->capture->events.size();
        if (!acknowledged) dropped_.fetch_add(count,std::memory_order_relaxed);
        offset_+=count;prepared_.reset();
        if (offset_==slots_[head_]->events.size()) {
            slots_[head_].reset();offset_=0;head_=(head_+1)%capacity;
            pending_.fetch_sub(1,std::memory_order_release);
        }
        return true;
    }

private:
    const std::string session_;
    const core::RuntimeGeneration generation_;
    std::atomic_bool stopped_{};
    std::atomic_size_t pending_{};
    std::atomic_uint64_t dropped_{};
    std::mutex mutex_;
    std::array<Capture,capacity> slots_{};
    std::size_t head_{},offset_{};
    std::uint64_t serial_{};
    Capture latest_;
    std::shared_ptr<const Prepared> prepared_;
};

} // namespace synth::client
