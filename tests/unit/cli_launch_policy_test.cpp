#include <windows.h>
#include <atomic>
#include <cstdio>
#include <iostream>
#include <istream>
#include <mutex>
#include <string>
#include <thread>
#include "cli_launch_policy.h"

// Every Win32 entry point used by SingleInstanceGuard is replaced below.
// This test never opens a global named mutex/event or another process/thread.
namespace fake_win {
struct State {
    bool gate_busy = false, run_busy = false;
    unsigned run_waits = 0, stop_signals = 0, reset_signals = 0;
    unsigned callbacks = 0, creates = 0;
} state;
inline HANDLE handle(size_t id) { return reinterpret_cast<HANDLE>(id); }
inline HANDLE create_mutex(LPSECURITY_ATTRIBUTES, BOOL, LPCWSTR name) {
    ++state.creates;
    return handle(std::wstring(name).find(L".launch") != std::wstring::npos ? 1 : 2);
}
inline DWORD wait_one(HANDLE item, DWORD) {
    if (item == handle(1)) return state.gate_busy ? WAIT_TIMEOUT : WAIT_OBJECT_0;
    if (item == handle(2)) {
        ++state.run_waits;
        return state.run_busy && state.run_waits == 1 ? WAIT_TIMEOUT : WAIT_OBJECT_0;
    }
    return WAIT_OBJECT_0;
}
inline HANDLE create_event(LPSECURITY_ATTRIBUTES, BOOL, BOOL, LPCWSTR name) {
    ++state.creates;
    return handle(name ? 3 : 4);
}
inline BOOL set_event(HANDLE item) { if (item == handle(3)) ++state.stop_signals; return TRUE; }
inline BOOL reset_event(HANDLE) { ++state.reset_signals; return TRUE; }
inline DWORD last_error() { return ERROR_SUCCESS; }
inline BOOL release_mutex(HANDLE) { return TRUE; }
inline BOOL close_handle(HANDLE) { return TRUE; }
inline HANDLE current_process() { return handle(6); }
inline HANDLE current_thread() { return handle(7); }
inline BOOL duplicate_handle(HANDLE, HANDLE, HANDLE, LPHANDLE result, DWORD, BOOL, DWORD) {
    *result = handle(5); return TRUE;
}
inline DWORD wait_many(DWORD, const HANDLE*, BOOL, DWORD) { return WAIT_OBJECT_0; }
inline BOOL cancel_io(HANDLE) { return TRUE; }
inline void stop() { ++state.callbacks; }
} // namespace fake_win

#define CreateMutexW fake_win::create_mutex
#define WaitForSingleObject fake_win::wait_one
#define CreateEventW fake_win::create_event
#define SetEvent fake_win::set_event
#define ResetEvent fake_win::reset_event
#define GetLastError fake_win::last_error
#define ReleaseMutex fake_win::release_mutex
#define CloseHandle fake_win::close_handle
#define GetCurrentProcess fake_win::current_process
#define GetCurrentThread fake_win::current_thread
#define DuplicateHandle fake_win::duplicate_handle
#define WaitForMultipleObjects fake_win::wait_many
#define CancelSynchronousIo fake_win::cancel_io
#include "single_instance.h"
#undef CreateMutexW
#undef WaitForSingleObject
#undef CreateEventW
#undef SetEvent
#undef ResetEvent
#undef GetLastError
#undef ReleaseMutex
#undef CloseHandle
#undef GetCurrentProcess
#undef GetCurrentThread
#undef DuplicateHandle
#undef WaitForMultipleObjects
#undef CancelSynchronousIo

