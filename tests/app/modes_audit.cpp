// Isolated audit entry point: no menu, live database, single-instance events,
// configuration loading, or notification startup. Run in a scratch directory.
#include "common.h"
#include "traverse.h"
#include "engine.h"
#include "search.h"
#include "search_range.h"
#include "targets.h"
#include "zone_search.h"
#include "hybrid_search.h"
#include "interval_digits.h"
#include "tests/app/selftest.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

using Bytes = std::array<uint8_t, 32>;
static unsigned checks = 0, failures = 0;
static void expect(bool ok, const char* label) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL: %s\n", label); }
}
static ChunkIdx ordinal(uint64_t value) { ChunkIdx x; x.from_u64(value); return x; }
static Bytes bytes(const u256 value) { Bytes b; u256_to_be32(b.data(), value); return b; }
static void reset(int mode, int bits) {
    g_cfg = Config{}; g_cfg.mode = mode; g_cfg.chunk_bits = bits;
    g_cfg.det_hex = -1; g_cfg.mask_hex.clear(); g_cfg.mask_froz = 0;
    g_cfg.tg_token.clear(); g_cfg.tg_chat.clear(); g_cfg.signal_to.clear();
    g_quiet = true; g_stop = false; g_stop_flag = 0;
}

static void finite_modes() {
    uint64_t chunks = 0;
    for (int bits = 10; bits <= 40; ++bits) {
        const uint64_t size = UINT64_C(1) << bits;
        for (uint64_t width : {UINT64_C(1), size - 1, size, size + 1,
                               size * 3 + 17, size * 17 - 1, size * 65 + 13}) {
            for (int location = 0; location < 4; ++location) {
                for (int mode : {TRAV_SEQ, TRAV_KXE}) {
                    reset(mode, bits);
                    if (location == 0) u256_from_u64(g_range_start, 1);
                    if (location == 1) { u256_zero(g_range_start); g_range_start[1] = 1; u256_sub_u64(g_range_start, g_range_start, 11); }
                    if (location == 2) { u256_zero(g_range_start); g_range_start[2] = 1; u256_sub_u64(g_range_start, g_range_start, 11); }
                    if (location == 3) u256_sub_u64(g_range_start, EC_N, width + 1);
                    u256_add_u64(g_range_end, g_range_start, width - 1);
                    compute_total_blocks256();
                    const uint64_t total = (width - 1) / size + 1;
                    expect(std::memcmp(g_total_blocks256, ordinal(total).b, 32) == 0, "finite total blocks");
                    std::vector<std::pair<Bytes, uint64_t>> pieces;
                    uint64_t completed = 0; std::mt19937_64 rng(73);
                    for (uint64_t i = 0; i < total; ++i) {
                        u256 key{}, end{}, accounted{}; uint64_t count = 0; int zone = -1;
                        const bool ready = next_chunk(ordinal(i), key, count, zone, rng);
                        expect(ready && count > 0 && count <= size, "finite valid block");
                        if (!ready || !count) break;
                        u256_add_u64(end, key, count - 1);
                        expect(search_key_in_range(key) && search_key_in_range(end)
                               && u256_cmp(end, key) >= 0, "finite bounds and scalar validity");
                        completed += count;
                        expect(checkpoint_completed_keys(ordinal(i + 1), accounted)
                               && accounted[0] == completed && !(accounted[1] | accounted[2] | accounted[3]),
                               "finite exact completion count including permuted tail");
                        pieces.emplace_back(bytes(key), count); ++chunks;
                    }
                    std::sort(pieces.begin(), pieces.end());
                    u256 expected; u256_set(expected, g_range_start);
                    for (const auto& piece : pieces) {
                        expect(piece.first == bytes(expected), "finite no gaps or overlapping blocks");
                        u256_add_u64(expected, expected, piece.second);
                    }
                    u256 after; u256_add_u64(after, g_range_end, 1);
                    expect(completed == width && u256_cmp(expected, after) == 0, "finite covers entire interval once");
                    u256 key{}; uint64_t count = 0; int zone = 0;
                    expect(!next_chunk(ordinal(total), key, count, zone, rng), "finite stops at exact end");
                }
            }
        }
    }
    // Indices above 2^64 must remain distinct in both finite traversals.
    for (int mode : {TRAV_SEQ, TRAV_KXE}) {
        reset(mode, 10); u256_from_u64(g_range_start, 1); u256_sub_u64(g_range_end, EC_N, 1);
        compute_total_blocks256(); std::mt19937_64 rng(91); std::vector<Bytes> seen;
        for (int bit : {0, 63, 64, 65, 127, 128, 190, 244, 245}) {
            ChunkIdx id{}; id.b[31 - bit / 8] = static_cast<uint8_t>(1u << (bit % 8));
            u256 key{}, end{}; uint64_t count = 0; int zone = 0;
            bool ready = next_chunk(id, key, count, zone, rng);
            expect(ready && count > 0, "finite full 256-bit index accepted");
            if (!ready || !count) continue;
            u256_add_u64(end, key, count - 1);
            expect(search_key_in_range(key) && search_key_in_range(end), "finite large index stays inside range");
            const Bytes b = bytes(key);
            expect(std::find(seen.begin(), seen.end(), b) == seen.end(), "finite distinct large indices");
            seen.push_back(b);
        }
    }
    std::printf("Finite traversal: %llu blocks, chunk_bits=10..40, four scalar locations\n", (unsigned long long)chunks);
}

