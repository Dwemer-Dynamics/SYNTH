#pragma once

#include "core/cancellation.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace synth::media_fetch {

inline constexpr std::size_t maximum_media_bytes = 16U * 1024U * 1024U;

[[nodiscard]] inline bool is_sha256(std::string_view value) noexcept {
    return value.size() == 64 && std::ranges::all_of(value, [](char character) {
               return (character >= '0' && character <= '9') ||
                      (character >= 'a' && character <= 'f');
           });
}

// Media IDs are identifiers, not URLs. The fixed form cannot carry an authority,
// query, fragment, traversal component, or alternate scheme.
[[nodiscard]] inline bool is_same_origin_media_id(std::string_view value) noexcept {
    constexpr std::string_view prefix{"media/"};
    return value.size() == prefix.size() + 64 && value.starts_with(prefix) &&
           is_sha256(value.substr(prefix.size()));
}

struct MediaReference final {
    std::string media_id;
    std::string content_type;
    std::string sha256;
    std::uint64_t size{};
};

[[nodiscard]] inline bool is_supported_content_type(std::string_view value) noexcept {
    return value == "audio/wav" || value == "audio/ogg";
}

[[nodiscard]] inline bool is_valid(const MediaReference& reference,
                                   std::size_t byte_limit = maximum_media_bytes) noexcept {
    return is_same_origin_media_id(reference.media_id) &&
           is_supported_content_type(reference.content_type) && is_sha256(reference.sha256) &&
           reference.media_id.substr(6) == reference.sha256 && reference.size != 0 &&
           reference.size <= byte_limit;
}

using Bytes = std::vector<std::byte>;
using Cancelled = std::function<bool()>;

enum class TransportFailure : unsigned char { none, cancelled, unavailable };

struct TransportResponse final {
    TransportFailure failure{TransportFailure::none};
    int status{};
    std::string content_type;
    Bytes bytes;
};

class ITransport {
public:
    virtual ~ITransport() = default;

    // max_bytes is a hard response-body limit. Implementations should stop reading
    // once it is exceeded and consult cancelled during blocking work.
    [[nodiscard]] virtual TransportResponse fetch(std::string_view same_origin_target,
                                                  std::size_t max_bytes,
                                                  const Cancelled& cancelled) = 0;
};

