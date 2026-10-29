#pragma once
#include "packed_traversal.h"
#include <algorithm>

// A logical wave block can span many launches. Only its final piece puts its
// ID in completed; the caller publishes those IDs after the entire batch succeeds.
// completed_counts is aligned with completed and carries the ORIGINAL logical
// key count, not the count of the last piece. Empty ordinals carry zero.
struct WaveGpuPiece { PackedKey start{}; uint64_t count = 0; };
struct WaveGpuBatch {
    std::vector<WaveGpuPiece> pieces;
    std::vector<PackedKey> full_starts;
    std::vector<ChunkIdx> completed;
    std::vector<uint64_t> completed_counts;
};
class WaveGpuCursor {
    ChunkIdx id_;
    u256 current_{};
    uint64_t left_ = 0;
    uint64_t original_count_ = 0;
public:
    // next(id,key,count) supplies the existing, clipped and deduplicated wave
    // traversal. Allocation happens before claiming any new logical block.
    template<class Next>
    void fill(WaveGpuBatch &batch, size_t capacity, uint64_t piece, Next next) {
        batch.pieces.clear(); batch.full_starts.clear(); batch.completed.clear();
        batch.completed_counts.clear();
        if (!capacity || !piece) return;
        const size_t claim_limit = capacity + 1024;
        batch.pieces.reserve(capacity); batch.full_starts.reserve(capacity);
        batch.completed.reserve(claim_limit + 1);
        batch.completed_counts.reserve(claim_limit + 1);
        size_t claims = 0;
        while (batch.pieces.size() < capacity) {
            if (!left_) {
                if (claims == claim_limit || !next(id_, current_, left_)) break;
                ++claims;
                original_count_ = left_;
                if (!left_) {
                    batch.completed.push_back(id_);
                    batch.completed_counts.push_back(0);
                    continue;
                }
            }
            WaveGpuPiece part;
            part.count = (std::min)(left_, piece);
            u256_to_be32(part.start.data(), current_);
            batch.pieces.push_back(part);
            if (part.count == piece) batch.full_starts.push_back(part.start);
            left_ -= part.count;
            if (left_) {
                u256 following; u256_add_u64(following, current_, part.count);
                u256_set(current_, following);
            } else {
                batch.completed.push_back(id_);
                batch.completed_counts.push_back(original_count_);
            }
        }
    }
};
