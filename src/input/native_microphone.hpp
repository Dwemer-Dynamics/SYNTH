#pragma once

#if !defined(_WIN32)
#error "NativeMicrophone is available only in Windows plugin builds"
#endif

#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <vector>

namespace synth::input {

class NativeMicrophone final {
public:
    NativeMicrophone() = default;
    NativeMicrophone(const NativeMicrophone&) = delete;
    NativeMicrophone& operator=(const NativeMicrophone&) = delete;
    ~NativeMicrophone() { cancel(); }

    [[nodiscard]] bool start(std::uint32_t maximum_seconds = 120) {
        std::scoped_lock lock{mutex_};
        if (recording_ || maximum_seconds == 0 || maximum_seconds > 120) return false;
        format_ = {};
        format_.wFormatTag = WAVE_FORMAT_PCM;
        format_.nChannels = 1;
        format_.nSamplesPerSec = 16000;
        format_.wBitsPerSample = 16;
        format_.nBlockAlign = static_cast<WORD>(format_.nChannels * format_.wBitsPerSample / 8);
        format_.nAvgBytesPerSec = format_.nSamplesPerSec * format_.nBlockAlign;
        pcm_.assign(static_cast<std::size_t>(format_.nAvgBytesPerSec) * maximum_seconds,
                    std::byte{});
        if (waveInOpen(&device_, WAVE_MAPPER, &format_, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
            device_ = nullptr;
            pcm_.clear();
            return false;
        }
        header_ = {};
        header_.lpData = reinterpret_cast<LPSTR>(pcm_.data());
        header_.dwBufferLength = static_cast<DWORD>(pcm_.size());
        if (waveInPrepareHeader(device_, &header_, sizeof(header_)) != MMSYSERR_NOERROR ||
            waveInAddBuffer(device_, &header_, sizeof(header_)) != MMSYSERR_NOERROR ||
            waveInStart(device_) != MMSYSERR_NOERROR) {
            close_device();
            pcm_.clear();
            return false;
        }
        recording_ = true;
        return true;
    }

    [[nodiscard]] std::optional<std::vector<std::byte>> stop() {
        std::scoped_lock lock{mutex_};
        if (!recording_ || device_ == nullptr) return std::nullopt;
        (void)waveInStop(device_);
        (void)waveInReset(device_);
        const auto recorded = std::min<std::size_t>(header_.dwBytesRecorded, pcm_.size());
        close_device();
        recording_ = false;
        if (recorded < format_.nBlockAlign * 160U) {
            pcm_.clear();
            return std::nullopt;
        }
        auto wave = make_wave(recorded);
        pcm_.clear();
        return wave;
    }

    void cancel() noexcept {
        std::scoped_lock lock{mutex_};
        if (device_ != nullptr) {
            (void)waveInStop(device_);
            (void)waveInReset(device_);
            close_device();
        }
        recording_ = false;
        pcm_.clear();
    }

    [[nodiscard]] bool recording() const noexcept {
        std::scoped_lock lock{mutex_};
        return recording_;
    }

    [[nodiscard]] std::uint32_t recent_peak(std::uint32_t window_ms = 100) const noexcept {
        std::scoped_lock lock{mutex_};
        if (!recording_ || device_ == nullptr || window_ms == 0) return 0;
        MMTIME position{};
        position.wType = TIME_BYTES;
        if (waveInGetPosition(device_, &position, sizeof(position)) != MMSYSERR_NOERROR) return 0;
        const auto recorded = std::min<std::size_t>(position.u.cb, pcm_.size());
        const auto window = std::min<std::size_t>(
            recorded, static_cast<std::size_t>(format_.nAvgBytesPerSec) * window_ms / 1000U);
        const auto begin = recorded - (window - (window % sizeof(std::int16_t)));
        std::uint32_t peak{};
        for (auto offset = begin; offset + 1 < recorded; offset += sizeof(std::int16_t)) {
            std::int16_t sample{};
            std::memcpy(&sample, pcm_.data() + offset, sizeof(sample));
            const auto magnitude = sample == std::numeric_limits<std::int16_t>::min()
                                       ? 32768U
                                       : static_cast<std::uint32_t>(std::abs(sample));
            peak = std::max(peak, magnitude);
        }
        return peak;
    }

private:
    static void write_u16(std::vector<std::byte>& bytes, std::size_t offset,
                          std::uint16_t value) noexcept {
        bytes[offset] = static_cast<std::byte>(value & 0xffU);
        bytes[offset + 1] = static_cast<std::byte>((value >> 8U) & 0xffU);
    }

    static void write_u32(std::vector<std::byte>& bytes, std::size_t offset,
                          std::uint32_t value) noexcept {
        for (unsigned byte = 0; byte < 4; ++byte)
            bytes[offset + byte] = static_cast<std::byte>((value >> (byte * 8U)) & 0xffU);
    }

    [[nodiscard]] std::vector<std::byte> make_wave(std::size_t recorded) const {
        std::vector<std::byte> wave(44 + recorded);
        std::memcpy(wave.data(), "RIFF", 4);
        write_u32(wave, 4, static_cast<std::uint32_t>(36 + recorded));
        std::memcpy(wave.data() + 8, "WAVEfmt ", 8);
        write_u32(wave, 16, 16);
        write_u16(wave, 20, format_.wFormatTag);
        write_u16(wave, 22, format_.nChannels);
        write_u32(wave, 24, format_.nSamplesPerSec);
        write_u32(wave, 28, format_.nAvgBytesPerSec);
        write_u16(wave, 32, format_.nBlockAlign);
        write_u16(wave, 34, format_.wBitsPerSample);
        std::memcpy(wave.data() + 36, "data", 4);
        write_u32(wave, 40, static_cast<std::uint32_t>(recorded));
        std::copy_n(pcm_.begin(), static_cast<std::ptrdiff_t>(recorded), wave.begin() + 44);
        return wave;
    }

    void close_device() noexcept {
        if (device_ == nullptr) return;
        if ((header_.dwFlags & WHDR_PREPARED) != 0) {
            (void)waveInUnprepareHeader(device_, &header_, sizeof(header_));
        }
        (void)waveInClose(device_);
        device_ = nullptr;
        header_ = {};
    }

    mutable std::mutex mutex_;
    HWAVEIN device_{};
    WAVEFORMATEX format_{};
    WAVEHDR header_{};
    std::vector<std::byte> pcm_;
    bool recording_{};
};

}  // namespace synth::input
