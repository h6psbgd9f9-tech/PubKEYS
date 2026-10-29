#pragma once
#include "common.h"
#include "search_range.h"

// A candidate from the current assigned/completed search interval. It is a
// local console sample, not a finding and not necessarily the last comparison.
inline std::string search_status_sample() {
    u256 key;
    {
        std::lock_guard<std::mutex> lock(g_mtx_sample);
        if (!g_sample_valid) return {};
        u256_set(key, g_sample_key);
    }
    if (!search_key_in_range(key)) return {};
    uint8_t bytes[32];
    u256_to_be32(bytes, key);
    return to_hex(bytes, 32);
}
