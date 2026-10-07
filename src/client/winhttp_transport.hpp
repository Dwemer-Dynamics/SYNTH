#pragma once

#include "client/http.hpp"

#if !defined(_WIN32)
#error "WinHttpTransport is available only in Windows plugin builds"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace synth::client {

namespace detail {

inline std::wstring utf8_to_wide(std::string_view value) {
    if (value.empty()) return {};
    const auto count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                            static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) throw std::invalid_argument{"invalid UTF-8 for WinHTTP"};
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(), count) != count) {
        throw std::runtime_error{"unable to convert WinHTTP text"};
    }
    return result;
}

inline std::string wide_to_utf8(std::wstring_view value) {
    if (value.empty()) return {};
    const auto count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                           static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) return {};
    std::string result(static_cast<std::size_t>(count), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(), count, nullptr, nullptr) != count) {
        return {};
    }
    return result;
}

class InternetHandle final {
public:
    InternetHandle() = default;
    explicit InternetHandle(HINTERNET value) noexcept : value_{value} {}
    ~InternetHandle() { reset(); }
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
    InternetHandle(InternetHandle&& other) noexcept : value_{std::exchange(other.value_, nullptr)} {}
    InternetHandle& operator=(InternetHandle&& other) noexcept {
        if (this != &other) reset(std::exchange(other.value_, nullptr));
        return *this;
    }
    void reset(HINTERNET value = nullptr) noexcept {
        if (value_ != nullptr) WinHttpCloseHandle(value_);
        value_ = value;
    }
    [[nodiscard]] HINTERNET get() const noexcept { return value_; }
    [[nodiscard]] HINTERNET release() noexcept { return std::exchange(value_, nullptr); }
    [[nodiscard]] explicit operator bool() const noexcept { return value_ != nullptr; }

private:
    HINTERNET value_{};
};

// Owns the callback state until HANDLE_CLOSING, including cancelled operations.
class AsyncRequest final {
public:
    explicit AsyncRequest(HINTERNET handle) : handle_{handle} {
        DWORD_PTR context = reinterpret_cast<DWORD_PTR>(this);
        if (!WinHttpSetOption(handle_.get(), WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context)))
            throw std::runtime_error{"WinHTTP request context setup failed"};
        if (WinHttpSetStatusCallback(handle_.get(), &callback,
                WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES, 0)
                == WINHTTP_INVALID_STATUS_CALLBACK)
            throw std::runtime_error{"WinHTTP request callback setup failed"};
    }

    ~AsyncRequest() {
        // All API invocations have returned before destruction. Closing cancels any
        // pending async operation, but its buffers and context must survive callbacks.
        WinHttpCloseHandle(handle_.release());
        std::unique_lock lock{mutex_};
        changed_.wait(lock, [this] { return closed_; });
    }
    AsyncRequest(const AsyncRequest&) = delete;
    AsyncRequest& operator=(const AsyncRequest&) = delete;
    void begin() {
        std::scoped_lock lock{mutex_};
        completed_ = error_ = bytes_ = 0;
    }

    // Worker-only wait: cancellation never closes a handle from the game thread.
    [[nodiscard]] DWORD wait(DWORD expected, const Cancelled& cancelled,
                             std::chrono::steady_clock::time_point deadline) {
        std::unique_lock lock{mutex_};
        while (completed_ != expected && error_ == 0) {
            if (cancelled && cancelled()) return ERROR_WINHTTP_OPERATION_CANCELLED;
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) return ERROR_WINHTTP_TIMEOUT;
            changed_.wait_until(lock, std::min(deadline, now + std::chrono::milliseconds{25}));
        }
        if (cancelled && cancelled()) return ERROR_WINHTTP_OPERATION_CANCELLED;
        if (std::chrono::steady_clock::now() >= deadline) return ERROR_WINHTTP_TIMEOUT;
        return error_;
    }

    [[nodiscard]] DWORD bytes() const {
        std::scoped_lock lock{mutex_};
        return bytes_;
    }

private:
    static void CALLBACK callback(HINTERNET, DWORD_PTR context, DWORD status,
                                   void* information, DWORD length) noexcept {
        if (context == 0) return;
        auto& self = *reinterpret_cast<AsyncRequest*>(context);
        std::scoped_lock lock{self.mutex_};
        if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) {
            self.closed_ = true;
        } else if (status == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR &&
                   information != nullptr && length == sizeof(WINHTTP_ASYNC_RESULT)) {
            self.error_ = static_cast<WINHTTP_ASYNC_RESULT*>(information)->dwError;
        } else if (status == WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE &&
                   information != nullptr && length == sizeof(DWORD)) {
            self.completed_ = status;
            self.bytes_ = *static_cast<DWORD*>(information);
        } else if (status == WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE ||
                   status == WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE ||
                   status == WINHTTP_CALLBACK_STATUS_READ_COMPLETE) {
            self.completed_ = status;
            self.bytes_ = length;
        }
        // Notify under the lock: the final callback must not touch state after the
        // destructor observes closed_ and destroys its condition variable.
        self.changed_.notify_all();
    }

    InternetHandle handle_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    DWORD completed_{};
    DWORD error_{};
    DWORD bytes_{};
    bool closed_{};
};

}  // namespace detail

