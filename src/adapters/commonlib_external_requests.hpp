#pragma once
#include "adapters/commonlib_capture.hpp"
#include "client/plugin_session.hpp"
#include "integration/native_api.hpp"

namespace synth::adapters::commonlib {
struct ExternalAdmission { std::uint32_t form_id{},kind{}; bool admitted{}; };

// Drain at most one copied request on the ordinary safe game pump, never in the public callback.
[[nodiscard]] inline std::optional<ExternalAdmission> pump_external_requests(
    runtime::IFalloutRuntime& runtime, client::PluginSession& session, bool allowed,
    const std::function<bool(integration::PromptTarget)>& open_prompt = {}) {
    runtime.assert_game_thread();
    auto& requests=integration::external_requests;
    requests.set_available(allowed && session.external_admission_ready());
    const auto request=requests.take();
    if (!request) return {};
    ExternalAdmission result{request->actor_form_id,request->kind,false};
    const auto snapshot=runtime.capture_actor_snapshot(core::SnapshotClock::now(),request->actor_form_id);
    if (request->epoch!=requests.get_epoch()) return result;
    if (request->kind == SYNTH_EXTERNAL_OPEN_PROMPT) {
        if (open_prompt) {
            auto target=session.prepare_external_prompt(snapshot,request->actor_form_id);
            if (target) result.admitted=open_prompt(std::move(*target));
        }
    } else result.admitted=session.submit_external(snapshot,*request);
    if (result.admitted) requests.set_available(false);
    return result;
}
}
