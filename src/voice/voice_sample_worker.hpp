#pragma once

#include "client/winhttp_transport.hpp"
#include "core/cancellation.hpp"
#include "json/json.hpp"
#include "voice/voice_sample_resolver.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace synth::voice {

// Owns a separate connection and worker: bulk cloning cannot occupy dialogue/speech lanes.
class SampleWorker final {
public:
    SampleWorker(const std::string& url, std::uint32_t connect_timeout,
                 core::CancellationToken generation, std::string session_id,
                 std::filesystem::path data_root = {})
        : transport_{url, connect_timeout}, generation_{std::move(generation)},
          session_id_{std::move(session_id)}, data_root_{std::move(data_root)}, worker_{[this] { run(); }} {}

    ~SampleWorker() {
        request_stop();
        transport_.cancel_all();
        if (worker_.joinable()) worker_.join();
    }

    void configure(std::vector<std::string> plugins) {
        std::scoped_lock lock{mutex_};
        plugins_ = std::move(plugins);
    }

    void request(std::string voice, std::string actor) {
        voice = lower(std::move(voice));
        if (!safe_component(voice) || generation_.is_cancelled()) return;
        std::scoped_lock lock{mutex_};
        const auto now = std::chrono::steady_clock::now();
        if (jobs_.size() >= 16 || (attempted_.contains(voice) && attempted_[voice] > now)) return;
        attempted_[voice] = now + std::chrono::minutes{5};
        jobs_.push_back({std::move(voice), std::move(actor), false});
        changed_.notify_one();
    }

    void request_all() {
        std::scoped_lock lock{mutex_};
        if (bulk_queued_ || generation_.is_cancelled()) return;
        if (plugins_.empty()) { status_ = "Connect SYNTH before importing voices."; return; }
        bulk_queued_ = true;
        jobs_.push_back({{}, {}, true});
        status_ = "Voice import queued";
        changed_.notify_one();
    }

    void cancel() {
        epoch_.fetch_add(1, std::memory_order_release);
        std::scoped_lock lock{mutex_};
        jobs_.clear();
        attempted_.clear();
        status_ = "Cancelling voice import; saved samples are kept.";
        changed_.notify_one();
    }

    void request_stop() noexcept {
        stopping_.store(true, std::memory_order_release);
        changed_.notify_one();
    }

    std::string status() const {
        std::scoped_lock lock{mutex_};
        return status_;
    }

private:
    struct Job { std::string voice, actor; bool all{}; };
    class ServiceError final : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    void status(std::string value) {
        std::scoped_lock lock{mutex_};
        status_ = std::move(value);
    }

