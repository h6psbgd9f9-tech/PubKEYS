#pragma once
#include "hash.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>

// Streaming digest: checkpoint size must not determine temporary RAM usage.
class CheckpointDigest {
    sha256d::Ctx state_;
    uint8_t pending_[64]{};
    size_t used_ = 0;
    uint64_t bytes_ = 0;
public:
    CheckpointDigest() { state_.init(); }
    void add(const void* input, size_t size) {
        const auto* data = static_cast<const uint8_t*>(input);
        bytes_ += static_cast<uint64_t>(size);
        while (size) {
            const size_t take = (std::min)(size, sizeof pending_ - used_);
            std::memcpy(pending_ + used_, data, take);
            used_ += take; data += take; size -= take;
            if (used_ == sizeof pending_) { state_.block(pending_); used_ = 0; }
        }
    }
    void finish(uint8_t out[32]) {
        uint8_t tail[128]{};
        std::memcpy(tail, pending_, used_); tail[used_] = 0x80;
        const size_t size = used_ < 56 ? 64 : 128;
        const uint64_t bits = bytes_ * 8;
        for (int i = 0; i < 8; ++i) tail[size - 1 - i] = uint8_t(bits >> (8 * i));
        state_.block(tail); if (size == 128) state_.block(tail + 64);
        for (int i = 0; i < 8; ++i)
            for (int j = 0; j < 4; ++j) out[i * 4 + j] = uint8_t(state_.h[i] >> (24 - 8 * j));
    }
};

inline bool preserve_rejected_checkpoint(const std::string& path,
                                         std::string& saved, std::string& why) {
    saved.clear();
    std::error_code error;
    const auto source = std::filesystem::u8path(path);
    const bool exists = std::filesystem::exists(source, error);
    if (error) { why = "Cannot inspect rejected checkpoint: " + error.message(); return false; }
    if (!exists) return true;
    static std::atomic<uint64_t> serial{0};
    const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
    for (;;) {
        const std::string target = path + ".rejected_" + std::to_string(stamp)
            + "_" + std::to_string(serial.fetch_add(1));
        if (std::filesystem::exists(std::filesystem::u8path(target), error)) continue;
        if (!error) std::filesystem::rename(source, std::filesystem::u8path(target), error);
        if (error) { why = "Cannot preserve rejected checkpoint; overwrite blocked: " + error.message(); return false; }
        saved = target; return true;
    }
}
