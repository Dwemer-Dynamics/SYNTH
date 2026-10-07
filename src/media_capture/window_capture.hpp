#pragma once

#if !defined(_WIN32)
#error "Window capture is available only in Windows plugin builds"
#endif

#include <windows.h>
#include <objidl.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace synth::media_capture {

struct CapturedImage final {
    std::vector<std::byte> bytes;
    std::string content_type{"image/jpeg"};
    std::uint32_t width{};
    std::uint32_t height{};
};

// Owned top-down BGRX pixels cross the worker boundary, never a DC, bitmap or COM pointer.
struct CapturedPixels final {
    std::vector<std::byte> bytes;
    std::uint32_t width{};
    std::uint32_t height{};
};

namespace detail {

struct WindowSearch final {
    DWORD process_id{};
    HWND result{};
};

inline BOOL CALLBACK find_process_window(HWND window, LPARAM parameter) {
    auto& search = *reinterpret_cast<WindowSearch*>(parameter);
    DWORD process{};
    GetWindowThreadProcessId(window, &process);
    if (process == search.process_id && IsWindowVisible(window) && GetWindow(window, GW_OWNER) == nullptr) {
        search.result = window;
        return FALSE;
    }
    return TRUE;
}

[[nodiscard]] inline HWND game_window() {
    const auto process_id = GetCurrentProcessId();
    auto window = GetForegroundWindow();
    DWORD foreground_process{};
    if (window != nullptr) GetWindowThreadProcessId(window, &foreground_process);
    if (window != nullptr && foreground_process == process_id) return window;
    WindowSearch search{process_id, nullptr};
    EnumWindows(find_process_window, reinterpret_cast<LPARAM>(&search));
    if (search.result == nullptr) throw std::runtime_error{"Fallout window is unavailable"};
    return search.result;
}

inline void require(HRESULT result, const char* operation) {
    if (FAILED(result)) throw std::runtime_error{std::string{"PipVision "} + operation + " failed"};
}

}  // namespace detail

// Freeze input-time pixels only; Windows readback still needs game-side latency validation.
[[nodiscard]] inline CapturedPixels capture_game_window_pixels() {
    const auto window = detail::game_window();
    RECT bounds{};
    if (!GetClientRect(window, &bounds)) throw std::runtime_error{"Fallout client bounds are unavailable"};
    const auto width = bounds.right - bounds.left;
    const auto height = bounds.bottom - bounds.top;
    if (width < 1 || height < 1 || width > 8192 || height > 8192 ||
        static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) > 32'000'000ULL) {
        throw std::runtime_error{"Fallout client bounds exceed PipVision limits"};
    }

    HDC source = GetDC(window);
    HDC memory = source == nullptr ? nullptr : CreateCompatibleDC(source);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels{};
    HBITMAP bitmap = memory == nullptr
                         ? nullptr
                         : CreateDIBSection(source, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    HGDIOBJ previous = bitmap == nullptr ? nullptr : SelectObject(memory, bitmap);
    const auto captured = previous != nullptr && previous != HGDI_ERROR &&
                          BitBlt(memory, 0, 0, width, height, source, 0, 0, SRCCOPY | CAPTUREBLT);
    const auto flushed = captured && GdiFlush();
    if (previous != nullptr && previous != HGDI_ERROR) SelectObject(memory, previous);
    if (memory != nullptr) DeleteDC(memory);
    if (source != nullptr) ReleaseDC(window, source);
    if (!flushed || bitmap == nullptr || pixels == nullptr) {
        if (bitmap != nullptr) DeleteObject(bitmap);
        throw std::runtime_error{"Fallout frame capture failed"};
    }

    try {
        const auto size = static_cast<std::size_t>(width) * height * 4;
        const auto* first = static_cast<const std::byte*>(pixels);
        std::vector<std::byte> bytes(first, first + size);
        DeleteObject(bitmap);
        return {std::move(bytes), static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)};
    } catch (...) {
        DeleteObject(bitmap);
        throw;
    }
}