static void random_mode() {
    const uint64_t samples = 400000;
    reset(TRAV_RANDOM, 10); u256_from_u64(g_range_start, 1); u256_from_u64(g_range_end, 257);
    std::array<uint64_t, 257> histogram{}; std::mt19937_64 rng(0x7C3264B1ULL);
    bool valid = true;
    for (uint64_t i = 0; i < samples; ++i) {
        u256 key{}; uint64_t count = 0; int zone = 0;
        valid &= next_chunk(ordinal(i), key, count, zone, rng) && key[0] >= 1 && key[0] <= 257;
        if (key[0] >= 1 && key[0] <= 257) ++histogram[key[0] - 1];
        valid &= count == 258 - key[0] && !(key[1] | key[2] | key[3]);
    }
    expect(valid, "random non-power-of-two range and exact clipping");
    double chi2 = 0, maximum_z = 0; const double expected = (double)samples / histogram.size();
    for (uint64_t count : histogram) {
        const double z = std::abs((double)count - expected) / std::sqrt(expected);
        chi2 += z*z; maximum_z = (std::max)(maximum_z, z);
    }
    expect(chi2 < 400 && maximum_z < 6, "random start distribution on 257 keys");
    std::printf("Random starts: N=%llu, buckets=257, chi2=%.3f (df=256), max deviation=%.3f sigma\n",
                (unsigned long long)samples, chi2, maximum_z);
    // Full-width draws: every variable nibble must vary and every scan tail
    // must remain valid, including the EC_N upper bound.
    for (auto range : {std::pair<int,int>{0,1}, {0,256}, {70,71}, {252,256}, {255,256}}) {
        reset(TRAV_RANDOM, 40); g_cfg.range_start_bit=range.first; g_cfg.range_end_bit=range.second;
        compute_range(); valid = true; std::array<uint16_t,64> observed{};
        for (uint64_t i=0; i<20000; ++i) {
            u256 key{}, end{}; uint64_t count=0; int zone=0;
            valid &= next_chunk(ordinal(i),key,count,zone,rng) && count>0;
            if (!count) continue;
            u256_add_u64(end,key,count-1);
            valid &= search_key_in_range(key) && search_key_in_range(end) && u256_cmp(end,key)>=0;
            const auto b=bytes(key);
            for(int p=0;p<64;++p)observed[p]|=uint16_t(1u<<interval_digits::digit(b,p));
        }
        expect(valid, "random full-width and narrow scalar bounds");
        const auto low=bytes(g_range_start),high=bytes(g_range_end);
        for(int p=0;p<64;++p)expect(observed[p]==interval_digits::allowed_digits(low,high,p),
                                  "random visits all allowed values at every nibble position");
    }
}

