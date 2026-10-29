#pragma once
#include <stdint.h>
// Experimental benchmark API only. Segments are disjoint complete runs; step=1.
// gpu_init: 1 prepares roots on GPU, 0 uses the CPU reference preparation.
// verify_centers compares every GPU center and key against the CPU reference.
extern "C" uint64_t cuda_scan_run_packed(const uint8_t* first_keys, int segments,
    uint64_t keys_per_segment, int gpu_init, int verify_centers,
    uint8_t* out_keys, int max_out, int* found);
// V2: exact unique wave translations from the existing traversal implementation.
extern "C" uint64_t packed_wave_translation_plan(uint8_t* deltas64,uint64_t* ordinals,uint64_t capacity);
extern "C" void packed_wave_start_base(uint64_t start_no,uint8_t* out32);
extern "C" int cuda_scan_set_packed_wave_plan(const uint8_t* deltas64,uint64_t states,uint64_t segment_keys);
// first_state is within the first supplied cycle base. GPU verifies [2^252,n-1].
// expected_starts may be nonnull for exact CPU-original start/center validation.
extern "C" uint64_t cuda_scan_run_packed_wave(const uint8_t* cycle_bases,int base_count,
    uint64_t first_state,int segments,const uint8_t* expected_starts,
    uint8_t* out_keys,int max_out,int* found);
