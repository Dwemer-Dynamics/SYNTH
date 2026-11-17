#pragma once

#include "core/snapshot.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace synth::core {

// What the player just read. Copied game text only: it carries no claim that anyone else read or heard it.
enum class ReadingKind : std::uint8_t { book, note, magazine, holotape, terminal };

[[nodiscard]] constexpr std::string_view reading_kind_name(ReadingKind kind) noexcept {
    switch (kind) {
        case ReadingKind::book: return "book";
        case ReadingKind::note: return "note";
        case ReadingKind::magazine: return "magazine";
        case ReadingKind::holotape: return "holotape";
        case ReadingKind::terminal: return "terminal";
    }
    return {};
}

inline constexpr std::size_t reading_title_limit = 128;  // code points
inline constexpr std::size_t reading_text_limit = 4000;  // code points

namespace reading_detail {

// Length of the well-formed UTF-8 sequence at the start of `text`, or zero when it is malformed.
[[nodiscard]] inline std::size_t sequence_length(std::string_view text) noexcept {
    const auto byte = [&](std::size_t i) { return static_cast<unsigned char>(text[i]); };
    const auto cont = [&](std::size_t i) { return i < text.size() && (byte(i) & 0xC0) == 0x80; };
    if (text.empty()) return 0;
    const auto lead = byte(0);
    if (lead < 0x80) return 1;
    if (lead >= 0xC2 && lead <= 0xDF) return cont(1) ? 2 : 0;
    if (lead >= 0xE0 && lead <= 0xEF) {
        if (!cont(1) || !cont(2)) return 0;
        if (lead == 0xE0 && byte(1) < 0xA0) return 0;   // overlong
        if (lead == 0xED && byte(1) >= 0xA0) return 0;  // surrogate
        return 3;
    }
    if (lead >= 0xF0 && lead <= 0xF4) {
        if (!cont(1) || !cont(2) || !cont(3)) return 0;
        if (lead == 0xF0 && byte(1) < 0x90) return 0;   // overlong
        if (lead == 0xF4 && byte(1) >= 0x90) return 0;  // above U+10FFFF
        return 4;
    }
    return 0;
}

// Game text carries Scaleform markup, page-break tokens and alias placeholders. Remove them without
// interpreting anything; unmatched or oversized angle brackets stay as the prose they probably are.
[[nodiscard]] inline std::string clean(std::string_view raw, bool keep_newlines) {
    std::string out;
    out.reserve(raw.size());
    std::size_t i = 0;
    while (i < raw.size()) {
        const auto rest = raw.substr(i);
        if (rest.starts_with("[pagebreak]")) {
            out += keep_newlines ? "\n\n" : " ";
            i += 11;
            continue;
        }
        if (rest.front() == '<' && rest.size() > 1 && (std::isalpha(static_cast<unsigned char>(rest[1])) || rest[1] == '/')) {
            const auto close = rest.find('>');
            if (close != std::string_view::npos && close <= 64 && close > 1 && rest.find('<', 1) > close) {
                i += close + 1;
                continue;
            }
        }
        const auto length = sequence_length(rest);
        if (length == 0) { ++i; continue; }  // malformed byte: dropped, never repaired into other text
        if (length == 1) {
            const auto c = static_cast<unsigned char>(rest.front());
            if (c == '\r') {
                if (rest.size() > 1 && rest[1] == '\n') ++i;
                out += keep_newlines ? '\n' : ' ';
            } else if (c == '\n') {
                out += keep_newlines ? '\n' : ' ';
            } else if (c == '\t') {
                out += ' ';
            } else if (c >= 0x20 && c != 0x7F) {
                out += static_cast<char>(c);
            }
            ++i;
            continue;
        }
        out.append(rest.substr(0, length));
        i += length;
    }
    return out;
}

[[nodiscard]] inline std::string squeeze(std::string_view text) {
    // At most one blank line between paragraphs, no trailing whitespace on a line, no outer blank space.
    std::string out;
    std::size_t newlines = 0;
    for (const char c : text) {
        if (c == '\n') { ++newlines; continue; }
        if (c == ' ' && (out.empty() || out.back() == '\n' || out.back() == ' ') && newlines == 0) continue;
        if (newlines != 0) {
            while (!out.empty() && out.back() == ' ') out.pop_back();
            if (!out.empty()) out.append(std::min<std::size_t>(newlines, 2), '\n');
            newlines = 0;
            if (c == ' ') continue;
        }
        out += c;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

// Longest prefix of at most `limit` code points; `cut` is set when something was left out.
[[nodiscard]] inline std::string prefix(std::string_view text, std::size_t limit, bool& cut) {
    std::size_t bytes = 0, points = 0;
    while (bytes < text.size()) {
        if (points == limit) { cut = true; break; }
        const auto length = sequence_length(text.substr(bytes));
        bytes += length == 0 ? 1 : length;
        ++points;
    }
    return std::string{text.substr(0, bytes)};
}

}  // namespace reading_detail

struct BoundedReadingText final {
    std::string text;
    bool truncated{};
};

[[nodiscard]] inline BoundedReadingText bound_reading_text(std::string_view raw) {
    bool cut = false;
    auto text = reading_detail::prefix(reading_detail::squeeze(reading_detail::clean(raw, true)), reading_text_limit, cut);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\n')) text.pop_back();
    return {std::move(text), cut};
}

[[nodiscard]] inline std::string bound_reading_title(std::string_view raw) {
    bool cut = false;
    return reading_detail::prefix(reading_detail::squeeze(reading_detail::clean(raw, false)), reading_title_limit, cut);
}

struct ReadingEvent final {
    std::uint64_t serial{};
    ReadingKind kind{};
    std::uint32_t form_id{};
    std::string origin_plugin;
    std::string title;
    std::string text;  // Empty for audio holotapes and anything whose text could not be read.
    bool text_truncated{};
    SnapshotClock::time_point observed_at{};

    [[nodiscard]] bool valid() const noexcept {
        if (serial == 0 || serial > RuntimeGeneration::maximum_wire_value) return false;
        if (form_id == 0 || form_id == UINT32_MAX) return false;
        if (origin_plugin.empty() || origin_plugin.size() > 255 ||
            origin_plugin.find_first_of("/\\") != std::string::npos) return false;
        const auto ends = [&](std::string_view suffix) {
            return origin_plugin.size() > suffix.size() &&
                std::equal(suffix.rbegin(), suffix.rend(), origin_plugin.rbegin(), [](char a, char b) {
                    return a == static_cast<char>(std::tolower(static_cast<unsigned char>(b)));
                });
        };
        if (!ends(".esm") && !ends(".esp") && !ends(".esl")) return false;
        if (title.empty() || title != bound_reading_title(title)) return false;
        if (!text.empty() && text != bound_reading_text(text).text) return false;
        return text_truncated ? !text.empty() : true;
    }
};

// Same item read again within the window is not a new reading: the NPCs already know its text.
class ReadingRecents final {
public:
    static constexpr std::size_t capacity = 32;
    static constexpr std::chrono::minutes window{10};

    [[nodiscard]] bool admit(ReadingKind kind, std::uint32_t form_id, std::string_view title, SnapshotClock::time_point now) {
        for (auto& entry : entries_) {
            if (entry.form_id != form_id || entry.kind != kind || entry.title != title) continue;
            const bool fresh = now >= entry.at && now - entry.at < window;
            entry.at = now;
            return !fresh;
        }
        auto& slot = entries_[next_++ % capacity];
        slot = {kind, form_id, std::string{title}, now};
        return true;
    }
    void clear() { entries_ = {}; next_ = 0; }

private:
    struct Entry { ReadingKind kind{}; std::uint32_t form_id{}; std::string title; SnapshotClock::time_point at{}; };
    std::array<Entry, capacity> entries_{};
    std::size_t next_{};
};

}  // namespace synth::core
