#pragma once

#include "core/cancellation.hpp"
#include "audio/speech_animation.hpp"
#include "runtime/fallout_runtime.hpp"
#include "presentation/dialogue_presentation.hpp"

#if !defined(_WIN32)
#error "NativeAudioPlayback is available only in Windows plugin builds"
#endif

#include <windows.h>
#if _WIN32_WINNT < 0x0602
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif
#include <xaudio2.h>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace synth::audio {

struct NativeClip final {
    std::uint64_t generation{};
    std::string request_id;
    std::string turn_id;
    std::string utterance_id;
    std::string speaker_id;
    std::shared_ptr<const std::vector<std::byte>> bytes;
    float volume{1.0F};
    float pan{};
    std::uint32_t skip_begin_ms{};
    std::uint32_t skip_end_ms{};
    core::CancellationToken cancellation;
    std::shared_ptr<const runtime::RuntimeFacingRequest> facing;
    presentation::DialogueCaptions::Caption caption;
    std::shared_ptr<const SpeechAnimation> lip_animation;
};

struct PlaybackFrame final {
    std::uint64_t generation{};
    std::string speaker_id;
    float normalized_level{};
    std::shared_ptr<const runtime::RuntimeFacingRequest> facing;
    presentation::DialogueCaptions::Caption caption;
    MouthWeights mouth{};
    std::string utterance_id;
    core::CancellationToken cancellation;
};

class NativeAudioPlayback final {
public:
    explicit NativeAudioPlayback(std::size_t capacity = 16) : capacity_{capacity} {
        if (capacity == 0) throw std::invalid_argument{"audio playback capacity is zero"};
    }

    NativeAudioPlayback(const NativeAudioPlayback&) = delete;
    NativeAudioPlayback& operator=(const NativeAudioPlayback&) = delete;

    ~NativeAudioPlayback() {
        halt();
        if (engine_thread_.joinable()) engine_thread_.join();
    }

    [[nodiscard]] bool enqueue(NativeClip clip) {
        if (!clip.bytes || clip.bytes->empty() || clip.generation == 0 || clip.request_id.empty() ||
            clip.turn_id.empty() || clip.utterance_id.empty()) {
            return false;
        }
        // enqueue runs on the existing speech worker, before taking the playback lock.
        // Animation is optional: an allocation failure must not discard valid audio.
        if (clip.caption && !clip.lip_animation) {
            try { clip.lip_animation = std::make_shared<const SpeechAnimation>(clip.caption->text); }
            catch (...) {}
        }
        std::scoped_lock lock{mutex_};
        if (clip.cancellation.generation().valid() && clip.cancellation.is_cancelled()) return true;
        if (terminal_ || engine_state_ == EngineState::failed ||
            queued_.size() + (active_ ? 1U : 0U) >= capacity_) return false;
        queued_.push_back(std::move(clip));
        if (engine_state_ == EngineState::idle) {
            engine_state_ = EngineState::initializing;
            try {
                engine_thread_ = std::thread{[this] { engine_loop(); }};
            } catch (...) {
                engine_state_ = EngineState::failed;
                queued_.clear();
                return false;
            }
        }
        return true;
    }

    // Called on the game thread to retire completed voices and begin the next clip.
    [[nodiscard]] bool pump(std::uint64_t current_generation,
                            presentation::DialogueCaptions::Caption* failed_caption = nullptr) noexcept {
        std::scoped_lock lock{mutex_};
        if (terminal_) return true;
        if (engine_state_ == EngineState::failed) {
            // Drain one failed clip per frame instead of discarding its dialogue text.
            if (!queued_.empty()) {
                if (failed_caption) *failed_caption = queued_.front().caption;
                queued_.pop_front();
            }
            if (!engine_failure_reported_) {
                engine_failure_reported_ = true;
                return false;
            }
            return true;
        }
        if (engine_state_ != EngineState::ready) return true;
        if (active_) {
            if (active_->clip.generation != current_generation ||
                (active_->clip.caption && active_->clip.caption->scene_rejected()) ||
                (active_->clip.cancellation.generation().valid() && active_->clip.cancellation.is_cancelled())) {
                destroy_active();
            } else if (paused_) {
                return true;
            }
        }
        if (active_) {
            if (active_->clip.caption && !active_->clip.caption->scene_ready(core::SnapshotClock::now())) {
                if (!active_->scene_paused) (void)active_->voice->Stop();
                active_->scene_paused = true;
                return true;
            }
            if (active_->scene_paused) { (void)active_->voice->Start(); active_->scene_paused = false; }
            XAUDIO2_VOICE_STATE state{};
            active_->voice->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
            if (state.BuffersQueued == 0) {
                destroy_active(true);
            }
        }
        while (!queued_.empty() && (queued_.front().generation != current_generation ||
               (queued_.front().caption && queued_.front().caption->scene_rejected()) ||
               (queued_.front().cancellation.generation().valid() && queued_.front().cancellation.is_cancelled()))) {
            if (queued_.front().caption)
                queued_.front().caption->mark_delivery(presentation::DialogueDelivery::discarded);
            queued_.pop_front();
        }
        if (paused_) return true;
        if (!queued_.empty() && queued_.front().caption &&
            !queued_.front().caption->scene_ready(core::SnapshotClock::now())) return true;
        if (!active_ && !queued_.empty()) {
            auto clip = std::move(queued_.front());
            queued_.pop_front();
            const auto caption = clip.caption;
            try {
                start(std::move(clip));
            } catch (...) {
                destroy_active();
                if (failed_caption) *failed_caption = caption;
                return false;
            }
        }
        return true;
    }

