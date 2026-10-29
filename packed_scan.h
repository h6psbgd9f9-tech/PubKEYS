#pragma once
#include <stdint.h>

struct PackedSegment { uint32_t state,base; };
struct PackedZoneOp { int32_t pos; uint32_t mask,offset; };
enum { PACKED_SCAN_WAVE=1, PACKED_SCAN_ZONE=2 };

// Plan setup does not launch a search kernel. All bounds are inclusive.
extern "C" int cuda_scan_packed_configure(int mode,const uint8_t* deltas64,
    const PackedZoneOp* zone_ops,uint64_t rows,int top_pos,int top_min,int top_radix,
    const uint8_t lo32[32],const uint8_t hi32[32],uint64_t segment_keys);

// One complete packed attempt. Any invalid/clipped block, CUDA error,
// degeneracy or candidate overflow returns zero and found=0 for exact replay.
extern "C" uint64_t cuda_scan_packed_batch(const uint8_t* cycle_bases,int base_count,
    const PackedSegment* segments,int segment_count,uint8_t sample32[32],
    uint8_t* out_keys,int max_out,int* found);
