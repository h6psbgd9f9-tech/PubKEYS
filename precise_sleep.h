#ifndef PK_PRECISE_SLEEP_H
#define PK_PRECISE_SLEEP_H

#include <chrono>
#include <cmath>
#include <cstdint>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#endif

// Tests can force the portable path without changing the system timer period.
#ifndef PK_PRECISE_SLEEP_FORCE_FALLBACK
#define PK_PRECISE_SLEEP_FORCE_FALLBACK 0
#endif

namespace pk_timing {
enum class SleepResult { skipped, high_resolution, fallback, invalid_duration };

namespace detail {
#ifdef _WIN32
class ThreadTimer {
    // Resolve dynamically: older Windows/SDK target settings must retain the
    // normal sleep path. Context is always null; no APC or wake request is used.
    using CreateFn = HANDLE (WINAPI*)(LPSECURITY_ATTRIBUTES,LPCWSTR,DWORD,DWORD);
    using SetFn = BOOL (WINAPI*)(HANDLE,const LARGE_INTEGER*,LONG,PTIMERAPCROUTINE,LPVOID,void*,ULONG);
    HANDLE timer_ = nullptr;
    SetFn set_ = nullptr;
public:
    ThreadTimer() noexcept {
#if !PK_PRECISE_SLEEP_FORCE_FALLBACK
        const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
        if (!kernel) return;
        const auto create = reinterpret_cast<CreateFn>(GetProcAddress(kernel,"CreateWaitableTimerExW"));
        set_ = reinterpret_cast<SetFn>(GetProcAddress(kernel,"SetWaitableTimerEx"));
        if (!create || !set_) return;
        constexpr DWORD high_resolution = 0x00000002; // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
        timer_ = create(nullptr,nullptr,high_resolution,TIMER_MODIFY_STATE|SYNCHRONIZE);
#endif
    }
    ThreadTimer(const ThreadTimer&) = delete;
    ThreadTimer& operator=(const ThreadTimer&) = delete;
    ~ThreadTimer() { disable(); }
    void disable() noexcept {
        if (timer_) { CloseHandle(timer_); timer_ = nullptr; }
    }
    bool wait(double seconds) noexcept {
        if (!timer_ || !set_) return false;
        // Caller bounds each chunk to <=2 seconds. Round UP to 100 ns so a
        // positive duration cannot become due=0 (an absolute past timestamp).
        const auto ticks = static_cast<LONGLONG>(std::ceil(seconds * 10000000.0));
        LARGE_INTEGER due; due.QuadPart = -(ticks > 0 ? ticks : 1);
        if (set_(timer_,&due,0,nullptr,nullptr,nullptr,0)
                && WaitForSingleObject(timer_,INFINITE) == WAIT_OBJECT_0) return true;
        // Close/cancel an unusable timer. Future calls on this thread fall back.
        disable();
        return false;
    }
};
#endif
} // namespace detail

// For duty-cycle pauses only. The caller still computes
//     requested = busy * (100 - power_percent) / power_percent.
// This does not change timeBeginPeriod, system timer resolution, priority,
// GPU settings, or checkpoint position, and does not spin while waiting.
// Millisecond/microsecond chrono durations convert to this seconds duration.
// Nonpositive finite values are no-ops. NaN/infinity or durations beyond a
// conservative steady-clock horizon are explicitly rejected, avoiding integer
// overflow. Normal sleeps return only after the steady-clock deadline.
inline SleepResult sleep_for(std::chrono::duration<double> requested) {
    using Clock = std::chrono::steady_clock;
    using Duration = Clock::duration;
    const double seconds = requested.count();
    if (!std::isfinite(seconds)) return SleepResult::invalid_duration;
    if (seconds <= 0.0) return SleepResult::skipped;
    const long double safe_horizon = std::chrono::duration<long double>((Duration::max)()).count() / 4.0L;
    if (static_cast<long double>(seconds) >= safe_horizon) return SleepResult::invalid_duration;
    Duration delay = std::chrono::ceil<Duration>(requested);
    if (delay <= Duration::zero()) delay = Duration(1);
    const auto start = Clock::now();
    const auto since_epoch = start.time_since_epoch();
    if (since_epoch > Duration::zero() && delay > (Duration::max)() - since_epoch)
        return SleepResult::invalid_duration;
    const auto deadline = start + delay;
    bool used_precise = false, used_fallback = false;
    for (;;) {
        const auto now = Clock::now();
        if (now >= deadline) break;
        double remaining = std::chrono::duration<double>(deadline - now).count();
        if (remaining > 2.0) remaining = 2.0;
#if defined(_WIN32) && !PK_PRECISE_SLEEP_FORCE_FALLBACK
        static thread_local detail::ThreadTimer timer;
        if (timer.wait(remaining)) { used_precise = true; continue; }
#endif
        // Recompute after a failed wait, so fallback does not sleep the original
        // request twice. Two-second chunks also bound chrono/OS conversions.
        const auto fallback_now = Clock::now();
        if (fallback_now >= deadline) break;
        remaining = std::chrono::duration<double>(deadline - fallback_now).count();
        if (remaining > 2.0) remaining = 2.0;
        used_fallback = true;
        std::this_thread::sleep_for(std::chrono::duration<double>(remaining));
    }
    return used_fallback ? SleepResult::fallback
                        : (used_precise ? SleepResult::high_resolution : SleepResult::skipped);
}
} // namespace pk_timing
#endif