static void mask_mode() {
    for (int frozen=1; frozen<=4; ++frozen) for(uint64_t seed : {UINT64_C(0),UINT64_C(1),UINT64_C(0xABCD)}) {
        reset(TRAV_SEQ,10); g_cfg.mask_froz=frozen;
        u256 key; u256_from_u64(key,seed); const Bytes b=bytes(key); g_cfg.mask_hex=to_hex(b.data(),32);
        u256_from_u64(g_range_start,17); u256_from_u64(g_range_end,131071);
        bool mask_ok=mask_compute(); compute_total_blocks256();
        const uint64_t step=UINT64_C(1)<<(4*frozen);
        std::vector<uint64_t> expected;
        for(uint64_t value=seed; value<=131071; value+=step) if(value>=17) expected.push_back(value);
        expect(mask_ok==!expected.empty(),"mask empty intersection");
        expect(std::memcmp(g_total_blocks256, ordinal((expected.size()+1023)/1024).b, 32)==0,
               "mask total blocks count candidates rather than scalar width");
        std::mt19937_64 rng(17); uint64_t at=0;
        for(uint64_t i=0;i<=expected.size()/1024+1;++i) {
            uint64_t count=0; int zone=0; bool ready=next_chunk(ordinal(i),key,count,zone,rng);
            if(at==expected.size()) {expect(!ready,"mask stops after all candidates");break;}
            expect(ready && count>0 && count<=1024,"mask valid block");
            if(!ready || !count)break;
            for(uint64_t j=0;j<count;++j) {
                expect(at<expected.size() && key[0]+j*step==expected[at],"mask exact independent enumeration"); ++at;
            }
        }
        expect(at==expected.size(),"mask complete candidate coverage");
    }
    // A completely frozen key is one candidate only.
    reset(TRAV_SEQ,10);g_cfg.mask_froz=64;g_cfg.mask_hex=std::string(63,'0')+"1";
    u256_from_u64(g_range_start,1);u256_sub_u64(g_range_end,EC_N,1);
    expect(mask_compute(),"mask fully frozen valid key");compute_total_blocks256();
    u256 key;uint64_t count=0;int zone=0;std::mt19937_64 rng(0);
    expect(next_chunk(ordinal(0),key,count,zone,rng)&&key[0]==1&&count==1,"mask fully frozen emitted once");
    expect(!next_chunk(ordinal(1),key,count,zone,rng),"mask fully frozen exhaustion");
    expect(std::memcmp(g_total_blocks256,ordinal(1).b,32)==0,"mask fully frozen has one total block");
    for(int bit : {255,256-g_cfg.chunk_bits}) {
        ChunkIdx overflow{};overflow.b[31-bit/8]=static_cast<uint8_t>(1u<<(bit%8));
        expect(!chunk_bounds(overflow,key,count),"mask index shift rejects overflow instead of aliasing first block");
        expect(!next_chunk(overflow,key,count,zone,rng),"mask domain rejects overflowing ordinal");
    }
    // ceil((lo-K)/16^63) must not wrap modulo 2^256. Every candidate of
    // this mask is below n-100, so the intersection is empty.
    reset(TRAV_SEQ,10);g_cfg.mask_froz=63;g_cfg.mask_hex=std::string(63,'0')+"1";
    u256_sub_u64(g_range_start,EC_N,100);u256_sub_u64(g_range_end,EC_N,1);
    expect(!mask_compute(),"mask high lower bound does not overflow ceil division");
    compute_total_blocks256();
    expect(!next_chunk(ordinal(0),key,count,zone,rng),"mask empty high interval emits no out-of-range key");
    // Candidate count is 2^64-1: rounding it upward in uint64_t would wrap.
    reset(TRAV_SEQ,10);g_cfg.mask_froz=1;g_cfg.mask_hex=std::string(64,'0');
    g_cfg.range_start_bit=4;g_cfg.range_end_bit=68;compute_range();
    expect(g_mask_total[0]==UINT64_MAX && !(g_mask_total[1]|g_mask_total[2]|g_mask_total[3]),"mask 64-bit candidate count boundary");
    expect(g_total_chunks==(UINT64_C(1)<<54),"mask total chunks ceil does not overflow 64 bits");
}

static void scan_fixture(const u256 start, uint64_t count, bool packed) {
    g_test_found.clear(); g_last_found.clear(); g_count=0;
    g_cfg.want_nk=false; g_stop=false; g_stop_flag=0;
    g_db.items.clear();g_db.n_comp=g_db.n_uncomp=g_db.n_xonly=0;
    std::set<std::string> wanted;
    auto target=[&](const u256 key,bool inside) {
        Point point=ec_mul_G(key); uint8_t pub[33];
        pub[0]=static_cast<uint8_t>(2|(point.y[0]&1));fe_to_be32(pub+1,point.x);
        expect(g_db.add_hex(to_hex(pub,33)),"synthetic public target accepted");
        const auto b=bytes(key);if(inside)wanted.insert(to_hex(b.data(),32));
    };
    u256 key;
    for(uint64_t offset : {UINT64_C(0),count/2,count-1}) {u256_add_u64(key,start,offset);target(key,true);}
    u256_sub_u64(key,start,1);if(search_key_in_range(key))target(key,false);
    u256_add_u64(key,start,count);if(search_key_in_range(key))target(key,false);
    u256_sub_u64(key,g_range_start,1);target(key,false);
    u256_add_u64(key,g_range_end,1);target(key,false);
    g_db.build();expect(push_targets_to_backend(),"synthetic targets uploaded");
    bool scanned=false;
#ifdef PKH_HAVE_CUDA
    if(g_gpu_active && packed && count==4096) {
        const auto b=bytes(start);
        expect(scan_zone_configure(count),"zone/hybrid packed configure");
        scanned=scan_zone_batch(b.data(),1,count);
        expect(scanned,"zone/hybrid real packed scan");
    }
#endif
    if(!scanned)scan_chunk(start,count);
    expect(g_test_found==wanted,"known first/middle/last targets found, neighboring targets excluded");
    expect(g_count==count,"engine exact scanned candidate count");
}

