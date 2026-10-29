#include "found_card.h"

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

// Implemented by the bundled, offline QR encoder.
#include "key_qr.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <gdiplus.h>
#include <algorithm>
#include <cwchar>
#include <memory>

#pragma comment(lib, "gdiplus.lib")

namespace {

bool valid_hex_key(const std::string& key) {
    if (key.size() != 64) return false;
    for (unsigned char c : key) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}

bool path_to_wide(const std::string& utf8, std::wstring& result) {
    if (utf8.empty() || utf8.find('\0') != std::string::npos ||
        utf8.size() > static_cast<size_t>((std::numeric_limits<int>::max)())) return false;
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                         static_cast<int>(utf8.size()), nullptr, 0);
    if (count <= 0) return false;
    result.resize(static_cast<size_t>(count));
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                              static_cast<int>(utf8.size()), &result[0], count) == count;
}

class GdiSession {
public:
    GdiSession() {
        Gdiplus::GdiplusStartupInput input;
        ready_ = Gdiplus::GdiplusStartup(&token_, &input, nullptr) == Gdiplus::Ok;
    }
    ~GdiSession() { if (ready_) Gdiplus::GdiplusShutdown(token_); }
    bool ready() const { return ready_; }
    GdiSession(const GdiSession&) = delete;
    GdiSession& operator=(const GdiSession&) = delete;
private:
    ULONG_PTR token_ = 0;
    bool ready_ = false;
};

class TempFile {
public:
    ~TempFile() { if (!path.empty()) DeleteFileW(path.c_str()); }
    std::wstring path;
};

bool find_png_encoder(CLSID& result) {
    UINT count = 0, bytes = 0;
    if (Gdiplus::GetImageEncodersSize(&count, &bytes) != Gdiplus::Ok || !bytes) return false;
    std::unique_ptr<unsigned char[]> storage(new unsigned char[bytes]);
    auto* encoders = reinterpret_cast<Gdiplus::ImageCodecInfo*>(storage.get());
    if (Gdiplus::GetImageEncoders(count, bytes, encoders) != Gdiplus::Ok) return false;
    for (UINT i = 0; i < count; ++i) {
        if (encoders[i].MimeType && std::wcscmp(encoders[i].MimeType, L"image/png") == 0) {
            result = encoders[i].Clsid;
            return true;
        }
    }
    return false;
}

bool prepare_temp(const std::wstring& requested, std::wstring& destination,
                  TempFile& temp) {
    const DWORD required = GetFullPathNameW(requested.c_str(), 0, nullptr, nullptr);
    if (required == 0) return false;
    std::vector<wchar_t> absolute(static_cast<size_t>(required) + 1);
    const DWORD written = GetFullPathNameW(requested.c_str(),
        static_cast<DWORD>(absolute.size()), absolute.data(), nullptr);
    if (written == 0 || written >= absolute.size()) return false;
    destination.assign(absolute.data(), written);
    const size_t slash = destination.find_last_of(L"\\/");
    if (slash == std::wstring::npos || slash + 1 == destination.size()) return false;
    const std::wstring directory = destination.substr(0, slash + 1);
    // GetTempFileName creates the file exclusively in the same directory, so
    // replacement remains on one volume and concurrent matches do not collide.
    wchar_t name[MAX_PATH + 1] = {};
    if (!GetTempFileNameW(directory.c_str(), L"pkh", 0, name)) return false;
    temp.path = name;
    return true;
}