namespace detail {

[[nodiscard]] constexpr std::uint32_t rotate_right(std::uint32_t value,
                                                    unsigned count) noexcept {
    return (value >> count) | (value << (32U - count));
}

[[nodiscard]] inline std::array<std::byte, 32> sha256(const Bytes& input) {
    constexpr std::array<std::uint32_t, 64> constants{
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U,
        0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
        0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U,
        0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
        0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
        0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
        0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
        0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
        0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU,
        0x5b9cca4fU, 0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
        0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

    std::vector<std::uint8_t> padded;
    padded.reserve(input.size() + 72);
    for (const auto byte : input) padded.push_back(std::to_integer<std::uint8_t>(byte));
    padded.push_back(0x80U);
    while (padded.size() % 64 != 56) padded.push_back(0U);
    const auto bit_count = static_cast<std::uint64_t>(input.size()) * 8U;
    for (int shift = 56; shift >= 0; shift -= 8) {
        padded.push_back(static_cast<std::uint8_t>(bit_count >> shift));
    }

    std::array<std::uint32_t, 8> hash{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U,
                                      0xa54ff53aU, 0x510e527fU, 0x9b05688cU,
                                      0x1f83d9abU, 0x5be0cd19U};
    for (std::size_t offset = 0; offset < padded.size(); offset += 64) {
        std::array<std::uint32_t, 64> words{};
        for (std::size_t index = 0; index < 16; ++index) {
            const auto position = offset + index * 4;
            words[index] = (static_cast<std::uint32_t>(padded[position]) << 24U) |
                           (static_cast<std::uint32_t>(padded[position + 1]) << 16U) |
                           (static_cast<std::uint32_t>(padded[position + 2]) << 8U) |
                           static_cast<std::uint32_t>(padded[position + 3]);
        }
        for (std::size_t index = 16; index < words.size(); ++index) {
            const auto x = words[index - 15];
            const auto y = words[index - 2];
            const auto sigma0 = rotate_right(x, 7) ^ rotate_right(x, 18) ^ (x >> 3U);
            const auto sigma1 = rotate_right(y, 17) ^ rotate_right(y, 19) ^ (y >> 10U);
            words[index] = words[index - 16] + sigma0 + words[index - 7] + sigma1;
        }

        auto a = hash[0];
        auto b = hash[1];
        auto c = hash[2];
        auto d = hash[3];
        auto e = hash[4];
        auto f = hash[5];
        auto g = hash[6];
        auto h = hash[7];
        for (std::size_t index = 0; index < words.size(); ++index) {
            const auto sum1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
            const auto choose = (e & f) ^ (~e & g);
            const auto temporary1 = h + sum1 + choose + constants[index] + words[index];
            const auto sum0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
            const auto majority = (a & b) ^ (a & c) ^ (b & c);
            const auto temporary2 = sum0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + temporary1;
            d = c;
            c = b;
            b = a;
            a = temporary1 + temporary2;
        }
        hash[0] += a;
        hash[1] += b;
        hash[2] += c;
        hash[3] += d;
        hash[4] += e;
        hash[5] += f;
        hash[6] += g;
        hash[7] += h;
    }

    std::array<std::byte, 32> result{};
    for (std::size_t index = 0; index < hash.size(); ++index) {
        for (std::size_t byte = 0; byte < 4; ++byte) {
            result[index * 4 + byte] =
                static_cast<std::byte>(hash[index] >> (24U - static_cast<unsigned>(byte) * 8U));
        }
    }
    return result;
}

[[nodiscard]] inline std::string hex_sha256(const Bytes& input) {
    const auto digest = sha256(input);
    constexpr char digits[] = "0123456789abcdef";
    std::string result(64, '0');
    for (std::size_t index = 0; index < digest.size(); ++index) {
        const auto byte = std::to_integer<unsigned>(digest[index]);
        result[index * 2] = digits[byte >> 4U];
        result[index * 2 + 1] = digits[byte & 0x0fU];
    }
    return result;
}

}  // namespace detail

[[nodiscard]] inline std::string sha256_hex(const Bytes& input) {
    return detail::hex_sha256(input);
}

enum class FetchStatus : unsigned char {
    fetched,
    cache_hit,
    invalid_reference,
    cancelled,
    transport_error,
    unexpected_status,
    content_type_mismatch,
    size_mismatch,
    byte_limit_exceeded,
    sha256_mismatch,
};

struct FetchResult final {
    FetchStatus status{FetchStatus::transport_error};
    std::shared_ptr<const Bytes> bytes;

    [[nodiscard]] bool ok() const noexcept {
        return status == FetchStatus::fetched || status == FetchStatus::cache_hit;
    }
};

struct CacheLimits final {
    std::size_t maximum_entries{32};
    std::size_t maximum_bytes{32U * 1024U * 1024U};
    std::size_t maximum_object_bytes{maximum_media_bytes};
};

class MediaFetcher final {
public:
    MediaFetcher(ITransport& transport, const core::GenerationClock& generations,
                 CacheLimits limits = {})
        : transport_{transport}, generations_{generations}, limits_{limits} {
        if (limits_.maximum_entries == 0 || limits_.maximum_bytes == 0 ||
            limits_.maximum_object_bytes == 0 ||
            limits_.maximum_object_bytes > limits_.maximum_bytes) {
            throw std::invalid_argument{"media cache limits are invalid"};
        }
    }

