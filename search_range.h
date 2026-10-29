#pragma once
#include "common.h"

// These are the effective inclusive bounds, already narrowed by det_hex.
// Used only when confirming a rare X hit, never in the GPU hot loop.
inline bool search_key_is_valid(const u256 key) {
    return (key[0] || key[1] || key[2] || key[3])
        && u256_cmp(key, EC_N) < 0;
}

inline bool search_key_in_range(const u256 key) {
    return search_key_is_valid(key)
        && u256_cmp(key, g_range_start) >= 0
        && u256_cmp(key, g_range_end) <= 0;
}

inline bool search_match_allowed(const u256 key, int kind) {
    if (!search_key_in_range(key)) return false;
    if (kind == 1) return true;
    if (kind != 2 || !g_cfg.want_nk) return false;
    u256 mirror;
    u256_sub(mirror, EC_N, key);
    // Only the generated source k is restricted to the chosen interval.
    // Its valid opposite point may match a target anywhere in the scalar domain.
    return search_key_is_valid(mirror);
}
