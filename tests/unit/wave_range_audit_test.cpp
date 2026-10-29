#include "common.h"
#include "traverse.h"
#include "packed_traversal.h"
#include <algorithm>
#include <array>
#include <map>
#include <numeric>
#include <cstdio>
#include <cstdlib>

// Synthetic scalars only. No search engine, target database, notification or
// persistent checkpoint is used by this audit.
using Key = std::array<uint8_t, 32>;
static uint64_t transitions = 0, packed_ids = 0;
static unsigned cases = 0;
static void require(bool ok, const char *what, uint64_t ordinal = 0) {
    if (!ok) {
        std::printf("FAIL case=%u ordinal=%llu: %s\n", cases,
                    (unsigned long long)ordinal, what);
        std::exit(1);
    }
}
static int digit(const u256 k, int pos) {
    return (int)((k[pos / 16] >> (4 * (pos % 16))) & 15);
}
static void put(u256 k, int pos, int value) {
    const uint64_t mask = UINT64_C(15) << (4 * (pos % 16));
    k[pos / 16] = (k[pos / 16] & ~mask) | ((uint64_t)value << (4 * (pos % 16)));
}
static Key bytes(const u256 k) { Key out{}; u256_to_be32(out.data(), k); return out; }
static uint64_t reference_clip(u256 key, uint64_t count) {
    if (u256_cmp(key, g_range_end) > 0) return 0;
    u256 last; u256_add_u64(last, key, count - 1);
    if (u256_cmp(last, key) < 0) {
        u256 tail; u256_sub(tail, g_range_end, key);
        count = tail[0] + 1;
    } else if (u256_cmp(last, g_range_end) > 0) {
        u256 tail; u256_sub(tail, g_range_end, key);
        count = tail[0] + 1;
    }
    if (u256_cmp(key, g_range_start) < 0) {
        u256 gap; u256_sub(gap, g_range_start, key);
        if (gap[3] || gap[2] || gap[1] || gap[0] >= count) return 0;
        count -= gap[0]; u256_set(key, g_range_start);
    }
    return count;
}
static void scalar_case(int skip, const u256 lo, const u256 hi, const u256 fixed,
                        bool random_start = false) {
    ++cases;
    g_cfg = Config{}; g_cfg.mode = TRAV_WAVE; g_cfg.det_hex = -1;
    g_cfg.tg_token.clear(); g_cfg.tg_chat.clear();
    u256_set(g_range_start, lo); u256_set(g_range_end, hi);
    if (!random_start) g_cfg.wave_start = to_hex(bytes(fixed).data(), 32);
    set_block_nibbles(skip); wave_set_seed(UINT64_C(0x3dc67a18a509bf24));
    int top = 63;
    while (top >= 0 && digit(lo, top) == digit(hi, top)) --top;
    const int width = (std::max)(0, top - skip + 1);
    const int min = top >= 0 ? digit(lo, top) : 0;
    const int radix = top >= 0 ? digit(hi, top) - min + 1 : 1;
    ChunkIdx id; u256 model; WaveInfo initial;
    wave_block_key(id, model, initial);
    for (int i = 0; i < skip; ++i) require(digit(model, i) == 0, "aligned base");
    const bool dedup = u256_cmp(model, lo) >= 0 && u256_cmp(model, hi) <= 0;
    std::map<Key, uint64_t> seen;
    std::mt19937_64 rng(83);
    uint64_t ordinal = 0;
    int phase = 0;
    auto check_state = [&](int pass, int w, bool high, int step, const u256 base) {
        id.from_u64(ordinal); u256 actual; WaveInfo info;
        wave_block_key(id, actual, info);
        require(u256_cmp(model, actual) == 0, "incremental wave model", ordinal);
        require(info.phase == phase && info.step == step && info.pass == pass
                && info.width == w && info.high == high, "phase metadata", ordinal);
        require(u256_cmp(info.base, base) == 0, "phase base", ordinal);
        const auto inserted = seen.emplace(bytes(model), ordinal);
        if (dedup) {
            require(info.duplicate == !inserted.second, "duplicate classification", ordinal);
            require(info.duplicate_of == inserted.first->second, "first duplicate ordinal", ordinal);
        } else require(!info.duplicate, "legacy lower partial base", ordinal);
        u256 expected; u256_set(expected, model);
        const uint64_t expected_count = info.duplicate ? 0 : reference_clip(expected, ZONE_BLOCK);
        uint64_t count = 0; int zone = -1;
        require(next_chunk(id, actual, count, zone, rng), "wave ordinal exists", ordinal);
        require(count == expected_count && (!count || u256_cmp(actual, expected) == 0)
                && zone == phase, "exact interval intersection", ordinal);
        if (count) {
            u256 last; u256_add_u64(last, actual, count - 1);
            require(count <= ZONE_BLOCK && u256_cmp(actual, lo) >= 0
                    && u256_cmp(last, actual) >= 0 && u256_cmp(last, hi) <= 0,
                    "emitted block bounds", ordinal);
        }
        ++transitions;
    };
    u256 phase_base; u256_set(phase_base, model);
    check_state(0, 0, false, 0, phase_base);
    auto walk = [&](int pass, int w, bool high) {
        ++phase; int period = 1;
        const int first = high ? width - w : 0, last = first + w;
        for (int j = first; j < last; ++j)
            period = std::lcm(period, skip + j == top ? radix : 16);
        u256_set(phase_base, model);
        for (int step = 1; step < period; ++step) {
            for (int j = first; j < last; ++j) {
                const int pos = skip + j, m = pos == top ? min : 0;
                const int r = pos == top ? radix : 16;
                put(model, pos, m + (digit(model, pos) - m + 1 + r) % r);
            }
            ++ordinal; check_state(pass, w, high, step, phase_base);
        }
    };
    for (int w = 1; w <= width; ++w) walk(1, w, false);
    for (int w = width - 1; w >= 1; --w) walk(2, w, true);
    for (int w = 2; w <= width; ++w) walk(3, w, true);
    for (int w = width - 1; w >= 1; --w) walk(4, w, false);
    require(ordinal + 1 == wave_states_per_start(), "all legacy states covered");
    // The full 256-bit quotient must affect random starts after 2^64 starts.
    if (random_start) {
        ChunkIdx large; large.b[23] = 1;
        const uint64_t states = wave_states_per_start();
        uint64_t carry = 0;
        for (int i = 31; i >= 0; --i) {
            const uint64_t value = (uint64_t)large.b[i] * states + carry;
            large.b[i] = (uint8_t)value; carry = value >> 8;
        }
        u256 later, again; WaveInfo a, b;
        wave_block_key(large, later, a); wave_block_key(large, again, b);
        require(u256_cmp(later, again) == 0 && a.phase == 0, "256-bit ordinal replay");
        if (width > 16) require(u256_cmp(later, initial.start) != 0, "wide start quotient retained");
    }
}
static void packed_case(int skip, const u256 lo, const u256 hi, const u256 fixed,
                        bool random_start, uint64_t begin) {
    g_cfg = Config{}; g_cfg.mode = TRAV_WAVE; g_cfg.det_hex = -1;
    u256_set(g_range_start, lo); u256_set(g_range_end, hi);
    if (!random_start) g_cfg.wave_start = to_hex(bytes(fixed).data(), 32);
    set_block_nibbles(skip); wave_set_seed(UINT64_C(0x3dc67a18a509bf24));
    PackedTraversalPlan plan;
    if (!packed_traversal_prepare(plan)) return;
    ChunkCounter counter; ChunkIdx first; first.from_u64(begin); counter.set(first);
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        PackedTraversalBatch batch;
        const auto before = counter.get();
        require(packed_traversal_claim(plan, counter, attempt ? 137 : 1, batch), "packed claim");
        require(!batch.pending_ids.empty() && batch.pending_ids.front() == before, "pending first ID");
        if (!batch.fast) continue; // Production replays every pending ID through next_chunk.
        size_t segment = 0, edge = 0;
        for (const auto &id : batch.pending_ids) {
            u256 expected; WaveInfo info; wave_block_key(id, expected, info);
            if (info.duplicate) continue;
            u256 clipped; u256_set(clipped, expected);
            const uint64_t count = reference_clip(clipped, ZONE_BLOCK);
            if (count != ZONE_BLOCK) {
                require(edge < batch.edge_ids.size() && batch.edge_ids[edge++] == id, "edge retained");
                continue;
            }
            require(segment < batch.segments.size(), "packed segment exists");
            const auto &s = batch.segments[segment++]; Key actual = batch.bases[s.base];
            for (int pos = 0; pos < 64; ++pos) {
                const unsigned shift = (pos & 1) ? 0 : 4;
                const int old = (actual[pos / 2] >> shift) & 15;
                const int delta = plan.deltas[(size_t)s.state * 64 + pos];
                const int value = pos == plan.top_pos
                    ? plan.top_min + (old - plan.top_min + delta) % plan.top_radix
                    : (old + delta) % 16;
                actual[pos / 2] = (uint8_t)((actual[pos / 2] & ~(15 << shift)) | (value << shift));
            }
            require(actual == bytes(expected), "packed/scalar key parity");
            ++packed_ids;
        }
        require(segment == batch.segments.size() && edge == batch.edge_ids.size(), "packet exact IDs");
        ChunkCounter replay; replay.set(before); PackedTraversalBatch again;
        require(packed_traversal_claim(plan, replay, attempt ? 137 : 1, again)
                && again.pending_ids == batch.pending_ids && again.bases == batch.bases
                && again.edge_ids == batch.edge_ids && again.segments.size() == batch.segments.size(),
                "discarded prefetch replay");
        for (size_t i = 0; i < batch.segments.size(); ++i)
            require(again.segments[i].base == batch.segments[i].base
                    && again.segments[i].state == batch.segments[i].state, "replayed segment");
    }
}
int main() {
    for (int skip = 2; skip <= 15; ++skip) {
        for (int radix = 2; radix <= 16; ++radix) {
            u256 lo{}, hi{}, fixed{};
            const int top = skip + 2;
            // Same fixed prefix and nonzero low bound; both ends cut through
            // blocks, including blocks above the 64-bit limb boundary.
            put(lo, top + 1, 10); put(hi, top + 1, 10);
            put(lo, top, 16 - radix); put(hi, top, 15);
            put(lo, skip + 1, 3); put(hi, skip + 1, 12);
            lo[0] |= 123; hi[0] |= 171;
            u256_add_u64(fixed, lo, 10);
            scalar_case(skip, lo, hi, fixed);
            if (skip == 3 || skip == 4) {
                packed_case(skip, lo, hi, fixed, false, 0);
                packed_case(skip, lo, hi, fixed, true, 71);
            }
        }
        u256 lo{}, hi{}, fixed{}; lo[0] = 1;
        for (int i = 0; i < 4; ++i) hi[i] = UINT64_MAX;
        hi[3] = UINT64_C(0xefffffffffffffff); // radix 15 stresses lcm(15,16)=240.
        u256_set(fixed, lo); scalar_case(skip, lo, hi, fixed, true);
        if (skip == 3 || skip == 4) packed_case(skip, lo, hi, fixed, true, 0);
        // Fewer significant positions than the block, plus a single scalar.
        u256_zero(lo); u256_zero(hi); lo[0] = 123; hi[0] = 171; u256_set(fixed, lo);
        scalar_case(skip, lo, hi, fixed);
        u256_set(hi, lo); scalar_case(skip, lo, hi, fixed);
    }
    std::printf("PASS: %u cases; %llu scalar states; %llu packed segments; bounds, independent model, metadata, duplicate IDs, 256-bit replay\n",
                cases, (unsigned long long)transitions, (unsigned long long)packed_ids);
}
