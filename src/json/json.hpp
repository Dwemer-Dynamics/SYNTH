#pragma once

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace synth::json {

struct Limits final {
    std::size_t maximum_bytes{1'048'576};
    std::size_t maximum_depth{32};
    std::size_t maximum_string_bytes{262'144};
    std::size_t maximum_array_items{16'384};
    std::size_t maximum_object_members{16'384};
};

class Error : public std::runtime_error {
public:
    Error(std::string message, std::size_t offset)
        : std::runtime_error{std::move(message)}, offset_{offset} {}
    [[nodiscard]] std::size_t offset() const noexcept { return offset_; }
private:
    std::size_t offset_{};
};

// Callers may reduce optional payload data on byte exhaustion, but never on malformed JSON.
class ByteLimitError final : public Error {
public:
    explicit ByteLimitError(std::size_t offset) : Error{"JSON byte limit exceeded", offset} {}
};

class Value final {
public:
    using Array = std::vector<Value>;
    using Object = std::vector<std::pair<std::string, Value>>;
    using Storage = std::variant<std::nullptr_t, bool, std::int64_t, std::uint64_t, double,
                                 std::string, Array, Object>;

    Value() noexcept : storage_{nullptr} {}
    Value(std::nullptr_t) noexcept : storage_{nullptr} {}
    Value(bool value) noexcept : storage_{value} {}
    Value(std::int64_t value) noexcept : storage_{value} {}
    Value(std::uint64_t value) noexcept : storage_{value} {}
    Value(int value) noexcept : storage_{static_cast<std::int64_t>(value)} {}
    Value(double value) : storage_{checked_double(value)} {}
    Value(std::string value) : storage_{std::move(value)} {}
    Value(const char* value) : storage_{std::string{value}} {}
    Value(Array value) : storage_{std::move(value)} {}
    Value(Object value) : storage_{std::move(value)} {}

    [[nodiscard]] const Storage& storage() const noexcept { return storage_; }
    [[nodiscard]] bool is_null() const noexcept { return std::holds_alternative<std::nullptr_t>(storage_); }
    [[nodiscard]] bool is_bool() const noexcept { return std::holds_alternative<bool>(storage_); }
    [[nodiscard]] bool is_number() const noexcept {
        return std::holds_alternative<std::int64_t>(storage_) ||
               std::holds_alternative<std::uint64_t>(storage_) ||
               std::holds_alternative<double>(storage_);
    }
    [[nodiscard]] bool is_string() const noexcept { return std::holds_alternative<std::string>(storage_); }
    [[nodiscard]] bool is_array() const noexcept { return std::holds_alternative<Array>(storage_); }
    [[nodiscard]] bool is_object() const noexcept { return std::holds_alternative<Object>(storage_); }

    [[nodiscard]] bool as_bool() const { return std::get<bool>(storage_); }
    [[nodiscard]] const std::string& as_string() const { return std::get<std::string>(storage_); }
    [[nodiscard]] const Array& as_array() const { return std::get<Array>(storage_); }
    [[nodiscard]] const Object& as_object() const { return std::get<Object>(storage_); }
    [[nodiscard]] std::int64_t as_int64() const { return std::get<std::int64_t>(storage_); }
    [[nodiscard]] std::uint64_t as_uint64() const { return std::get<std::uint64_t>(storage_); }
    [[nodiscard]] double as_double() const {
        if (const auto* value = std::get_if<double>(&storage_)) return *value;
        if (const auto* value = std::get_if<std::int64_t>(&storage_)) return static_cast<double>(*value);
        return static_cast<double>(std::get<std::uint64_t>(storage_));
    }

    [[nodiscard]] const Value* find(std::string_view key) const noexcept {
        const auto* object = std::get_if<Object>(&storage_);
        if (!object) return nullptr;
        for (const auto& [name, value] : *object) if (name == key) return &value;
        return nullptr;
    }

    friend bool operator==(const Value&, const Value&) = default;

private:
    static double checked_double(double value) {
        if (!std::isfinite(value)) throw std::invalid_argument{"JSON numbers must be finite"};
        return value;
    }
    Storage storage_;
};

namespace detail {

[[nodiscard]] inline bool valid_utf8(std::string_view text) noexcept {
    std::size_t index = 0;
    while (index < text.size()) {
        const auto first = static_cast<unsigned char>(text[index++]);
        if (first <= 0x7f) continue;
        std::uint32_t code{};
        unsigned continuation{};
        if (first >= 0xc2 && first <= 0xdf) { code = first & 0x1fU; continuation = 1; }
        else if (first >= 0xe0 && first <= 0xef) { code = first & 0x0fU; continuation = 2; }
        else if (first >= 0xf0 && first <= 0xf4) { code = first & 0x07U; continuation = 3; }
        else return false;
        if (continuation > text.size() - index) return false;
        for (unsigned count = 0; count < continuation; ++count) {
            const auto next = static_cast<unsigned char>(text[index++]);
            if ((next & 0xc0U) != 0x80U) return false;
            code = (code << 6U) | (next & 0x3fU);
        }
        if ((continuation == 2 && code < 0x800U) || (continuation == 3 && code < 0x10000U) ||
            code > 0x10ffffU || (code >= 0xd800U && code <= 0xdfffU)) return false;
    }
    return true;
}

inline void append_utf8(std::string& output, std::uint32_t code, std::size_t offset) {
    if (code > 0x10ffffU || (code >= 0xd800U && code <= 0xdfffU))
        throw Error{"invalid Unicode scalar value", offset};
    if (code <= 0x7fU) output.push_back(static_cast<char>(code));
    else if (code <= 0x7ffU) {
        output.push_back(static_cast<char>(0xc0U | (code >> 6U)));
        output.push_back(static_cast<char>(0x80U | (code & 0x3fU)));
    } else if (code <= 0xffffU) {
        output.push_back(static_cast<char>(0xe0U | (code >> 12U)));
        output.push_back(static_cast<char>(0x80U | ((code >> 6U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | (code & 0x3fU)));
    } else {
        output.push_back(static_cast<char>(0xf0U | (code >> 18U)));
        output.push_back(static_cast<char>(0x80U | ((code >> 12U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | ((code >> 6U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | (code & 0x3fU)));
    }
}

class Parser final {
public:
    Parser(std::string_view input, Limits limits) : input_{input}, limits_{limits} {
        if (input.size() > limits.maximum_bytes) throw ByteLimitError{0};
        if (limits.maximum_bytes == 0 || limits.maximum_depth == 0 ||
            limits.maximum_string_bytes == 0 || limits.maximum_array_items == 0 ||
            limits.maximum_object_members == 0) throw std::invalid_argument{"invalid JSON limits"};
    }

    Value parse_document() {
        whitespace();
        auto result = value(1);
        whitespace();
        if (position_ != input_.size()) fail("trailing input");
        return result;
    }

private:
    [[noreturn]] void fail(std::string message) const { throw Error{std::move(message), position_}; }
    [[nodiscard]] char take() {
        if (position_ == input_.size()) fail("unexpected end of input");
        return input_[position_++];
    }
    [[nodiscard]] bool consume(char expected) noexcept {
        if (position_ < input_.size() && input_[position_] == expected) { ++position_; return true; }
        return false;
    }
    void whitespace() noexcept {
        while (position_ < input_.size()) {
            const char c = input_[position_];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
            ++position_;
        }
    }
    void literal(std::string_view remainder) {
        if (input_.substr(position_, remainder.size()) != remainder) fail("invalid literal");
        position_ += remainder.size();
    }
    [[nodiscard]] Value value(std::size_t depth) {
        if (depth > limits_.maximum_depth) fail("JSON depth limit exceeded");
        if (position_ == input_.size()) fail("expected value");
        switch (input_[position_]) {
        case 'n': ++position_; literal("ull"); return nullptr;
        case 't': ++position_; literal("rue"); return true;
        case 'f': ++position_; literal("alse"); return false;
        case '"': return string();
        case '[': return array(depth);
        case '{': return object(depth);
        default:
            if (input_[position_] == '-' || (input_[position_] >= '0' && input_[position_] <= '9'))
                return number();
            fail("expected value");
        }
    }
    [[nodiscard]] std::uint16_t hex4() {
        std::uint16_t result{};
        for (unsigned count = 0; count < 4; ++count) {
            const char c = take();
            result = static_cast<std::uint16_t>(result << 4U);
            if (c >= '0' && c <= '9') result = static_cast<std::uint16_t>(result | (c - '0'));
            else if (c >= 'a' && c <= 'f') result = static_cast<std::uint16_t>(result | (c - 'a' + 10));
            else if (c >= 'A' && c <= 'F') result = static_cast<std::uint16_t>(result | (c - 'A' + 10));
            else fail("invalid Unicode escape");
        }
        return result;
    }
    [[nodiscard]] Value string() {
        (void)take();
        std::string result;
        while (position_ < input_.size()) {
            const auto byte = static_cast<unsigned char>(take());
            if (byte == '"') {
                if (!valid_utf8(result)) fail("invalid UTF-8 string");
                return result;
            }
            if (byte < 0x20U) fail("unescaped control character");
            if (byte != '\\') result.push_back(static_cast<char>(byte));
            else {
                const char escaped = take();
                switch (escaped) {
                case '"': result.push_back('"'); break; case '\\': result.push_back('\\'); break;
                case '/': result.push_back('/'); break; case 'b': result.push_back('\b'); break;
                case 'f': result.push_back('\f'); break; case 'n': result.push_back('\n'); break;
                case 'r': result.push_back('\r'); break; case 't': result.push_back('\t'); break;
                case 'u': {
                    auto code = static_cast<std::uint32_t>(hex4());
                    if (code >= 0xd800U && code <= 0xdbffU) {
                        if (!consume('\\') || !consume('u')) fail("missing low surrogate");
                        const auto low = static_cast<std::uint32_t>(hex4());
                        if (low < 0xdc00U || low > 0xdfffU) fail("invalid low surrogate");
                        code = 0x10000U + ((code - 0xd800U) << 10U) + (low - 0xdc00U);
                    } else if (code >= 0xdc00U && code <= 0xdfffU) fail("unpaired low surrogate");
                    append_utf8(result, code, position_);
                    break;
                }
                default: fail("invalid escape");
                }
            }
            if (result.size() > limits_.maximum_string_bytes) fail("JSON string limit exceeded");
        }
        fail("unterminated string");
    }
    [[nodiscard]] Value number() {
        const auto start = position_;
        (void)consume('-');
        if (consume('0')) {
            if (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9')
                fail("leading zero in number");
        } else {
            if (position_ == input_.size() || input_[position_] < '1' || input_[position_] > '9')
                fail("invalid number");
            while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') ++position_;
        }
        bool integral = true;
        if (consume('.')) {
            integral = false;
            const auto fraction = position_;
            while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') ++position_;
            if (position_ == fraction) fail("invalid fraction");
        }
        if (position_ < input_.size() && (input_[position_] == 'e' || input_[position_] == 'E')) {
            integral = false; ++position_; if (position_ < input_.size() && (input_[position_] == '+' || input_[position_] == '-')) ++position_;
            const auto exponent = position_;
            while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') ++position_;
            if (position_ == exponent) fail("invalid exponent");
        }
        const auto text = input_.substr(start, position_ - start);
        if (integral) {
            if (text.front() == '-') {
                std::int64_t parsed{}; const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
                if (result.ec == std::errc{} && result.ptr == text.data() + text.size()) return parsed;
            } else {
                std::uint64_t parsed{}; const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
                if (result.ec == std::errc{} && result.ptr == text.data() + text.size()) return parsed;
            }
        }
        double parsed{}; const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed, std::chars_format::general);
        if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || !std::isfinite(parsed))
            fail("number outside finite range");
        return parsed;
    }
    [[nodiscard]] Value array(std::size_t depth) {
        (void)take(); whitespace(); Value::Array result;
        if (consume(']')) return result;
        for (;;) {
            if (result.size() == limits_.maximum_array_items) fail("JSON array limit exceeded");
            result.push_back(value(depth + 1)); whitespace();
            if (consume(']')) return result;
            if (!consume(',')) fail("expected comma or array end");
            whitespace();
        }
    }
    [[nodiscard]] Value object(std::size_t depth) {
        (void)take(); whitespace(); Value::Object result;
        if (consume('}')) return result;
        for (;;) {
            if (result.size() == limits_.maximum_object_members) fail("JSON object limit exceeded");
            if (position_ == input_.size() || input_[position_] != '"') fail("expected object key");
            auto key = string().as_string();
            for (const auto& member : result) if (member.first == key) fail("duplicate object key");
            whitespace(); if (!consume(':')) fail("expected colon"); whitespace();
            result.emplace_back(std::move(key), value(depth + 1)); whitespace();
            if (consume('}')) return result;
            if (!consume(',')) fail("expected comma or object end");
            whitespace();
        }
    }

    std::string_view input_;
    Limits limits_;
    std::size_t position_{};
};

inline void checked_append(std::string& output, std::string_view text, const Limits& limits) {
    if (text.size() > limits.maximum_bytes - output.size()) throw ByteLimitError{output.size()};
    output.append(text);
}

inline void write_string(std::string& output, std::string_view text, const Limits& limits) {
    if (text.size() > limits.maximum_string_bytes) throw Error{"JSON string limit exceeded", output.size()};
    if (!valid_utf8(text)) throw Error{"invalid UTF-8 string", output.size()};
    checked_append(output, "\"", limits);
    constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char c : text) {
        switch (c) {
        case '"': checked_append(output, "\\\"", limits); break;
        case '\\': checked_append(output, "\\\\", limits); break;
        case '\b': checked_append(output, "\\b", limits); break;
        case '\f': checked_append(output, "\\f", limits); break;
        case '\n': checked_append(output, "\\n", limits); break;
        case '\r': checked_append(output, "\\r", limits); break;
        case '\t': checked_append(output, "\\t", limits); break;
        default:
            if (c < 0x20U) {
                char escaped[6]{'\\', 'u', '0', '0', hex[c >> 4U], hex[c & 0x0fU]};
                checked_append(output, std::string_view{escaped, 6}, limits);
            } else checked_append(output, std::string_view{reinterpret_cast<const char*>(&c), 1}, limits);
        }
    }
    checked_append(output, "\"", limits);
}

inline void write_value(std::string& output, const Value& value, const Limits& limits, std::size_t depth) {
    if (depth > limits.maximum_depth) throw Error{"JSON depth limit exceeded", output.size()};
    std::visit([&](const auto& item) {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, std::nullptr_t>) checked_append(output, "null", limits);
        else if constexpr (std::is_same_v<T, bool>) checked_append(output, item ? "true" : "false", limits);
        else if constexpr (std::is_same_v<T, std::string>) write_string(output, item, limits);
        else if constexpr (std::is_same_v<T, Value::Array>) {
            if (item.size() > limits.maximum_array_items) throw Error{"JSON array limit exceeded", output.size()};
            checked_append(output, "[", limits);
            for (std::size_t index = 0; index < item.size(); ++index) {
                if (index) checked_append(output, ",", limits);
                write_value(output, item[index], limits, depth + 1);
            }
            checked_append(output, "]", limits);
        } else if constexpr (std::is_same_v<T, Value::Object>) {
            if (item.size() > limits.maximum_object_members) throw Error{"JSON object limit exceeded", output.size()};
            checked_append(output, "{", limits);
            for (std::size_t index = 0; index < item.size(); ++index) {
                for (std::size_t earlier = 0; earlier < index; ++earlier)
                    if (item[earlier].first == item[index].first) throw Error{"duplicate object key", output.size()};
                if (index) checked_append(output, ",", limits);
                write_string(output, item[index].first, limits); checked_append(output, ":", limits);
                write_value(output, item[index].second, limits, depth + 1);
            }
            checked_append(output, "}", limits);
        } else {
            if constexpr (std::is_same_v<T, double>) if (!std::isfinite(item)) throw Error{"JSON numbers must be finite", output.size()};
            char buffer[64];
            const auto result = [&] {
                if constexpr (std::is_same_v<T, double>) return std::to_chars(buffer, buffer + sizeof(buffer), item, std::chars_format::general);
                else return std::to_chars(buffer, buffer + sizeof(buffer), item);
            }();
            if (result.ec != std::errc{}) throw Error{"cannot encode number", output.size()};
            checked_append(output, std::string_view{buffer, static_cast<std::size_t>(result.ptr - buffer)}, limits);
        }
    }, value.storage());
}

}  // namespace detail

[[nodiscard]] inline Value parse(std::string_view input, Limits limits = {}) {
    return detail::Parser{input, limits}.parse_document();
}

[[nodiscard]] inline std::string write(const Value& value, Limits limits = {}) {
    if (limits.maximum_bytes == 0 || limits.maximum_depth == 0 || limits.maximum_string_bytes == 0 ||
        limits.maximum_array_items == 0 || limits.maximum_object_members == 0)
        throw std::invalid_argument{"invalid JSON limits"};
    std::string output;
    detail::write_value(output, value, limits, 1);
    return output;
}

}  // namespace synth::json
