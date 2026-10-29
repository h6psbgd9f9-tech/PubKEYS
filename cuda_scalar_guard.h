#pragma once
#include <stdint.h>
#include <limits.h>
#include "u128_compat.h"

// Host-side checks for the integer scalar labels used by the CUDA backend.
// Curve addition is modulo n, whereas a truncated byte label wraps modulo
// 2^256. Those are different operations: no prepared center or continuation
// may silently lose a carry, even when the requested tail contains one key.
namespace pkh_cuda_scalar {

inline bool add_mul_fits(const uint8_t first[32], const uint8_t step[32], uint64_t count) {
    if (!first || !step) return false;
    uint64_t carry = 0;
    for (int j = 31; j >= 0; --j) {
        const u128 value = (u128)step[j] * count + carry + first[j];
        carry = (uint64_t)(value >> 8);
    }
    return carry == 0;
}

inline bool mul_fits(const uint8_t step[32], uint64_t count) {
    static const uint8_t zero[32] = {};
    return add_mul_fits(zero, step, count);
}

inline bool mul_u64(uint64_t a, uint64_t b, uint64_t& result) {
    if (b && a > UINT64_MAX / b) return false;
    result = a * b;
    return true;
}

// Match cuda_scan_run's launch sizing. Include ALL prepared thread centers
// and the final advance retained for continuation, not only reported keys.
// Conservatively rejecting unused centers is harmless: large masked steps
// fall back to the exact CPU path, while a unit step has ample headroom below
// 2^256 for every valid secp256k1 interval and the application's launch sizes.
inline bool launch_last_offset(uint64_t keys, int max_blocks, int threads,
                               int half, uint64_t& last_offset) {
    if (!keys || max_blocks <= 0 || threads <= 0 || half <= 0 || half > INT_MAX / 2)
        return false;
    const uint64_t span = (uint64_t)half * 2;
    const uint64_t max_threads = (uint64_t)max_blocks * (uint64_t)threads;
    if (max_threads > INT_MAX) return false;
    const uint64_t batches = keys / span + (keys % span != 0);
    const uint64_t rounds = batches / max_threads + (batches % max_threads != 0);
    uint64_t denominator;
    if (!rounds || !mul_u64(rounds, (uint64_t)threads, denominator)) return false;
    uint64_t blocks = batches / denominator;
    if (blocks < 1) blocks = 1;
    if (blocks > (uint64_t)max_blocks) blocks = (uint64_t)max_blocks;
    const uint64_t used_threads = blocks * (uint64_t)threads;
    uint64_t round_keys, final_end;
    if (rounds == UINT64_MAX || !mul_u64(used_threads, span, round_keys)
        || !mul_u64(rounds + 1, round_keys, final_end) || final_end < (uint64_t)half)
        return false;
    last_offset = final_end - (uint64_t)half;
    return true;
}

inline bool launch_fits(const uint8_t first[32], const uint8_t step[32],
                        uint64_t keys, int max_blocks, int threads, int half) {
    uint64_t last_offset;
    return launch_last_offset(keys, max_blocks, threads, half, last_offset)
        && add_mul_fits(first, step, last_offset);
}
} // namespace pkh_cuda_scalar