    void halt() noexcept {
        {
            std::scoped_lock lock{mutex_};
            for (const auto& clip : queued_) if (clip.caption)
                clip.caption->mark_delivery(presentation::DialogueDelivery::discarded);
            queued_.clear();
            destroy_active();
            terminal_ = true;
        }
        engine_condition_.notify_all();
    }

    void interrupt() noexcept {
        std::scoped_lock lock{mutex_};
        for (const auto& clip : queued_) if (clip.caption)
            clip.caption->mark_delivery(presentation::DialogueDelivery::discarded);
        queued_.clear();
        destroy_active();
    }

    void set_paused(bool paused) noexcept {
        std::scoped_lock lock{mutex_};
        if (terminal_ || paused_ == paused) return;
        paused_ = paused;
        if (!active_) return;
        if (paused_) {
            (void)active_->voice->Stop();
        } else if (!active_->clip.caption || active_->clip.caption->scene_ready(core::SnapshotClock::now())) {
            (void)active_->voice->Start();
        } else {
            active_->scene_paused = true;
        }
    }

    [[nodiscard]] bool paused() const noexcept {
        std::scoped_lock lock{mutex_};
        return paused_;
    }

    [[nodiscard]] bool idle() const noexcept {
        std::scoped_lock lock{mutex_};
        return queued_.empty() && !active_;
    }

    // Capture is performed by the host after this bounded queue inspection releases its lock.
    [[nodiscard]] presentation::DialogueCaptions::Caption validation_candidate(core::SnapshotClock::time_point now) const {
        std::unique_lock lock{mutex_,std::try_to_lock};
        if (!lock.owns_lock()) return {};
        const auto due = [now](const auto& value) {
            return value && value->admission && !value->cancellation.is_cancelled() &&
                !value->scene_rejected() && !value->scene_ready(now);
        };
        if (active_ && due(active_->clip.caption)) return active_->clip.caption;
        return !queued_.empty() && due(queued_.front().caption) ? queued_.front().caption : nullptr;
    }

    // Returns the current native voice envelope for game-thread facial animation.
    [[nodiscard]] std::optional<PlaybackFrame> active_frame() const noexcept {
        std::scoped_lock lock{mutex_};
        if (!active_ || paused_ || terminal_ || active_->clip.cancellation.is_cancelled() || (active_->clip.caption &&
            !active_->clip.caption->scene_ready(core::SnapshotClock::now()))) return std::nullopt;
        XAUDIO2_VOICE_STATE state{};
        active_->voice->GetState(&state);
        const auto& signal = active_->signal;
        const auto level = signal_level(signal, state.SamplesPlayed);
        MouthWeights mouth{};
        if (active_->clip.lip_animation && signal.sample_rate != 0) {
            mouth = active_->clip.lip_animation->sample(
                static_cast<double>(state.SamplesPlayed) / signal.sample_rate,
                static_cast<double>(signal.play_length) / signal.sample_rate, level);
        }
        return PlaybackFrame{active_->clip.generation, active_->clip.speaker_id, level, active_->clip.facing,
                             active_->clip.caption, mouth, active_->clip.utterance_id, active_->clip.cancellation};
    }

private:
    enum class EngineState : unsigned char { idle, initializing, ready, failed };

    struct WaveView final {
        std::vector<std::byte> format;
        const std::byte* audio{};
        std::uint32_t audio_bytes{};
    };

    enum class SampleEncoding : unsigned char { unsupported, unsigned_pcm8, signed_pcm, ieee_float };

