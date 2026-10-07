#pragma once
#include "integration/external_requests.hpp"

namespace synth::integration {
inline ExternalRequests external_requests;
}

// Stable C export; callable by native mods only while this module remains loaded.
extern "C" __declspec(dllexport) const SynthExternalAPIV1* SYNTH_GetExternalAPI(std::uint32_t version) noexcept {
    static const SynthExternalAPIV1 api{
        sizeof(SynthExternalAPIV1),SYNTH_EXTERNAL_API_VERSION,
        []() -> std::uint64_t { return synth::integration::external_requests.get_epoch(); },
        [](const SynthExternalRequestV1* request) -> std::uint32_t {
            return synth::integration::external_requests.submit(request);
        }};
    return version==SYNTH_EXTERNAL_API_VERSION ? &api : nullptr;
}
