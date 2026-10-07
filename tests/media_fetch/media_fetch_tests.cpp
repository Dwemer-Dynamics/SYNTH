#include "media_fetch/media_fetch.hpp"

#include <cstddef>
#include <deque>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace synth::media_fetch;
using synth::core::CancellationSource;
using synth::core::GenerationClock;

int assertions{};

#define CHECK(condition)                                                                        \
    do {                                                                                        \
        ++assertions;                                                                           \
        if (!(condition)) {                                                                     \
            throw std::runtime_error{std::string{"CHECK failed: "} + #condition + " at " +     \
                                     __FILE__ + ":" + std::to_string(__LINE__)};                \
        }                                                                                       \
    } while (false)

template <class Exception, class Function>
void check_throws(Function&& function) {
    ++assertions;
    try {
        std::forward<Function>(function)();
    } catch (const Exception&) {
        return;
    }
    throw std::runtime_error{"expected exception was not thrown"};
}

Bytes bytes(std::string_view value) {
    Bytes result;
    result.reserve(value.size());
    for (const auto character : value) {
        result.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
    }
    return result;
}

MediaReference reference(std::string_view value, std::string content_type = "audio/wav") {
    auto payload = bytes(value);
    auto sha256 = detail::hex_sha256(payload);
    return {"media/" + sha256, std::move(content_type), std::move(sha256), payload.size()};
}

class ScriptedTransport final : public ITransport {
public:
    struct Request final {
        std::string target;
        std::size_t max_bytes{};
    };

    std::deque<TransportResponse> responses;
    std::vector<Request> requests;
    std::function<void()> during_fetch;

    [[nodiscard]] TransportResponse fetch(std::string_view target, std::size_t max_bytes,
                                          const Cancelled& cancelled) override {
        requests.push_back({std::string{target}, max_bytes});
        CHECK(!target.starts_with("http://"));
        CHECK(!target.starts_with("https://"));
        CHECK(target.starts_with("/media/"));
        if (cancelled()) return {TransportFailure::cancelled, 0, {}, {}};
        if (during_fetch) during_fetch();
        if (cancelled()) return {TransportFailure::cancelled, 0, {}, {}};
        if (responses.empty()) throw std::logic_error{"missing scripted media response"};
        auto response = std::move(responses.front());
        responses.pop_front();
        return response;
    }
};

TransportResponse response(std::string_view value, std::string content_type = "audio/wav",
                           int status = 200) {
    return {TransportFailure::none, status, std::move(content_type), bytes(value)};
}

