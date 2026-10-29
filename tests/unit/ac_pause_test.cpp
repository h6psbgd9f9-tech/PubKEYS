#include "common.h"
#include "platform.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

// This executable links util.cpp, never platform.cpp or the search backends.
// Its power source is synthetic; no hardware/configuration/network is touched.
static std::atomic<bool> test_stop{false};
std::atomic<bool>& g_stop = test_stop;
volatile sig_atomic_t g_stop_flag = 0;
std::mutex g_mtx_cout;
static std::atomic<PowerSource> test_source{PowerSource::mains};
PowerSource plat_power_source() { return test_source.load(); }
void plat_console_utf8() {}

static unsigned checks = 0;
static void check(bool condition, const char* message) {
    ++checks;
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); std::fflush(stderr); std::abort(); }
}
template<class Predicate> static void eventually(Predicate predicate, const char* message) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!predicate() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    check(predicate(), message);
}

static void reasons_and_clock() {
    std::atomic<bool> stop{false}; SearchPause pause; pause.reset(&stop);
    check(!pause.requested() && !pause.manual_requested() && !pause.automatic_requested(), "clean state");
    pause.request(); pause.set_automatic(true);
    check(pause.manual_requested() && pause.automatic_requested(), "both pause reasons retained");
    pause.resume();
    check(pause.requested() && !pause.manual_requested() && pause.automatic_requested(), "manual resume cannot bypass AC loss");
    pause.set_automatic(false);
    check(!pause.requested(), "AC return resumes when manual reason cleared");
    pause.set_automatic(true); pause.request(); pause.set_automatic(false);
    check(pause.requested() && pause.manual_requested() && !pause.automatic_requested(), "AC return preserves manual pause");
    pause.resume(); check(!pause.requested(), "manual resume after AC return");

    pause.set_automatic(true);
    const auto frozen = pause.now();
    const auto wall_start = SearchPause::Clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    check(pause.now() == frozen, "active search clock freezes on battery");
    pause.request(); pause.resume(); pause.set_automatic(true);
    check(pause.now() == frozen, "overlapping/repeated reasons do not reset pause clock");
    pause.set_automatic(false);
    const auto omitted = (SearchPause::Clock::now() - wall_start) - (pause.now() - frozen);
    check(omitted >= std::chrono::milliseconds(50), "resume excludes battery wait from active time");
    pause.request(); pause.set_automatic(true); pause.clear();
    check(!pause.requested() && !pause.manual_requested() && !pause.automatic_requested(), "cleanup clears both reasons");
}

