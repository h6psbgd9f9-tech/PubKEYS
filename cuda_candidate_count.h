#pragma once
#include <stdint.h>

/* The device count is 64-bit: one legal launch can visit 2^32 keys.
   Equal to capacity means "at least capacity" and requires replay. Never
   cast to the public int before clamping: that hid signed 32-bit overflow. */
static inline int cuda_candidate_count_clamped(uint64_t count,
                                                int device_capacity,
                                                int host_capacity) {
    int cap = device_capacity < host_capacity ? device_capacity : host_capacity;
    if (cap <= 0) return 0;
    return count < (uint64_t)cap ? (int)count : cap;
}