void test_sha256_vectors_and_identifier_validation() {
    CHECK(detail::hex_sha256(bytes("")) ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(detail::hex_sha256(bytes("abc")) ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    const auto valid = reference("wave");
    CHECK(is_same_origin_media_id(valid.media_id));
    CHECK(is_valid(valid));
    CHECK(!is_same_origin_media_id("https://host/media/" + valid.sha256));
    CHECK(!is_same_origin_media_id("//host/media/" + valid.sha256));
    CHECK(!is_same_origin_media_id("media/../" + valid.sha256));
    CHECK(!is_same_origin_media_id("/media/" + valid.sha256));
    CHECK(!is_same_origin_media_id("media/" + std::string(64, 'A')));
    CHECK(!is_same_origin_media_id("media/" + valid.sha256 + "?download=1"));
    auto mismatched_id = valid;
    mismatched_id.sha256 = std::string(64, '0');
    CHECK(!is_valid(mismatched_id));
}

void test_verified_fetch_and_cache_hit() {
    GenerationClock generations;
    CancellationSource cancellation{generations.current()};
    ScriptedTransport transport;
    transport.responses.push_back(response("RIFF-wave"));
    MediaFetcher fetcher{transport, generations, {4, 64, 32}};
    const auto media = reference("RIFF-wave");

    const auto first = fetcher.fetch(media, cancellation.token());
    CHECK(first.status == FetchStatus::fetched);
    CHECK(first.bytes && *first.bytes == bytes("RIFF-wave"));
    CHECK(transport.requests.size() == 1);
    CHECK(transport.requests.front().target == '/' + media.media_id);
    CHECK(transport.requests.front().max_bytes == media.size);
    CHECK(fetcher.cached_entries() == 1);
    CHECK(fetcher.cached_bytes() == media.size);

    const auto second = fetcher.fetch(media, cancellation.token());
    CHECK(second.status == FetchStatus::cache_hit);
    CHECK(second.bytes == first.bytes);
    CHECK(transport.requests.size() == 1);
}

void test_verification_failures_do_not_cache() {
    GenerationClock generations;
    CancellationSource cancellation{generations.current()};
    ScriptedTransport transport;
    MediaFetcher fetcher{transport, generations, {4, 64, 32}};

    auto expected = reference("good");
    transport.responses.push_back(response("good", "audio/ogg"));
    CHECK(fetcher.fetch(expected, cancellation.token()).status ==
          FetchStatus::content_type_mismatch);

    transport.responses.push_back(response("evil"));
    CHECK(fetcher.fetch(expected, cancellation.token()).status == FetchStatus::sha256_mismatch);

    transport.responses.push_back(response("go"));
    CHECK(fetcher.fetch(expected, cancellation.token()).status == FetchStatus::size_mismatch);

    transport.responses.push_back(response("good-extra"));
    CHECK(fetcher.fetch(expected, cancellation.token()).status ==
          FetchStatus::byte_limit_exceeded);

    transport.responses.push_back(response("good", "audio/wav", 404));
    CHECK(fetcher.fetch(expected, cancellation.token()).status ==
          FetchStatus::unexpected_status);

    transport.responses.push_back({TransportFailure::unavailable, 0, {}, {}});
    CHECK(fetcher.fetch(expected, cancellation.token()).status == FetchStatus::transport_error);
    CHECK(fetcher.cached_entries() == 0);

    auto unsupported = expected;
    unsupported.content_type = "text/html";
    CHECK(fetcher.fetch(unsupported, cancellation.token()).status ==
          FetchStatus::invalid_reference);
    CHECK(transport.requests.size() == 6);
}

void test_lru_entry_and_byte_bounds() {
    GenerationClock generations;
    CancellationSource cancellation{generations.current()};
    ScriptedTransport transport;
    MediaFetcher fetcher{transport, generations, {2, 5, 5}};
    const auto a = reference("aa");
    const auto b = reference("bb");
    const auto c = reference("ccc");
    transport.responses.push_back(response("aa"));
    transport.responses.push_back(response("bb"));
    transport.responses.push_back(response("ccc"));
    transport.responses.push_back(response("bb"));

    CHECK(fetcher.fetch(a, cancellation.token()).ok());
    CHECK(fetcher.fetch(b, cancellation.token()).ok());
    CHECK(fetcher.fetch(a, cancellation.token()).status == FetchStatus::cache_hit);
    CHECK(fetcher.fetch(c, cancellation.token()).ok());
    CHECK(fetcher.cached_entries() == 2);
    CHECK(fetcher.cached_bytes() == 5);
    CHECK(fetcher.fetch(b, cancellation.token()).status == FetchStatus::fetched);
    CHECK(transport.requests.size() == 4);
    CHECK(fetcher.cached_entries() == 2);
    CHECK(fetcher.cached_bytes() == 5);
}

void test_generation_cancellation() {
    GenerationClock generations;
    ScriptedTransport transport;
    MediaFetcher fetcher{transport, generations, {2, 32, 16}};
    const auto media = reference("cancel-me");

    CancellationSource stale{generations.current()};
    (void)generations.advance();
    CHECK(fetcher.fetch(media, stale.token()).status == FetchStatus::cancelled);
    CHECK(transport.requests.empty());

    CancellationSource in_flight{generations.current()};
    transport.responses.push_back(response("cancel-me"));
    transport.during_fetch = [&] { (void)generations.advance(); };
    CHECK(fetcher.fetch(media, in_flight.token()).status == FetchStatus::cancelled);
    CHECK(fetcher.cached_entries() == 0);
    CHECK(transport.requests.size() == 1);

    CancellationSource explicitly_cancelled{generations.current()};
    explicitly_cancelled.cancel();
    CHECK(fetcher.fetch(media, explicitly_cancelled.token()).status == FetchStatus::cancelled);
    CHECK(transport.requests.size() == 1);
}

void test_limit_validation() {
    GenerationClock generations;
    ScriptedTransport transport;
    check_throws<std::invalid_argument>([&] {
        (void)MediaFetcher{transport, generations, {0, 1, 1}};
    });
    check_throws<std::invalid_argument>([&] {
        (void)MediaFetcher{transport, generations, {1, 4, 5}};
    });

    CancellationSource cancellation{generations.current()};
    MediaFetcher fetcher{transport, generations, {1, 4, 4}};
    const auto oversized = reference("12345");
    CHECK(fetcher.fetch(oversized, cancellation.token()).status ==
          FetchStatus::invalid_reference);
    CHECK(transport.requests.empty());
}

}  // namespace

int main() {
    try {
        test_sha256_vectors_and_identifier_validation();
        test_verified_fetch_and_cache_hit();
        test_verification_failures_do_not_cache();
        test_lru_entry_and_byte_bounds();
        test_generation_cancellation();
        test_limit_validation();
        std::cout << "media fetch tests passed (" << assertions << " assertions)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
