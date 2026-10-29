// ============================================================================
//  util.cpp — цвета, форматирование времени и чисел, мелкие строковые вещи.
// ============================================================================
#include "common.h"
#include "platform.h"   // кодировка консоли -- через слой систем, а не мимо него
#include "stop_confirmation.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <thread>     // сон в ожидании клавиши, см. pause_check
#if defined(_WIN32)
  #include <io.h>
  #include <windows.h>
#else
  #include <unistd.h>
#endif

// ЦВЕТА
// ========================================================
bool g_color = true;
const char *C_RESET, *C_BOLD, *C_RED, *C_GREEN, *C_YELLOW, *C_CYAN, *C_BLINK;

void init_colors() {
#if defined(_WIN32)
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD m = 0;
    if (GetConsoleMode(h, &m)) SetConsoleMode(h, m | 0x0004 /*VT processing*/);
    // Было SetConsoleOutputCP(CP_UTF8) прямо здесь -- вторая, независимая
    // копия того, что делает plat_console_utf8(). Ровно тот случай, от
    // которого заведён platform.h: две копии одного знания однажды разойдутся.
    plat_console_utf8();
    g_color = _isatty(_fileno(stdout)) != 0;
#else
    g_color = isatty(fileno(stdout)) != 0;
#endif
    if (g_color) {
        C_RESET="\033[0m"; C_BOLD="\033[1m"; C_RED="\033[31m"; C_GREEN="\033[32m";
        C_YELLOW="\033[33m"; C_CYAN="\033[36m"; C_BLINK="\033[5m";
    } else {
        C_RESET=C_BOLD=C_RED=C_GREEN=C_YELLOW=C_CYAN=C_BLINK="";
    }
}

// ========================================================
// МЕЛОЧИ
// ========================================================
std::chrono::steady_clock::time_point g_t0;

std::string format_elapsed() {
    long long el = (long long)std::chrono::duration_cast<std::chrono::seconds>(
                       search_now() - g_t0).count();
    char buf[32];
    snprintf(buf, sizeof(buf), "%02lld:%02lld:%02lld", el / 3600, (el % 3600) / 60, el % 60);
    return buf;
}

// Keyboard input is consumed by one monitor thread. Key-up tracking suppresses
// auto-repeat; holding Space cannot immediately resume or pause again.
static bool pause_keys_down[256]{};
static bool pause_announced = false;
static bool pause_power_announced = false;
static StopConfirmation stop_confirmation;

void pause_start(const std::atomic<bool>* stop) {
    g_pause.reset(stop);
    std::fill(std::begin(pause_keys_down), std::end(pause_keys_down), false);
    pause_announced = false;
    pause_power_announced = false;
    stop_confirmation = StopConfirmation{};
    // Sample before workers/helper launch, so a run started on battery waits.
    g_pause.update_power_source(plat_power_source());
}

