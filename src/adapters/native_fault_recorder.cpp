#include <Windows.h>
#include <Psapi.h>
#include <ShlObj.h>
#include "adapters/native_fault_recorder.hpp"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>

namespace synth::adapters {

// Process-lifetime, first-chance diagnostics. No debugger, exception suppression,
// game objects, stack contents, or network calls. The output handle is opened at
// plugin startup; normal frames only update their thread-local phase pointer.
class NativeFaultRecorderState final {
public:
    static void phase(const char* value) noexcept { phase_ = value; }

    static bool install(const std::filesystem::path& directory) {
        if (handler_) return true;
        wchar_t name[96]{};
        swprintf_s(name, L"SYNTH-fault-%lu-%llu.log", GetCurrentProcessId(), GetTickCount64());
        const auto path = directory / name;
        file_ = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
        if (file_ == INVALID_HANDLE_VALUE) return false;
        handler_ = AddVectoredExceptionHandler(1, record);
        if (!handler_) { CloseHandle(file_); file_ = INVALID_HANDLE_VALUE; return false; }
        constexpr char header[] = "SYNTH first-chance fault recorder armed; candidates are not necessarily fatal.\r\n";
        DWORD written{};
        WriteFile(file_, header, sizeof(header) - 1, &written, nullptr);
        return true;
    }

private:
    // Keep exception-path work bounded and pass every exception to its original handlers.
    static LONG CALLBACK record(EXCEPTION_POINTERS* failure) noexcept {
        if (!failure || !failure->ExceptionRecord || !failure->ContextRecord) return EXCEPTION_CONTINUE_SEARCH;
        const auto& exception = *failure->ExceptionRecord;
        if (exception.ExceptionCode != EXCEPTION_ACCESS_VIOLATION &&
            exception.ExceptionCode != EXCEPTION_IN_PAGE_ERROR &&
            exception.ExceptionCode != EXCEPTION_ILLEGAL_INSTRUCTION &&
            exception.ExceptionCode != 0xC0000374 && exception.ExceptionCode != 0xC0000409)
            return EXCEPTION_CONTINUE_SEARCH;
        // Do not attempt formatting or a stack walk during stack exhaustion.
        if (writing_.test_and_set(std::memory_order_acquire)) return EXCEPTION_CONTINUE_SEARCH;
        if (records_++ < 16) {
            MEMORY_BASIC_INFORMATION region{};
            VirtualQuery(exception.ExceptionAddress, &region, sizeof(region));
            wchar_t module[768]{};
            K32GetMappedFileNameW(GetCurrentProcess(), exception.ExceptionAddress, module, 767);
            char message[4096]{};
            const auto address = reinterpret_cast<std::uintptr_t>(exception.ExceptionAddress);
            const auto base = reinterpret_cast<std::uintptr_t>(region.AllocationBase);
            const int size = snprintf(message, sizeof(message),
                "candidate=%u thread=%lu code=0x%08lX pc=0x%llX base=0x%llX offset=0x%llX "
                "operation=%llu target=0x%llX phase=%.96s module=%ls\r\n",
                records_, GetCurrentThreadId(), exception.ExceptionCode,
                static_cast<unsigned long long>(address), static_cast<unsigned long long>(base),
                static_cast<unsigned long long>(address - base),
                static_cast<unsigned long long>(exception.NumberParameters > 0 ? exception.ExceptionInformation[0] : 0),
                static_cast<unsigned long long>(exception.NumberParameters > 1 ? exception.ExceptionInformation[1] : 0),
                phase_, module);
            DWORD written{};
            if (size > 0) WriteFile(file_, message, static_cast<DWORD>(size < static_cast<int>(sizeof(message)) ? size : sizeof(message) - 1), &written, nullptr);
            void* frames[24]{};
            const auto count = CaptureStackBackTrace(0, 24, frames, nullptr);
            for (USHORT i = 0; i < count; ++i) {
                MEMORY_BASIC_INFORMATION frame_region{};
                VirtualQuery(frames[i], &frame_region, sizeof(frame_region));
                const auto frame = reinterpret_cast<std::uintptr_t>(frames[i]);
                const auto frame_base = reinterpret_cast<std::uintptr_t>(frame_region.AllocationBase);
                const int length = snprintf(message, sizeof(message), " frame=%u pc=0x%llX base=0x%llX offset=0x%llX\r\n",
                    i, static_cast<unsigned long long>(frame), static_cast<unsigned long long>(frame_base),
                    static_cast<unsigned long long>(frame - frame_base));
                if (length > 0) WriteFile(file_, message, static_cast<DWORD>(length), &written, nullptr);
            }
        }
        writing_.clear(std::memory_order_release);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    inline static HANDLE file_{INVALID_HANDLE_VALUE};
    inline static void* handler_{};
    inline static std::atomic_flag writing_{};
    inline static unsigned records_{};
    inline static thread_local const char* phase_{"native/worker"};
};

void NativeFaultRecorder::phase(const char* value) noexcept { NativeFaultRecorderState::phase(value); }

bool NativeFaultRecorder::install(const std::filesystem::path& directory) {
    return NativeFaultRecorderState::install(directory);
}

bool NativeFaultRecorder::install_game_log() {
    wchar_t documents[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, SHGFP_TYPE_CURRENT, documents))) return false;
    return install(std::filesystem::path{documents} / "My Games/Fallout4/F4SE");
}

} // namespace synth::adapters
