#pragma once

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace synth::diagnostics {

enum class LogLevel : unsigned char { trace, debug, info, warning, error, critical };

[[nodiscard]] constexpr std::string_view to_string(LogLevel level) noexcept {
    switch (level) {
    case LogLevel::trace: return "trace";
    case LogLevel::debug: return "debug";
    case LogLevel::info: return "info";
    case LogLevel::warning: return "warning";
    case LogLevel::error: return "error";
    case LogLevel::critical: return "critical";
    }
    return "unknown";
}

struct Record {
    LogLevel level{LogLevel::info};
    std::string request_id;
    std::string message;
    std::optional<std::string> payload;
    std::chrono::system_clock::time_point timestamp{std::chrono::system_clock::now()};
};

struct Policy {
    bool include_payloads{false};
    std::size_t maximum_message_bytes{2'048};
    std::size_t maximum_payload_bytes{4'096};
    std::size_t maximum_request_id_bytes{128};
};

namespace detail {

inline char lower(char value) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
}

inline std::string lower(std::string_view value) {
    std::string result{value};
    std::ranges::transform(result, result.begin(), [](char character) { return lower(character); });
    return result;
}

inline bool sensitive_name(std::string_view value) {
    const auto name = lower(value);
    return name == "authorization" || name == "proxy-authorization" || name == "cookie" ||
           name == "set-cookie" || name == "x-api-key" || name == "api-key" ||
           name == "apikey" || name == "api_key" || name == "password" ||
           name == "passwd" || name == "secret" || name == "token" ||
           name == "access_token" || name == "refresh_token" || name == "client_secret" ||
           name == "credential" || name == "credentials";
}

inline bool name_character(char value) {
    return std::isalnum(static_cast<unsigned char>(value)) != 0 || value == '-' || value == '_';
}

inline void redact_url_credentials(std::string& text) {
    std::size_t position = 0;
    while ((position = text.find("://", position)) != std::string::npos) {
        const auto authority_start = position + 3;
        const auto authority_end = text.find_first_of("/?# \t\r\n", authority_start);
        const auto at = text.find('@', authority_start);
        if (at != std::string::npos && (authority_end == std::string::npos || at < authority_end)) {
            text.replace(authority_start, at - authority_start, "[REDACTED]");
            position = authority_start + std::string_view{"[REDACTED]@"}.size();
        } else {
            position = authority_start;
        }
    }
}

inline void redact_assignments(std::string& text) {
    std::size_t cursor = 0;
    while (cursor < text.size()) {
        if (!name_character(text[cursor])) {
            ++cursor;
            continue;
        }
        const auto name_start = cursor;
        while (cursor < text.size() && name_character(text[cursor])) ++cursor;
        const auto name_end = cursor;
        auto separator = cursor;
        while (separator < text.size() && (text[separator] == ' ' || text[separator] == '\t' ||
                                           text[separator] == '"' || text[separator] == '\'')) {
            ++separator;
        }
        if (separator >= text.size() || (text[separator] != '=' && text[separator] != ':')) continue;
        if (!sensitive_name(std::string_view{text}.substr(name_start, name_end - name_start))) continue;
        auto value_start = separator + 1;
        while (value_start < text.size() &&
               (text[value_start] == ' ' || text[value_start] == '\t' || text[value_start] == '"' ||
                text[value_start] == '\'')) {
            ++value_start;
        }
        auto value_end = value_start;
        const auto sensitive = lower(std::string_view{text}.substr(name_start, name_end - name_start));
        const auto whole_line = sensitive == "authorization" || sensitive == "proxy-authorization";
        if (whole_line) {
            while (value_end < text.size() && text[value_end] != '\r' && text[value_end] != '\n') {
                ++value_end;
            }
        } else {
            while (value_end < text.size() && text[value_end] != '&' && text[value_end] != ',' &&
                   text[value_end] != ';' && text[value_end] != '\r' && text[value_end] != '\n' &&
                   text[value_end] != ' ' && text[value_end] != '\t' && text[value_end] != '"' &&
                   text[value_end] != '\'') {
                ++value_end;
            }
        }
        text.replace(value_start, value_end - value_start, "[REDACTED]");
        cursor = value_start + std::string_view{"[REDACTED]"}.size();
    }
}

inline std::string bounded(std::string value, std::size_t maximum) {
    if (value.size() <= maximum) return value;
    if (maximum == 0) return {};
    constexpr std::string_view marker{"...[truncated]"};
    if (maximum <= marker.size()) return value.substr(0, maximum);
    value.resize(maximum - marker.size());
    value.append(marker);
    return value;
}

}  // namespace detail

[[nodiscard]] inline std::string redact(std::string_view untrusted) {
    std::string result{untrusted};
    detail::redact_url_credentials(result);
    detail::redact_assignments(result);
    return result;
}

[[nodiscard]] inline Record make_record(LogLevel level,
                                        std::string_view request_id,
                                        std::string_view message,
                                        std::optional<std::string_view> payload = std::nullopt,
                                        Policy policy = {}) {
    Record result;
    result.level = level;
    result.request_id = detail::bounded(redact(request_id), policy.maximum_request_id_bytes);
    result.message = detail::bounded(redact(message), policy.maximum_message_bytes);
    if (policy.include_payloads && payload) {
        result.payload = detail::bounded(redact(*payload), policy.maximum_payload_bytes);
    }
    return result;
}

class Buffer {
  public:
    explicit Buffer(std::size_t capacity = 256) : capacity_{capacity} {}

    void push(Record record) {
        if (capacity_ == 0) return;
        if (records_.size() == capacity_) records_.erase(records_.begin());
        records_.push_back(std::move(record));
    }

    [[nodiscard]] const std::vector<Record>& records() const noexcept { return records_; }

  private:
    std::size_t capacity_;
    std::vector<Record> records_;
};

}  // namespace synth::diagnostics
