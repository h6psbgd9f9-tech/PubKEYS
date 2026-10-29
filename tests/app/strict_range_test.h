#pragma once
#include "search_range.h"
#include "packed_traversal.h"

extern void slow_confirm(const u256 center, int idx, bool plus);
#ifdef USE_SECP
extern void secp_hit(const uint8_t ck[32], int idx, int plus, void *);
#endif

// End-to-end tests use the real confirmation/reporting paths in quiet mode:
// public targets and found scalars stay in memory, no files or notifications.
int strict_range_self_test() {
    const Config saved_cfg = g_cfg;
    const bool saved_quiet = g_quiet;
    const int saved_skip = g_skip_low;
    u256 saved_lo, saved_hi, saved_step;
    u256_set(saved_lo, g_range_start); u256_set(saved_hi, g_range_end);
    u256_set(saved_step, g_step);
    g_quiet = true;
    g_stop.store(false);
    g_cfg.mask_hex.clear(); g_cfg.mask_froz = 0; g_cfg.threads = 1;
    u256 one; u256_from_u64(one, 1); set_scan_step(one);
    int checks = 0, failures = 0;
    auto check = [&](bool ok, const std::string &name) {
        ++checks;
        if (!ok) { ++failures; std::cout << "  [FAIL] strict range: " << name << "\n"; }
    };
    auto hex = [](const u256 key) {
        uint8_t bytes[32]; u256_to_be32(bytes, key); return to_hex(bytes, 32);
    };
    auto targets = [&](const Point &p, int mask) {
        g_db.items.clear(); g_db.n_comp = g_db.n_uncomp = g_db.n_xonly = 0;
        uint8_t bytes[33]; fe_to_be32(bytes + 1, p.x);
        bytes[0] = (uint8_t)(2 | (p.y[0] & 1));
        if (mask & 1) g_db.add_hex(to_hex(bytes, 33));
        bytes[0] ^= 1;
        if (mask & 2) g_db.add_hex(to_hex(bytes, 33));
        g_db.build();
        return push_targets_to_backend();
    };

    struct Case { const char *name; int low, high, digit, pick; bool mirror_inside; };
    const Case cases[] = {
        {"70..71", 70, 71, -1, 0, false},
        {"252..256 upper n-1", 252, 256, -1, 1, false},
        {"full lower 1", 0, 256, -1, 2, true},
        {"252..256 middle", 252, 256, -1, 3, true},
        {"det_hex=8", 252, 256, 8, 3, false},
        {"det_hex=f", 252, 256, 15, 1, false},
    };
    for (const Case &c : cases) {
        g_cfg.range_start_bit = c.low; g_cfg.range_end_bit = c.high;
        g_cfg.det_hex = c.digit; g_cfg.mode = TRAV_RANDOM; compute_range();
        u256 key;
        if (c.pick == 1) u256_sub_u64(key, EC_N, 1);
        else if (c.pick == 2) u256_from_u64(key, 1);
        else if (c.pick == 3) { u256_zero(key); key[3] = 1ULL << 63; key[0] = 4096; }
        else u256_add_u64(key, g_range_start, 4096);
        u256 mirror; u256_sub(mirror, EC_N, key);
        Point p = ec_mul_G(key);
        check(!p.inf && search_key_in_range(key), std::string(c.name) + " test key");
        check(search_key_in_range(mirror)==c.mirror_inside,
              std::string(c.name)+" mirror inside/outside fixture");
        for (int enabled = 0; enabled <= 1; ++enabled) for (int mask = 1; mask <= 3; ++mask) {
            g_cfg.want_nk = enabled != 0;
            if (!targets(p, mask)) { ++failures; continue; }
            check(g_db.confirm_mask(p.x, (int)(p.y[0] & 1)) == mask,
                  std::string(c.name) + " both parity target bits");
            std::set<std::string> expected;
            if (mask & 1) expected.insert(hex(key));
            if ((mask & 2) && enabled) expected.insert(hex(mirror));
            auto run = [&](const char *path, auto body) {
                g_test_found.clear(); g_last_found.clear(); body();
                check(g_test_found == expected, std::string(c.name) + "/" + path
                    + " targets=" + std::to_string(mask) + " mirror=" + std::to_string(enabled));
            };
            run("slow edge", [&] { scan_slow_range(key, 1, false); });
            run("own batch hit", [&] { slow_confirm(key, 0, true); });
#ifdef USE_SECP
            run("secp batch hit", [&] { uint8_t kb[32]; u256_to_be32(kb, key); secp_hit(kb, 0, 1, nullptr); });
#endif
            run("scan chunk", [&] {
                u256 from, last; u256_set(from, key); u256_add_u64(last, from, 1028);
                if (u256_cmp(last, g_range_end) > 0) u256_sub_u64(from, key, 1028);
                scan_chunk(from, 1029);
            });
            run("report guard", [&] {
                if (mask & 1) report_found(key, p.x, p.y, 1);
                if (mask & 2) report_found(key, p.x, p.y, 2);
            });
        }
        // Inject malformed / out-of-range callbacks directly at the final guard.
        u256 bad;
        g_test_found.clear(); g_cfg.want_nk = true;
        u256_zero(bad); report_found(bad, p.x, p.y, 1); report_found(bad, p.x, p.y, 2);
        u256_set(bad, EC_N); report_found(bad, p.x, p.y, 1); report_found(bad, p.x, p.y, 2);
        u256_sub_u64(bad, g_range_start, 1); report_found(bad, p.x, p.y, 1); report_found(bad, p.x, p.y, 2);
        u256_add_u64(bad, g_range_end, 1); report_found(bad, p.x, p.y, 1); report_found(bad, p.x, p.y, 2);
        report_found(key, p.x, p.y, 0); report_found(key, p.x, p.y, 3);
        check(g_test_found.empty(), std::string(c.name) + " final guard invalid keys/kinds");
    }

#ifdef PKH_HAVE_CUDA
    if (g_gpu_active) {
        set_block_nibbles(4);
        for (int mode : {TRAV_ZONE, TRAV_WAVE}) for (int digit : {-1, 8}) {
            g_cfg.mode = mode; g_cfg.det_hex = digit;
            g_cfg.range_start_bit = 252; g_cfg.range_end_bit = 256;
            g_cfg.zone_nibbles = 4;
            g_cfg.wave_start.clear(); compute_range();
            zone_set_seed(917321); wave_set_seed(329717);
            bool ready = false;
            u256 start, key, mirror;
            uint8_t zone_start[32]{};
            // Both finite modes use the same exact-start packed GPU bridge.
            ChunkIdx ordinal;ordinal.from_u64(0);uint64_t count=0;int zone=-1;
            std::mt19937_64 rng(329717);
            ready=next_chunk(ordinal,start,count,zone,rng)&&count>=65536
                &&scan_zone_configure(65536);
            u256_to_be32(zone_start,start);
            check(ready, "packed prepare mode=" + std::to_string(mode));
            if (!ready) continue;
            // Put the target near the tail, to exercise packed offset recovery.
            u256_add_u64(key, start, 65535); u256_sub(mirror, EC_N, key);
            Point p = ec_mul_G(key);
            for (int enabled = 0; enabled <= 1; ++enabled) for (int mask = 1; mask <= 3; ++mask) {
                g_cfg.want_nk = enabled != 0;
                if (!targets(p, mask)) { ++failures; continue; }
                std::set<std::string> expected;
                if (mask & 1) expected.insert(hex(key));
                if ((mask & 2) && enabled) expected.insert(hex(mirror));
                g_test_found.clear();
                const bool done = scan_zone_batch(zone_start, 1, 65536);
                check(done && g_test_found == expected, "packed mode=" + std::to_string(mode)
                    + " digit=" + std::to_string(digit) + " targets=" + std::to_string(mask)
                    + " mirror=" + std::to_string(enabled));
            }
        }
    }
#endif

    g_cfg = saved_cfg; g_quiet = saved_quiet; set_block_nibbles(saved_skip);
    u256_set(g_range_start, saved_lo); u256_set(g_range_end, saved_hi); set_scan_step(saved_step);
    g_test_found.clear(); g_last_found.clear();
    std::cout << "STRICT_RANGE_CHECKS=" << checks << " FAILURES=" << failures << "\n";
    return failures;
}
