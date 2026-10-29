#pragma once
#include "hybrid_walk.h"
#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

namespace pkh_hybrid {
struct HybridPiece {
    uint8_t key[32]{};
    uint64_t count = 0;
    HybridInfo info;
};
struct HybridBatch {
    std::vector<HybridPiece> pieces;
    std::vector<uint8_t> full_starts; // Full pieces, ready for the GPU upload.
    uint64_t keys = 0, version = 0;
    HybridTraversal next;
    HybridBatch state_only() const {
        HybridBatch result; result.next=next; result.version=version; result.owner_=owner_; result.generation_=generation_; return result;
    }
private:
    const void* owner_ = nullptr;
    uint64_t generation_=0;
    friend class HybridSearch;
};
struct HybridStatus {
    bool initialized = false, started = false, exhausted = false, single_prefix = false;
    uint64_t sequence = 0, offset = 0, block_keys = 0, version = 0;
    int skip = 0;
    uint8_t current[32]{};
};

// Preparing is speculative: dropping a failed/partial batch changes nothing.
// Commit only after all scans, confirmations and local finding saves succeed.
class HybridSearch {
public:
    bool init(int nibbles, const uint8_t low_inclusive[32],
              const uint8_t high_inclusive[32], const uint8_t seed[32],
              const std::string& identity, std::string& why);
    bool prepare_batch(size_t max_pieces, uint64_t piece_keys,
                       HybridBatch& batch, std::string& why) const;
    bool commit_batch(const HybridBatch& batch, std::string& why);
    bool prepare_after(const HybridBatch& previous, size_t max_pieces, uint64_t piece_keys,
                       HybridBatch& batch, std::string& why) const;
    HybridStatus status() const;
    void reset();
    std::string checkpoint_path() const;
    bool save(const std::string& path, std::string& why) const;
    bool load(const std::string& path, std::string& why);
private:
    static bool fill_batch(size_t max_pieces, uint64_t piece_keys, HybridBatch& batch, std::string& why);
    mutable std::mutex state_mutex_, io_mutex_;
    HybridTraversal initial_, committed_;
    std::string identity_hash_;
    uint64_t version_ = 0;
    uint64_t generation_ = 0;
    bool initialized_ = false;
    mutable std::string rejected_path_;
};
} // namespace pkh_hybrid
