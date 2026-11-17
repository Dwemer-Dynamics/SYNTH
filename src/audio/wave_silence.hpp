#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace synth::audio {

// True when the bytes are a structurally valid RIFF/WAVE whose sample data is digital silence: every
// PCM sample at the zero level (0x80 for unsigned 8-bit, otherwise all-zero bytes; float zero is all-zero
// bytes). Any other sample, an unparseable container or a missing data chunk is not "silent": invalid media
// keeps its own rejection. A silent reply must never play; the line is delivered as text instead.
[[nodiscard]] inline bool wave_is_silent(std::span<const std::byte> bytes) noexcept {
    const auto u16 = [&](std::size_t at) {
        return static_cast<std::uint32_t>(std::to_integer<unsigned>(bytes[at])) |
               static_cast<std::uint32_t>(std::to_integer<unsigned>(bytes[at + 1])) << 8U;
    };
    const auto u32 = [&](std::size_t at) { return u16(at) | (u16(at + 2) << 16U); };
    const auto tag = [&](std::size_t at, const char (&name)[5]) {
        return std::memcmp(bytes.data() + at, name, 4) == 0;
    };
    if (bytes.size() < 44 || !tag(0, "RIFF") || !tag(8, "WAVE")) return false;
    const auto end = static_cast<std::size_t>(u32(4)) + 8U;
    if (end < 44 || end > bytes.size()) return false;
    std::uint32_t format_tag{}, bits{};
    bool has_format{}, has_data{};
    for (std::size_t offset = 12; offset + 8 <= end;) {
        const auto size = static_cast<std::size_t>(u32(offset + 4));
        const auto start = offset + 8;
        if (size > end - start) return false;
        if (tag(offset, "fmt ") && size >= 16) {
            format_tag = u16(start);
            bits = u16(start + 14);
            // Extensible carries the real encoding in the first dword of its sub-format GUID.
            if (format_tag == 0xFFFEU && size >= 40) format_tag = u32(start + 24);
            has_format = true;
        } else if (tag(offset, "data")) {
            if (!has_format || size == 0) return false;
            has_data = true;
            const auto zero = format_tag == 1U && bits == 8U ? std::byte{0x80} : std::byte{0};
            for (std::size_t i = 0; i < size; ++i)
                if (bytes[start + i] != zero) return false;
        }
        offset = start + size + (size & 1U);
    }
    return has_data;
}

} // namespace synth::audio
