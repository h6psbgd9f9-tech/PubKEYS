#pragma once

#include "traverse.h"
#include "packed_scan.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

using PackedKey = std::array<uint8_t, 32>;

struct PackedTraversalPlan {
    int mode = 0;
    int skip_low = 0;
    int top_pos = -1;
    int top_min = 0;
    int top_radix = 0;
    uint64_t legacy_states = 0;
    std::vector<uint8_t> deltas;             // unique wave rows, 64 bytes each
    std::vector<PackedZoneOp> zone_ops;      // nominal zone ordinal -> operation
    std::vector<uint32_t> row_by_ordinal;    // UINT32_MAX denotes a duplicate
    std::vector<int> display_phase;         // nominal ordinal -> phase / zone
    PackedKey lo{}, hi{};
};

struct PackedTraversalBatch {
    std::vector<ChunkIdx> pending_ids;       // every claimed ID, including duplicates
    std::vector<PackedSegment> segments;     // state row / operation and base index
    std::vector<PackedKey> bases;
    std::vector<ChunkIdx> edge_ids;         // clipped blocks excluded from GPU packet
    ChunkIdx sample_id;
    PackedKey sample_key{};
    int sample_zone = -1;
    bool fast = true;
};

// Configuration, seeds and range must remain fixed while a prepared plan is used.
// Preparation never changes the traversal counter or completion watermark.
bool packed_traversal_prepare(PackedTraversalPlan &plan);

// Claims IDs only; the caller commits pending_ids after successful work. When
// fast is false, replay all pending_ids through the existing scalar traversal.
// A fast empty batch is valid: it consists entirely of retained duplicate IDs.
bool packed_traversal_claim(const PackedTraversalPlan &plan, ChunkCounter &counter,
                            size_t wanted, PackedTraversalBatch &batch);
