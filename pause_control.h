#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <functional>
#include "platform.h"

// One input owner (the monitor), cooperative gates in every search worker.
// Acknowledgment is published only when every registered worker is at a gate.
class SearchPause {
public:
    using Clock = std::chrono::steady_clock;
    void reset(const std::atomic<bool>* stop) {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = stop;
        elapsed_ = Clock::duration::zero();
        manual_.store(false);
        automatic_.store(false);
        requested_.store(false);
    }
    bool requested() const { return requested_.load(std::memory_order_acquire); }
    bool manual_requested() const { return manual_.load(std::memory_order_acquire); }
    bool automatic_requested() const { return automatic_.load(std::memory_order_acquire); }
    void request() {
        std::lock_guard<std::mutex> lock(mutex_);
        manual_.store(true, std::memory_order_release);
        update_locked();
    }
    void resume() {
        std::lock_guard<std::mutex> lock(mutex_);
        manual_.store(false, std::memory_order_release);
        update_locked();
    }
    void set_automatic(bool paused) {
        std::lock_guard<std::mutex> lock(mutex_);
        automatic_.store(paused, std::memory_order_release);
        update_locked();
    }
    void update_power_source(PowerSource source) {
        if (source == PowerSource::battery) set_automatic(true);
        else if (source == PowerSource::mains) set_automatic(false);
        // An unavailable/unknown reading must never release a battery pause.
    }
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        manual_.store(false, std::memory_order_release);
        automatic_.store(false, std::memory_order_release);
        update_locked();
    }
    bool ready() {
        std::lock_guard<std::mutex> lock(mutex_);
        return requested() && workers_ != 0 && waiting_ == workers_;
    }
    bool checkpoint(const std::function<bool()>& cancel = {}) {
        if (!requested()) return false;
        std::unique_lock<std::mutex> lock(mutex_);
        ++waiting_;
        while (requested() && !(stop_ && stop_->load(std::memory_order_relaxed))
               && !(cancel && cancel()))
            cv_.wait_for(lock, std::chrono::milliseconds(25));
        --waiting_;
        return true;
    }
    Clock::time_point now() {
        std::lock_guard<std::mutex> lock(mutex_);
        return (requested() ? since_ : Clock::now()) - elapsed_;
    }
    class Worker {
    public:
        explicit Worker(SearchPause& owner) : owner_(owner) {
            std::lock_guard<std::mutex> lock(owner_.mutex_); ++owner_.workers_;
        }
        ~Worker() {
            std::lock_guard<std::mutex> lock(owner_.mutex_); --owner_.workers_;
        }
        Worker(const Worker&) = delete;
        Worker& operator=(const Worker&) = delete;
    private:
        SearchPause& owner_;
    };
private:
    void update_locked() {
        const bool next = manual_requested() || automatic_requested();
        if (next == requested()) return;
        if (next) since_ = Clock::now();
        else elapsed_ += Clock::now() - since_;
        requested_.store(next, std::memory_order_release);
        if (!next) cv_.notify_all();
    }
    std::atomic<bool> requested_{false};
    std::atomic<bool> manual_{false}, automatic_{false};
    std::mutex mutex_;
    std::condition_variable cv_;
    const std::atomic<bool>* stop_ = nullptr;
    unsigned workers_ = 0, waiting_ = 0;
    Clock::time_point since_{};
    Clock::duration elapsed_{};
};

inline SearchPause g_pause;
inline SearchPause::Clock::time_point search_now() { return g_pause.now(); }
enum class PauseEvent { none, requested, paused, resumed, power_lost, power_restored };
void pause_start(const std::atomic<bool>* stop);
PauseEvent pause_poll(const std::function<bool(int)>& on_key = {});
