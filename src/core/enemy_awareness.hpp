#pragma once

#include "combat_target.hpp"
#include <array>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace synth::core {

struct VisibleEnemySnapshot final {
    CombatantIdentity actor;
    double distance{};
    bool hostile{}, in_combat{}, visible{};
    bool operator==(const VisibleEnemySnapshot&) const = default;
};
struct RecentEnemySnapshot final {
    CombatantIdentity actor;
    std::string observation;
    std::uint64_t age_ms{};
    bool operator==(const RecentEnemySnapshot&) const = default;
};
struct EnemyAwarenessSnapshot final {
    bool available{};
    std::vector<VisibleEnemySnapshot> current;
    // Private capture evidence only. A visually observed corpse cannot establish prior hostility by itself.
    std::vector<CombatantIdentity> observed_dead;
    std::vector<RecentEnemySnapshot> recent;
    bool operator==(const EnemyAwarenessSnapshot&) const = default;
};

inline bool same_enemy(const CombatantIdentity& left,const CombatantIdentity& right) noexcept {
    const auto lower=[](unsigned char value){return value>='A' && value<='Z' ? value+('a'-'A') : value;};
    return left.form_id==right.form_id && left.playthrough_id==right.playthrough_id &&
        left.origin_plugin.size()==right.origin_plugin.size() &&
        std::equal(left.origin_plugin.begin(),left.origin_plugin.end(),right.origin_plugin.begin(),
            [&](unsigned char a,unsigned char b){return lower(a)==lower(b);});
}
inline bool valid_enemy_awareness(const EnemyAwarenessSnapshot& value,std::uint32_t owner,
                                  std::string_view playthrough,bool alive,bool disabled) noexcept {
    if (!value.available) return value.current.empty() && value.observed_dead.empty() && value.recent.empty();
    if (owner==0 || owner==0x14 || !alive || disabled || value.current.size()>8 || value.recent.size()>8 || value.observed_dead.size()>8) return false;
    const auto valid_identity=[&](const CombatantIdentity& actor) {
        return actor.valid() && actor.playthrough_id==playthrough && actor.form_id!=owner && actor.form_id!=0x14;
    };
    for (std::size_t i=0;i<value.current.size();++i) {
        const auto& item=value.current[i];
        if (!valid_identity(item.actor) || !std::isfinite(item.distance) || item.distance<0 || item.distance>2000 || !item.hostile || !item.visible) return false;
        for (std::size_t j=0;j<i;++j) if (same_enemy(item.actor,value.current[j].actor)) return false;
    }
    for (std::size_t i=0;i<value.observed_dead.size();++i) {
        if (!valid_identity(value.observed_dead[i])) return false;
        for (std::size_t j=0;j<i;++j) if (same_enemy(value.observed_dead[i],value.observed_dead[j])) return false;
        for (const auto& live:value.current) if (same_enemy(value.observed_dead[i],live.actor)) return false;
    }
    for (std::size_t i=0;i<value.recent.size();++i) {
        const auto& item=value.recent[i];
        if (!valid_identity(item.actor) || item.age_ms>30000 || (item.observation!="seen_hostile" && item.observation!="seen_dead")) return false;
        for (std::size_t j=0;j<i;++j) if (same_enemy(item.actor,value.recent[j].actor)) return false;
        for (const auto& live:value.current) if (same_enemy(item.actor,live.actor)) return false;
    }
    return true;
}

// Fair bounded windows. Monotonic rotation contains no actor/session knowledge and never grants visibility.
inline std::array<std::size_t,4> enemy_awareness_scan_counts(const std::array<std::size_t,4>& sizes) noexcept {
    std::array<std::size_t,4> counts{};std::size_t tier{};
    for (std::size_t read=0;read<64;++read) {
        bool found{};
        for (std::size_t attempt=0;attempt<4;++attempt) {
            const auto next=tier;tier=(tier+1)%4;
            if (counts[next]==sizes[next]) continue;
            ++counts[next];found=true;break;
        }
        if (!found) break;
    }
    return counts;
}

