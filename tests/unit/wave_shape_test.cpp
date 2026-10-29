#include "common.h"
#include "traverse.h"
#include <array>
#include <map>
#include <numeric>
#include <cstdio>
#include <cstdlib>
using Bytes=std::array<uint8_t,32>;
static int digit(const u256 k,int p){return (int)((k[p/16]>>(4*(p%16)))&15);}
static void check(bool ok){if(!ok){std::puts("FAIL: wave shape/coverage");std::exit(1);}}
int main(){
    uint64_t checked=0;unsigned cases=0;
    for(int n=3;n<=10;++n)for(auto range: {std::pair<int,int>{0,256},{250,256},{251,256},{159,180},{139,140},{70,71},{0,8}}){
        g_cfg=Config{};g_cfg.mode=TRAV_WAVE;g_cfg.det_hex=-1;
        g_cfg.range_start_bit=range.first;g_cfg.range_end_bit=range.second;
        set_block_nibbles(n);compute_range();wave_set_seed(1234567);
        int top=63;while(top>=0&&digit(g_range_start,top)==digit(g_range_end,top))--top;
        const int width=(std::max)(0,top-n+1);
        const int minimum=top>=0?digit(g_range_start,top):0;
        const int radix=top>=0?digit(g_range_end,top)-minimum+1:1;
        const uint64_t states=wave_states_per_start();
        for(unsigned cycle=0;cycle<2;++cycle){
            ChunkIdx id;id.from_u64(cycle*states);u256 model;WaveInfo initial;
            wave_block_key(id,model,initial);check(initial.phase==0);
            const bool valid_start=u256_cmp(model,g_range_start)>=0&&u256_cmp(model,g_range_end)<=0;
            std::map<Bytes,uint64_t> seen;Bytes bytes;u256_to_be32(bytes.data(),model);seen[bytes]=0;
            uint64_t ordinal=0;int phase=0;
            auto walk=[&](int pass,int w,bool high){
                ++phase;int period=1;
                const int first=high?width-w:0,last=first+w;
                for(int j=first;j<last;++j)period=std::lcm(period,n+j==top?radix:16);
                for(int step=1;step<period;++step){
                    // Independent incremental model: add one to each active
                    // nibble, with no carry into adjacent nibbles.
                    for(int j=first;j<last;++j){
                        const int p=n+j,lo=p==top?minimum:0,r=p==top?radix:16;
                        const int value=lo+(digit(model,p)-lo+1+r)%r;
                        const uint64_t mask=UINT64_C(15)<<(4*(p%16));
                        model[p/16]=(model[p/16]&~mask)|((uint64_t)value<<(4*(p%16)));
                    }
                    ++ordinal;id.from_u64(cycle*states+ordinal);u256 actual;WaveInfo info;
                    wave_block_key(id,actual,info);
                    check(u256_cmp(actual,model)==0&&info.phase==phase&&info.step==step
                          &&info.pass==pass&&info.width==w&&info.high==high);
                    u256_to_be32(bytes.data(),model);auto inserted=seen.emplace(bytes,ordinal);
                    if(valid_start)check(info.duplicate==!inserted.second);
                    u256 clipped;u256_set(clipped,actual);
                    uint64_t count=info.duplicate?0:clamp_chunk_to_range(clipped,ZONE_BLOCK);
                    if(count){u256 end;u256_add_u64(end,clipped,count-1);
                        check(count<=ZONE_BLOCK&&u256_cmp(clipped,g_range_start)>=0&&u256_cmp(end,g_range_end)<=0);}
                    ++checked;
                }
            };
            for(int w=1;w<=width;++w)walk(1,w,false);
            for(int w=width-1;w>=1;--w)walk(2,w,true);
            for(int w=2;w<=width;++w)walk(3,w,true);
            for(int w=width-1;w>=1;--w)walk(4,w,false);
            check(ordinal+1==states);
        }
        ++cases;
    }
    std::printf("PASS: %u cases, %llu wave transitions; four passes, nibble increments, duplicates, bounds\n",cases,(unsigned long long)checked);
}