static unsigned passed = 0, failed = 0;
static void check(bool condition, const char* label) {
    if (condition) ++passed;
    else { ++failed; std::cerr << "FAIL " << label << '\n'; }
}
int main() {
    const char* no_args[] = {"test"};
    check(!cli_help_requested(1, no_args), "normal launch is not help");
    check(!cli_exclusive_diagnostic(1, no_args), "normal launch is not a diagnostic");
    for (const char* flag : {"-h", "--help"}) {
        const char* args[] = {"test", flag};
        check(cli_help_requested(2, args), "help flag recognized before guard");
        check(!cli_exclusive_diagnostic(2, args), "help does not need exclusive resource");
    }
    for (const char* flag : {"--test", "--range-test", "--strict-range-test", "--bench"}) {
        const char* args[] = {"test", flag, "2"};
        check(cli_exclusive_diagnostic(3, args), "computational diagnostic never takes over");
        check(!cli_help_requested(3, args), "diagnostic is not implicit help");
    }
    const char* search_args[] = {"test", "--kangaroo-gpu-db", "synthetic-public-key"};
    check(cli_removed_kangaroo_requested(3, search_args), "removed search cannot enter application");
    check(!cli_removed_kangaroo_requested(1, no_args), "ordinary launch remains available");
    for (const char* flag : {"--kangaroo", "--kangaroo-gpu", "--kangaroo-gpu-db", "--kangaroo-test",
                             "--kangaroo-selftest", "--kangaroo-unknown", "--kangaroo123"}) {
        const char* args[] = {"test", flag, "synthetic"};
        check(cli_removed_kangaroo_requested(3, args), "all removed aliases rejected");
        const char* with_help[] = {"test", "--help", flag};
        check(cli_removed_kangaroo_requested(3, with_help), "removed command wins even with help");
    }
    for (const char* flag : {"--test", "--bench", "--help", "-h", "database.txt", "prefix--kangaroo"}) {
        const char* args[] = {"test", flag};
        check(!cli_removed_kangaroo_requested(2, args), "ordinary arguments are not removed commands");
    }
    const char* combined[] = {"test", "--test", "--help"};
    check(cli_help_requested(3, combined), "explicit help wins over diagnostic workload");

    fake_win::state = {};
    fake_win::state.run_busy = true;
    {
        SingleInstanceGuard guard(L"TEST_ONLY_NO_REAL_IPC", fake_win::stop, "test");
        check(!guard.acquired(), "active search rejects diagnostics");
        check(guard.error() == ERROR_ALREADY_EXISTS, "active search has clear diagnostic result");
    }
    check(fake_win::state.stop_signals == 0, "busy diagnostic never signals current search");
    check(fake_win::state.callbacks == 0, "busy diagnostic never invokes stop callback");
    check(fake_win::state.reset_signals == 0, "busy diagnostic never resets current stop event");
    check(fake_win::state.run_waits == 1, "busy diagnostic does not wait for or replace current run");
    check(fake_win::state.creates == 2, "busy diagnostic does not open another owner's stop event");

    fake_win::state = {};
    fake_win::state.gate_busy = true;
    {
        SingleInstanceGuard guard(L"TEST_ONLY_NO_REAL_IPC", fake_win::stop, "test");
        check(!guard.acquired(), "concurrent launch rejects diagnostics");
        check(guard.error() == ERROR_TIMEOUT, "concurrent launch reports busy gate");
    }
    check(fake_win::state.creates == 1, "busy launch does not open run/stop IPC");
    check(fake_win::state.stop_signals == 0 && fake_win::state.callbacks == 0,
          "busy launch cannot signal any stop");

    fake_win::state = {};
    {
        SingleInstanceGuard guard(L"TEST_ONLY_NO_REAL_IPC", fake_win::stop, "test");
        check(guard.acquired(), "idle diagnostics acquire normal exclusive guard");
    }
    check(fake_win::state.stop_signals == 0 && fake_win::state.callbacks == 0,
          "idle diagnostics cannot stop a prior search");
    check(fake_win::state.reset_signals == 1, "new idle owner initializes own event");

    fake_win::state = {};
    fake_win::state.run_busy = true;
    {
        SingleInstanceGuard guard(L"TEST_ONLY_NO_REAL_IPC", fake_win::stop, "test");
        check(!guard.acquired(), "default search launch refuses an active search");
    }
    check(fake_win::state.stop_signals == 0 && fake_win::state.run_waits == 1,
          "default search launch cannot request takeover");
    std::cout << "CLI/guard isolated checks: " << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