static void multiple_workers() {
    std::atomic<bool> stop{false}; SearchPause pause; pause.reset(&stop);
    std::atomic<unsigned> registered{0}; std::atomic<uint64_t> completed{0};
    std::vector<std::thread> workers;
    std::vector<std::vector<uint64_t>> visited(3);
    // A search launched on battery must not perform its first work packet.
    pause.set_automatic(true);
    for (unsigned i = 0; i < 3; ++i) workers.emplace_back([&, i] {
        SearchPause::Worker worker(pause); ++registered;
        uint64_t cursor = 0;
        while (!stop.load()) {
            pause.checkpoint(); if (stop.load()) break;
            visited[i].push_back(cursor++);
            ++completed; std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    eventually([&]{return registered == 3 && pause.ready();}, "all workers acknowledge initial battery pause");
    check(completed == 0, "initial battery pause executes zero work packets");
    pause.set_automatic(false);
    eventually([&]{return completed >= 12;}, "workers resume after AC return");
    for (unsigned iteration = 0; iteration < 50; ++iteration) {
        pause.set_automatic(true);
        eventually([&]{return pause.ready();}, "rapid outage waits for every worker");
        const uint64_t before = completed;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        check(completed == before, "acknowledged outage has no work progress");
        pause.set_automatic(false);
        eventually([&]{return completed >= before + 3;}, "rapid AC return continues same workers");
    }
    pause.request(); pause.set_automatic(true);
    eventually([&]{return pause.ready();}, "overlapping manual/AC pause acknowledged");
    const uint64_t held = completed;
    pause.set_automatic(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    check(completed == held && pause.requested(), "AC restoration leaves manually paused workers stopped");
    // A genuine user stop must release gates without an AC restoration.
    stop = true;
    for (auto& worker : workers) worker.join();
    check(completed == held, "stop while paused does not execute another packet");
    uint64_t total = 0;
    for (const auto& sequence : visited) {
        for (size_t index = 0; index < sequence.size(); ++index)
            check(sequence[index] == index, "automatic resume preserves exact per-worker cursor without gaps or repeats");
        total += sequence.size();
    }
    check(total == completed, "every completed packet has exactly one recorded cursor");
    check(!pause.ready(), "worker destruction removes acknowledgement registration");
    pause.clear(); stop = false; pause.reset(&stop);
    check(!pause.requested(), "next session does not inherit AC/manual state");
}

static void acknowledgement_requires_every_worker() {
    std::atomic<bool> stop{false}, release_second{false}, second_registered{false};
    SearchPause pause; pause.reset(&stop); pause.set_automatic(true);
    check(!pause.ready(), "zero registered workers cannot acknowledge stopped search");
    std::thread first([&] { SearchPause::Worker worker(pause); pause.checkpoint(); });
    std::thread second([&] {
        SearchPause::Worker worker(pause); second_registered = true;
        while (!release_second.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        pause.checkpoint();
    });
    eventually([&]{return second_registered.load();}, "second worker registered outside pause gate");
    check(!pause.ready(), "partial worker acknowledgement cannot claim search paused");
    release_second = true;
    eventually([&]{return pause.ready();}, "last worker reaching gate acknowledges complete pause");
    pause.set_automatic(false); first.join(); second.join();
}

static void callback_cancellation() {
    std::atomic<bool> stop{false}, cancel{false}, finished{false};
    SearchPause pause; pause.reset(&stop); pause.set_automatic(true);
    std::thread worker([&] {
        SearchPause::Worker registration(pause);
        pause.checkpoint([&]{return cancel.load();}); finished = true;
    });
    eventually([&]{return pause.ready();}, "cancelable worker acknowledges pause");
    cancel = true;
    eventually([&]{return finished.load();}, "external termination escapes automatic pause");
    worker.join();
    check(pause.automatic_requested() && !stop, "external cancellation does not mutate AC reason or global stop");
    pause.clear();
}

static void set_source_and_poll(PowerSource source) {
    test_source = source;
    // Allow implementations to bound OS queries to a subsecond polling cadence.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(600);
    do { pause_poll(); std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
    while (std::chrono::steady_clock::now() < deadline);
}
static void production_power_policy() {
    g_stop = false; g_stop_flag = 0; g_shutdown_requested = false;
    test_source = PowerSource::battery; pause_start(&g_stop);
    check(g_pause.automatic_requested(), "pause_start synchronously detects battery before worker launch");
    set_source_and_poll(PowerSource::unknown);
    check(g_pause.automatic_requested(), "unknown power status cannot resume a battery pause");
    set_source_and_poll(PowerSource::mains);
    check(!g_pause.automatic_requested() && !g_pause.requested(), "mains restoration clears only automatic reason");
    g_pause.request(); set_source_and_poll(PowerSource::battery);
    check(g_pause.manual_requested() && g_pause.automatic_requested(), "production polling retains both reasons");
    set_source_and_poll(PowerSource::mains);
    check(g_pause.manual_requested() && !g_pause.automatic_requested() && g_pause.requested(), "production AC return preserves manual pause");
    g_pause.clear(); test_source = PowerSource::mains; pause_start(&g_stop);
    set_source_and_poll(PowerSource::unknown);
    check(!g_pause.requested(), "unknown after confirmed mains preserves last known source");
    g_pause.clear();
}

static void transition_events_are_not_repeated() {
    g_stop = false; g_stop_flag = 0; g_shutdown_requested = false;
    test_source = PowerSource::battery; pause_start(&g_stop);
    check(pause_poll() == PauseEvent::power_lost, "initial battery produces one loss event");
    check(pause_poll() == PauseEvent::none, "unchanged battery does not repeat event");
    test_source = PowerSource::unknown;
    check(pause_poll() == PauseEvent::none && g_pause.automatic_requested(), "unknown neither fabricates event nor resumes");
    test_source = PowerSource::mains;
    check(pause_poll() == PauseEvent::power_restored, "confirmed mains produces restoration event");
    check(pause_poll() == PauseEvent::none, "unchanged mains does not repeat event");
    test_source = PowerSource::battery;
    check(pause_poll() == PauseEvent::power_lost, "new real outage produces a new loss event");
    test_source = PowerSource::mains;
    check(pause_poll() == PauseEvent::power_restored, "rapid restoration event survives previous outage");
    g_pause.clear();
}

#ifdef _WIN32
static void keyboard_child() {
    DWORD owners[16]{};
    check(GetConsoleProcessList(owners, 16) == 1 && owners[0] == GetCurrentProcessId(), "keyboard events target this isolated console only");
    const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    check(FlushConsoleInputBuffer(input) != 0, "isolated console input ready");
    g_stop = false; g_stop_flag = 0; g_shutdown_requested = false;
    test_source = PowerSource::battery; pause_start(&g_stop);
    int power = 65; unsigned power_callbacks = 0;
    auto power_key = [&](int pressed) {
        if (pressed != '+' && pressed != '-' && pressed != '=' && pressed != '_') return false;
        ++power_callbacks; power += pressed == '+' || pressed == '=' ? 5 : -5; return true;
    };
    auto key = [&](WORD virtual_key, wchar_t character, DWORD modifiers = 0) {
        INPUT_RECORD records[2]{};
        for (int i = 0; i < 2; ++i) {
            records[i].EventType = KEY_EVENT;
            auto& event = records[i].Event.KeyEvent;
            event.bKeyDown = i == 0; event.wRepeatCount = 1;
            event.wVirtualKeyCode = virtual_key;
            event.uChar.UnicodeChar = character; event.dwControlKeyState = modifiers;
        }
        DWORD written = 0;
        check(WriteConsoleInputW(input, records, 2, &written) && written == 2, "write only isolated test input");
        return pause_poll(power_key);
    };
    key(VK_SPACE, L' '); key('A', L'a'); key(VK_OEM_PLUS, L'+'); key(VK_OEM_MINUS, L'-');
    key('C', wchar_t(3), LEFT_CTRL_PRESSED);
    check(g_pause.automatic_requested() && g_pause.requested() && !g_stop, "keyboard/copy cannot bypass battery pause");
    check(power == 65 && power_callbacks == 0, "battery pause blocks power edits and preserves exact power");
    // Queue input while power is still absent, then restore AC before the next
    // monitor poll. Stale battery-period commands must not alter resume power.
    INPUT_RECORD queued[4]{};
    for (int i = 0; i < 4; ++i) {
        queued[i].EventType = KEY_EVENT;
        auto& event = queued[i].Event.KeyEvent;
        event.bKeyDown = (i % 2) == 0; event.wRepeatCount = 1;
        event.wVirtualKeyCode = i < 2 ? VK_OEM_PLUS : VK_OEM_MINUS;
        event.uChar.UnicodeChar = i < 2 ? L'+' : L'-';
    }
    DWORD queued_count = 0;
    check(WriteConsoleInputW(input, queued, 4, &queued_count) && queued_count == 4, "queue only isolated battery-period power keys");
    test_source = PowerSource::mains;
    check(pause_poll(power_key) == PauseEvent::power_restored, "restoration event drains stale input before reporting resume");
    pause_poll(power_key);
    check(power_callbacks == 0, "queued battery-period power keys are not applied after AC return");
    check(!g_pause.requested() && power == 65, "automatic resume restores same 65 percent");
    key(VK_OEM_PLUS, L'+');
    check(power == 70 && power_callbacks == 1, "power keyboard works normally again on mains");
    set_source_and_poll(PowerSource::battery);
    key('Q', L'q'); key('Y', L'y');
    check(g_stop && g_stop_flag == 1 && g_shutdown_requested, "confirmed user stop still works during AC pause");
    g_pause.clear();
}
static void run_keyboard_child() {
    wchar_t path[MAX_PATH]{}; check(GetModuleFileNameW(nullptr, path, MAX_PATH) != 0, "locate own test executable");
    std::wstring command = L"\"" + std::wstring(path) + L"\" --keyboard-child";
    STARTUPINFOW startup{}; startup.cb = sizeof startup;
    startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    check(CreateProcessW(path, command.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE,
          nullptr, nullptr, &startup, &process) != 0, "start hidden independent keyboard test console");
    CloseHandle(process.hThread);
    const DWORD waited = WaitForSingleObject(process.hProcess, 15000);
    if (waited != WAIT_OBJECT_0) TerminateProcess(process.hProcess, 124);
    DWORD code = 0; GetExitCodeProcess(process.hProcess, &code); CloseHandle(process.hProcess);
    check(waited == WAIT_OBJECT_0 && code == 0, "isolated keyboard, power preservation and stop tests pass");
}
#endif

int main(int argc, char** argv) {
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    if (argc == 2 && std::string(argv[1]) == "--keyboard-child") {
        keyboard_child(); return 0;
    }
#endif
    reasons_and_clock(); multiple_workers(); acknowledgement_requires_every_worker(); callback_cancellation();
    production_power_policy(); transition_events_are_not_repeated();
#ifdef _WIN32
    run_keyboard_child();
#endif
    std::printf("PASS: %u AC/manual pause, worker, active clock, injected source, isolated keyboard and unchanged power checks; no GPU/hardware/config/network operations.\n", checks);
}