// Encode an owned frame on the worker. No game/window access or file output occurs here.
[[nodiscard]] inline CapturedImage encode_jpeg(const CapturedPixels& pixels, float quality = 0.88F) {
    const auto width = pixels.width;
    const auto height = pixels.height;
    const auto pixel_count = static_cast<std::uint64_t>(width) * height;
    if (width == 0 || height == 0 || width > 8192 || height > 8192 || pixel_count > 32'000'000ULL ||
        pixels.bytes.size() != pixel_count * 4) {
        throw std::runtime_error{"PipVision pixel buffer exceeds media limits"};
    }
    const auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com) && com != RPC_E_CHANGED_MODE) {
        throw std::runtime_error{"PipVision image encoder initialization failed"};
    }
    // Declared before all interfaces so COM remains initialized through their destruction.
    struct ApartmentCleanup final {
        bool initialized;
        ~ApartmentCleanup() { if (initialized) CoUninitialize(); }
    } apartment{SUCCEEDED(com)};
    {
        using Microsoft::WRL::ComPtr;
        ComPtr<IWICImagingFactory> factory;
        detail::require(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                         IID_PPV_ARGS(&factory)),
                        "image factory");
        ComPtr<IWICBitmap> source_bitmap;
        const auto stride = static_cast<UINT>(width * 4);
        const auto byte_count = static_cast<UINT>(stride * height);
        detail::require(factory->CreateBitmapFromMemory(
                            static_cast<UINT>(width), static_cast<UINT>(height),
                            GUID_WICPixelFormat32bppBGR, stride, byte_count,
                            reinterpret_cast<BYTE*>(const_cast<std::byte*>(pixels.bytes.data())), &source_bitmap),
                        "bitmap import");

        ComPtr<IWICFormatConverter> converter;
        detail::require(factory->CreateFormatConverter(&converter), "format converter");
        detail::require(converter->Initialize(source_bitmap.Get(), GUID_WICPixelFormat24bppBGR,
                                               WICBitmapDitherTypeNone, nullptr, 0.0,
                                               WICBitmapPaletteTypeCustom),
                        "pixel conversion");

        ComPtr<IStream> stream;
        detail::require(CreateStreamOnHGlobal(nullptr, TRUE, &stream), "memory stream");
        ComPtr<IWICBitmapEncoder> encoder;
        detail::require(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder),
                        "JPEG encoder");
        detail::require(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache),
                        "JPEG stream");
        ComPtr<IWICBitmapFrameEncode> frame;
        ComPtr<IPropertyBag2> properties;
        detail::require(encoder->CreateNewFrame(&frame, &properties), "JPEG frame");
        PROPBAG2 option{};
        option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT value;
        VariantInit(&value);
        value.vt = VT_R4;
        value.fltVal = (std::max)(0.1F, (std::min)(quality, 0.95F));
        detail::require(properties->Write(1, &option, &value), "JPEG quality");
        VariantClear(&value);
        detail::require(frame->Initialize(properties.Get()), "JPEG frame initialization");
        detail::require(frame->SetSize(static_cast<UINT>(width), static_cast<UINT>(height)),
                        "JPEG dimensions");
        WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
        detail::require(frame->SetPixelFormat(&format), "JPEG pixel format");
        detail::require(frame->WriteSource(converter.Get(), nullptr), "JPEG pixels");
        detail::require(frame->Commit(), "JPEG frame commit");
        detail::require(encoder->Commit(), "JPEG commit");
        STATSTG status{};
        detail::require(stream->Stat(&status, STATFLAG_NONAME), "JPEG length");
        const auto length = status.cbSize.QuadPart;
        if (length == 0 || length > 16U * 1024U * 1024U) {
            throw std::runtime_error{"PipVision JPEG exceeds media limits"};
        }
        std::vector<std::byte> bytes(static_cast<std::size_t>(length));
        HGLOBAL global{};
        detail::require(GetHGlobalFromStream(stream.Get(), &global), "JPEG buffer");
        if (GlobalSize(global) < bytes.size()) throw std::runtime_error{"PipVision JPEG buffer is truncated"};
        const auto* data = static_cast<const std::byte*>(GlobalLock(global));
        if (data == nullptr) throw std::runtime_error{"PipVision JPEG buffer is unavailable"};
        std::memcpy(bytes.data(), data, bytes.size());
        GlobalUnlock(global);
        return {std::move(bytes), "image/jpeg", static_cast<std::uint32_t>(width),
                static_cast<std::uint32_t>(height)};
    }
}

}  // namespace synth::media_capture
