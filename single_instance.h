#pragma once

#include <istream>
#include <string>

struct SingleInstanceStop {};

#ifdef _WIN32
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <thread>

// One guard on the application's main thread. Names identify the application,
// not its filename or directory, so renamed copies use the same protocol.
class SingleInstanceGuard {
public:
    using StopCallback = void (*)();

    SingleInstanceGuard(const wchar_t* name, StopCallback stop,
                        const char* /*application*/, DWORD timeout_ms = 0)
        : callback_(stop) {
        const std::wstring base(name);
        gate_ = CreateMutexW(nullptr, FALSE, (base + L".launch").c_str());
        if (!gate_) { error_ = GetLastError(); return; }
        DWORD result = WaitForSingleObject(gate_, timeout_ms);
        if (!owned(result)) { set_wait_error(result); return; }
        gate_owned_ = true;
        run_ = CreateMutexW(nullptr, FALSE, (base + L".run").c_str());
        if (!run_) { error_ = GetLastError(); return; }
        result = WaitForSingleObject(run_, 0);
        if (result == WAIT_TIMEOUT) {
            // A new launch must not interrupt a running search, including
            // launches made by maintenance scripts and computational tests.
            error_ = ERROR_ALREADY_EXISTS;
            return;
        }
        if (!owned(result)) { set_wait_error(result); return; }
        run_owned_ = true;
        request_ = CreateEventW(nullptr, TRUE, FALSE, (base + L".stop").c_str());
        if (!request_) { error_ = GetLastError(); return; }
        // Only the new owner initializes the legacy stop-listener event.
        // A rejected launch never creates, signals or resets that event.
        if (!ResetEvent(request_)) { error_ = GetLastError(); return; }
        done_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!done_) { error_ = GetLastError(); return; }
        if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                             GetCurrentProcess(), &main_thread_,
                             THREAD_TERMINATE, FALSE, 0)) {
            error_ = GetLastError(); return;
        }
        current() = this;
        try { listener_ = std::thread([this]() { listen(); }); }
        catch (...) { current() = nullptr; error_ = ERROR_NOT_ENOUGH_MEMORY; return; }
        acquired_ = true;
        ReleaseMutex(gate_);
        gate_owned_ = false;
    }

    ~SingleInstanceGuard() {
        if (done_) SetEvent(done_);
        if (listener_.joinable()) listener_.join();
        if (current() == this) current() = nullptr;
        if (main_thread_) CloseHandle(main_thread_);
        if (done_) CloseHandle(done_);
        if (request_) CloseHandle(request_);
        if (gate_owned_) ReleaseMutex(gate_);
        if (gate_) CloseHandle(gate_);
        if (!acquired_) {
            if (run_owned_) ReleaseMutex(run_);
            if (run_) CloseHandle(run_);
        }
        // Deliberately retain the owned run handle until process exit. It
        // protects static destructors/CRT flushes too, beyond this destructor.
        // Windows releases it when the main thread exits, also after a crash.
        // This guard must not be reused for multiple sessions in one process.
    }

    SingleInstanceGuard(const SingleInstanceGuard&) = delete;
    SingleInstanceGuard& operator=(const SingleInstanceGuard&) = delete;
    bool acquired() const { return acquired_; }
    DWORD error() const { return error_; }
    bool stopping() const { return stopping_.load(std::memory_order_acquire); }

    static void throw_if_stopping() {
        if (current() && current()->stopping()) throw SingleInstanceStop{};
    }

    static bool read_line(std::istream& input, std::string& line) {
        SingleInstanceGuard* self = current();
        if (!self) return bool(std::getline(input, line));
        // Cancellation is limited to this input scope. In particular, never
        // cancel a checkpoint write or a pending-found durable save.
        {
            std::lock_guard<std::mutex> lock(self->input_mutex_);
            if (self->stopping()) throw SingleInstanceStop{};
            self->input_pending_ = true;
        }
        bool result;
        try { result = bool(std::getline(input, line)); }
        catch (...) {
            std::lock_guard<std::mutex> lock(self->input_mutex_);
            self->input_pending_ = false;
            throw;
        }
        {
            std::lock_guard<std::mutex> lock(self->input_mutex_);
            self->input_pending_ = false;
        }
        throw_if_stopping();
        return result;
    }

private:
    static SingleInstanceGuard*& current() {
        static SingleInstanceGuard* instance = nullptr;
        return instance;
    }
    static bool owned(DWORD result) {
        return result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
    }
    void set_wait_error(DWORD result) {
        error_ = result == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
    }
    void listen() {
        HANDLE events[] = {done_, request_};
        if (WaitForMultipleObjects(2, events, FALSE, INFINITE) != WAIT_OBJECT_0 + 1)
            return;
        stopping_.store(true, std::memory_order_release);
        do {
            // Repeat because some application self-tests reset their stop flag.
            // This independent request remains set until the process exits.
            if (callback_) callback_();
            {
                std::lock_guard<std::mutex> lock(input_mutex_);
                if (input_pending_) CancelSynchronousIo(main_thread_);
            }
            // Retry closes the race where the request arrives immediately
            // before getline enters ReadFile/ReadConsole.
        } while (WaitForSingleObject(done_, 50) == WAIT_TIMEOUT);
    }
    HANDLE gate_ = nullptr, run_ = nullptr, request_ = nullptr;
    HANDLE done_ = nullptr, main_thread_ = nullptr;
    bool gate_owned_ = false, run_owned_ = false, acquired_ = false;
    DWORD error_ = ERROR_SUCCESS;
    StopCallback callback_;
    std::atomic<bool> stopping_{false};
    std::thread listener_;
    std::mutex input_mutex_;
    bool input_pending_ = false;
};

inline bool single_instance_read_line(std::istream& input, std::string& line) {
    return SingleInstanceGuard::read_line(input, line);
}
inline void single_instance_check_stop() { SingleInstanceGuard::throw_if_stopping(); }
#else
inline bool single_instance_read_line(std::istream& input, std::string& line) {
    return bool(std::getline(input, line));
}
inline void single_instance_check_stop() {}
#endif
