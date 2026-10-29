#pragma once
#include <algorithm>
#include <functional>
#include <thread>
#include <vector>

inline int bounded_worker_threads(int requested) {
    const unsigned available = std::thread::hardware_concurrency();
    const int limit = available ? static_cast<int>(available) : 16;
    return (std::max)(1, (std::min)(requested, limit));
}

// Join already-created workers if a later thread constructor throws.
class ThreadJoinScope {
    std::vector<std::thread>* group_ = nullptr;
    std::thread* single_ = nullptr;
    std::function<void()> stop_;
public:
    ThreadJoinScope(std::vector<std::thread>& group, std::function<void()> stop)
        : group_(&group), stop_(std::move(stop)) {}
    ThreadJoinScope(std::thread& single, std::function<void()> stop)
        : single_(&single), stop_(std::move(stop)) {}
    ~ThreadJoinScope() {
        bool pending = single_ && single_->joinable();
        if (group_) for (auto& thread : *group_) pending |= thread.joinable();
        if (!pending) return;
        stop_();
        if (single_ && single_->joinable()) single_->join();
        if (group_) for (auto& thread : *group_) if (thread.joinable()) thread.join();
    }
    ThreadJoinScope(const ThreadJoinScope&) = delete;
    ThreadJoinScope& operator=(const ThreadJoinScope&) = delete;
};
