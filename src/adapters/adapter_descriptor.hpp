#pragma once

#include "core/runtime_identity.hpp"
#include "core/snapshot.hpp"

#include <array>
#include <compare>
#include <cstdint>
#include <span>
#include <string_view>

namespace synth::adapters {

struct Version final {
    std::uint16_t major{};
    std::uint16_t minor{};
    std::uint16_t patch{};
    std::uint16_t build{};

    auto operator<=>(const Version&) const = default;
};

struct AdapterDescriptor final {
    std::string_view plugin_name;
    std::string_view adapter_name;
    Version plugin_version;
    core::Game game;
    core::RuntimeVariant variant;
    Version runtime_version;
    Version script_extender_version;
    std::span<const core::RuntimeCapability> capabilities;
};

inline constexpr Version synth_plugin_version{0, 1, 0, 0};
inline constexpr Version flat_runtime_version{1, 11, 240, 0};
inline constexpr Version flat_f4se_version{0, 7, 9, 0};
inline constexpr Version vr_runtime_version{1, 2, 72, 0};
inline constexpr Version f4sevr_version{0, 6, 21, 0};

inline constexpr std::array flat_capabilities{core::RuntimeCapability::notifications};
inline constexpr std::array vr_capabilities{
    core::RuntimeCapability::notifications,
    core::RuntimeCapability::hmd_pose,
    core::RuntimeCapability::controller_pose,
};

inline constexpr AdapterDescriptor flat_descriptor{
    "SYNTH",
    "flat-f4se",
    synth_plugin_version,
    core::Game::fallout4,
    core::RuntimeVariant::flat,
    flat_runtime_version,
    flat_f4se_version,
    flat_capabilities,
};

inline constexpr AdapterDescriptor vr_descriptor{
    "SYNTHVR",
    "vr-f4sevr",
    synth_plugin_version,
    core::Game::fallout4,
    core::RuntimeVariant::vr,
    vr_runtime_version,
    f4sevr_version,
    vr_capabilities,
};

[[nodiscard]] constexpr bool supports_exact_environment(
    const AdapterDescriptor& descriptor,
    core::RuntimeVariant detected_variant,
    Version detected_runtime,
    Version detected_script_extender) noexcept {
    return detected_variant == descriptor.variant && detected_runtime == descriptor.runtime_version &&
           detected_script_extender == descriptor.script_extender_version;
}

[[nodiscard]] constexpr bool has_capability(
    const AdapterDescriptor& descriptor,
    core::RuntimeCapability capability) noexcept {
    for (const auto supported : descriptor.capabilities) {
        if (supported == capability) {
            return true;
        }
    }
    return false;
}

}  // namespace synth::adapters