// One known observer per session; no scans before selection or after Stop. Cadence uses monotonic time.
class EnemyAwarenessSampler final {
public:
    void clear() noexcept { owner_.reset();next_=0;combat_until_=0;generation_=0;last_now_=0; }
    void select(CombatantIdentity owner,std::uint64_t generation) {
        if (!owner.valid() || owner.form_id==0x14 || !generation) throw std::invalid_argument{"invalid awareness sampler owner"};
        if (generation_!=generation || (owner_ && owner_->playthrough_id!=owner.playthrough_id)) clear();
        owner_=std::move(owner);generation_=generation;
    }
    [[nodiscard]] std::optional<CombatantIdentity> due(std::uint64_t now,bool in_combat,std::uint64_t generation) {
        if (!owner_ || generation_!=generation) return {};
        if (now<last_now_) { clear();return {}; }
        last_now_=now;
        if (in_combat) combat_until_=now+10000;
        if (now<next_ || !combat_until_ || now>combat_until_) return {};
        next_=now+1000;return owner_;
    }
    [[nodiscard]] bool owns(const CombatantIdentity& actor,std::uint64_t generation) const noexcept {
        return owner_ && generation_==generation && same_enemy(*owner_,actor);
    }
private:
    std::optional<CombatantIdentity> owner_;
    std::uint64_t next_{},combat_until_{},generation_{},last_now_{};
};

// Session-owned, bounded observations. Call with monotonic elapsed milliseconds, never game/calendar time.
// The owning PluginSession clears this on Stop/retirement; generation/playthrough changes also clear it here.
class EnemyAwarenessCache final {
public:
    void clear() noexcept { owners_.clear(); playthrough_.clear(); generation_=0; last_now_=0; }
    [[nodiscard]] EnemyAwarenessSnapshot observe(const CombatantIdentity& owner,const EnemyAwarenessSnapshot& captured,
                                                 std::uint64_t generation,std::uint64_t now) {
        if (!owner.valid() || owner.form_id==0x14 || !generation || !captured.available ||
            !valid_enemy_awareness(captured,owner.form_id,owner.playthrough_id,true,false))
            throw std::invalid_argument{"invalid enemy observation owner"};
        if (generation_!=generation || playthrough_!=owner.playthrough_id || now<last_now_) clear();
        generation_=generation;playthrough_=owner.playthrough_id;last_now_=now;
        for (auto& item:owners_) std::erase_if(item.seen,[&](const Seen& seen) { return now<seen.when || now-seen.when>30000; });
        std::erase_if(owners_,[](const Owner& item){return item.seen.empty();});
        auto found=std::ranges::find_if(owners_,[&](const Owner& item){return same_enemy(item.actor,owner);});
        if (found==owners_.end()) {
            if (owners_.size()==16) owners_.erase(owners_.begin());
            owners_.push_back({owner,{}});found=std::prev(owners_.end());
        }
        auto& seen=found->seen;
        // Only a prior, unexpired hostile observation can be promoted to an actually witnessed death.
        for (const auto& dead:captured.observed_dead) {
            auto known=std::ranges::find_if(seen,[&](const Seen& item){return same_enemy(item.actor,dead);});
            if (known!=seen.end() && known->observation!="seen_dead") { known->actor=dead;known->observation="seen_dead";known->when=now; }
        }
        for (const auto& live:captured.current) {
            std::erase_if(seen,[&](const Seen& item){return same_enemy(item.actor,live.actor);});
            if (seen.size()==16) seen.erase(seen.begin());
            seen.push_back({live.actor,"seen_hostile",now});
        }
        EnemyAwarenessSnapshot result{true,captured.current,{},{}};
        for (auto item=seen.rbegin();item!=seen.rend() && result.recent.size()<8;++item) {
            if (std::ranges::any_of(result.current,[&](const auto& live){return same_enemy(live.actor,item->actor);})) continue;
            result.recent.push_back({item->actor,item->observation,now-item->when});
        }
        return result;
    }
private:
    struct Seen { CombatantIdentity actor;std::string observation;std::uint64_t when{}; };
    struct Owner { CombatantIdentity actor;std::vector<Seen> seen; };
    std::vector<Owner> owners_;
    std::string playthrough_;
    std::uint64_t generation_{},last_now_{};
};
} // namespace synth::core
