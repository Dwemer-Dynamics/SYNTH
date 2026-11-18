// Location catalog export: the engine-free state of one export run and of the first-entry upserts.
//
// Threads: every method here runs on the game thread, because the reader callback touches engine
// objects. The network lane only carries the encoded request (SynthClient::send_location_catalog)
// and hands its RequestOutcome back to answered() on the game thread, so no state is shared.
// One object belongs to one runtime session; a new generation creates a new one, which drops a
// stale queue and its late answers.
//
// The export is paced by the server: the next batch is built only after the previous answer, so a
// slow server, a stop or a failed batch never leaves a queue of unsent engine reads.
#pragma once

#include "client/synth_client.hpp"
#include "core/location_catalog.hpp"
#include "protocol_native/v1_codec.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace synth::client {

// What the server said about one request. A transport or protocol failure is `accepted == false`.
struct LocationCatalogAnswer final {
    bool accepted{};
    std::size_t received{};
    std::size_t valid{};
    std::size_t refused{};
    // The server's own statement that it kept the records. A storage-less server answers accepted with stored false.
    bool stored{};
};

// The server's only catalog acknowledgement: start, one location_catalog_accepted status whose detail is
// the counts as JSON, then a complete end. Anything else, including extra lines or malformed counts, is a
// failed batch and never counted as stored.
[[nodiscard]] inline LocationCatalogAnswer location_catalog_answer(const RequestOutcome& outcome) {
    if (outcome.status != RequestStatus::complete || outcome.lines.size() != 3) return {};
    const auto* status = std::get_if<protocol_native::Status>(&outcome.lines[1].payload);
    const auto* end = std::get_if<protocol_native::End>(&outcome.lines[2].payload);
    if (!std::holds_alternative<protocol_native::Start>(outcome.lines[0].payload) || !status || !end ||
        status->code != "location_catalog_accepted" || end->status != "complete") return {};
    try {
        const auto detail = json::parse(status->detail);
        const auto& refused = protocol_native::required(detail, "refused");
        if (!refused.is_array()) return {};
        const auto& stored = protocol_native::required(detail, "stored");
        if (!stored.is_bool()) return {};
        return {true, static_cast<std::size_t>(protocol_native::integer(detail, "received")),
                static_cast<std::size_t>(protocol_native::integer(detail, "valid")), refused.as_array().size(), stored.as_bool()};
    } catch (const std::exception&) {
        return {};
    }
}

class LocationExport final {
public:
    // Reads one location on the game thread; empty when the form is gone, unnamed or unreadable.
    using Reader = std::function<std::optional<core::CatalogLocation>(const core::PluginForm&)>;

    // What the Utilities window shows. Counts cover the current or last export.
    struct Progress final {
        bool running{};
        std::size_t total{};
        std::size_t sent{};
        std::size_t accepted{};
        // Dropped by the client (unreadable or out of bounds), refused by the server, or in a failed batch.
        std::size_t refused{};
        // The server accepted the request but is not storing yet: the run ended early and may be started again.
        bool server_not_storing{};
        std::string message;
    };

    explicit LocationExport(std::string playthrough_id) : playthrough_id_{std::move(playthrough_id)} {}

    // Starts a run over the given locations unless one is running. The caller supplies the export id.
    [[nodiscard]] bool begin(std::string export_id, std::vector<core::PluginForm> locations) {
        if (progress_.running || !runtime::social_id(export_id)) return false;
        export_id_ = std::move(export_id);
        queue_.assign(std::make_move_iterator(locations.begin()), std::make_move_iterator(locations.end()));
        sequence_ = 0;
        stop_requested_ = false;
        awaiting_answer_ = false;
        progress_ = {};
        progress_.running = true;
        progress_.total = queue_.size();
        return true;
    }

    // Stops after the batch on its way; locations already stored stay on the server.
    void stop() noexcept { stop_requested_ = true; }

    [[nodiscard]] const Progress& progress() const noexcept { return progress_; }

    // Builds the next batch, or nothing when the run is over or a batch is still unanswered. The last batch
    // carries final. Records the reader cannot read, or that break a bound, count as refused without a request.
    [[nodiscard]] std::optional<protocol_native::LocationCatalogRequest> next_batch(const Reader& read) {
        if (!progress_.running || awaiting_answer_) return std::nullopt;
        if (stop_requested_) { finish("Stopped"); return std::nullopt; }
        protocol_native::LocationCatalogRequest request;
        request.mode = protocol_native::LocationCatalogMode::batch;
        request.playthrough_id = playthrough_id_;
        request.export_id = export_id_;
        while (!queue_.empty() && request.locations.size() < core::maximum_catalog_batch) {
            const auto form = std::move(queue_.front());
            queue_.pop_front();
            auto location = read(form);
            if (location && location->valid() && !already_in(request, *location)) request.locations.push_back(std::move(*location));
            else { ++progress_.refused; ++progress_.sent; }
        }
        if (request.locations.empty()) { finish(); return std::nullopt; }
        request.sequence = sequence_++;
        request.final = queue_.empty();
        in_flight_ = request.locations.size();
        awaiting_answer_ = true;
        progress_.message = "Sending locations";
        return request;
    }

    // One batch was answered (or failed) on the network lane. A failed batch is counted and the run goes on.
    void answered(const LocationCatalogAnswer& answer) {
        if (!progress_.running || !awaiting_answer_) return;
        awaiting_answer_ = false;
        progress_.sent += in_flight_;
        if (answer.accepted && !answer.stored) {
            // Nothing was kept, so nothing is counted as accepted and the rest is not sent; the export can run again.
            in_flight_ = 0;
            progress_.server_not_storing = true;
            finish("Server not storing locations yet");
            return;
        }
        if (answer.accepted) {
            const auto stored = std::min(answer.valid, in_flight_);
            progress_.accepted += stored;
            progress_.refused += in_flight_ - stored;
        } else {
            progress_.refused += in_flight_;
        }
        in_flight_ = 0;
        if (queue_.empty()) finish();
    }

    // One location on its first entry in this session, before the context that names it. A location is
    // upserted once per session; the export is separate and may send the same record again, which the
    // server treats as an unchanged upsert.
    [[nodiscard]] std::optional<protocol_native::LocationCatalogRequest> upsert_on_first_entry(const core::PluginForm& form,
                                                                                               const Reader& read) {
        if (std::ranges::any_of(sent_, [&](const core::PluginForm& seen) { return seen.same_form(form); })) return std::nullopt;
        auto location = read(form);
        if (!location || !location->valid()) return std::nullopt;
        sent_.push_back(form);
        protocol_native::LocationCatalogRequest request;
        request.mode = protocol_native::LocationCatalogMode::upsert;
        request.playthrough_id = playthrough_id_;
        request.locations.push_back(std::move(*location));
        return request;
    }

private:
    [[nodiscard]] static bool already_in(const protocol_native::LocationCatalogRequest& request, const core::CatalogLocation& location) {
        return std::ranges::any_of(request.locations, [&](const core::CatalogLocation& held) { return held.ref.same_form(location.ref); });
    }

    void finish(std::string message = "Finished") {
        progress_.running = false;
        progress_.message = std::move(message);
        queue_.clear();
    }

    std::string playthrough_id_;
    std::string export_id_;
    std::deque<core::PluginForm> queue_;
    std::vector<core::PluginForm> sent_;
    Progress progress_;
    std::uint64_t sequence_{};
    std::size_t in_flight_{};
    bool stop_requested_{};
    bool awaiting_answer_{};
};

}  // namespace synth::client
