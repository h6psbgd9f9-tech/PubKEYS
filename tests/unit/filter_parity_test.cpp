// Standalone CPU audit. Build both with and without -DPKH_BLOOM_MUL32=1.
#include "bloom.h"
#include "cuda_candidate_count.h"
#include <cstdio>
#include <cstdint>
#include <vector>

static uint64_t next64(uint64_t &s) {
    s += UINT64_C(0x9e3779b97f4a7c15);
    uint64_t z = s;
    z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
    return z ^ (z >> 31);
}
static unsigned bit64(const uint64_t x[4], int j) {
    uint64_t v = bloom_src(x, j) ^ ((uint64_t)j * UINT64_C(0x9e3779b97f4a7c15));
    return (unsigned)(((v * bloom_mul(j)) >> 20) & 511U);
}
static unsigned bit32(const uint64_t x[4], int j) {
    uint32_t v = (uint32_t)bloom_src(x, j) ^ ((uint32_t)j * UINT32_C(0x7f4a7c15));
    return (unsigned)(((v * (uint32_t)bloom_mul(j)) >> 20) & 511U);
}
static bool check_bits(const uint64_t x[4]) {
    for (int j = 0; j < BLOOM_HASHES; ++j)
        if (bit64(x, j) != bit32(x, j) || bloom_bit(x, j) != bit64(x, j)) return false;
    return true;
}
static void add64(uint64_t *data, uint64_t mask, const uint64_t x[4]) {
    uint64_t *p = data + bloom_block_of(x, mask) * BLOOM_BLOCK_WORDS;
    for (int j = 0; j < BLOOM_HASHES; ++j) {
        unsigned b = bit64(x, j);
        p[b >> 6] |= UINT64_C(1) << (b & 63);
    }
}
static int maybe64(const uint64_t *data, uint64_t mask, const uint64_t x[4]) {
    const uint64_t *p = data + bloom_block_of(x, mask) * BLOOM_BLOCK_WORDS;
    for (int j = 0; j < BLOOM_HASHES; ++j) {
        unsigned b = bit64(x, j);
        if (!(p[b >> 6] & (UINT64_C(1) << (b & 63)))) return 0;
    }
    return 1;
}
int main() {
    const uint64_t counts[] = {0, 1, 63, 64, 65, 65535, 65536, 65537,
        UINT64_C(0x7fffffff), UINT64_C(0x80000000), UINT64_C(0xffffffff),
        UINT64_C(0x100000000), UINT64_MAX};
    for (int cap : {64, 65536}) for (uint64_t n : counts) {
        int want = n < (uint64_t)cap ? (int)n : cap;
        if (cuda_candidate_count_clamped(n, 65536, cap) != want ||
            cuda_candidate_count_clamped(n, cap, 65536) != want) {
            std::printf("FAIL count boundary %llu cap %d\n", (unsigned long long)n, cap);
            return 1;
        }
    }
    if (cuda_candidate_count_clamped(UINT64_MAX, 65536, 0) != 0 ||
        cuda_candidate_count_clamped(UINT64_MAX, -1, 65536) != 0) return 1;

    uint64_t checked = 0, x[4] = {};
    for (int bit = 0; bit < 256; ++bit) {
        for (int i = 0; i < 4; ++i) x[i] = 0;
        x[bit / 64] = UINT64_C(1) << (bit % 64);
        if (!check_bits(x)) return 1;
        for (int i = 0; i < 4; ++i) x[i] = ~x[i];
        if (!check_bits(x)) return 1;
        checked += 2;
    }
    for (uint64_t v : {UINT64_C(0), UINT64_C(1), UINT64_MAX,
                       UINT64_C(0xffffffff), UINT64_C(0x100000000),
                       UINT64_C(0xaaaaaaaaaaaaaaaa), UINT64_C(0x5555555555555555)}) {
        for (int i = 0; i < 4; ++i) x[i] = v;
        if (!check_bits(x)) return 1;
        ++checked;
    }

    const uint64_t insert_count = 100000, probes = 2000000;
    const uint64_t blocks = bloom_blocks_for(insert_count), mask = blocks - 1;
    std::vector<uint64_t> reference(blocks * BLOOM_BLOCK_WORDS, 0);
    std::vector<uint64_t> actual(reference.size(), 0);
    uint64_t seed = UINT64_C(0x813928317382);
    const uint64_t initial_seed = seed;
    for (uint64_t i = 0; i < insert_count; ++i) {
        for (int j = 0; j < 4; ++j) x[j] = next64(seed);
        add64(reference.data(), mask, x);
        bloom_add(actual.data(), mask, x);
    }
    if (reference != actual) { std::puts("FAIL filter bitmap parity"); return 1; }
    seed = initial_seed;
    for (uint64_t i = 0; i < insert_count; ++i) {
        for (int j = 0; j < 4; ++j) x[j] = next64(seed);
        if (!bloom_maybe(actual.data(), mask, x)) {
            std::puts("FAIL false negative among inserted targets"); return 1;
        }
    }
    uint64_t false_positives = 0;
    for (uint64_t i = 0; i < probes; ++i) {
        for (int j = 0; j < 4; ++j) x[j] = next64(seed);
        if (!check_bits(x)) { std::puts("FAIL multiply parity"); return 1; }
        int expected = maybe64(reference.data(), mask, x);
        if (bloom_maybe(actual.data(), mask, x) != expected) {
            std::puts("FAIL filter query parity"); return 1;
        }
        false_positives += expected;
        ++checked;
    }
    std::printf("PASS PKH_BLOOM_MUL32=%d: %llu vectors x 8 hashes; 100000 inserted targets, "
                "0 false negatives; 2000000 queries, %llu identical false positives; "
                "signed32/unsigned32/uint64 counter boundaries pass\n", PKH_BLOOM_MUL32,
                (unsigned long long)checked, (unsigned long long)false_positives);
    return 0;
}
