#pragma once

#include "client/http.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace synth::client {

enum class FakeScenarioKind : unsigned char {
    respond,
    timeout,
    server_loss,
    malformed,
    oversized,
};

struct FakeScenario final {
    FakeScenarioKind kind{FakeScenarioKind::respond};
    HttpResponse response{200, "application/json", "{}"};
    std::size_t oversized_body_bytes{};
};

class FakeHttpServer final {
public:
    void start() noexcept { running_ = true; }
    void stop() noexcept { running_ = false; }
    void restart() noexcept { ++restart_count_; running_ = true; }
    [[nodiscard]] bool running() const noexcept { return running_; }
    [[nodiscard]] std::size_t restart_count() const noexcept { return restart_count_; }
    [[nodiscard]] const std::vector<HttpRequest>& requests() const noexcept { return requests_; }

    void enqueue(FakeScenario scenario) { scenarios_.push_back(std::move(scenario)); }

    [[nodiscard]] HttpResult receive(const HttpRequest& request, const Cancelled& cancelled) {
        requests_.push_back(request);
        if (cancelled && cancelled()) return {TransportFailure::cancelled, {}};
        if (!running_) return {TransportFailure::server_unavailable, {}};
        if (scenarios_.empty()) throw std::logic_error{"fake server has no scripted response"};
        auto scenario = std::move(scenarios_.front());
        scenarios_.pop_front();
        switch (scenario.kind) {
        case FakeScenarioKind::timeout:
            return {TransportFailure::timeout, {}};
        case FakeScenarioKind::server_loss:
            running_ = false;
            return {TransportFailure::server_unavailable, {}};
        case FakeScenarioKind::malformed:
            return {TransportFailure::none,
                    {200, "application/json", std::string{"{\"truncated\":"}}};
        case FakeScenarioKind::oversized:
            return {TransportFailure::none,
                    {200, "application/json", std::string(scenario.oversized_body_bytes, 'x')}};
        case FakeScenarioKind::respond:
            return {TransportFailure::none, std::move(scenario.response)};
        }
        throw std::logic_error{"unknown fake server scenario"};
    }

private:
    bool running_{true};
    std::size_t restart_count_{};
    std::deque<FakeScenario> scenarios_;
    std::vector<HttpRequest> requests_;
};

class FakeHttpTransport final : public IHttpTransport {
public:
    explicit FakeHttpTransport(FakeHttpServer& server) noexcept : server_{server} {}

    [[nodiscard]] HttpResult send(const HttpRequest& request,
                                  const Cancelled& cancelled) override {
        if (request.timeout_ms == 0) throw std::invalid_argument{"HTTP timeout must be nonzero"};
        if (request.target.empty() || request.target.front() != '/')
            throw std::invalid_argument{"HTTP target must be absolute-path relative"};
        return server_.receive(request, cancelled);
    }

    void cancel_all() noexcept override { ++cancel_count_; }
    [[nodiscard]] std::size_t cancel_count() const noexcept { return cancel_count_; }

private:
    FakeHttpServer& server_;
    std::size_t cancel_count_{};
};

}  // namespace synth::client
