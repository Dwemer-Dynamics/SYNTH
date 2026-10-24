#pragma once
#include <cstdint>
using HRESULT = std::int32_t;
using BYTE = unsigned char;
using UINT32 = std::uint32_t;
using DWORD = std::uint32_t;
inline constexpr HRESULT S_OK = 0;
inline constexpr HRESULT E_FAIL = -1;
inline constexpr unsigned COINIT_MULTITHREADED = 0;
#define SUCCEEDED(value) ((value) >= 0)
#define FAILED(value) ((value) < 0)
inline HRESULT CoInitializeEx(void*, unsigned) { return S_OK; }
inline void CoUninitialize() {}
inline constexpr unsigned WAVE_FORMAT_PCM = 1;
inline constexpr unsigned WAVE_FORMAT_IEEE_FLOAT = 3;
inline constexpr unsigned WAVE_FORMAT_EXTENSIBLE = 0xFFFE;
#pragma pack(push, 2)
struct WAVEFORMATEX {
    std::uint16_t wFormatTag{}, nChannels{};
    std::uint32_t nSamplesPerSec{}, nAvgBytesPerSec{};
    std::uint16_t nBlockAlign{}, wBitsPerSample{}, cbSize{};
};
struct WAVEFORMATEXTENSIBLE {
    WAVEFORMATEX Format;
    std::uint16_t Samples{};
    std::uint32_t dwChannelMask{};
    struct { std::uint32_t Data1{}; std::uint16_t Data2{}, Data3{}; BYTE Data4[8]{}; } SubFormat;
};
#pragma pack(pop)