bool flush_and_replace(TempFile& temp, const std::wstring& destination) {
    HANDLE file = CreateFileW(temp.path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const BOOL flushed = FlushFileBuffers(file);
    const BOOL closed = CloseHandle(file);
    if (!flushed || !closed) return false;
    if (!MoveFileExW(temp.path.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return false;
    temp.path.clear();
    return true;
}

} // namespace
#endif

bool write_found_key_card(const std::string& hex64,
                          const std::string& utf8path,
                          std::string& why,
                          const std::string& app_name) {
    why.clear();
#ifndef _WIN32
    (void)hex64;
    (void)utf8path;
    (void)app_name;
    why = "Private-key PNG rendering is supported on Windows only.";
    return false;
#else
    try {
    if (!valid_hex_key(hex64)) {
        why = "The private key must contain exactly 64 hexadecimal digits.";
        return false;
    }
    std::wstring requested;
    if (!path_to_wide(utf8path, requested)) {
        why = "The PNG output path is not valid UTF-8.";
        return false;
    }
    std::wstring label;
    if (app_name.size() > 24 || !path_to_wide(app_name, label)) {
        why = "The card application name is not valid UTF-8 or is too long.";
        return false;
    }
    label += L" · Private key";
    std::vector<uint8_t> modules;
    int qr_size = 0;
    if (!make_key_qr(hex64, modules, qr_size, why)) return false;
    if (qr_size <= 0 || qr_size > 177 ||
        modules.size() != static_cast<size_t>(qr_size) * static_cast<size_t>(qr_size)) {
        why = "The offline QR encoder returned invalid image dimensions.";
        return false;
    }
    GdiSession session;
    if (!session.ready()) {
        why = "Windows could not initialize PNG rendering.";
        return false;
    }
    using namespace Gdiplus;
    constexpr int width = 360, height = 560;
    Bitmap bitmap(width, height, PixelFormat24bppRGB);
    if (bitmap.GetLastStatus() != Ok || bitmap.SetResolution(96.0f, 96.0f) != Ok) {
        why = "Windows could not allocate the private-key image.";
        return false;
    }
    {
        Graphics graphics(&bitmap);
        FontFamily key_family(L"Consolas");
        FontFamily ui_family(L"Segoe UI");
        if (key_family.GetLastStatus() != Ok || !key_family.IsStyleAvailable(FontStyleBold)) {
            why = "The Windows Consolas Bold font is unavailable.";
            return false;
        }
        Font key_font(&key_family, 16.0f, FontStyleBold, UnitPoint);
        Font title_font(&ui_family, 15.0f, FontStyleBold, UnitPoint);
        Font note_font(&ui_family, 10.0f, FontStyleRegular, UnitPoint);
        if (graphics.GetLastStatus() != Ok || key_font.GetLastStatus() != Ok ||
            title_font.GetLastStatus() != Ok || note_font.GetLastStatus() != Ok) {
            why = "Windows could not initialize the card fonts.";
            return false;
        }
        SolidBrush ink(Color(255, 21, 30, 43));
        SolidBrush muted(Color(255, 71, 85, 105));
        SolidBrush black(Color(255, 0, 0, 0));
        Pen border(Color(255, 219, 226, 235), 1.0f);
        StringFormat centered;
        centered.SetAlignment(StringAlignmentCenter);
        centered.SetLineAlignment(StringAlignmentCenter);
        centered.SetFormatFlags(StringFormatFlagsNoWrap);
        bool ok = graphics.Clear(Color(255, 255, 255, 255)) == Ok;
        ok = graphics.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit) == Ok && ok;
        ok = graphics.DrawRectangle(&border, 0, 0, width - 1, height - 1) == Ok && ok;
        ok = graphics.DrawString(label.c_str(), -1, &title_font,
            RectF(16.0f, 18.0f, 328.0f, 32.0f), &centered, &ink) == Ok && ok;
        for (int row = 0; row < 4; ++row) {
            const std::string part = hex64.substr(static_cast<size_t>(row) * 16, 16);
            const std::wstring line(part.begin(), part.end());
            ok = graphics.DrawString(line.c_str(), 16, &key_font,
                RectF(20.0f, 64.0f + static_cast<REAL>(row) * 30.0f, 320.0f, 30.0f),
                &centered, &ink) == Ok && ok;
        }
        ok = graphics.DrawString(L"QR: полный HEX-ключ", -1, &note_font,
            RectF(16.0f, 188.0f, 328.0f, 26.0f), &centered, &muted) == Ok && ok;
        // Integer coordinates and no smoothing keep QR module edges sharp.
        // The four-module white quiet zone is part of the rendered square.
        constexpr int quiet = 4;
        const int pitch = 288 / (qr_size + 2 * quiet);
        if (pitch < 1) {
            why = "The offline QR code is too large for the card.";
            return false;
        }
        const int qr_pixels = (qr_size + 2 * quiet) * pitch;
        const int left = (width - qr_pixels) / 2 + quiet * pitch;
        const int top = 222 + (288 - qr_pixels) / 2 + quiet * pitch;
        ok = graphics.SetSmoothingMode(SmoothingModeNone) == Ok && ok;
        for (int y = 0; y < qr_size; ++y) {
            for (int x = 0; x < qr_size; ++x) {
                if (modules[static_cast<size_t>(y) * qr_size + x]) {
                    ok = graphics.FillRectangle(&black, left + x * pitch,
                        top + y * pitch, pitch, pitch) == Ok && ok;
                }
            }
        }
        ok = graphics.DrawString(L"Для копирования — текст в Telegram", -1, &note_font,
            RectF(12.0f, 521.0f, 336.0f, 26.0f), &centered, &muted) == Ok && ok;
        graphics.Flush(FlushIntentionSync);
        if (!ok) {
            why = "Windows could not draw the complete private-key card.";
            return false;
        }
    }
    CLSID png_encoder;
    if (!find_png_encoder(png_encoder)) {
        why = "The Windows PNG encoder is unavailable.";
        return false;
    }
    std::wstring destination;
    TempFile temp;
    if (!prepare_temp(requested, destination, temp)) {
        why = "Cannot create a PNG temporary file in the output folder.";
        return false;
    }
    if (bitmap.Save(temp.path.c_str(), &png_encoder, nullptr) != Ok) {
        why = "Windows could not save the private-key PNG.";
        return false;
    }
    if (!flush_and_replace(temp, destination)) {
        why = "Windows could not flush and finalize the private-key PNG.";
        return false;
    }
    return true;
    } catch (...) {
        why = "Windows could not finish rendering the private-key card.";
        return false;
    }
#endif
}
