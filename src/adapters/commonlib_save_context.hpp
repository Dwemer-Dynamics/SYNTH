#pragma once

#include "core/save_context.hpp"
#include <F4SE/F4SE.h>

namespace synth::adapters::commonlib {

inline core::SaveContextStore save_context_store;

// F4SE owns co-save I/O. These callbacks never inspect engine objects or wait on a worker.
inline void save_context_callback(const F4SE::SerializationInterface* serialization) noexcept {
    try {
        if (serialization) (void)core::write_saved_context(*serialization, save_context_store);
    } catch (...) {}
}

inline void load_context_callback(const F4SE::SerializationInterface* serialization) noexcept {
    try {
        if (serialization) core::read_saved_context(*serialization, save_context_store);
        else (void)save_context_store.reset();
    } catch (...) { (void)save_context_store.reset(); }
}

inline void revert_context_callback(const F4SE::SerializationInterface*) noexcept {
    (void)save_context_store.reset();
}

inline bool register_save_context() {
    const auto* serialization = F4SE::GetSerializationInterface();
    if (!serialization || serialization->Version() < F4SE::SerializationInterface::kVersion) return false;
    serialization->SetUniqueID(0x53594E54); // SYNT, shared flat/VR record identity; lanes remain separately validated.
    serialization->SetSaveCallback(save_context_callback);
    serialization->SetLoadCallback(load_context_callback);
    serialization->SetRevertCallback(revert_context_callback);
    return true;
}

} // namespace synth::adapters::commonlib
