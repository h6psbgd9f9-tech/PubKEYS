#pragma once
// Runs before the real targets, step and traversal plan are installed.
// Uses the production kernels and an independent CPU field implementation.
// Always destroys its temporary CUDA state; no test target survives startup.
#include "cuda_scan.h"
#include "packed_scan.h"
#include "secp256k1.h"
#include "bloom.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <vector>

namespace pkh_gpu_check {
using Key = std::array<uint8_t, 32>;

inline Key key_at(const u256 first, uint64_t offset) {
    u256 k; u256_add_u64(k, first, offset);
    Key result{}; fe_to_be32(result.data(), k);
    return result;
}

inline bool same_candidates(const uint8_t* output, int found,
                            std::vector<Key> expected) {
    if (found < 0 || (size_t)found != expected.size()) return false;
    std::vector<Key> actual((size_t)found);
    for (int i = 0; i < found; ++i)
        std::memcpy(actual[(size_t)i].data(), output + (size_t)i * 32, 32);
    std::sort(actual.begin(), actual.end());
    std::sort(expected.begin(), expected.end());
    return actual == expected;
}

inline bool targets(const std::vector<Key>& keys, bool use_bloom, std::string& why) {
    uint8_t xs[8 * 32]{};
    // Deliberately sparse and deterministic: these vectors have no false hits.
    std::vector<uint64_t> bloom(256 * BLOOM_BLOCK_WORDS, 0);
    for (size_t i = 0; i < keys.size(); ++i) {
        u256 k; fe_from_be32(k, keys[i].data());
        const Point p = ec_mul_G(k);
        if (p.inf) { why = "GPU startup check: invalid CPU reference"; return false; }
        fe_to_be32(xs + i * 32, p.x);
        bloom_add(bloom.data(), 255, p.x);
    }
    char reason[256]{};
    if (!cuda_scan_set_targets(use_bloom ? nullptr : xs, (int)keys.size(),
                               use_bloom ? bloom.data() : nullptr,
                               use_bloom ? 255 : 0, reason, sizeof(reason))) {
        why = std::string("GPU startup check: targets: ") + reason;
        return false;
    }
    return true;
}

inline bool run(int device, int threads, std::string& why) {
    why.clear();
    char reason[256]{};
    // Enough temporary threads for one 65536-key packed segment, independent
    // of CUDA_HALF. The normal launch is checked at the production block size.
    const uint64_t block_keys = (uint64_t)threads * CUDA_SPAN;
    if (!block_keys) { why = "GPU startup check: invalid launch size"; return false; }
    const int blocks = (int)std::max<uint64_t>(2, (65536 + block_keys - 1) / block_keys);
    if (!cuda_scan_init(device, blocks, threads, reason, sizeof(reason))) {
        why = std::string("GPU startup check: ") + reason;
        return false;
    }
    struct Cleanup { ~Cleanup() { cuda_scan_shutdown(); } } cleanup;
    const uint64_t total = (uint64_t)blocks * block_keys;
    ec_build_comb();
    u256 first;
    hex_to_u256("C0FFEE000000000000000000000000000000000000000000FFFFFFFFFFFFFF7F", first);
    const uint64_t positions[] = {0, CUDA_HALF, CUDA_SPAN - 1, total - 1,
                                 total, total + CUDA_HALF, total + CUDA_SPAN - 1, 2 * total - 1};
    uint8_t output[64 * 32]{};
    for (uint64_t step_value : {UINT64_C(1), UINT64_C(4096)}) {
        std::vector<Key> keys;
        for (uint64_t position : positions) keys.push_back(key_at(first, position * step_value));
        for (bool use_bloom : {false, true}) {
            u256 s; u256_from_u64(s, step_value);
            uint8_t step[32]; fe_to_be32(step, s);
            if (!cuda_scan_set_step(step, reason, sizeof(reason))) {
                why = std::string("GPU startup check: step: ") + reason; return false;
            }
            if (!targets(keys, use_bloom, why)) return false;
            // Second call must consume the persisted next centers and keys.
            for (int part = 0; part < 2; ++part) {
                const Key start = key_at(first, (uint64_t)part * total * step_value);
                int found = 0;
                const uint64_t checked = cuda_scan_run(start.data(), total, output, 64, &found);
                const std::vector<Key> expected(keys.begin() + part * 4, keys.begin() + (part + 1) * 4);
                uint8_t degenerate[32]{};
                if (checked != total || !same_candidates(output, found, expected)
                    || cuda_scan_take_degenerate(degenerate, 1) != 0) {
                    why = std::string("GPU startup check failed: ") + (use_bloom ? "Bloom" : "direct")
                        + ", step=" + std::to_string(step_value) + ", part=" + std::to_string(part);
                    return false;
                }
            }
        }
    }

    // Wave/zone use a different field implementation and scalar multiplication
    // kernel. Check both generators with a nonzero nibble change and known hits.
    uint8_t unit[32]{}; unit[31] = 1;
    if (!cuda_scan_set_step(unit, reason, sizeof(reason))) {
        why = std::string("GPU startup check: unit step: ") + reason; return false;
    }
    const Key base = key_at(first, 0);
    Key start = base;
    start[29] = (uint8_t)((start[29] & 0xf0) | ((start[29] + 2) & 15));
    u256 packed_first; fe_from_be32(packed_first, start.data());
    std::vector<uint64_t> offsets = {0, CUDA_HALF - 1, CUDA_HALF, CUDA_SPAN - 1,
                                    12345, 32767, 32768, 65535};
    std::sort(offsets.begin(), offsets.end());
    offsets.erase(std::unique(offsets.begin(), offsets.end()), offsets.end());
    std::vector<Key> expected;
    for (uint64_t offset : offsets) expected.push_back(key_at(packed_first, offset));
    uint8_t deltas[64]{}; deltas[59] = 2;
    PackedZoneOp op{59, 65535, 1};
    PackedSegment segment{0, 0};
    Key lo{}; lo[31] = 1;
    u256 upper; u256_sub_u64(upper, EC_N, 1);
    Key hi{}; fe_to_be32(hi.data(), upper);
    for (int mode : {PACKED_SCAN_WAVE, PACKED_SCAN_ZONE}) {
        for (bool use_bloom : {false, true}) {
            if (!targets(expected, use_bloom, why)) return false;
            if (!cuda_scan_packed_configure(mode, deltas, &op, 1, 0, 0, 16,
                                            lo.data(), hi.data(), 65536)) {
                why = "GPU startup check: packed configuration failed"; return false;
            }
            int found = 0; Key sample{};
            const uint64_t checked = cuda_scan_packed_batch(base.data(), 1, &segment, 1,
                sample.data(), output, 64, &found);
            if (checked != 65536 || sample != start || !same_candidates(output, found, expected)) {
                why = std::string("GPU startup check failed: packed ")
                    + (mode == PACKED_SCAN_WAVE ? "wave" : "zone")
                    + (use_bloom ? ", Bloom" : ", direct");
                return false;
            }
        }
    }
    return true;
}
} // namespace pkh_gpu_check
