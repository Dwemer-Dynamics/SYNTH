#pragma once
#include "radio/diagnostic.hpp"
#include <charconv>

namespace synth::adapters::commonlib {
class RadioProbeAPI final {
    static std::string generation(std::monostate) {
        const auto value = radio::probe_mailbox.generation();
        return value.valid() ? std::to_string(value.value()) : std::string{};
    }
    static bool request(std::monostate, std::int32_t operation, std::int32_t slot, std::string_view generation_text) {
        if (operation < 0 || operation > 4 || slot < 0 || slot > 1 ||
            generation_text.empty() || generation_text.size() > 16 || generation_text.front() == '0') return false;
        std::uint64_t value{};
        const auto parsed = std::from_chars(generation_text.data(), generation_text.data() + generation_text.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != generation_text.data() + generation_text.size() ||
            value == 0 || value > core::RuntimeGeneration::maximum_wire_value) return false;
        return radio::probe_mailbox.submit({core::RuntimeGeneration::from_value(value),
            static_cast<radio::ProbeOperation>(operation), static_cast<unsigned char>(slot)});
    }
    static std::string observation(std::monostate) { return radio::probe_mailbox.diagnostic(); }
public:
    static bool bind(RE::BSScript::IVirtualMachine& vm) {
        bool bound = vm.BindNativeMethod(new RE::BSScript::NativeFunction("SYNTHRadioProbeNative", "Generation", generation, false));
        bound &= vm.BindNativeMethod(new RE::BSScript::NativeFunction("SYNTHRadioProbeNative", "Request", request, false));
        bound &= vm.BindNativeMethod(new RE::BSScript::NativeFunction("SYNTHRadioProbeNative", "Observation", observation, false));
        return bound;
    }
};
} // namespace synth::adapters::commonlib
