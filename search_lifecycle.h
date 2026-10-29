#pragma once
#include <atomic>
#include <exception>
#include <utility>

// Only the owning thread reads/rethrows after joining every guarded thread.
// An exception must never escape a scanning or status std::thread.
class SearchThreadFailure {
    std::atomic<bool> failed_{false};
    std::exception_ptr first_;
public:
    template<class Work, class Stop>
    void run(Work&& work, Stop&& stop) noexcept {
        try {
            std::forward<Work>(work)();
        } catch (...) {
            if (!failed_.exchange(true, std::memory_order_relaxed))
                first_ = std::current_exception();
            try { std::forward<Stop>(stop)(); } catch (...) {}
        }
    }
    bool failed() const noexcept { return failed_.load(std::memory_order_relaxed); }
    void rethrow_after_join() const { if (first_) std::rethrow_exception(first_); }
};

inline bool search_keep_console_open(bool interactive, bool input_console,
                                     bool output_console, bool external_shutdown,
                                     bool error_exit) noexcept {
    return interactive && input_console && output_console
        && (error_exit || !external_shutdown);
}

inline bool search_close_completion_wait(bool external_shutdown, bool error_exit) noexcept {
    return external_shutdown && !error_exit;
}