PauseEvent pause_poll(const std::function<bool(int)>& on_key) {
    g_pause.update_power_source(plat_power_source());
    const bool on_battery = g_pause.automatic_requested();
    PauseEvent power_event = PauseEvent::none;
    if (on_battery != pause_power_announced) {
        pause_power_announced = on_battery;
        pause_announced = false;
        power_event = on_battery ? PauseEvent::power_lost : PauseEvent::power_restored;
    }
#ifdef _WIN32
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0, count = 0;
    if (GetConsoleMode(input, &mode)) {
        // Consume bounded batches so a busy input stream cannot starve stop.
        for (unsigned i = 0; i < 256; ++i) {
            INPUT_RECORD record{};
            if (!PeekConsoleInputW(input, &record, 1, &count) || !count) break;
            if (!ReadConsoleInputW(input, &record, 1, &count) || !count) break;
            if (record.EventType != KEY_EVENT) continue;
            const auto& key = record.Event.KeyEvent;
            const unsigned vk = key.wVirtualKeyCode;
            if (vk >= 256) continue;
            if (!key.bKeyDown) { pause_keys_down[vk] = false; continue; }
            const bool repeat = pause_keys_down[vk];
            pause_keys_down[vk] = true;
            if (repeat || vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU) continue;
            // Copy/paste and other modified shortcuts are not search commands.
            if (key.dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED |
                                          LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) continue;
            const auto action = stop_confirmation.key((int)key.uChar.UnicodeChar);
            if (action != StopConfirmation::Action::pass) {
                if (action == StopConfirmation::Action::confirm) set_stop();
                std::lock_guard<std::mutex> output(g_mtx_cout);
                if (action == StopConfirmation::Action::prompt)
                    std::cout << "\n  Остановить поиск? Y — подтвердить в течение 10 секунд; другая клавиша — отменить. Поиск продолжается.\n";
                else if (action == StopConfirmation::Action::cancel)
                    std::cout << "\n  Остановка отменена.\n";
                else if (action == StopConfirmation::Action::confirm)
                    std::cout << "\n  Остановка подтверждена. Завершаю текущие вычисления.\n";
                std::cout.flush();
                if (action == StopConfirmation::Action::confirm) return PauseEvent::none;
                continue;
            }
            // Q/Y above remains available. Other keys cannot bypass the AC
            // interlock or change the percentage retained for automatic resume.
            // Also consume commands queued on battery during the restoration
            // poll. They must not immediately change the retained percentage.
            if (on_battery || power_event == PauseEvent::power_restored) continue;
            if (on_key && on_key((int)key.uChar.UnicodeChar)) continue;
            if (g_pause.manual_requested()) {
                g_pause.resume();
                pause_announced = false;
                return PauseEvent::resumed;
            }
            if (vk == VK_SPACE) {
                g_pause.request();
                return PauseEvent::requested;
            }
        }
    }
#endif
    if (power_event != PauseEvent::none) return power_event;
    if (g_pause.ready() && !pause_announced) {
        pause_announced = true;
        return PauseEvent::paused;
    }
    return PauseEvent::none;
}

bool pause_check() { return g_pause.checkpoint(); }

// 1234567 -> "1 234 567"
std::string fmt_num(uint64_t v) {
    std::string s = std::to_string(v), out;
    int c = 0;
    for (int i = (int)s.size() - 1; i >= 0; i--) {
        out += s[i];
        if (++c % 3 == 0 && i > 0) out += ' ';
    }
    std::reverse(out.begin(), out.end());
    return out;
}

std::string now_string() {
    time_t t = time(nullptr);
    // localtime отдаёт указатель на ОБЩИЙ статический буфер (на glibc -- один
    // на процесс). Штамп времени берут все рабочие потоки сразу через журнал
    // участков, плюс фоновые потоки повторной отправки уведомлений: два
    // одновременных вызова дают перемешанную дату. Плюс localtime возвращает
    // nullptr при сбитых часах, а strftime его тут же разыменовывал.
    struct tm tmv{};
#ifdef _WIN32
    if (localtime_s(&tmv, &t) != 0) return "\?\?\?\?-\?\?-\?\? \?\?:\?\?:\?\?";
#else
    if (!localtime_r(&t, &tmv))     return "\?\?\?\?-\?\?-\?\? \?\?:\?\?:\?\?";
#endif
    char b[64];
    if (!strftime(b, sizeof(b), "%Y-%m-%d %H:%M:%S", &tmv)) return "\?\?\?\?-\?\?-\?\? \?\?:\?\?:\?\?";
    return b;
}

std::string trim(std::string s) {
    while (!s.empty() && (s.back()=='\r'||s.back()=='\n'||s.back()==' '||s.back()=='\t')) s.pop_back();
    size_t b = s.find_first_not_of(" \t");
    return (b == std::string::npos) ? std::string() : s.substr(b);
}

// ========================================================

// Дополняет строку до нужной ширины ПО СИМВОЛАМ, а не по байтам: в UTF-8
// кириллица занимает по два байта, и обычный %-30s разъезжается.
std::string pad_utf8(const std::string &s, size_t width) {
    size_t chars = 0;
    for (unsigned char c : s) if ((c & 0xC0) != 0x80) chars++;
    // Строка уже шире нужного -- дополнять нечем. Прежний вариант дописывал
    // в этом случае ровно один пробел, то есть "дополнение до ширины"
    // превращалось в "ширина + 1" и колонка разъезжалась ровно там, где
    // выравнивание и нужно.
    return s + std::string(chars < width ? width - chars : 0, ' ');
}
