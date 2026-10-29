// CPU metadata only. No scanner, CUDA runtime, target database, notifications,
// configuration or checkpoint persistence is linked or opened by this fixture.
#include "wave_gpu_batch.h"
#include "wave_gpu_parallel.h"
#include "wave_gpu_prefetch.h"
#include <atomic>
#include <cstdio>
#include <map>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
uint64_t checks=0,cases=0;
void require(bool ok,const char* why) {++checks;if(!ok)throw std::runtime_error(why);}
uint64_t ordinal(const ChunkIdx& id) {
    uint64_t value=0;for(unsigned i=24;i<32;++i)value=(value<<8)|id.b[i];return value;
}
void aligned(const WaveGpuBatch& batch) {
    require(batch.completed.size()==batch.completed_counts.size(),"completion count alignment");
}
void menu_sizes() {
    for(unsigned n=3;n<=15;++n) {
        ++cases;
        const uint64_t logical=UINT64_C(1)<<(4*n);
        const uint64_t piece=(std::min)(logical,UINT64_C(65536));
        const uint64_t launches=n<=6?(3*logical/piece+6)/7:5;
        WaveGpuCursor cursor;WaveGpuBatch batch;
        uint64_t supplied=0,scanned=0,completed=0,credited=0;
        for(uint64_t launch=0;launch<launches;++launch) {
            cursor.fill(batch,7,piece,[&](ChunkIdx& id,u256 key,uint64_t& count) {
                if(supplied==3)return false;
                id.from_u64(supplied);u256_zero(key);key[1]=1;
                key[0]=supplied*logical;count=logical;++supplied;return true;
            });
            aligned(batch);require(batch.pieces.size()<=7,"bounded piece capacity");
            for(const auto& part:batch.pieces) {
                u256 key;fe_from_be32(key,part.start.data());
                require(key[1]==1&&key[0]==scanned,"exact contiguous piece start");
                require(part.count==piece,"full menu-sized pieces");scanned+=part.count;
            }
            for(size_t i=0;i<batch.completed.size();++i) {
                require(ordinal(batch.completed[i])==completed++,"logical completion order");
                require(batch.completed_counts[i]==logical,"original logical weight");
                credited+=batch.completed_counts[i];
            }
            require(completed==scanned/logical,"only whole logical blocks complete");
            require(credited==completed*logical&&credited<=scanned,"no partial work credited");
        }
        if(n<=6)require(scanned==3*logical&&completed==3,"finished finite menu case");
        else require(completed==0&&credited==0,"large unfinished logical block withheld");
    }
}
void empty_tail_and_retained_cursor() {
    ++cases;WaveGpuCursor cursor;WaveGpuBatch batch;unsigned supplied=0;
    const uint64_t lengths[]={0,65537,0,4095};
    uint64_t scanned=0,credited=0;size_t completed=0;
    for(unsigned launch=0;launch<4;++launch) {
        cursor.fill(batch,1,65536,[&](ChunkIdx& id,u256 key,uint64_t& count) {
            if(supplied==4)return false;
            id.from_u64(supplied);u256_zero(key);key[0]=UINT64_MAX-100;
            count=lengths[supplied++];return true;
        });
        aligned(batch);
        for(const auto& part:batch.pieces) {
            scanned+=part.count;
            if(part.count==1) {u256 k;fe_from_be32(k,part.start.data());require(k[1]==1&&k[0]==65435,"cross-limb retained tail");}
        }
        for(size_t i=0;i<batch.completed.size();++i) {
            require(ordinal(batch.completed[i])==completed,"empty/tail ordinal order");
            require(batch.completed_counts[i]==lengths[completed],"empty/tail original weight");
            credited+=batch.completed_counts[i];++completed;
        }
        if(launch==0)require(completed==1&&credited==0,"first partial packet credits only empty ordinal");
        if(launch==1)require(completed==2&&credited==65537,"final one-key piece credits whole logical block once");
    }
    require(scanned==69632&&credited==69632&&completed==4,"finite exact summed completion weights");
    require(batch.completed.empty()&&batch.completed_counts.empty(),"reused packet clears prior completion weights");
}
void pause_and_wide_weights() {
    ++cases;WaveGpuCursor cursor;WaveGpuBatch batch;bool allow=true;unsigned claims=0;
    auto next=[&](ChunkIdx& id,u256 key,uint64_t& count) {
        if(!allow||claims)return false;
        id.from_u64(77);u256_from_u64(key,1);count=131073;++claims;return true;
    };
    cursor.fill(batch,1,65536,next);aligned(batch);
    require(batch.completed.empty()&&claims==1,"partial before pause is not a completion");
    // A pause does not consume/commit this prepared packet. Retain it unchanged.
    const WaveGpuBatch retained=batch;allow=false;
    require(retained.completed.empty()&&retained.completed_counts.empty(),"retained/retry packet cannot credit partial logical work");
    cursor.fill(batch,1,65536,next);aligned(batch);
    require(batch.completed.empty()&&claims==1,"cursor drains existing block without claiming new work");
    cursor.fill(batch,1,65536,next);aligned(batch);
    require(batch.completed.size()==1&&ordinal(batch.completed[0])==77,"last pending piece finishes original ID");
    require(batch.pieces.size()==1&&batch.pieces[0].count==1&&batch.completed_counts[0]==131073,"logical count not final piece count");
    for(uint64_t size:{UINT64_C(1)<<60,UINT64_MAX}) {
        ++cases;WaveGpuCursor wide;unsigned supplied=0;
        wide.fill(batch,1,size,[&](ChunkIdx& id,u256 key,uint64_t& count) {
            if(supplied++)return false;id.from_u64(91);u256_from_u64(key,1);count=size;return true;
        });
        aligned(batch);require(batch.completed.size()==1&&batch.completed_counts[0]==size,"64-bit completion weight preserved");
    }
    // Invalid capacities clear a reused batch without claiming or retaining old weights.
    cursor.fill(batch,0,65536,next);aligned(batch);
    require(batch.pieces.empty()&&batch.completed.empty()&&batch.completed_counts.empty(),"zero capacity clears weights");
    cursor.fill(batch,3,0,next);aligned(batch);require(batch.completed_counts.empty(),"zero piece clears weights");
}
uint64_t synthetic_count(uint64_t id) {return id%7==0?0:id%11==0?17:4096;}
void parallel_weights(unsigned workers) {
    ++cases;WaveGpuParallel parallel(workers);WaveGpuBatch batch;uint64_t next=0;
    const auto owner=std::this_thread::get_id();uint64_t total=0,completed=0;
    for(unsigned pass=0;pass<6;++pass) {
        parallel.fill(batch,257,4096,[&](ChunkIdx& id) {
            require(std::this_thread::get_id()==owner,"claims stay serial");
            if(next==1031)return false;id.from_u64(next++);return true;
        },[](const ChunkIdx& id,u256 key,uint64_t& count) {
            const uint64_t n=ordinal(id);u256_zero(key);key[1]=n+1;count=synthetic_count(n);return true;
        });
        aligned(batch);require(batch.completed.size()<=257,"parallel ID capacity");
        size_t piece=0;
        for(size_t i=0;i<batch.completed.size();++i) {
            const uint64_t id=ordinal(batch.completed[i]),weight=synthetic_count(id);
            require(id==completed++,"parallel completion ordering");
            require(batch.completed_counts[i]==weight,"parallel actual weight including zero");total+=weight;
            if(weight) {
                require(piece<batch.pieces.size(),"nonempty parallel piece exists");
                require(batch.pieces[piece].count==weight,"parallel piece exact count");
                u256 key;fe_from_be32(key,batch.pieces[piece++].start.data());require(key[1]==id+1,"parallel piece mapped to correct ID");
            }
        }
        require(piece==batch.pieces.size(),"no unassociated piece");
    }
    uint64_t expected=0;for(uint64_t i=0;i<1031;++i)expected+=synthetic_count(i);
    require(completed==1031&&total==expected,"parallel all completion weights exact");
    next=0;
    bool threw=false;
    try {parallel.fill(batch,37,4096,[&](ChunkIdx& id){id.from_u64(next++);return true;},
        [](const ChunkIdx& id,u256 key,uint64_t& count){if(ordinal(id)==17)throw std::runtime_error("synthetic metadata failure");u256_from_u64(key,1);count=4096;return true;});}
    catch(const std::runtime_error&){threw=true;}
    aligned(batch);require(threw&&batch.pieces.empty()&&batch.completed.empty()&&batch.completed_counts.empty(),"failed preparation publishes neither IDs nor weights");
    next=0;parallel.fill(batch,9,4096,[&](ChunkIdx& id){id.from_u64(next++);return true;},
        [](const ChunkIdx&,u256 key,uint64_t& count){u256_from_u64(key,1);count=1;return true;});
    aligned(batch);require(batch.completed_counts==std::vector<uint64_t>(9,1),"pool remains usable after failure");
    parallel.fill(batch,0,4096,[](ChunkIdx&){throw std::runtime_error("unexpected claim");return false;},
        [](const ChunkIdx&,u256,uint64_t&){return true;});
    aligned(batch);require(batch.completed_counts.empty(),"parallel zero capacity clears counts");
}
void prefetch_move() {
    ++cases;WaveGpuCursor cursor;uint64_t next=0;
    WaveGpuPrefetch queue([&] {
        WaveGpuBatch batch;cursor.fill(batch,3,4096,[&](ChunkIdx& id,u256 key,uint64_t& count){
            if(next==9)return false;id.from_u64(next);u256_zero(key);key[1]=next+1;count=next%3?4096:0;++next;return true;
        });return batch;
    },true);
    uint64_t id=0;
    for(;;){WaveGpuBatch batch;queue.take(batch);aligned(batch);if(batch.completed.empty()&&batch.pieces.empty())break;
        for(size_t i=0;i<batch.completed.size();++i){require(ordinal(batch.completed[i])==id,"prefetch ID move order");require(batch.completed_counts[i]==(id%3?4096:0),"prefetch moves aligned count vector");++id;}}
    require(id==9,"prefetch entire finite completion sequence");
}
}
int main() {try {
    menu_sizes();empty_tail_and_retained_cursor();pause_and_wide_weights();parallel_weights(1);parallel_weights(4);prefetch_move();
    std::printf("PASS shape completion weights checks=%llu cases=%llu GPU=0 notifications=0\n",(unsigned long long)checks,(unsigned long long)cases);
    return 0;
}catch(const std::exception& error){std::fprintf(stderr,"FAIL shape completion weights: %s\n",error.what());return 1;}}
