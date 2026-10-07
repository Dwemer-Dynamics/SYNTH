#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace synth::voice {

inline constexpr std::uint32_t maximum_sample_bytes = 8U * 1024U * 1024U;

inline std::string lower(std::string value) {
    for (auto& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

inline bool safe_component(const std::string& value) {
    return !value.empty() && value.size() <= 128 &&
        std::ranges::all_of(value, [](unsigned char c) {
            return std::isalnum(c) || c == '_' || c == '-' || c == '+';
        });
}

struct Sample final {
    std::string voice_id;
    std::string original_name;
    std::filesystem::path file;
    std::uint64_t offset{};
    std::uint32_t bytes{};
    std::uint32_t unpacked_bytes{};
    bool compressed{};
    bool loose{};
};

// Reads only copied game paths on a worker. It never opens an engine resource or RE object.
class SampleIndex final {
public:
    using Cancelled = std::function<bool()>;
    using Samples = std::map<std::string, Sample>;

    const Samples& samples() const noexcept { return samples_; }
    std::size_t unsupported_archives() const noexcept { return unsupported_; }

    void scan(const std::filesystem::path& data, const std::vector<std::string>& plugins,
              const Cancelled& cancelled) {
        samples_.clear();
        unsupported_ = 0;
        std::vector<std::string> enabled;
        for (const auto& plugin : plugins) enabled.push_back(lower(plugin));
        std::vector<std::filesystem::path> archives;
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator(data, error)) {
            if (cancelled()) throw std::runtime_error{"Voice discovery cancelled"};
            if (lower(entry.path().extension().string()) != ".ba2" || entry.is_symlink()) continue;
            const auto archive = lower(entry.path().filename().string());
            const bool active = std::ranges::any_of(enabled, [&](const auto& plugin) {
                const auto stem = std::filesystem::path{plugin}.stem().string();
                return archive.starts_with(stem + " - ");
            });
            if (active && archives.size() < 256) archives.push_back(entry.path());
        }
        std::ranges::sort(archives);
        for (const auto& archive : archives) scan_archive(archive, enabled, cancelled);

        // MO2 exposes winning loose files through the process's virtual Data tree.
        std::size_t visited{};
        for (const auto& plugin : plugins) {
            if (plugin.find_first_of("/\\") != std::string::npos) continue;
            const auto root = data / "Sound/Voice" / plugin;
            if (!std::filesystem::is_directory(root, error)) continue;
            for (const auto& entry : std::filesystem::recursive_directory_iterator(
                     root, std::filesystem::directory_options::skip_permission_denied, error)) {
                if (cancelled()) throw std::runtime_error{"Voice discovery cancelled"};
                if (++visited > 100'000) throw std::runtime_error{"Loose voice discovery limit exceeded"};
                if (!entry.is_regular_file() || entry.is_symlink()) continue;
                const auto size = entry.file_size();
                if (size == 0 || size > maximum_sample_bytes) continue;
                consider(Sample{{}, entry.path().lexically_relative(data).generic_string(),
                                entry.path(), 0, static_cast<std::uint32_t>(size),
                                static_cast<std::uint32_t>(size), false, true}, enabled);
            }
        }
    }

    static std::string read(const Sample& sample) {
        if (sample.bytes == 0 || sample.bytes > maximum_sample_bytes ||
            sample.unpacked_bytes > maximum_sample_bytes) throw std::runtime_error{"Invalid voice size"};
        std::ifstream input{sample.file, std::ios::binary | std::ios::ate};
        const auto end = input.tellg();
        if (end < 0 || sample.offset > static_cast<std::uint64_t>(end) ||
            sample.bytes > static_cast<std::uint64_t>(end) - sample.offset)
            throw std::runtime_error{"Voice entry is outside its source file"};
        input.seekg(static_cast<std::streamoff>(sample.offset));
        std::string bytes(sample.bytes, '\0');
        if (!input.read(bytes.data(), bytes.size())) throw std::runtime_error{"Voice sample read failed"};
        return bytes;
    }

private:
    template<class T> static T read_integer(std::istream& input) {
        std::array<unsigned char, sizeof(T)> bytes{};
        if (!input.read(reinterpret_cast<char*>(bytes.data()), bytes.size()))
            throw std::runtime_error{"Truncated BA2 voice index"};
        T result{};
        for (std::size_t i = 0; i < bytes.size(); ++i) result |= static_cast<T>(bytes[i]) << (8 * i);
        return result;
    }

    // Select a bounded, conversational-length candidate, with winning loose files preferred.
    void consider(Sample sample, const std::vector<std::string>& plugins) {
        auto path = lower(sample.original_name);
        std::ranges::replace(path, '\\', '/');
        std::vector<std::string> parts;
        for (std::size_t start = 0, end;; start = end + 1) {
            end = path.find('/', start);
            parts.push_back(path.substr(start, end == std::string::npos ? end : end - start));
            if (end == std::string::npos) break;
        }
        if (parts.size() != 5 || parts[0] != "sound" || parts[1] != "voice" ||
            std::ranges::find(plugins, parts[2]) == plugins.end() || !safe_component(parts[3])) return;
        const auto ext = std::filesystem::path{parts[4]}.extension().string();
        if (ext != ".fuz" && ext != ".xwm" && ext != ".wav" && ext != ".ogg") return;
        // Do not turn creature noises or explicitly named combat/death barks into speech voices.
        for (const auto* noise : {"death", "breath", "combat", "attack", "dogmeat", "crdog", "crbrahmin"}) {
            if (path.find(noise) != std::string::npos) return;
        }
        const auto ideal = ext == ".wav" ? 440'000U : 80'000U;
        if (sample.unpacked_bytes < (ext == ".wav" ? 132'000U : 28'000U) ||
            sample.unpacked_bytes > maximum_sample_bytes) return;
        sample.voice_id = parts[3];
        const auto score = [ideal](const Sample& candidate) {
            return candidate.unpacked_bytes > ideal ? candidate.unpacked_bytes - ideal : ideal - candidate.unpacked_bytes;
        };
        const auto found = samples_.find(sample.voice_id);
        if (found == samples_.end() || (sample.loose && !found->second.loose) ||
            (sample.loose == found->second.loose && score(sample) < score(found->second)))
            samples_.insert_or_assign(sample.voice_id, std::move(sample));
    }

    void scan_archive(const std::filesystem::path& archive, const std::vector<std::string>& plugins,
                      const Cancelled& cancelled) {
        std::ifstream input{archive, std::ios::binary | std::ios::ate};
        const auto end = input.tellg();
        if (end < 24) { ++unsupported_; return; }
        const auto size = static_cast<std::uint64_t>(end);
        input.seekg(0);
        if (read_integer<std::uint32_t>(input) != 0x58445442U) { ++unsupported_; return; }
        const auto version = read_integer<std::uint32_t>(input);
        const auto type = read_integer<std::uint32_t>(input);
        if (type == 0x30315844U) return; // DX10 texture archives contain no voices.
        if (version != 1 || type != 0x4C524E47U) { ++unsupported_; return; }
        const auto count = read_integer<std::uint32_t>(input);
        const auto names = read_integer<std::uint64_t>(input);
        if (count > 1'000'000 || names < 24ULL + 36ULL * count || names >= size)
            throw std::runtime_error{"Invalid BA2 voice record bounds"};
        struct Record { std::uint64_t offset; std::uint32_t packed, unpacked; };
        std::vector<Record> records;
        records.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            if (cancelled()) throw std::runtime_error{"Voice discovery cancelled"};
            input.seekg(24ULL + 36ULL * i + 16);
            records.push_back({read_integer<std::uint64_t>(input), read_integer<std::uint32_t>(input),
                               read_integer<std::uint32_t>(input)});
        }
        input.seekg(static_cast<std::streamoff>(names));
        for (const auto& record : records) {
            if (cancelled()) throw std::runtime_error{"Voice discovery cancelled"};
            const auto length = read_integer<std::uint16_t>(input);
            if (length == 0 || length > 1024) throw std::runtime_error{"Invalid BA2 voice name"};
            std::string name(length, '\0');
            if (!input.read(name.data(), length)) throw std::runtime_error{"Truncated BA2 voice name"};
            const auto bytes = record.packed ? record.packed : record.unpacked;
            if (record.offset < 24ULL + 36ULL * count || record.offset > names ||
                bytes > names - record.offset || bytes > maximum_sample_bytes ||
                record.unpacked > maximum_sample_bytes) continue;
            consider(Sample{{}, std::move(name), archive, record.offset, bytes,
                            record.unpacked, record.packed != 0, false}, plugins);
        }
    }

    Samples samples_;
    std::size_t unsupported_{};
};

} // namespace synth::voice
