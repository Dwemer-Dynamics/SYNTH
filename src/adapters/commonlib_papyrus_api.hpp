#pragma once
#include "integration/native_api.hpp"
#include "integration/papyrus_requests.hpp"
#if !defined(SYNTH_WITH_F4SEVR)
#include "adapters/flat_wait_package.hpp"
#endif

namespace synth::adapters::commonlib {

// These callbacks receive primitive VM values, never actors, handles or game objects.
inline bool papyrus_available(std::monostate) {
    return integration::external_requests.get_epoch() != 0;
}
inline std::int32_t papyrus_speak_exact(std::monostate, std::int32_t actor, std::string_view text) {
    return integration::submit_papyrus_request(integration::external_requests, SYNTH_EXTERNAL_SPEAK_EXACT, actor, text);
}
inline std::int32_t papyrus_comment(std::monostate, std::int32_t actor) {
    return integration::submit_papyrus_request(integration::external_requests, SYNTH_EXTERNAL_COMMENT, actor, {});
}
inline std::int32_t papyrus_react(std::monostate, std::int32_t actor, std::string_view direction) {
    return integration::submit_papyrus_request(integration::external_requests, SYNTH_EXTERNAL_REACT, actor, direction);
}
inline std::int32_t papyrus_ask(std::monostate, std::int32_t actor, std::string_view question) {
    return integration::submit_papyrus_request(integration::external_requests, SYNTH_EXTERNAL_ASK, actor, question);
}
inline std::int32_t papyrus_open_prompt(std::monostate, std::int32_t actor) {
    return integration::submit_papyrus_request(integration::external_requests, SYNTH_EXTERNAL_OPEN_PROMPT, actor, {});
}

// F4SE owns VM registration; subsequent callbacks only enqueue copied requests for the ordinary safe pump.
inline bool bind_papyrus_api(RE::BSScript::IVirtualMachine* vm) noexcept try {
    if (!vm) return false;
    const auto bind = [vm](const char* name, auto callback) {
        const auto bound = vm->BindNativeMethod(new RE::BSScript::NativeFunction("SYNTHNative", name, callback, false));
        if (bound) vm->SetCallableFromTasklets("SYNTHNative", name, true);
        return bound;
    };
    bool bound = bind("IsAvailable", papyrus_available);
    bound &= bind("SpeakExact", papyrus_speak_exact);
    bound &= bind("Comment", papyrus_comment);
    bound &= bind("React", papyrus_react);
    bound &= bind("Ask", papyrus_ask);
    bound &= bind("OpenPrompt", papyrus_open_prompt);
#if !defined(SYNTH_WITH_F4SEVR)
    bound &= FlatWaitPackage::bind(*vm);
#endif
#if defined(SYNTH_WITH_F4SEVR)
    spdlog::info("SYNTH Papyrus native registration complete: {}", bound);
#else
    REX::INFO("SYNTH Papyrus native registration complete: {}", bound);
#endif
    return bound;
} catch (...) {
    return false; // Optional integration must not unwind through F4SE's VM callback.
}

inline bool register_papyrus_api() noexcept try {
    const auto* papyrus = F4SE::GetPapyrusInterface();
    return papyrus && papyrus->Version() == F4SE::PapyrusInterface::kVersion && papyrus->Register(bind_papyrus_api);
} catch (...) {
    return false;
}
}
