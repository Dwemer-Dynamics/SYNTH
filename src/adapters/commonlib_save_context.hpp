#pragma once

#include "core/save_context.hpp"
#include <F4SE/F4SE.h>

// The one bcrypt.dll import, declared with its exact ABI types. Including <windows.h> here would
// define near/far and other macros ahead of later SYNTH and CommonLib headers.
extern "C" long __stdcall BCryptGenRandom(void* algorithm, unsigned char* buffer, unsigned long size,
                                          unsigned long flags);

namespace synth::adapters::commonlib {

inline constexpr unsigned long bcrypt_use_system_preferred_rng = 0x00000002; // BCRYPT_USE_SYSTEM_PREFERRED_RNG

#if defined(SYNTH_WITH_F4SEVR)
inline core::SaveContextStore save_context_store{core::RuntimeVariant::vr};
#else
inline core::SaveContextStore save_context_store{core::RuntimeVariant::flat};
#endif

// Called only by NewGame or a successful PostLoadGame that needs a fresh identity, outside the store lock.
// Never called from a frame, save or load serializer callback. Any failure leaves no identity.
[[nodiscard]] inline std::optional<core::PlaythroughUuid> system_random_identity() noexcept {
    core::PlaythroughUuid bytes{};
    if (BCryptGenRandom(nullptr, bytes.data(), static_cast<unsigned long>(bytes.size()), bcrypt_use_system_preferred_rng) != 0)
        return std::nullopt;
    return bytes;
}

// Game-thread observers set once at registration: a revert outside a load hands teardown to the existing
// deferred lifecycle path, and a failed co-save write is reported once per process (never retried).
struct SaveContextObserver final {
    void (*revoked)() noexcept{};
    void (*write_failed)() noexcept{};
};
inline SaveContextObserver save_context_observer;

// F4SE owns co-save I/O. These callbacks never inspect engine objects or wait on a worker.
inline void save_context_callback(const F4SE::SerializationInterface* serialization) noexcept {
    static bool reported{};
    bool written{};
    try {
        written = serialization && core::write_saved_context(*serialization, save_context_store);
    } catch (...) {}
    if (!written && !std::exchange(reported, true) && save_context_observer.write_failed) save_context_observer.write_failed();
}

// Claims the open load at entry and stages into exactly that load; PostLoadGame alone decides whether it
// becomes active. A missing interface or any exception stages a malformed outcome through the claim.
inline void load_context_callback(const F4SE::SerializationInterface* serialization) noexcept {
    try {
        if (serialization) core::read_saved_context(*serialization, save_context_store);
        else (void)save_context_store.enter_load();
    } catch (...) {}
}

inline void revert_context_callback(const F4SE::SerializationInterface*) noexcept {
    bool revoked{};
    try { revoked = save_context_store.revert(); } catch (...) {}
    if (revoked && save_context_observer.revoked) save_context_observer.revoked();
}

inline bool register_save_context(SaveContextObserver observer) {
    save_context_observer = observer;
    const auto* serialization = F4SE::GetSerializationInterface();
    if (!serialization || serialization->Version() < F4SE::SerializationInterface::kVersion) return false;
    serialization->SetUniqueID(0x53594E54); // SYNT, shared flat/VR record identity; lanes remain separately validated.
    serialization->SetSaveCallback(save_context_callback);
    serialization->SetLoadCallback(load_context_callback);
    serialization->SetRevertCallback(revert_context_callback);
    return true;
}

} // namespace synth::adapters::commonlib
