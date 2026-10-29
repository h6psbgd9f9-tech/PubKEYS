#pragma once
// A is sampled as a whole; every permitted value of B is visited in order.
// No application globals, CUDA or neighbouring project are required.
#include "zone_walk.h"
#include <limits>

namespace pkh_hybrid {
using pkh_zone::be256;
using pkh_zone::be256_cmp;
using pkh_zone::be256_from_u64;
using pkh_zone::be256_add;
using pkh_zone::be256_add_u64;
using pkh_zone::be256_sub;
using pkh_zone::be256_mask;

struct HybridInfo {
    uint64_t ordinal = 0;
    be256 raw{}; // A || 0; clipped block begin may be greater than this value.
};

struct HybridTraversal {
    be256 lo{}, hi{}, seed{}, current{}; // [lo, hi), current = A || 0.
    uint64_t rng[4]{};
    uint64_t sequence = 0, offset = 0;
    int skip = 0;
    bool started = false, exhausted = false;
    // Derived from the interval; not trusted from a checkpoint.
    be256 prefix_first{}, prefix_last{}, prefix_max_offset{};
    int prefix_bits = 0;
    bool single_prefix = false;

    static void shift_right(const be256& key, int nibbles, be256& out) {
        be256 result{};
        const int bytes=nibbles/2, bits=(nibbles&1)*4;
        for(int i=bytes;i<32;++i) {
            const int source=i-bytes;
            unsigned value=unsigned(key.b[source])>>bits;
            if(bits && source>0)value|=unsigned(key.b[source-1])<<(8-bits);
            result.b[i]=static_cast<uint8_t>(value);
        }
        out=result;
    }
    static void shift_left(const be256& key, int nibbles, be256& out) {
        be256 result{};
        const int bytes=nibbles/2, bits=(nibbles&1)*4;
        for(int i=0;i+bytes<32;++i) {
            const int source=i+bytes;
            unsigned value=unsigned(key.b[source])<<bits;
            if(bits && source+1<32)value|=unsigned(key.b[source+1])>>(8-bits);
            result.b[i]=static_cast<uint8_t>(value);
        }
        out=result;
    }
    static bool valid_range(const be256& begin, const be256& end) {
        static const uint8_t order_bytes[32] = {
            0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
            0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xfe,
            0xba,0xae,0xdc,0xe6,0xaf,0x48,0xa0,0x3b,
            0xbf,0xd2,0x5e,0x8c,0xd0,0x36,0x41,0x41
        };
        be256 zero{}, order{}; std::memcpy(order.b, order_bytes, 32);
        return be256_cmp(&begin, &zero) > 0 && be256_cmp(&begin, &end) < 0
            && be256_cmp(&end, &order) <= 0;
    }
    uint64_t block_keys() const { return UINT64_C(1) << (4 * skip); }
    static uint64_t rotate_left(uint64_t x, int n) {
        return (x << n) | (x >> (64 - n));
    }
    uint64_t random_u64() {
        // xoshiro256**; save all words to continue precisely after a restart.
        const uint64_t result = rotate_left(rng[1] * 5, 7) * 9;
        const uint64_t t = rng[1] << 17;
        rng[2] ^= rng[0]; rng[3] ^= rng[1]; rng[1] ^= rng[2]; rng[0] ^= rng[3];
        rng[2] ^= t; rng[3] = rotate_left(rng[3], 45);
        return result;
    }
    void init(int nibbles, const be256& begin, const be256& end,
              const be256& random_seed) {
        *this = HybridTraversal{};
        skip = nibbles < 0 ? 0 : (nibbles > 15 ? 15 : nibbles);
        lo = begin; hi = end; seed = random_seed;
        exhausted = nibbles < 3 || nibbles > 15 || !valid_range(lo, hi);
        if (!exhausted) {
            be256 last = hi; pkh_zone::ZoneTraversal::decrement(last);
            shift_right(lo, skip, prefix_first);
            shift_right(last, skip, prefix_last);
            be256_sub(&prefix_last, &prefix_first, &prefix_max_offset);
            single_prefix = be256_cmp(&prefix_first, &prefix_last) == 0;
            for (int i = 0; i < 32; ++i) if (prefix_max_offset.b[i]) {
                prefix_bits = (31 - i) * 8;
                for (uint8_t value = prefix_max_offset.b[i]; value; value >>= 1) ++prefix_bits;
                break;
            }
        }
        for (int w = 0; w < 4; ++w)
            for (int b = 0; b < 8; ++b) rng[w] = (rng[w] << 8) | seed.b[8 * w + b];
        if (!(rng[0] | rng[1] | rng[2] | rng[3])) rng[0] = UINT64_C(0x9e3779b97f4a7c15);
        for (int i = 0; i < 16; ++i) (void)random_u64();
    }
    bool step() {
        if (exhausted) return false;
        if ((started && single_prefix) || sequence == (std::numeric_limits<uint64_t>::max)()) {
            exhausted = true; return false;
        }
        be256 draw{};
        // Uniform over entire prefixes, including the two clipped edge blocks.
        // Key sampling followed by alignment would underweight those blocks.
        if (prefix_bits) do {
            for (int w = 0; w < 4; ++w) {
                const uint64_t value = random_u64();
                for (int b = 0; b < 8; ++b)
                    draw.b[8 * w + b] = static_cast<uint8_t>(value >> (56 - 8 * b));
            }
            be256_mask(&draw, prefix_bits);
        } while (be256_cmp(&draw, &prefix_max_offset) > 0);
        be256 prefix = prefix_first; be256_add(&prefix, &draw);
        shift_left(prefix, skip, current);
        offset = 0; started = true; ++sequence;
        return true;
    }
    void fill(HybridInfo& info) const {
        info = HybridInfo{}; info.ordinal = sequence ? sequence - 1 : 0; info.raw = current;
    }
    bool next(be256& key, HybridInfo& info) {
        if (!step()) return false;
        key = current; fill(info); return true;
    }
    // offset is measured from clipped begin, never from the aligned prefix.
    uint64_t bounds(be256& begin, be256& end, HybridInfo& info) {
        begin = end = be256{}; info = HybridInfo{};
        if (exhausted || (!started && !step())) return 0;
        fill(info); begin = current; end = current;
        be256_add_u64(&end, block_keys());
        if (be256_cmp(&end, &current) < 0 || be256_cmp(&end, &hi) > 0) end = hi;
        if (be256_cmp(&begin, &lo) < 0) begin = lo;
        if (be256_cmp(&begin, &end) >= 0) return 0;
        be256 length{}; be256_sub(&end, &begin, &length);
        uint64_t count = 0;
        for (int i = 24; i < 32; ++i) count = (count << 8) | length.b[i];
        return count;
    }
    bool advance() { offset = 0; return step(); }
};
} // namespace pkh_hybrid
