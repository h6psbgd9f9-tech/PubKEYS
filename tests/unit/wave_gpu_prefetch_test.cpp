#include "wave_gpu_prefetch.h"
#include <atomic>
#include <cstdio>
#include <stdexcept>
#include <vector>

static unsigned checks=0;
static void require(bool ok) { ++checks; if(!ok)throw std::runtime_error("prefetch invariant"); }
static void coverage(bool enabled,size_t capacity,uint64_t piece) {
    WaveGpuCursor cursor;
    const std::vector<uint64_t> lengths={piece*3+1,7,piece*2,piece-1};
    uint64_t supplied=0,base=1,scanned=0,expected_key=1,finished=0;
    {
        WaveGpuPrefetch queue([&] {
            WaveGpuBatch batch;
            cursor.fill(batch,capacity,piece,[&](ChunkIdx& id,u256 key,uint64_t& count) {
                if(supplied==lengths.size())return false;
                id.from_u64(supplied);u256_from_u64(key,base);count=lengths[supplied++];base+=count;return true;
            });
            return batch;
        },enabled);
        for(;;) {
            WaveGpuBatch batch;queue.take(batch);
            if(batch.pieces.empty()&&batch.completed.empty())break;
            require(batch.pieces.size()<=capacity);
            for(const auto& part:batch.pieces) {
                u256 key;fe_from_be32(key,part.start.data());
                require(key[0]==expected_key&&!key[1]&&!key[2]&&!key[3]);
                require(part.count>0&&part.count<=piece);
                expected_key+=part.count;scanned+=part.count;
            }
            for(const auto& id:batch.completed) {
                ChunkIdx expected;expected.from_u64(finished++);require(id==expected);
            }
        }
    }
    uint64_t total=0;for(auto n:lengths)total+=n;
    require(scanned==total&&finished==lengths.size()&&supplied==lengths.size());
}
int main() {try {
    for(bool enabled:{false,true})for(size_t capacity:{size_t(1),size_t(7)})
        for(uint64_t piece:{UINT64_C(4096),UINT64_C(65536)})coverage(enabled,capacity,piece);
    // A look-ahead is CPU-only and must finish while its captured state is alive.
    std::promise<void> entered,release;auto ready=entered.get_future();auto permission=release.get_future();
    std::atomic<unsigned> calls{0};std::atomic<bool> joined{false};
    {
        WaveGpuPrefetch queue([&] {
            const unsigned call=calls.fetch_add(1);
            WaveGpuBatch batch;
            if(call==0)batch.pieces.push_back(WaveGpuPiece{});
            else {entered.set_value();permission.wait();joined=true;}
            return batch;
        },true);
        WaveGpuBatch first;queue.take(first);ready.wait();
        require(!joined.load()&&calls==2);release.set_value();
    }
    require(joined.load());
    // Exceptions in background preparation reach the caller, without publishing
    // that packet as processed work or terminating the helper thread.
    unsigned turn=0;bool caught=false;
    {
        WaveGpuPrefetch queue([&] {
            if(turn++!=0)throw std::runtime_error("synthetic preparation failure");
            WaveGpuBatch batch;batch.pieces.push_back(WaveGpuPiece{});return batch;
        },true);
        WaveGpuBatch batch;queue.take(batch);
        try {queue.take(batch);}catch(const std::runtime_error&){caught=true;}
    }
    require(caught);
    std::printf("PASS prefetch checks=%u; exact packets, tails, finite end, synchronous/async parity, join, exception propagation\n",checks);
    return 0;
}catch(const std::exception& e){std::printf("FAIL %s\n",e.what());return 1;}}