    struct WaveSignal final {
        const std::byte* data{};
        std::size_t data_offset{};
        std::uint32_t audio_bytes{};
        std::uint32_t sample_rate{};
        std::uint16_t channels{};
        std::uint16_t block_align{};
        std::uint16_t bits_per_sample{};
        SampleEncoding encoding{SampleEncoding::unsupported};
        std::uint32_t play_begin{};
        std::uint32_t play_length{};
    };

    struct Active final {
        NativeClip clip;
        IXAudio2SourceVoice* voice{};
        WaveSignal signal;
        bool scene_paused{};
    };

    // Owns COM and the XAudio2 engine for their complete lifetimes. Session
    // startup never creates an audio device; the thread begins only after a
    // real TTS clip is queued and cannot block Fallout's game thread.
    void engine_loop() noexcept {
        const auto com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const auto com_ready = SUCCEEDED(com_result);
        IXAudio2* engine{};
        IXAudio2MasteringVoice* mastering{};
        std::uint32_t destination_channels{};

        if (com_ready && SUCCEEDED(XAudio2Create(&engine)) && engine != nullptr &&
            SUCCEEDED(engine->CreateMasteringVoice(&mastering)) && mastering != nullptr) {
            XAUDIO2_VOICE_DETAILS details{};
            mastering->GetVoiceDetails(&details);
            destination_channels = details.InputChannels;
        }

        if (engine == nullptr || mastering == nullptr || destination_channels == 0) {
            if (mastering != nullptr) mastering->DestroyVoice();
            if (engine != nullptr) engine->Release();
            if (com_ready) CoUninitialize();
            {
                std::scoped_lock lock{mutex_};
                engine_state_ = EngineState::failed;
            }
            engine_condition_.notify_all();
            return;
        }

        {
            std::unique_lock lock{mutex_};
            if (!terminal_) {
                engine_ = engine;
                mastering_ = mastering;
                destination_channels_ = destination_channels;
                engine_state_ = EngineState::ready;
                engine_condition_.notify_all();
                engine_condition_.wait(lock, [this] { return terminal_; });
                engine_ = nullptr;
                mastering_ = nullptr;
                destination_channels_ = 0;
            }
        }

        mastering->DestroyVoice();
        engine->Release();
        if (com_ready) CoUninitialize();
    }

    [[nodiscard]] static std::uint32_t u32(const std::byte* value) noexcept {
        return std::to_integer<std::uint32_t>(value[0]) |
               (std::to_integer<std::uint32_t>(value[1]) << 8U) |
               (std::to_integer<std::uint32_t>(value[2]) << 16U) |
               (std::to_integer<std::uint32_t>(value[3]) << 24U);
    }

    [[nodiscard]] static WaveView parse_wave(const std::vector<std::byte>& bytes) {
        const auto tag = [&](std::size_t offset, const char* expected) {
            return offset + 4 <= bytes.size() &&
                   std::equal(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                              bytes.begin() + static_cast<std::ptrdiff_t>(offset + 4),
                              reinterpret_cast<const std::byte*>(expected));
        };
        if (bytes.size() < 44 || !tag(0, "RIFF") || !tag(8, "WAVE"))
            throw std::invalid_argument{"TTS media is not RIFF/WAVE"};
        WaveView result;
        for (std::size_t offset = 12; offset + 8 <= bytes.size();) {
            const auto size = static_cast<std::size_t>(u32(bytes.data() + offset + 4));
            const auto start = offset + 8;
            if (size > bytes.size() - start) throw std::invalid_argument{"truncated WAVE chunk"};
            if (tag(offset, "fmt ")) {
                if (size < 16 || size > sizeof(WAVEFORMATEXTENSIBLE))
                    throw std::invalid_argument{"unsupported WAVE format chunk"};
                result.format.assign(bytes.begin() + static_cast<std::ptrdiff_t>(start),
                                     bytes.begin() + static_cast<std::ptrdiff_t>(start + size));
                if (result.format.size() == 16) {
                    result.format.resize(18);
                    result.format[16] = std::byte{0};
                    result.format[17] = std::byte{0};
                }
            } else if (tag(offset, "data")) {
                if (size == 0 || size > std::numeric_limits<std::uint32_t>::max())
                    throw std::invalid_argument{"invalid WAVE data chunk"};
                result.audio = bytes.data() + start;
                result.audio_bytes = static_cast<std::uint32_t>(size);
            }
            const auto padded = size + (size & 1U);
            if (padded > bytes.size() - start) break;
            offset = start + padded;
        }
        if (result.format.empty() || result.audio == nullptr)
            throw std::invalid_argument{"WAVE is missing format or audio data"};
        const auto* format = reinterpret_cast<const WAVEFORMATEX*>(result.format.data());
        if ((format->wFormatTag != WAVE_FORMAT_PCM && format->wFormatTag != WAVE_FORMAT_IEEE_FLOAT &&
             format->wFormatTag != WAVE_FORMAT_EXTENSIBLE) ||
            format->nChannels == 0 || format->nSamplesPerSec == 0 || format->nBlockAlign == 0 ||
            result.audio_bytes % format->nBlockAlign != 0) {
            throw std::invalid_argument{"unsupported WAVE encoding"};
        }
        return result;
    }