    [[nodiscard]] FetchResult fetch(const MediaReference& reference,
                                    const core::CancellationToken& cancellation) {
        if (!is_valid(reference, limits_.maximum_object_bytes)) {
            return {FetchStatus::invalid_reference, {}};
        }
        if (!cancellation.is_current(generations_)) {
            return {FetchStatus::cancelled, {}};
        }
        if (auto cached = find_cached(reference.media_id)) {
            return {FetchStatus::cache_hit, std::move(cached)};
        }

        const auto cancelled = [&] { return !cancellation.is_current(generations_); };
        auto response = transport_.fetch('/' + reference.media_id,
                                         static_cast<std::size_t>(reference.size), cancelled);
        if (cancelled() || response.failure == TransportFailure::cancelled) {
            return {FetchStatus::cancelled, {}};
        }
        if (response.failure != TransportFailure::none) {
            return {FetchStatus::transport_error, {}};
        }
        if (response.status != 200) {
            return {FetchStatus::unexpected_status, {}};
        }
        if (response.content_type != reference.content_type) {
            return {FetchStatus::content_type_mismatch, {}};
        }
        if (response.bytes.size() > limits_.maximum_object_bytes ||
            response.bytes.size() > reference.size) {
            return {FetchStatus::byte_limit_exceeded, {}};
        }
        if (response.bytes.size() != reference.size) {
            return {FetchStatus::size_mismatch, {}};
        }
        if (detail::hex_sha256(response.bytes) != reference.sha256) {
            return {FetchStatus::sha256_mismatch, {}};
        }
        if (cancelled()) {
            return {FetchStatus::cancelled, {}};
        }

        auto bytes = std::make_shared<const Bytes>(std::move(response.bytes));
        insert(reference.media_id, bytes);
        return {FetchStatus::fetched, std::move(bytes)};
    }

    [[nodiscard]] std::size_t cached_entries() const noexcept {
        std::scoped_lock lock{mutex_};
        return cache_.size();
    }

    [[nodiscard]] std::size_t cached_bytes() const noexcept {
        std::scoped_lock lock{mutex_};
        return cached_bytes_;
    }

    void clear() noexcept {
        std::scoped_lock lock{mutex_};
        cache_.clear();
        recency_.clear();
        cached_bytes_ = 0;
    }

private:
    struct CacheEntry final {
        std::shared_ptr<const Bytes> bytes;
        std::list<std::string>::iterator recency;
    };

    [[nodiscard]] std::shared_ptr<const Bytes> find_cached(const std::string& media_id) {
        std::scoped_lock lock{mutex_};
        const auto found = cache_.find(media_id);
        if (found == cache_.end()) return {};
        recency_.splice(recency_.begin(), recency_, found->second.recency);
        return found->second.bytes;
    }

    void insert(const std::string& media_id, const std::shared_ptr<const Bytes>& bytes) {
        std::scoped_lock lock{mutex_};
        if (const auto existing = cache_.find(media_id); existing != cache_.end()) {
            recency_.splice(recency_.begin(), recency_, existing->second.recency);
            return;
        }
        while (!cache_.empty() &&
               (cache_.size() >= limits_.maximum_entries ||
                cached_bytes_ + bytes->size() > limits_.maximum_bytes)) {
            const auto& victim = recency_.back();
            const auto found = cache_.find(victim);
            cached_bytes_ -= found->second.bytes->size();
            cache_.erase(found);
            recency_.pop_back();
        }
        recency_.push_front(media_id);
        cache_.emplace(media_id, CacheEntry{bytes, recency_.begin()});
        cached_bytes_ += bytes->size();
    }

    ITransport& transport_;
    const core::GenerationClock& generations_;
    CacheLimits limits_;
    mutable std::mutex mutex_;
    std::size_t cached_bytes_{};
    std::list<std::string> recency_;
    std::unordered_map<std::string, CacheEntry> cache_;
};

}  // namespace synth::media_fetch
