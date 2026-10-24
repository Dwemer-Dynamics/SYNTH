#pragma once
#include "windows.h"
#include <string>
#include <stdexcept>
#include <vector>

inline constexpr UINT32 XAUDIO2_END_OF_STREAM = 0x40;
inline constexpr UINT32 XAUDIO2_VOICE_NOSAMPLESPLAYED = 0x100;
struct XAUDIO2_VOICE_STATE { UINT32 BuffersQueued{}; std::uint64_t SamplesPlayed{}; };
struct XAUDIO2_VOICE_DETAILS { UINT32 InputChannels = 2; };
struct XAUDIO2_BUFFER {
    UINT32 Flags{}, AudioBytes{};
    const BYTE* pAudioData{};
    UINT32 PlayBegin{}, PlayLength{};
};
class IXAudio2SourceVoice;
namespace fake_xaudio {
inline std::vector<std::string> events;
inline IXAudio2SourceVoice* voice{};
inline bool fail_create{}, fail_submit{}, fail_start{};
inline bool fail_matrix{}, fail_volume{};
inline bool throw_matrix{};
inline UINT32 destination_channels{2}, source_channels{}, matrix_source_channels{}, matrix_destination_channels{};
inline DWORD destination_mask{3};
inline float volume{};
inline std::vector<float> matrix;
inline std::vector<std::uint64_t> start_samples;
inline void reset() {
    events.clear(); matrix.clear(); start_samples.clear(); fail_create = fail_submit = fail_start = fail_matrix = fail_volume = false;
    throw_matrix=false;
    destination_channels = 2; destination_mask = 3; source_channels = matrix_source_channels = matrix_destination_channels = 0;
    volume = 0.0F;
}
}
class IXAudio2MasteringVoice {
public:
    void GetVoiceDetails(XAUDIO2_VOICE_DETAILS* value) { *value = {fake_xaudio::destination_channels}; }
    HRESULT GetChannelMask(DWORD* value) { *value = fake_xaudio::destination_mask; return S_OK; }
    void DestroyVoice() { delete this; }
};
class IXAudio2SourceVoice {
public:
    HRESULT SetVolume(float value) {
        if (fake_xaudio::fail_volume) return E_FAIL;
        fake_xaudio::volume = value; return S_OK;
    }
    HRESULT SetOutputMatrix(IXAudio2MasteringVoice*, UINT32 source, UINT32 destination, const float* values) {
        if (fake_xaudio::throw_matrix) throw std::runtime_error{"injected matrix exception"};
        if (fake_xaudio::fail_matrix) return E_FAIL;
        fake_xaudio::matrix_source_channels = source; fake_xaudio::matrix_destination_channels = destination;
        fake_xaudio::matrix.assign(values, values + source * destination);
        return S_OK;
    }
    HRESULT SubmitSourceBuffer(const XAUDIO2_BUFFER*) {
        fake_xaudio::events.push_back("submit"); queued = 1;
        return fake_xaudio::fail_submit ? E_FAIL : S_OK;
    }
    HRESULT Start() {
        fake_xaudio::events.push_back("start");
        fake_xaudio::start_samples.push_back(samples_played);
        if (fake_xaudio::fail_start) return E_FAIL;
        running=true; return S_OK;
    }
    HRESULT Stop() { fake_xaudio::events.push_back("stop"); running=false; return S_OK; }
    HRESULT FlushSourceBuffers() { fake_xaudio::events.push_back("flush"); queued = 0; return S_OK; }
    void DestroyVoice() { fake_xaudio::events.push_back("destroy"); fake_xaudio::voice = nullptr; delete this; }
    void GetState(XAUDIO2_VOICE_STATE* value, UINT32 = 0) { *value = {queued, samples_played}; }
    void advance(std::uint64_t samples) { if(running && queued) samples_played+=samples; }
    bool running{};
    std::uint64_t samples_played{};
    UINT32 queued{};
};
class IXAudio2 {
public:
    HRESULT CreateMasteringVoice(IXAudio2MasteringVoice** value) { *value = new IXAudio2MasteringVoice; return S_OK; }
    HRESULT CreateSourceVoice(IXAudio2SourceVoice** value, const WAVEFORMATEX* format) {
        fake_xaudio::events.push_back("create");
        if (fake_xaudio::fail_create) return E_FAIL;
        fake_xaudio::source_channels = format->nChannels;
        *value = new IXAudio2SourceVoice; fake_xaudio::voice = *value; return S_OK;
    }
    void Release() { delete this; }
};
inline HRESULT XAudio2Create(IXAudio2** value) { *value = new IXAudio2; return S_OK; }

namespace fake_xaudio {
// Model the documented API independently: destination D receives sum(input[S] * matrix[SourceChannels*D+S]).
inline std::vector<float> render(const std::vector<float>& input) {
    std::vector<float> output(matrix_destination_channels,0.0F);
    if(!voice || !voice->running) return output;
    for(UINT32 destination=0;destination<matrix_destination_channels;++destination)
        for(UINT32 source=0;source<matrix_source_channels;++source)
            output[destination]+=input.at(source)*matrix.at(matrix_source_channels*destination+source)*volume;
    return output;
}
}