    [[nodiscard]] static float sample_value(const std::byte* sample,
                                            std::uint16_t bits,
                                            SampleEncoding encoding) noexcept {
        if (encoding == SampleEncoding::unsigned_pcm8 && bits == 8) {
            return (static_cast<float>(std::to_integer<unsigned char>(*sample)) - 128.0F) / 128.0F;
        }
        if (encoding == SampleEncoding::ieee_float && bits == 32) {
            float value{};
            std::memcpy(&value, sample, sizeof(value));
            return std::isfinite(value) ? std::clamp(value, -1.0F, 1.0F) : 0.0F;
        }
        if (encoding != SampleEncoding::signed_pcm) return 0.0F;
        if (bits == 16) {
            std::int16_t value{};
            std::memcpy(&value, sample, sizeof(value));
            return static_cast<float>(value) / 32768.0F;
        }
        if (bits == 24) {
            auto value = static_cast<std::int32_t>(std::to_integer<unsigned char>(sample[0])) |
                         (static_cast<std::int32_t>(std::to_integer<unsigned char>(sample[1])) << 8) |
                         (static_cast<std::int32_t>(std::to_integer<unsigned char>(sample[2])) << 16);
            if ((value & 0x00800000) != 0) value |= ~0x00FFFFFF;
            return static_cast<float>(value) / 8388608.0F;
        }
        if (bits == 32) {
            std::int32_t value{};
            std::memcpy(&value, sample, sizeof(value));
            return static_cast<float>(value) / 2147483648.0F;
        }
        return 0.0F;
    }

    [[nodiscard]] static float signal_level(const WaveSignal& signal,
                                            std::uint64_t samples_played) noexcept {
        if (signal.encoding == SampleEncoding::unsupported || signal.play_length == 0 ||
            signal.block_align == 0 || signal.sample_rate == 0) {
            return 0.0F;
        }
        if (samples_played >= signal.play_length) return 0.0F;
        const auto relative = samples_played;
        const auto frame = static_cast<std::uint64_t>(signal.play_begin) + relative;
        const auto available_frames = signal.audio_bytes / signal.block_align;
        if (frame >= available_frames) return 0.0F;
        const auto window = std::min<std::uint64_t>(signal.sample_rate / 50U,
                                                    signal.play_length - relative);
        const auto stride = std::max<std::uint64_t>(1, window / 64U);
        const auto bytes_per_sample = signal.bits_per_sample / 8U;
        if (window == 0 || bytes_per_sample == 0) return 0.0F;
        if (signal.channels == 0 || signal.channels > 8 ||
            signal.block_align < static_cast<std::uint32_t>(signal.channels) * bytes_per_sample) return 0.0F;
        if (signal.data == nullptr) return 0.0F;
        const auto* data = signal.data + signal.data_offset;
        double total{};
        std::size_t count{};
        for (std::uint64_t index = 0; index < window; index += stride) {
            const auto* sample = data + (frame + index) * signal.block_align;
            float peak{};
            for (std::uint16_t channel = 0; channel < signal.channels; ++channel) {
                const auto value = sample_value(sample + channel * bytes_per_sample, signal.bits_per_sample, signal.encoding);
                if (std::isfinite(value)) peak = std::max(peak, std::abs(value));
            }
            total += peak;
            ++count;
        }
        if (count == 0) return 0.0F;
        const auto average = static_cast<float>(total / static_cast<double>(count));
        return std::sqrt(std::clamp((average - 0.008F) * 5.0F, 0.0F, 1.0F));
    }

