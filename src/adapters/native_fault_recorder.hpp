#pragma once
#include <filesystem>

namespace synth::adapters {

// Keep Windows SDK macros and exception machinery out of the engine adapter headers.
class NativeFaultRecorder final {
public:
    static void phase(const char* value) noexcept;
    static bool install(const std::filesystem::path& directory);
    static bool install_game_log();
};

} // namespace synth::adapters