static void known_targets() {
    unsigned cases=0;
    for(int mode : {TRAV_SEQ,TRAV_KXE,TRAV_RANDOM,TRAV_WAVE,TRAV_ZONE,TRAV_HYBRID}) {
        reset(mode,12);g_cfg.threads=1;g_cfg.range_start_bit=70;g_cfg.range_end_bit=71;
        g_cfg.zone_nibbles=g_cfg.hybrid_nibbles=3;set_block_nibbles(3);compute_range();
        u256_add_u64(g_range_start,g_range_start,7);u256_add_u64(g_range_end,g_range_start,131070);
        compute_total_blocks256();compute_top_nibble();compute_zone_plan();
        zone_set_seed(7137);wave_set_seed(7137);std::mt19937_64 rng(7137);
        u256 one;u256_from_u64(one,1);set_scan_step(one);
        if(mode==TRAV_HYBRID) {
            uint8_t lo[32],hi[32],seed[32]{};u256_to_be32(lo,g_range_start);u256_to_be32(hi,g_range_end);seed[31]=91;
            std::string why;
            pkh_hybrid::HybridSearch hybrid;
            expect(hybrid.init(3,lo,hi,seed,"synthetic-engine-audit",why),"hybrid search setup");
            for(unsigned i=0;i<8;++i) {
                u256 key;uint64_t count=0;
                {
                    pkh_hybrid::HybridBatch batch;expect(hybrid.prepare_batch(1,4096,batch,why)&&!batch.pieces.empty(),"hybrid engine batch");
                    if(batch.pieces.empty())break;fe_from_be32(key,batch.pieces[0].key);count=batch.pieces[0].count;
                    scan_fixture(key,count,true);expect(hybrid.commit_batch(batch,why),"hybrid engine commit");
                }
                ++cases;
            }
        } else {
            for(unsigned i=0,produced=0;i<128&&produced<8;++i) {
                u256 key;uint64_t count=0;int zone=0;
                if(!next_chunk(ordinal(i),key,count,zone,rng))break;
                if(!count)continue;scan_fixture(key,count,false);++cases;++produced;
            }
        }
    }
    expect(cases==48,"all six search modes exercised by actual scan engine");
    std::printf("Known-target engine audit: %u cases, all six modes, first/middle/last + excluded neighbors\n",cases);
}

int main(int argc,char** argv) {
    init_colors();g_cfg=Config{};g_quiet=true;build_table();
    const std::string option=argc>1?argv[1]:"--audit";
#ifdef PKH_HAVE_CUDA
    if(option=="--gpu-strict" || option=="--gpu-known") {
        char why[256]{};
        if(!cuda_scan_init(0,4,32,why,sizeof why)) {std::printf("FAIL CUDA init: %s\n",why);return 1;}
        cuda_scan_layout(&g_gpu_blocks,&g_gpu_threads,&g_gpu_vram);g_gpu_active=true;g_gpu_power_pct=100;
        if(option=="--gpu-strict") {const int result=strict_range_self_test();gpu_backend_stop();return result;}
        known_targets();gpu_backend_stop();
        std::printf("GPU known-target checks=%u failures=%u\n",checks,failures);return failures?1:0;
    }
#endif
    if(option=="--selftest") return self_test();
    if(option=="--range") return range_self_test();
    if(option=="--strict") return strict_range_self_test();
    if(option=="--known") {known_targets();std::printf("CPU known-target checks=%u failures=%u\n",checks,failures);return failures?1:0;}
    finite_modes();random_mode();mask_mode();
    std::printf("Modes audit: checks=%u failures=%u\n",checks,failures);
    return failures?1:0;
}
