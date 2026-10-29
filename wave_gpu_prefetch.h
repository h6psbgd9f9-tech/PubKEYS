#pragma once
#include "wave_gpu_batch.h"
#include <functional>
#include <future>
#include <utility>

// One CPU-only look-ahead packet. The producer owns its cursor; only one
// invocation is in flight. Destroying the future joins preparation before
// the cursor/settings can go away. No GPU work or completion is speculative.
class WaveGpuPrefetch {
    std::function<WaveGpuBatch()> prepare_;
    bool enabled_;
    std::future<WaveGpuBatch> next_;
public:
    WaveGpuPrefetch(std::function<WaveGpuBatch()> prepare, bool enabled)
        : prepare_(std::move(prepare)), enabled_(enabled) {}
    WaveGpuPrefetch(const WaveGpuPrefetch&) = delete;
    WaveGpuPrefetch& operator=(const WaveGpuPrefetch&) = delete;
    void take(WaveGpuBatch& batch) {
        batch = next_.valid() ? next_.get() : prepare_();
        if (enabled_ && (!batch.pieces.empty() || !batch.completed.empty())) {
            // Failure to create the helper thread keeps synchronous preparation.
            try { next_ = std::async(std::launch::async, [this] { return prepare_(); }); }
            catch (...) { enabled_ = false; }
        }
    }
};
