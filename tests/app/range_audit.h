#ifndef PKH_RANGE_AUDIT_H
#define PKH_RANGE_AUDIT_H
#include "interval_digits.h"
#include <algorithm>

int range_self_test() {
    const Config saved=g_cfg;
    const int saved_skip=g_skip_low;
    const bool saved_quiet=g_quiet;
    const uint64_t saved_seed=wave_get_seed();
    g_quiet=true;
    int failures=0, checked=0;
    g_cfg=Config{}; g_cfg.det_hex=-1; g_cfg.chunk_bits=12; g_cfg.zone_nibbles=g_cfg.wave_nibbles=3;
    g_cfg.mode=TRAV_SEQ; g_cfg.mask_hex.clear(); g_cfg.mask_froz=0;
    auto expect=[&](bool ok,const char* name) {
        std::cout << (ok ? "[OK] " : "[FAIL] ") << name << "\n";
        if (!ok) ++failures;
    };
    bool bounds_ok=true, digits_ok=true;
    for (int upper=1;upper<=256;++upper) for (int lower=0;lower<upper;++lower) {
        g_cfg.range_start_bit=lower; g_cfg.range_end_bit=upper; g_cfg.det_hex=-1;
        compute_range();
        u256 low={},high={}; low[lower/64]=1ULL<<(lower%64);
        for(int bit=0;bit<upper;++bit) high[bit/64]|=1ULL<<(bit%64);
        u256 cap; u256_sub_u64(cap,EC_N,1);
        if(u256_cmp(high,cap)>0) u256_set(high,cap);
        bounds_ok &= u256_cmp(low,g_range_start)==0 && u256_cmp(high,g_range_end)==0;
        const int top=(upper-1)/4;
        const int first=(int)((low[top/16]>>(4*(top%16)))&15);
        const int last=(int)((high[top/16]>>(4*(top%16)))&15);
        digits_ok &= g_top_ni==63-top && g_top_vmin==first && g_top_vmax==last;
        ++checked;
    }
    expect(bounds_ok,"all 32896 bit intervals have exact inclusive scalar bounds");
    expect(digits_ok,"all 32896 intervals have correct first HEX position and allowed top digits");
    struct Interval {int low,high;};
    for(const auto c : {Interval{0,1},Interval{0,12},Interval{8,10},Interval{66,67},Interval{70,71},Interval{252,256},Interval{255,256},Interval{0,256}}) {
        for(int mode : {TRAV_RANDOM,TRAV_SEQ,TRAV_ZONE,TRAV_KXE,TRAV_WAVE}) {
            g_cfg.range_start_bit=c.low; g_cfg.range_end_bit=c.high; g_cfg.mode=mode;
            g_cfg.det_hex=-1; g_cfg.wave_start.clear(); set_block_nibbles(3); compute_range();
            std::mt19937_64 rng(7137); zone_set_seed(7137); wave_set_seed(7137);
            bool ok=true;
            for(uint64_t i=0;i<128;++i) {
                ChunkIdx index; index.from_u64(i); u256 key; uint64_t count=0; int zone=0;
                if(!next_chunk(index,key,count,zone,rng) || !count) continue;
                u256 last; u256_add_u64(last,key,count-1);
                ok &= u256_cmp(key,g_range_start)>=0 && u256_cmp(last,g_range_end)<=0 && u256_cmp(last,key)>=0;
            }
            char label[128]; snprintf(label,sizeof(label),"mode %d: every sampled chunk inside %d..%d",mode,c.low,c.high);
            expect(ok,label);
        }
    }
    for(const auto c : {Interval{70,71},Interval{252,256}}) for(int skip : {3,8}) {
        g_cfg.range_start_bit=c.low; g_cfg.range_end_bit=c.high; g_cfg.mode=TRAV_WAVE; g_cfg.det_hex=-1;
        set_block_nibbles(skip); compute_range();
        uint8_t low[32]; u256_to_be32(low,g_range_start); g_cfg.wave_start=to_hex(low,32);
        uint8_t digest[32]={}; bool ok=true; int top_seen=0;
        const uint64_t states=wave_states_per_start();
        for(uint64_t i=0;i<states;++i) {
            ChunkIdx index; index.from_u64(i); u256 key; WaveInfo info; wave_block_key(index,key,info);
            uint8_t buf[64]; memcpy(buf,digest,32); u256_to_be32(buf+32,key); sha256(buf,64,digest);
            if(c.low==70) {
                const int digit=buf[32+23]>>4;
                if(digit>=4 && digit<=7) top_seen|=1<<(digit-4); else ok=false;
                ok &= info.top==17 && u256_cmp(key,g_range_start)>=0 && u256_cmp(key,g_range_end)<=0;
            }
        }
        if(c.low==70) expect(ok && top_seen==15,"wave visits all 18 puzzle positions, top digit only 4..7");
        std::cout << "WAVE_VECTOR " << c.low << " " << c.high << " " << skip << " " << states << " " << to_hex(digest,32) << "\n";
    }
    // Independently verify the one-digit transition, raw alignment, and exact
    // clipping, including narrow intervals where N must shrink below three.
    for (const auto c : {Interval{0,1},Interval{0,4},Interval{0,16},Interval{8,16},
                         Interval{65,71},Interval{70,71},Interval{252,256},Interval{1,256}})
      for (int skip : {3,8}) {
        g_cfg.range_start_bit=c.low; g_cfg.range_end_bit=c.high;
        g_cfg.mode=TRAV_ZONE; g_cfg.det_hex=-1; g_cfg.wave_start.clear();
        g_cfg.zone_nibbles=skip; compute_range(); zone_set_seed(8675309);
        // Explicit answers for this fixture table: 1 or 15 keys require
        // scalar blocks; both 16-bit cases retain exactly three low digits.
        const int expected_skip=(c.high==1||c.high==4)?0:
            (c.high==16&&skip==8)?3:skip;
        const uint64_t expected_block=UINT64_C(1)<<(4*expected_skip);
        bool step_ok=true, block_ok=true;
        int top_seen=0, produced=0;
        u256 previous{};
        std::vector<std::string> replay_keys;
        for(uint64_t ordinal=0;ordinal<1024;++ordinal) {
            ChunkIdx index; index.from_u64(ordinal);
            u256 key; ZoneInfo info; zone_block_key(index,key,info);
            if (!info.valid) break;
            ++produced;
            uint8_t kb[32],pb[32];
            u256_to_be32(kb,info.raw); u256_to_be32(pb,info.base);
            auto digit=[](const uint8_t b[32],int pos) {return (b[pos/2] >> ((pos&1)?0:4)) & 15;};
            for(int pos=64-info.skip;pos<64;++pos) step_ok &= digit(kb,pos)==0;
            if(ordinal) {
                int changed=0,position=-1;
                for(int pos=0;pos<64;++pos) if(digit(kb,pos)!=digit(pb,pos)) {++changed;position=pos;}
                step_ok &= changed==1 && position==info.pos && position<64-info.skip
                        && u256_cmp(info.base,previous)==0;
            } else step_ok &= info.pos==-1;
            if(c.low==70 && c.high==71) {
                const int top=digit(kb,46);
                step_ok &= top>=4 && top<=7;
                if(top>=4 && top<=7) top_seen |= 1<<(top-4);
            }
            u256_set(previous,info.raw);
            replay_keys.push_back(to_hex(kb,32));
            u256 raw_end; u256_add_u64(raw_end,info.raw,info.block_keys-1);
            u256 elow,ehigh;
            u256_set(elow,u256_cmp(info.raw,g_range_start)<0 ? g_range_start : info.raw);
            u256_set(ehigh,(u256_cmp(raw_end,info.raw)<0 || u256_cmp(raw_end,g_range_end)>0)
                           ? g_range_end : raw_end);
            u256 width;u256_sub(width,ehigh,elow);
            block_ok &= u256_cmp(key,elow)==0 && width[1]==0 && width[2]==0 && width[3]==0
                     && info.count==width[0]+1 && info.skip==expected_skip
                     && info.block_keys==expected_block
                     && u256_cmp(key,g_range_start)>=0 && u256_cmp(ehigh,g_range_end)<=0;
            // This retained transition chain is a reference adapter. The
            // production next_chunk now has independent balanced buckets.
        }
        zone_set_seed(8675309);
        for(size_t i=0;i<replay_keys.size();++i) {
            ChunkIdx index;index.from_u64(i);u256 key;ZoneInfo info;zone_block_key(index,key,info);
            uint8_t kb[32];u256_to_be32(kb,info.raw);
            step_ok &= info.valid && to_hex(kb,32)==replay_keys[i];
        }
        if(c.high==1) {
            ChunkIdx index;index.from_u64(1);u256 key;ZoneInfo ended;zone_block_key(index,key,ended);
            step_ok &= produced==1 && !ended.valid;
        }
        char label[192];snprintf(label,sizeof label,
            "zone reference: one allowed digit, clipping and reproducible chain: %d..%d, N=%d",
            c.low,c.high,skip);
        expect(step_ok&&block_ok&&produced>0,label);
        if(c.low==70&&c.high==71)
            expect(top_seen==15,"zone reaches all allowed symbols 4..7 of position 18");
      }
    {
        g_cfg.range_start_bit=70;g_cfg.range_end_bit=71;g_cfg.det_hex=4;
        g_cfg.mode=TRAV_ZONE;g_cfg.zone_nibbles=3;compute_range();zone_set_seed(8675309);
        bool fixed=true;
        for(uint64_t i=0;i<1024;++i) {
            ChunkIdx index;index.from_u64(i);u256 key;ZoneInfo info;zone_block_key(index,key,info);
            fixed &= info.valid && ((key[1]>>4)&15)==4;
        }
        expect(fixed&&g_zone_count==14,"zone keeps fixed top digit, 14 positions above three-digit block remain free");
        ChunkIdx too_wide;too_wide.from_u64(0);too_wide.b[23]=1;
        u256 key;ZoneInfo info;zone_block_key(too_wide,key,info);
        expect(!info.valid,"zone reference does not truncate a sequence larger than 64 bits");
    }
    // Production modes: every admitted scalar in a bounded interval appears
    // exactly once, even when a whole logical block is larger than a bucket.
    for(int mode:{TRAV_WAVE,TRAV_ZONE}) for(const auto c:{Interval{0,1},Interval{0,4},Interval{0,16},Interval{8,16},Interval{12,20}}) {
        g_cfg.mode=mode;g_cfg.wave_start.clear();g_cfg.range_start_bit=c.low;g_cfg.range_end_bit=c.high;
        g_cfg.det_hex=-1;g_cfg.zone_nibbles=g_cfg.wave_nibbles=3;set_block_nibbles(3);wave_set_seed(7137);compute_range();
        bool ok=traversal_full_coverage()&&mode_has_checkpoint();uint64_t total=0;
        for(int i=0;i<24;++i)ok&=g_total_blocks256[i]==0;
        for(int i=24;i<32;++i)total=(total<<8)|g_total_blocks256[i];
        ok&=total>0&&total<=1048576;
        std::vector<std::pair<uint64_t,uint64_t>> windows;
        std::mt19937_64 rng(7137);
        for(uint64_t i=0;ok&&i<total;++i){
            ChunkIdx index;index.from_u64(i);u256 key;uint64_t count=0;int phase=-1;
            if(!next_chunk(index,key,count,phase,rng)){ok=false;break;}
            if(!count)continue;
            u256 last;u256_add_u64(last,key,count-1);
            ok&=u256_cmp(key,g_range_start)>=0&&u256_cmp(last,g_range_end)<=0&&u256_cmp(last,key)>=0;
            windows.emplace_back(key[0],last[0]);
        }
        std::sort(windows.begin(),windows.end());uint64_t next=g_range_start[0];
        for(const auto& window:windows){ok&=window.first==next;next=window.second+1;}
        ok&=!windows.empty()&&next==g_range_end[0]+1;
        ChunkIdx end;std::memcpy(end.b,g_total_blocks256,32);u256 unused;uint64_t count=9;int phase=-1;
        ok&=!next_chunk(end,unused,count,phase,rng)&&count==0;
        char label[128];snprintf(label,sizeof label,"production mode %d exact complete union without repeats: %d..%d",mode,c.low,c.high);
        expect(ok,label);
    }
    // A fixed wave start is exact. The prefix before its shifted block is
    // emitted by the complement, rather than silently omitted or revisited.
    {
        g_cfg.range_start_bit=0;g_cfg.range_end_bit=4;g_cfg.det_hex=-1;g_cfg.mode=TRAV_WAVE;
        g_cfg.wave_start=std::string(63,'0')+"f";set_block_nibbles(3);compute_range();
        ChunkIdx index;index.from_u64(0);u256 key;uint64_t count=0;int zone=0;std::mt19937_64 rng(1);
        bool ok=next_chunk(index,key,count,zone,rng)&&key[0]==15&&count==1;
        bool seen[16]{};unsigned visited=0;uint64_t total=0;
        for(int i=0;i<24;++i)ok&=g_total_blocks256[i]==0;
        for(int i=24;i<32;++i)total=(total<<8)|g_total_blocks256[i];
        ok&=total>0&&total<1024;
        for(uint64_t i=0;ok&&i<total;++i){
            index.from_u64(i);count=0;
            if(!next_chunk(index,key,count,zone,rng)){ok=false;break;}
            for(uint64_t j=0;j<count;++j){const uint64_t value=key[0]+j;
                if(value<1||value>15||seen[value]){ok=false;break;}
                seen[value]=true;++visited;
            }
        }
        expect(ok&&visited==15,"fixed start f is exact; complement covers 1..e once");
        const std::string initial_checkpoint = checkpoint_path();
        expect(mode_has_checkpoint()&&initial_checkpoint.rfind("checkpoint_wave_balanced_v1_",0)==0,
               "fixed wave uses new versioned balanced checkpoint path");
        set_block_nibbles(4);
        expect(checkpoint_path()!=initial_checkpoint,"changing wave block size changes checkpoint identity");
        set_block_nibbles(3);
        g_cfg.wave_start = std::string(63, '0') + "e";
        expect(checkpoint_path()!=initial_checkpoint,"changing fixed wave start changes checkpoint identity");
        g_cfg.wave_start = std::string(63, '0') + "f";
        expect(checkpoint_path() == initial_checkpoint,
               "shape checkpoint identity is stable when parameters are restored");
    }
    g_cfg=saved; wave_set_seed(saved_seed);set_block_nibbles(saved_skip); compute_range();g_quiet=saved_quiet;
    std::cout << "Range audit: " << checked << " intervals, failures=" << failures << "\n";
    return failures ? 1 : 0;
}
#endif
