#include "scan_secp.h"
#include "bloom.h"
#include <cstdio>
#include <cstring>

static void candidate(const uint8_t*, int, int, void* user) { ++*(int*)user; }

int main() {
    if (!scan_secp_init()) return 1;
    void* scan = scan_secp_create();
    if (!scan) return 1;
    uint8_t key[32]{}; key[29] = 1; // 65536, outside degenerate batches.
    uint8_t pub[33]{}, y[32]{};
    bool ok = scan_secp_pubkey(key, pub, y) != 0;
    int hits = 0;
    volatile int stop = 0;
    ok = ok && scan_secp_set_targets(pub + 1, 1, nullptr, 0)
        && scan_secp_run(scan, key, 1, candidate, &hits, &stop) == SECP_SPAN && hits == 1;

    uint8_t bad[64]{};
    std::memcpy(bad, pub + 1, 32);
    std::memset(bad + 32, 255, 32);
    // Invalid input after a valid target must clear even partly loaded state.
    const struct { const uint8_t* x; int n; } cases[] = {
        {pub + 1, 9}, {nullptr, 1}, {pub + 1, -1}, {bad, 2}, {bad + 32, 1}
    };
    for (const auto& c : cases) {
        ok = ok && scan_secp_set_targets(pub + 1, 1, nullptr, 0);
        const int loaded = scan_secp_set_targets(c.x, c.n, nullptr, 0);
        hits = 0;
        const uint64_t checked = scan_secp_run(scan, key, 1, candidate, &hits, &stop);
        ok = ok && !loaded && checked == 0 && hits == 0;
    }
    // Recover normally, then explicitly clear. No abort and no stale targets.
    hits = 0;
    ok = ok && scan_secp_set_targets(pub + 1, 1, nullptr, 0)
        && scan_secp_run(scan, key, 1, candidate, &hits, &stop) == SECP_SPAN && hits == 1
        && scan_secp_set_targets(nullptr, 0, nullptr, 0)
        && scan_secp_run(scan, key, 1, candidate, &hits, &stop) == 0;
    uint64_t bloom[BLOOM_BLOCK_WORDS];
    for (auto& word : bloom) word = UINT64_MAX;
    hits = 0;
    ok = ok && scan_secp_set_targets(nullptr, 9, bloom, 0)
        && scan_secp_run(scan, key, 1, candidate, &hits, &stop) == SECP_SPAN && hits == SECP_SPAN;
    scan_secp_destroy(scan);
    std::printf("%s: CPU target validation, invalid-state rejection, reload and Bloom\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