    void start(NativeClip clip) {
        auto wave = parse_wave(*clip.bytes);
        auto* format = reinterpret_cast<const WAVEFORMATEX*>(wave.format.data());
        IXAudio2SourceVoice* voice{};
        if (FAILED(engine_->CreateSourceVoice(&voice, format)) || voice == nullptr)
            throw std::runtime_error{"XAudio2 source voice creation failed"};
        const auto volume = std::clamp(clip.volume, 0.0F, 2.0F);
        (void)voice->SetVolume(volume);
        if (format->nChannels == 1 && destination_channels_ >= 2) {
            std::vector<float> matrix(destination_channels_, 0.0F);
            const auto pan = std::clamp(clip.pan, -1.0F, 1.0F);
            matrix[0] = std::sqrt((1.0F - pan) * 0.5F);
            matrix[1] = std::sqrt((1.0F + pan) * 0.5F);
            (void)voice->SetOutputMatrix(mastering_, 1, destination_channels_, matrix.data());
        }
        XAUDIO2_BUFFER buffer{};
        buffer.Flags = XAUDIO2_END_OF_STREAM;
        buffer.AudioBytes = wave.audio_bytes;
        buffer.pAudioData = reinterpret_cast<const BYTE*>(wave.audio);
        const auto total_samples = wave.audio_bytes / format->nBlockAlign;
        const auto begin_samples = std::min<std::uint64_t>(
            total_samples,
            static_cast<std::uint64_t>(format->nSamplesPerSec) * clip.skip_begin_ms / 1000ULL);
        const auto end_samples = std::min<std::uint64_t>(
            total_samples - begin_samples,
            static_cast<std::uint64_t>(format->nSamplesPerSec) * clip.skip_end_ms / 1000ULL);
        if (begin_samples + end_samples >= total_samples) {
            voice->DestroyVoice();
            throw std::invalid_argument{"configured TTS clip trim removes all audio"};
        }
        buffer.PlayBegin = static_cast<UINT32>(begin_samples);
        buffer.PlayLength = static_cast<UINT32>(total_samples - begin_samples - end_samples);
        if (FAILED(voice->SubmitSourceBuffer(&buffer)) || FAILED(voice->Start())) {
            voice->DestroyVoice();
            throw std::runtime_error{"XAudio2 playback start failed"};
        }
        WaveSignal signal;
        signal.data = clip.bytes->data();
        signal.data_offset = static_cast<std::size_t>(wave.audio - clip.bytes->data());
        signal.audio_bytes = wave.audio_bytes;
        signal.sample_rate = format->nSamplesPerSec;
        signal.channels = format->nChannels;
        signal.block_align = format->nBlockAlign;
        signal.bits_per_sample = format->wBitsPerSample;
        signal.play_begin = buffer.PlayBegin;
        signal.play_length = buffer.PlayLength;
        if (format->wFormatTag == WAVE_FORMAT_PCM) {
            signal.encoding = format->wBitsPerSample == 8 ? SampleEncoding::unsigned_pcm8
                                                          : SampleEncoding::signed_pcm;
        } else if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
            signal.encoding = SampleEncoding::ieee_float;
        } else if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                   wave.format.size() >= sizeof(WAVEFORMATEXTENSIBLE)) {
            const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wave.format.data());
            signal.encoding = extensible->SubFormat.Data1 == WAVE_FORMAT_IEEE_FLOAT
                                  ? SampleEncoding::ieee_float
                                  : extensible->SubFormat.Data1 == WAVE_FORMAT_PCM
                                        ? (format->wBitsPerSample == 8 ? SampleEncoding::unsigned_pcm8
                                                                       : SampleEncoding::signed_pcm)
                                        : SampleEncoding::unsupported;
        }
        active_ = std::make_unique<Active>(Active{std::move(clip), voice, signal});
        if (active_->clip.caption)
            active_->clip.caption->mark_delivery(presentation::DialogueDelivery::playing);
    }

    void destroy_active(bool completed = false) noexcept {
        if (!active_) return;
        if (active_->clip.caption) active_->clip.caption->mark_delivery(
            completed ? presentation::DialogueDelivery::spoken : presentation::DialogueDelivery::discarded);
        (void)active_->voice->Stop();
        (void)active_->voice->FlushSourceBuffers();
        active_->voice->DestroyVoice();
        active_.reset();
    }

    std::size_t capacity_{};
    IXAudio2* engine_{};
    IXAudio2MasteringVoice* mastering_{};
    std::uint32_t destination_channels_{};
    mutable std::mutex mutex_;
    std::condition_variable engine_condition_;
    std::thread engine_thread_;
    std::deque<NativeClip> queued_;
    std::unique_ptr<Active> active_;
    EngineState engine_state_{EngineState::idle};
    bool engine_failure_reported_{};
    bool paused_{};
    bool terminal_{};
};

}  // namespace synth::audio
