#include "stop_confirmation.h"
#include "console_interrupt_guard.h"
#include "common.h"
#include <cassert>
#include <fstream>
#include <string>

static std::atomic<bool> test_stop{false};
std::atomic<bool>& g_stop = test_stop;
volatile sig_atomic_t g_stop_flag = 0;
std::mutex g_mtx_cout;
void plat_console_utf8() {} // tests do not initialize application output

int main(int argc, char** argv) {
    using S = StopConfirmation;
    using A = S::Action;
    const auto t = S::Clock::now();
    S s;
    assert(s.key(3, t) == A::ignore);
    assert(s.key('y', t) == A::pass);
    assert(s.key('q', t) == A::prompt);
    assert(s.key(3, t) == A::ignore);
    assert(s.key('Y', t + std::chrono::seconds(9)) == A::confirm);
    assert(s.key('Y', t) == A::pass);
    assert(s.key('Q', t) == A::prompt);
    assert(s.key('n', t) == A::cancel);
    assert(s.key('y', t) == A::pass);
    assert(s.key('q', t) == A::prompt);
    assert(s.key('y', t + std::chrono::seconds(10)) == A::pass);
    assert(s.key('q', t) == A::prompt);
    assert(s.key(' ', t) == A::cancel); // cancellation must not pause the search
    assert(s.key('+', t) == A::pass);
#ifdef _WIN32
    assert(ignore_copy_interrupt(CTRL_C_EVENT));
    assert(ignore_copy_interrupt(CTRL_BREAK_EVENT));
    assert(!ignore_copy_interrupt(CTRL_CLOSE_EVENT));
    if (argc == 3 && std::string(argv[1]) == "--console-test") {
        // Only deliver real events when this test owns a separate console.
        DWORD processes[16]{};
        if (GetConsoleProcessList(processes, 16) != 1 ||
            processes[0] != GetCurrentProcessId()) return 2;
        if (!SetConsoleCtrlHandler(ignore_copy_interrupt, TRUE)) return 3;
        for (int i = 0; i < 3; ++i) {
            if (!GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0)) return 4;
            Sleep(100);
            if (!GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, 0)) return 5;
            Sleep(100);
        }
        const auto input = GetStdHandle(STD_INPUT_HANDLE);
        FlushConsoleInputBuffer(input);
        pause_start(&g_stop);
        auto key = [&](WORD vk, wchar_t ch, DWORD modifiers = 0) {
            INPUT_RECORD records[2]{};
            for (int i = 0; i < 2; ++i) {
                records[i].EventType = KEY_EVENT;
                auto& k = records[i].Event.KeyEvent;
                k.bKeyDown = i == 0;
                k.wRepeatCount = 1;
                k.wVirtualKeyCode = vk;
                k.uChar.UnicodeChar = ch;
                k.dwControlKeyState = modifiers;
            }
            DWORD written = 0;
            assert(WriteConsoleInputW(input, records, 2, &written) && written == 2);
            return pause_poll();
        };
        key('C', 3, LEFT_CTRL_PRESSED);
        assert(!g_stop && !g_pause.requested());
        key('Q', L'q');
        assert(!g_stop && !g_pause.requested());
        key('Y', L'y', LEFT_CTRL_PRESSED);
        assert(!g_stop);
        key(VK_SPACE, L' ');
        assert(!g_stop && !g_pause.requested()); // cancels, not pauses
        assert(key(VK_SPACE, L' ') == PauseEvent::requested);
        key('C', 3, LEFT_CTRL_PRESSED);
        assert(g_pause.requested() && !g_stop); // copying cannot resume a pause
        key('Q', L'q');
        key('N', L'n');
        assert(g_pause.requested() && !g_stop);
        key('Q', L'q');
        key('Y', L'y');
        assert(g_stop && g_stop_flag == 1 && g_shutdown_requested);
        std::ofstream(argv[2]) << "PASS: repeated console interrupts, copy while paused, cancellation and confirmed graceful stop\n";
    }
#endif
}
