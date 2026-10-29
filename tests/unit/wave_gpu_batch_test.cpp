#include "wave_gpu_batch.h"
#include <cstdio>
#include <cstdlib>
static void check(bool ok) { if(!ok){std::puts("FAIL");std::exit(1);} }
int main(){
    // Every menu size: bounded launches, contiguous coverage, no premature commit.
    for(int n=3;n<=15;++n){
        const uint64_t block=UINT64_C(1)<<(4*n);
        const uint64_t piece=(std::min)(block,UINT64_C(65536));
        WaveGpuCursor cursor;WaveGpuBatch batch;
        uint64_t supplied=0,processed=0,finished=0;
        const uint64_t loops=n<=6?(3*block/piece+6)/7:5;
        for(uint64_t launch=0;launch<loops;++launch){
            cursor.fill(batch,7,piece,[&](ChunkIdx &id,u256 key,uint64_t &count){
                if(supplied==3)return false;
                id.from_u64(supplied);u256_zero(key);key[1]=1;
                key[0]=supplied*block;count=block;++supplied;return true;
            });
            check(batch.pieces.size()<=7);
            for(const auto &part:batch.pieces){
                u256 key;fe_from_be32(key,part.start.data());
                check(key[1]==1&&key[0]==processed&&part.count==piece);
                processed+=part.count;
            }
            for(const auto &id:batch.completed){
                ChunkIdx expected;expected.from_u64(finished++);check(id==expected);
            }
            check(finished==processed/block);
        }
        if(n<=6)check(processed==3*block&&finished==3);
        else check(finished==0);
    }
    // Empty/duplicate blocks, clipped tails, cross-limb carry and a block
    // crossing launch boundaries. A retry must use the same retained pieces.
    WaveGpuCursor cursor;WaveGpuBatch batch;unsigned supplied=0;
    const uint64_t lengths[]={0,65537,0,4095};
    uint64_t checked=0;unsigned completed=0,full=0,tail=0;
    for(int launch=0;launch<4;++launch){
        cursor.fill(batch,1,65536,[&](ChunkIdx &id,u256 key,uint64_t &count){
            if(supplied==4)return false;
            id.from_u64(supplied);u256_zero(key);key[0]=UINT64_MAX-100;
            count=lengths[supplied++];return true;
        });
        for(const auto &p:batch.pieces){
            checked+=p.count;u256 key;fe_from_be32(key,p.start.data());
            if(p.count==1)check(key[1]==1&&key[0]==65435);
            if(p.count==65536)++full;else ++tail;
        }
        completed+=(unsigned)batch.completed.size();
        if(launch==0)check(completed==1); // The 65,537-key block is incomplete.
    }
    check(checked==69632&&completed==4&&full==1&&tail==2);
    std::puts("PASS: sizes 3..15, bounded batches, exact coverage, completion, tails, carry");
}
