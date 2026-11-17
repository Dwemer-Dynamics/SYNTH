#pragma once

#include "protocol_native/v1_codec.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <string_view>

namespace synth::client {

// Why the current conversation ended. Kept for diagnostics; every reason retires the id.
enum class ConversationEnd { load, reconnect, cell_change, idle, player_cancel, halt };

// One conversation_id per conversation (dialogue.conversation). The id is created here, never by the server. It
// belongs to the game thread's view of the player's conversation but is read by the network lane when a request is
// encoded, so it is a small mutex-guarded value. It lives in memory only: a load, reconnect (new generation),
// player cell change, explicit cancel, halt or a long idle gap all end it, and the next player request starts a
// fresh one. Nothing is persisted, so a reloaded game never resumes an old conversation.
class ConversationContinuity final {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr auto default_idle_limit = std::chrono::minutes{10};

    explicit ConversationContinuity(Clock::duration idle_limit = default_idle_limit) noexcept
        : idle_limit_{idle_limit}, engine_{seed()} {}

    // The id the next request belongs to. Starts a conversation when there is none or the last one went idle.
    // Only a request that carries the id counts as activity, so receipts, contexts and cancels never extend it.
    [[nodiscard]] std::string touch(Clock::time_point now = Clock::now()) {
        std::scoped_lock lock{mutex_};
        if (!id_.empty() && now - last_activity_ > idle_limit_) retire_locked(ConversationEnd::idle);
        if (id_.empty()) id_ = make_id_locked();
        last_activity_ = now;
        return id_;
    }

    // The live id without starting or extending anything; empty when none.
    [[nodiscard]] std::string current() const {
        std::scoped_lock lock{mutex_};
        return id_;
    }

    // Ends the conversation. Returns the id that ended, when there was one.
    std::optional<std::string> end(ConversationEnd reason) {
        std::scoped_lock lock{mutex_};
        if (id_.empty()) return std::nullopt;
        auto ended = id_;
        retire_locked(reason);
        return ended;
    }

    // A cell change retires the conversation only when the player really moved to another cell.
    std::optional<std::string> observe_cell(const std::string& cell) {
        std::scoped_lock lock{mutex_};
        const bool changed = !cell_.empty() && cell_ != cell;
        cell_ = cell;
        if (!changed || id_.empty()) return std::nullopt;
        auto ended = id_;
        retire_locked(ConversationEnd::cell_change);
        return ended;
    }

    [[nodiscard]] std::optional<ConversationEnd> last_end() const {
        std::scoped_lock lock{mutex_};
        return last_end_;
    }

    // Lowercase UUID version 4 from the engine. Exposed so tests can prove the exact shape.
    [[nodiscard]] static std::string format_uuid_v4(const std::array<std::uint8_t, 16>& bytes) {
        auto b = bytes;
        b[6] = static_cast<std::uint8_t>((b[6] & 0x0F) | 0x40);
        b[8] = static_cast<std::uint8_t>((b[8] & 0x3F) | 0x80);
        constexpr std::string_view hex = "0123456789abcdef";
        std::string out;
        out.reserve(36);
        for (std::size_t i = 0; i < b.size(); ++i) {
            if (i == 4 || i == 6 || i == 8 || i == 10) out.push_back('-');
            out.push_back(hex[b[i] >> 4]);
            out.push_back(hex[b[i] & 0x0F]);
        }
        return out;
    }

private:
    static std::mt19937_64 seed() {
        std::random_device device;
        std::seed_seq sequence{device(), device(), device(), device()};
        return std::mt19937_64{sequence};
    }

    std::string make_id_locked() {
        std::array<std::uint8_t, 16> bytes{};
        for (std::size_t i = 0; i < bytes.size(); i += 8) {
            auto word = engine_();
            for (std::size_t j = 0; j < 8; ++j) bytes[i + j] = static_cast<std::uint8_t>(word >> (8 * j));
        }
        return format_uuid_v4(bytes);
    }

    void retire_locked(ConversationEnd reason) {
        id_.clear();
        last_end_ = reason;
    }

    mutable std::mutex mutex_;
    Clock::duration idle_limit_;
    std::mt19937_64 engine_;
    std::string id_, cell_;
    Clock::time_point last_activity_{};
    std::optional<ConversationEnd> last_end_;
};

}  // namespace synth::client
