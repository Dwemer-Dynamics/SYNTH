#pragma once
#include "integration/external_requests.hpp"

namespace synth::integration {

// Papyrus supplies value arguments only; capture the current epoch and copy into the existing bounded queue.
[[nodiscard]] inline std::int32_t submit_papyrus_request(ExternalRequests& queue, std::uint32_t kind,
    std::int32_t actor_form_id, std::string_view text) noexcept {
    if (text.size() > 1000 || text.find('\0') != std::string_view::npos) return SYNTH_EXTERNAL_INVALID;
    SynthExternalRequestV1 request{};
    request.size = sizeof(request);
    request.kind = kind;
    // Papyrus Int is signed; high-bit runtime FormIDs must retain their full 32-bit representation.
    request.actor_form_id = static_cast<std::uint32_t>(actor_form_id);
    request.epoch = queue.get_epoch();
    if (!text.empty()) std::memcpy(request.text, text.data(), text.size());
    return static_cast<std::int32_t>(queue.submit(&request));
}

}
