#include "common.h"
#include "traverse.h"
#include "packed_traversal.h"
#include "search.h"
#include <future>
#include <cstdio>
int main(int argc,char **argv){
    // A prefetched packet reserves IDs, but a checkpoint must only include
    // finished GPU work. Restart must reproduce the discarded packet exactly.
    g_cfg=Config{};g_cfg.mode=TRAV_WAVE;g_cfg.det_hex=-1;
    g_cfg.range_start_bit=argc>2?std::atoi(argv[2]):70;g_cfg.range_end_bit=argc>3?std::atoi(argv[3]):71;
    set_block_nibbles(argc>1?std::atoi(argv[1]):4);compute_range();wave_set_seed(0x51D0FA57ULL);
    PackedTraversalPlan plan;
    bool resume_ok=packed_traversal_prepare(plan);
    if(resume_ok) {
        ChunkCounter issued,resumed;PackedTraversalBatch first,unused,replay;
        checkpoint_reset(g_search,ChunkIdx{});
        resume_ok=packed_traversal_claim(plan,issued,41984,first);
        size_t segment=0;
        for(const auto &id:first.pending_ids){
            u256 expected;WaveInfo info;wave_block_key(id,expected,info);
            if(info.duplicate)continue;
            u256 clipped;u256_set(clipped,expected);
            if(clamp_chunk_to_range(clipped,ZONE_BLOCK)!=ZONE_BLOCK){
                resume_ok=resume_ok&&std::find(first.edge_ids.begin(),first.edge_ids.end(),id)!=first.edge_ids.end();continue;
            }
            if(segment>=first.segments.size()){resume_ok=false;break;}
            const auto &entry=first.segments[segment++];
            PackedKey translated=first.bases[entry.base];
            for(int pos=0;pos<64;++pos){
                const int shift=(pos%2)?0:4;
                int digit=(translated[pos/2]>>shift)&15;
                const int delta=plan.deltas[(size_t)entry.state*64+pos];
                digit=pos==plan.top_pos
                    ? plan.top_min+(digit-plan.top_min+delta)%plan.top_radix
                    : (digit+delta)%16;
                translated[pos/2]=(uint8_t)((translated[pos/2]&~(15<<shift))|(digit<<shift));
            }
            PackedKey reference;u256_to_be32(reference.data(),expected);
            resume_ok=resume_ok&&translated==reference;
        }
        resume_ok=resume_ok&&segment==first.segments.size();
        auto next=std::async(std::launch::async,[&] {
            return packed_traversal_claim(plan,issued,41984,unused);
        });
        for(const auto& id:first.pending_ids)chunk_done(g_search,id);
        resume_ok=next.get()&&resume_ok;
        const auto saved=checkpoint_seq(g_search);
        resume_ok=resume_ok&&!unused.pending_ids.empty()&&saved==unused.pending_ids.front();
        resumed.set(saved);
        resume_ok=packed_traversal_claim(plan,resumed,41984,replay)&&resume_ok;
        resume_ok=resume_ok&&replay.pending_ids==unused.pending_ids&&replay.bases==unused.bases
            &&replay.segments.size()==unused.segments.size()&&replay.sample_key==unused.sample_key;
        for(size_t i=0;resume_ok&&i<replay.segments.size();++i)
            resume_ok=replay.segments[i].base==unused.segments[i].base&&replay.segments[i].state==unused.segments[i].state;
    }
    
    std::printf("wave prefetch checkpoint/replay: %s\n",resume_ok?"PASS":"FAIL");
    return resume_ok?0:1;
}
