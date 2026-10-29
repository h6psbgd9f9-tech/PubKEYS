#pragma once
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace found_display {
inline std::string elapsed(std::chrono::steady_clock::time_point start) {
    auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - start).count();
    if (seconds < 0) seconds = 0;
    char text[96];
    std::snprintf(text, sizeof(text), "%02lld ч %02lld мин %02lld сек",
        (long long)(seconds / 3600), (long long)(seconds / 60 % 60),
        (long long)(seconds % 60));
    return text;
}

// Called only for a confirmed finding, after persistence has been attempted.
// No escape sequences or animation when stdout is redirected to a file.
inline void heading(const std::string& text, bool animate = true) {
    std::fflush(stdout);
#ifdef _WIN32
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    CONSOLE_SCREEN_BUFFER_INFO info{};
    int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0);
    if (length > 0 && GetConsoleMode(output, &mode) && GetConsoleScreenBufferInfo(output, &info)) {
        std::wstring wide((size_t)length, L' ');
        MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), &wide[0], length);
        const WORD yellow = (info.wAttributes & ~WORD(0x0f)) |
            FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY;
        const WORD hidden = (info.wAttributes & ~WORD(0x0f)) | ((info.wAttributes >> 4) & 0x0f);
        DWORD written = 0;
        SetConsoleTextAttribute(output, yellow);
        WriteConsoleW(output, wide.data(), (DWORD)wide.size(), &written, nullptr);
        // Avoid cursor animation on a wrapped line or the last buffer row.
        if (animate && info.dwCursorPosition.X == 0 && length < info.dwSize.X &&
                info.dwCursorPosition.Y + 1 < info.dwSize.Y) {
            for (int i = 0; i < 2; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(220));
                CONSOLE_SCREEN_BUFFER_INFO current{};
                if (!GetConsoleScreenBufferInfo(output, &current) ||
                    current.dwSize.X != info.dwSize.X || current.dwSize.Y != info.dwSize.Y) break;
                if (!SetConsoleCursorPosition(output, info.dwCursorPosition)) break;
                SetConsoleTextAttribute(output, hidden);
                WriteConsoleW(output, wide.data(), (DWORD)wide.size(), &written, nullptr);
                std::this_thread::sleep_for(std::chrono::milliseconds(180));
                SetConsoleCursorPosition(output, info.dwCursorPosition);
                SetConsoleTextAttribute(output, yellow);
                WriteConsoleW(output, wide.data(), (DWORD)wide.size(), &written, nullptr);
            }
        }
        SetConsoleTextAttribute(output, info.wAttributes);
        WriteConsoleW(output, L"\r\n", 2, &written, nullptr);
        return;
    }
#else
    if (isatty(fileno(stdout))) {
        std::printf("\033[1;33m%s\033[0m\n", text.c_str());
        std::fflush(stdout);
        return;
    }
#endif
    std::printf("%s\n", text.c_str());
    std::fflush(stdout);
}
}
