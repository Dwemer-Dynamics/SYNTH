#pragma once

#include <optional>
#include <string_view>

namespace synth::core {

enum class Game : unsigned char {
    fallout4,
};

enum class RuntimeVariant : unsigned char {
    flat,
    vr,
};

[[nodiscard]] constexpr std::string_view to_string(Game game) noexcept {
    switch (game) {
    case Game::fallout4:
        return "fo4";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::string_view to_string(RuntimeVariant variant) noexcept {
    switch (variant) {
    case RuntimeVariant::flat:
        return "flat";
    case RuntimeVariant::vr:
        return "vr";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::optional<Game> parse_game(std::string_view value) noexcept {
    if (value == "fo4") {
        return Game::fallout4;
    }
    return std::nullopt;
}

[[nodiscard]] constexpr std::optional<RuntimeVariant> parse_runtime_variant(
    std::string_view value) noexcept {
    if (value == "flat") {
        return RuntimeVariant::flat;
    }
    if (value == "vr") {
        return RuntimeVariant::vr;
    }
    return std::nullopt;
}

}  // namespace synth::core