class WinHttpTransport final : public IHttpTransport {
public:
    WinHttpTransport(std::string base_url, std::uint32_t connect_timeout_ms)
        : base_url_{std::move(base_url)}, connect_timeout_ms_{connect_timeout_ms} {
        if (base_url_.empty() || connect_timeout_ms_ == 0) {
            throw std::invalid_argument{"WinHTTP transport requires a base URL and timeout"};
        }
        crack_base_url();
        session_.reset(WinHttpOpen(L"SYNTH/1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                   WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC));
        if (!session_) throw std::runtime_error{"WinHttpOpen failed"};
        WinHttpSetTimeouts(session_.get(), static_cast<int>(connect_timeout_ms_),
                           static_cast<int>(connect_timeout_ms_),
                           static_cast<int>(connect_timeout_ms_),
                           static_cast<int>(connect_timeout_ms_));
        connection_.reset(WinHttpConnect(session_.get(), host_.c_str(), port_, 0));
        if (!connection_) throw std::runtime_error{"WinHttpConnect failed"};
    }

    [[nodiscard]] HttpResult send(const HttpRequest& request,
                                  const Cancelled& cancelled) override {
        validate_request(request);
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds{request.timeout_ms};
        const auto epoch = cancellation_epoch_.load(std::memory_order_acquire);
        const Cancelled stopped = [this, epoch, cancelled] {
            return cancellation_epoch_.load(std::memory_order_acquire) != epoch ||
                   (cancelled && cancelled());
        };
        if (stopped()) return {TransportFailure::cancelled, {}};
        if (request.body.size() > std::numeric_limits<DWORD>::max())
            return {TransportFailure::protocol_error, {}};

        const auto method = request.method == HttpMethod::get ? L"GET"
                            : request.method == HttpMethod::post ? L"POST" : L"PUT";
        const auto target = target_for(request.target);
        std::wstring headers;
        if (!request.content_type.empty())
            headers += L"Content-Type: " + detail::utf8_to_wide(request.content_type) + L"\r\n";
        if (!request.accept.empty())
            headers += L"Accept: " + detail::utf8_to_wide(request.accept) + L"\r\n";
        // The read buffer outlives AsyncRequest even on cancellation or exceptions.
        std::array<char, 64U * 1024U> buffer;
        const auto handle = WinHttpOpenRequest(connection_.get(), method, target.c_str(),
            nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
            secure_ ? WINHTTP_FLAG_SECURE : 0);
        if (handle == nullptr) return failure_from_last_error(epoch);
        detail::AsyncRequest active{handle};
        if (!WinHttpSetTimeouts(handle, static_cast<int>(connect_timeout_ms_),
                static_cast<int>(connect_timeout_ms_), static_cast<int>(request.timeout_ms),
                static_cast<int>(request.timeout_ms))) return failure_from_last_error(epoch);

        active.begin();
        if (!WinHttpSendRequest(handle,
                headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                headers.empty() ? 0 : static_cast<DWORD>(-1L),
                request.body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(request.body.data()),
                static_cast<DWORD>(request.body.size()), static_cast<DWORD>(request.body.size()),
                reinterpret_cast<DWORD_PTR>(&active))) return failure_from_last_error(epoch);
        auto error = active.wait(WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE, stopped, deadline);
        if (error != 0) return failure_from_last_error(epoch, error);

        active.begin();
        if (!WinHttpReceiveResponse(handle, nullptr)) return failure_from_last_error(epoch);
        error = active.wait(WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE, stopped, deadline);
        if (error != 0) return failure_from_last_error(epoch, error);
        HttpResponse response;
        DWORD status_size = sizeof(DWORD);
        DWORD status{};
        if (!WinHttpQueryHeaders(handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX))
            return failure_from_last_error(epoch);
        response.status = static_cast<int>(status);
        response.content_type = query_content_type(handle);

        for (;;) {
            if (stopped()) return {TransportFailure::cancelled, {}};
            auto requested = static_cast<DWORD>(buffer.size());
            if (request.response_chunk) {
                // Read only available bytes: a full-buffer read can wait for later sentences or EOF.
                active.begin();
                if (!WinHttpQueryDataAvailable(handle, nullptr)) return failure_from_last_error(epoch);
                error = active.wait(WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE, stopped, deadline);
                if (error != 0) return failure_from_last_error(epoch, error);
                requested = std::min(requested, active.bytes());
                if (requested == 0) break;
            }
            active.begin();
            // In async mode the byte-count pointer must be null; the callback owns it.
            if (!WinHttpReadData(handle, buffer.data(), requested, nullptr))
                return failure_from_last_error(epoch);
            error = active.wait(WINHTTP_CALLBACK_STATUS_READ_COMPLETE, stopped, deadline);
            if (error != 0) return failure_from_last_error(epoch, error);
            const auto read = active.bytes();
            if (read == 0) break;
            if (read > buffer.size() || read > request.maximum_response_bytes - response.body.size())
                return {TransportFailure::response_too_large, {}};
            response.body.append(buffer.data(), read);
            if (response.status >= 200 && response.status < 300 && request.response_chunk &&
                !request.response_chunk(std::string_view{buffer.data(), read}))
                return {TransportFailure::protocol_error, std::move(response)};
        }
        return {TransportFailure::none, std::move(response)};
    }

    void cancel_all() noexcept override {
        cancellation_epoch_.fetch_add(1, std::memory_order_acq_rel);
    }

private:
    void crack_base_url() {
        auto wide = detail::utf8_to_wide(base_url_);
        URL_COMPONENTS parts{};
        parts.dwStructSize = sizeof(parts);
        parts.dwHostNameLength = static_cast<DWORD>(-1L);
        parts.dwUrlPathLength = static_cast<DWORD>(-1L);
        if (!WinHttpCrackUrl(wide.c_str(), static_cast<DWORD>(wide.size()), 0, &parts)) {
            throw std::invalid_argument{"invalid WinHTTP base URL"};
        }
        if (parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS) {
            throw std::invalid_argument{"WinHTTP base URL must use HTTP or HTTPS"};
        }
        host_.assign(parts.lpszHostName, parts.dwHostNameLength);
        base_path_.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
        if (base_path_.empty()) base_path_ = L"/";
        if (base_path_.back() != L'/') base_path_.push_back(L'/');
        port_ = parts.nPort;
        secure_ = parts.nScheme == INTERNET_SCHEME_HTTPS;
    }

    void validate_request(const HttpRequest& request) const {
        if (request.timeout_ms == 0 || request.maximum_response_bytes == 0
            || request.target.empty() || request.target.front() != '/') {
            throw std::invalid_argument{"invalid WinHTTP request"};
        }
    }

    [[nodiscard]] std::wstring target_for(std::string_view target) const {
        auto relative = detail::utf8_to_wide(target);
        while (!relative.empty() && relative.front() == L'/') relative.erase(relative.begin());
        return base_path_ + relative;
    }

    [[nodiscard]] static std::string query_content_type(HINTERNET request) {
        DWORD size{};
        WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_TYPE, WINHTTP_HEADER_NAME_BY_INDEX,
                            nullptr, &size, WINHTTP_NO_HEADER_INDEX);
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size < sizeof(wchar_t)) return {};
        std::wstring value(size / sizeof(wchar_t), L'\0');
        if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_TYPE, WINHTTP_HEADER_NAME_BY_INDEX,
                                 value.data(), &size, WINHTTP_NO_HEADER_INDEX)) return {};
        while (!value.empty() && value.back() == L'\0') value.pop_back();
        return detail::wide_to_utf8(value);
    }

    [[nodiscard]] HttpResult failure_from_last_error(std::uint64_t cancellation_epoch,
                                                     DWORD error = GetLastError()) const noexcept {
        if (cancellation_epoch_.load(std::memory_order_acquire) != cancellation_epoch ||
            error == ERROR_WINHTTP_OPERATION_CANCELLED) {
            return {TransportFailure::cancelled, {}};
        }
        if (error == ERROR_WINHTTP_TIMEOUT) return {TransportFailure::timeout, {}};
        if (error == ERROR_WINHTTP_CANNOT_CONNECT || error == ERROR_WINHTTP_CONNECTION_ERROR
            || error == ERROR_WINHTTP_NAME_NOT_RESOLVED) {
            return {TransportFailure::server_unavailable, {}};
        }
        return {TransportFailure::protocol_error, {}};
    }

    std::string base_url_;
    std::uint32_t connect_timeout_ms_{};
    std::wstring host_;
    std::wstring base_path_;
    INTERNET_PORT port_{};
    bool secure_{};
    detail::InternetHandle session_;
    detail::InternetHandle connection_;
    std::atomic_uint64_t cancellation_epoch_{};
};

}  // namespace synth::client
