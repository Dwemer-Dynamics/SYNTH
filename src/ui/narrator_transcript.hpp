#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace synth::ui {

// Copied transcript page shared by the network owner and the existing chat renderer. A replacement session claims
// a new lease, so a late callback cannot reopen the previous session's private page.
class NarratorTranscript final {
public:
    struct Page { bool visible{}, older{}; std::vector<std::pair<std::string, std::string>> entries; };
    std::uint64_t claim() { std::scoped_lock lock{mutex_}; page_ = {}; return ++lease_; }
    void show(std::uint64_t lease, Page page) {
        std::scoped_lock lock{mutex_};
        if (lease == lease_) { page.visible = true; page_ = std::move(page); }
    }
    Page page() const { std::scoped_lock lock{mutex_}; return page_; }
    void hide() { std::scoped_lock lock{mutex_}; page_.visible = false; }
private:
    mutable std::mutex mutex_;
    std::uint64_t lease_{};
    Page page_;
};

inline NarratorTranscript narrator_transcript;
} // namespace synth::ui