    // Uploads a versioned envelope and checks ownership before accepting a saved/ready result.
    bool exchange(const Sample& sample, const std::string& actor, bool probe,
                  const client::Cancelled& cancelled) {
        const auto request_id = "voice:" + std::to_string(generation_.generation().value()) + ":" +
                                std::to_string(++sequence_);
        const auto metadata = json::write(json::Value::Object{
            {"schema", "synth.voice_sample.v2"}, {"request_id", request_id},
            {"session_id", session_id_}, {"generation", generation_.generation().value()},
            {"actor_name", actor}, {"original_name", sample.original_name},
            {"voice_id", sample.voice_id}, {"game", "fo4"},
            {"mode", probe ? "probe" : "import"},
            {"compression", sample.compressed ? "zlib" : "none"},
            {"unpacked_bytes", static_cast<std::uint64_t>(sample.unpacked_bytes)}});
        const auto boundary = "SYNTHVoiceBoundary" + std::to_string(GetCurrentProcessId()) +
                              std::to_string(sequence_);
        std::string body = "--" + boundary + "\r\nContent-Disposition: form-data; name=\"metadata\"\r\n\r\n" + metadata + "\r\n";
        if (!probe) {
            const auto bytes = SampleIndex::read(sample);
            body += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"sample.bin\"\r\nContent-Type: application/octet-stream\r\n\r\n";
            body += bytes;
            body += "\r\n";
        }
        body += "--" + boundary + "--\r\n";
        if (cancelled()) return false;
        const auto result = transport_.send(client::HttpRequest{
            .method = client::HttpMethod::post, .target = "/vsx.php",
            .content_type = "multipart/form-data; boundary=" + boundary,
            .body = std::move(body), .timeout_ms = 35'000,
            .accept = "application/json", .maximum_response_bytes = 16'384}, cancelled);
        if (cancelled()) return false;
        if (!result.delivered()) throw ServiceError{"Voice server unavailable or timed out"};
        const auto response = json::parse(result.response.body);
        const auto text = [&](const char* key) {
            const auto* field = response.find(key);
            return field && field->is_string() ? field->as_string() : std::string{};
        };
        const auto* generation = response.find("generation");
        if (text("schema") != "synth.voice_sample.response.v2" || text("request_id") != request_id ||
            !generation || !generation->is_number() ||
            generation->as_double() != static_cast<double>(generation_.generation().value()))
            throw std::runtime_error{"Voice response ownership mismatch"};
        if (probe && result.response.status == 404) return false;
        if (result.response.status >= 500 || result.response.status == 409)
            throw ServiceError{"Voice service stopped: " + text("error").substr(0, 180)};
        const auto* ok = response.find("ok");
        if (result.response.status != 200 || !ok || !ok->is_bool() || !ok->as_bool())
            throw std::runtime_error{"Voice import rejected: " + text("error").substr(0, 180)};
        const auto* ready = response.find("ready");
        if (!ready || !ready->is_bool() || !ready->as_bool())
            throw ServiceError{"Sample saved; voice backend is not ready (retry from Tools)"};
        return true;
    }

    void run() noexcept {
        SampleIndex index;
        bool indexed{};
        while (!stopping_.load(std::memory_order_acquire) && !generation_.is_cancelled()) {
            Job job;
            std::vector<std::string> plugins;
            std::uint64_t epoch{};
            {
                std::unique_lock lock{mutex_};
                changed_.wait_for(lock, std::chrono::milliseconds{250}, [&] {
                    return !jobs_.empty() || stopping_.load() || generation_.is_cancelled();
                });
                if (stopping_.load() || generation_.is_cancelled()) break;
                if (jobs_.empty()) { bulk_queued_ = false; continue; }
                job = std::move(jobs_.front());
                jobs_.pop_front();
                plugins = plugins_;
                epoch = epoch_.load(std::memory_order_acquire);
            }
            const auto job_deadline = std::chrono::steady_clock::now() +
                (job.all ? std::chrono::minutes{14} : std::chrono::minutes{1});
            const auto cancelled = [&] {
                return stopping_.load(std::memory_order_acquire) || generation_.is_cancelled() ||
                       epoch_.load(std::memory_order_acquire) != epoch ||
                       std::chrono::steady_clock::now() >= job_deadline;
            };
            try {
                if (!indexed) {
                    status("Discovering voice samples in active game files...");
                    std::array<wchar_t, 32768> executable{};
                    const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
                    if (!length || length >= executable.size()) throw std::runtime_error{"Cannot resolve game Data folder"};
                    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds{45};
                    index.scan(data_root_.empty() ? std::filesystem::path{executable.data()}.parent_path() / "Data" : data_root_, plugins,
                               [&] { return cancelled() || std::chrono::steady_clock::now() > until; });
                    indexed = true;
                }
                std::size_t complete{}, failed{}, total = job.all ? index.samples().size() : 1;
                std::string last_failure;
                for (const auto& [voice, sample] : index.samples()) {
                    if (!job.all && voice != job.voice) continue;
                    if (cancelled()) break;
                    status("Voice samples " + std::to_string(complete + failed) + "/" + std::to_string(total) + ": " + voice);
                    try {
                        if (!exchange(sample, job.actor, true, cancelled)) {
                            if (cancelled()) break;
                            (void)exchange(sample, job.actor, false, cancelled);
                        }
                        if (!cancelled()) ++complete;
                    } catch (const ServiceError&) {
                        // Stop promptly on backend/session failure, but skip individual bad recordings.
                        throw;
                    } catch (const std::exception& error) {
                        ++failed;
                        last_failure = voice + ": " + error.what();
                        status(error.what());
                        if (!job.all) throw;
                    }
                }
                if (std::chrono::steady_clock::now() >= job_deadline)
                    status("Voice import reached its time limit. Run Send All again to continue; saved samples are kept.");
                else if (cancelled()) status("Voice import cancelled; saved samples are kept.");
                else if (!job.all && complete == 0) status("No supported sample found for " + job.voice + ". Use TTS Studio to upload one.");
                else status("Voice samples: " + std::to_string(complete) + " ready, " + std::to_string(failed) +
                            " failed; " + std::to_string(index.unsupported_archives()) + " unsupported archives. " +
                            (last_failure.empty() ? "Preview in TTS Studio." : "Last failure: " + last_failure));
            } catch (const std::exception& error) {
                status(cancelled() ? "Voice import cancelled; saved samples are kept." : error.what());
            } catch (...) {
                status("Voice import failed; dialogue remains available.");
            }
            if (job.all) { std::scoped_lock lock{mutex_}; bulk_queued_ = false; }
        }
    }

    client::WinHttpTransport transport_;
    core::CancellationToken generation_;
    std::string session_id_;
    std::filesystem::path data_root_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<Job> jobs_;
    std::vector<std::string> plugins_;
    std::map<std::string, std::chrono::steady_clock::time_point> attempted_;
    std::string status_{"Voices import automatically when you speak to an NPC."};
    bool bulk_queued_{};
    std::atomic_bool stopping_{};
    std::atomic_uint64_t epoch_{};
    std::uint64_t sequence_{};
    std::thread worker_; // Last: every field is initialized before the worker starts.
};

} // namespace synth::voice
