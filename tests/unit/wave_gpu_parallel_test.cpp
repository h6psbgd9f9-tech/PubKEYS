// CPU-only tests of production finite traversal and the parallel metadata path.
// Deliberately excludes the engine, target loader, notification and persistence.
#include "traverse_modes.cpp"
#include "wave_gpu_parallel.h"
#include "wave_gpu_prefetch.h"
#include <windows.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>

std::string tg_default_token() { return {}; }
std::string tg_default_chat() { return {}; }
bool chunk_bounds(const ChunkIdx&,u256,uint64_t&) { throw std::runtime_error("unexpected sequential adapter"); }
bool mask_active() { return false; }
static uint8_t total_store[32]{};
uint8_t (&g_total_blocks256)[32]=total_store;
u256 g_mask_total{};
Config g_cfg;
static u256 low_store{},high_store{};
u256 &g_range_start=low_store,&g_range_end=high_store;
static std::atomic<uint64_t> count_store{1234567};
std::atomic<uint64_t> &g_count=count_store;
static std::atomic<bool> stop_store{false};
std::atomic<bool> &g_stop=stop_store;
volatile sig_atomic_t g_stop_flag=0;
void chunk_done(SearchContext&,const ChunkIdx&) { throw std::runtime_error("metadata published a completion"); }

namespace {
uint64_t checks=0,packets=0,rows=0,finite_keys=0;
unsigned cases=0;
void require(bool ok,const char* why) { ++checks; if(!ok)throw std::runtime_error(why); }
PackedKey bytes(const u256 key) { PackedKey out{};u256_to_be32(out.data(),key);return out; }
void unchanged() { require(g_count.load()==1234567,"metadata changed scanned-key counter"); }
void compare(const WaveGpuBatch& actual,const WaveGpuBatch& expected) {
    require(actual.full_starts==expected.full_starts,"full starts differ from serial cursor");
    require(actual.completed==expected.completed,"completion metadata differs from serial cursor");
    require(actual.pieces.size()==expected.pieces.size(),"piece count differs from serial cursor");
    for(size_t i=0;i<actual.pieces.size();++i) {
        require(actual.pieces[i].start==expected.pieces[i].start,"piece start differs from serial cursor");
        require(actual.pieces[i].count==expected.pieces[i].count,"piece length differs from serial cursor");
    }
    unchanged();++packets;rows+=actual.pieces.size();
}
void configure(int mode,const u256 low,const u256 high,bool anchor=false,int hex=-1) {
    g_cfg=Config{};g_cfg.mode=mode;g_cfg.wave_nibbles=g_cfg.zone_nibbles=3;
    g_cfg.range_start_bit=250;g_cfg.range_end_bit=256;g_cfg.det_hex=hex;
    u256_set(low_store,low);u256_set(high_store,high);
    if(anchor&&mode==TRAV_WAVE) {
        u256 chosen;u256_add_u64(chosen,low,3*4096+17);const auto key=bytes(chosen);
        g_cfg.wave_start=to_hex(key.data(),32);
    }
    set_block_nibbles(3);wave_set_seed(UINT64_C(0x3dc67a18a509bf24));compute_total_blocks256();
    require(traversal_block_bits()==12&&unique_blocks_mode(),"fixture uses production N3 finite traversal");
}
struct Claims {
    ChunkIdx next;uint64_t left=0;bool limited=false;std::thread::id owner;
    bool take(ChunkIdx& out) {
        require(std::this_thread::get_id()==owner,"claim callback must run serially on caller");
        if((limited&&!left)||memcmp(next.b,g_total_blocks256,32)>=0)return false;
        out=next;next.inc();if(limited)--left;return true;
    }
};
bool production_build(const ChunkIdx& id,u256 key,uint64_t& count) {
    thread_local std::mt19937_64 unused_rng(7);int phase=-1;
    return next_chunk(id,key,count,phase,unused_rng);
}
void parity_case(int mode,const u256 low,const u256 high,size_t capacity,
                 const ChunkIdx& first,uint64_t limit,bool anchor=false,int hex=-1,
                 bool finite=false) {
    ++cases;configure(mode,low,high,anchor,hex);
    Claims serial{first,limit,limit!=0,std::this_thread::get_id()},parallel=serial;
    WaveGpuCursor cursor;WaveGpuParallel pool;
    std::set<PackedKey> visited;uint64_t sum=0,tail_count=0;size_t passes=0;
    for(;;) {
        WaveGpuBatch expected,actual;
        cursor.fill(expected,capacity,4096,[&](ChunkIdx& id,u256 key,uint64_t& count) {
            return serial.take(id)&&production_build(id,key,count);
        });
        pool.fill(actual,capacity,4096,[&](ChunkIdx& id){return parallel.take(id);},production_build);
        compare(actual,expected);
        require(serial.next==parallel.next&&serial.left==parallel.left,"claim state exact serial parity");
        if(actual.pieces.empty()&&actual.completed.empty())break;
        require(++passes<100000,"finite packet iteration guard");
        for(size_t i=0;i<actual.pieces.size();++i) {
            const auto& p=actual.pieces[i];u256 key,last,difference;
            fe_from_be32(key,p.start.data());u256_add_u64(last,key,p.count-1);
            require(p.count>0&&p.count<=4096,"piece bounded to logical N3 block");
            require(u256_cmp(key,low)>=0&&u256_cmp(last,high)<=0&&u256_cmp(key,last)<=0,"piece inside inclusive source bounds");
            require(visited.insert(p.start).second,"no repeated block within tested ordinal run");
            if(finite) {
                u256_sub(difference,key,low);
                require(!difference[1]&&!difference[2]&&!difference[3]&&difference[0]%4096==0,"finite relative block grid");
                sum+=p.count;if(p.count<4096)++tail_count;
            }
        }
    }
    if(finite) {
        u256 width;u256_sub(width,high,low);require(!width[1]&&!width[2]&&!width[3],"small finite width fixture");
        const uint64_t n=width[0]+1,blocks=(n-1)/4096+1;
        require(sum==n&&visited.size()==blocks,"finite exact coverage without repeated blocks or gaps");
        require(tail_count==((n%4096)?1:0),"finite short tail exactly once");
        finite_keys+=sum;
    }
}
void production_parity() {
    ChunkIdx zero;
    for(int mode:{TRAV_WAVE,TRAV_ZONE}) {
        u256 low,high;u256_from_u64(low,7);u256_add_u64(high,low,4096*257+136);
        for(size_t capacity:{size_t(1),size_t(7),size_t(257),size_t(16384)})
            parity_case(mode,low,high,capacity,zero,0,false,-1,true);
        // A 64-bit carry, the scalar-order upper edge and an explicit wave anchor.
        u256_zero(low);low[0]=UINT64_MAX-20;u256_add_u64(high,low,4096*17+38);
        parity_case(mode,low,high,7,zero,0,false,-1,true);
        u256_sub_u64(low,EC_N,4096*19+73);u256_sub_u64(high,EC_N,1);
        parity_case(mode,low,high,7,zero,0,true,-1,true);
        u256_from_u64(low,41);u256_add_u64(high,low,111);
        parity_case(mode,low,high,7,zero,0,false,-1,true);
        u256_from_u64(low,41);u256_add_u64(high,low,4095);
        parity_case(mode,low,high,7,zero,0,false,-1,true);
        u256_from_u64(low,1);u256_set(high,low);
        parity_case(mode,low,high,7,zero,0,false,-1,true);
        // The actual wide interval reported by the user, with full-width IDs.
        u256_zero(low);low[3]=UINT64_C(1)<<58;u256_sub_u64(high,EC_N,1);
        parity_case(mode,low,high,257,zero,2049);
        parity_case(mode,low,high,16384,zero,32771);
        // The shipping RTX 4070 launch has 656*256 GPU threads / 4 = 41,984
        // one-piece N3 slots. Check its exact production-sized packet too.
        parity_case(mode,low,high,41984,zero,41984);
        ChunkIdx upper;upper.b[7]=1;upper.b[31]=251;
        parity_case(mode,low,high,257,upper,1027,true);
        // compute_range narrows a chosen top HEX digit to these exact boundaries.
        u256_zero(low);low[3]=UINT64_C(8)<<56;u256_zero(high);high[3]=UINT64_C(9)<<56;u256_sub_u64(high,high,1);
        parity_case(mode,low,high,257,upper,1027,false,8);
    }
}
struct Active {
    std::atomic<unsigned>& value;
    explicit Active(std::atomic<unsigned>& v):value(v){++value;}
    ~Active(){--value;}
};
void controls() {
    {
        WaveGpuParallel synchronous(1),capped(100);
        require(synchronous.worker_count()==1,"requested one worker stays synchronous");
        require(capped.worker_count()<=4,"oversized requested worker count capped");
        WaveGpuBatch direct;unsigned serial_claims=0;
        const auto owner=std::this_thread::get_id();
        synchronous.fill(direct,17,4096,[&](ChunkIdx& id){if(serial_claims==17)return false;id.from_u64(serial_claims++);return true;},
            [&](const ChunkIdx& id,u256 key,uint64_t& count){require(std::this_thread::get_id()==owner,"synchronous fallback uses caller");u256_from_u64(key,id.b[31]+1);count=4096;return true;});
        require(direct.pieces.size()==17&&direct.completed.size()==17,"synchronous fallback complete metadata");
    }
    WaveGpuParallel pool;WaveGpuBatch batch;
    unsigned claims=0;
    auto claim=[&](ChunkIdx& id){if(claims>=128)return false;id.from_u64(claims++);return true;};
    std::atomic<unsigned> active{0},maximum{0};std::mutex ids_mutex;std::set<std::thread::id> threads;
    auto build=[&](const ChunkIdx& id,u256 key,uint64_t& count) {
        Active held(active);unsigned peak=active.load(),old=maximum.load();
        while(old<peak&&!maximum.compare_exchange_weak(old,peak)){}
        {std::lock_guard<std::mutex> lock(ids_mutex);threads.insert(std::this_thread::get_id());}
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        u256_from_u64(key,id.b[31]*4096+1);count=4096;return true;
    };
    pool.fill(batch,128,4096,claim,build);
    require(batch.pieces.size()==128&&batch.completed.size()==128,"bounded pool complete metadata");
    require(pool.worker_count()>=1&&pool.worker_count()<=4,"pool bounded to at most four workers");
    require(threads.size()==pool.worker_count()&&maximum>=1&&maximum<=pool.worker_count(),"bounded metadata workers participate");
    require(active==0,"fill joins all active metadata jobs");unchanged();
    // Zero arguments must not claim work, even when the packet had old contents.
    claims=0;pool.fill(batch,0,4096,claim,build);require(claims==0&&batch.pieces.empty()&&batch.completed.empty(),"zero capacity no claim");
    pool.fill(batch,7,0,claim,build);require(claims==0&&batch.pieces.empty()&&batch.completed.empty(),"zero piece no claim");
    // A stop observed by the serial claim gate leaves only prepared metadata.
    unsigned claimed=0;stop_store=false;
    pool.fill(batch,128,4096,[&](ChunkIdx& id){if(g_stop.load())return false;id.from_u64(claimed++);if(claimed==17)stop_store=true;return true;},build);
    require(claimed==17&&batch.pieces.size()==17&&batch.completed.size()==17,"stop prevents additional claims");
    unchanged();stop_store=false;
    bool caught=false;claims=0;
    try {pool.fill(batch,128,4096,claim,[&](const ChunkIdx& id,u256 key,uint64_t& count) {
        Active held(active);if(id.b[31]==17)throw std::runtime_error("injected preparation error");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));u256_from_u64(key,1);count=4096;return true;
    });}catch(const std::runtime_error&){caught=true;}
    require(caught&&active==0&&batch.pieces.empty()&&batch.full_starts.empty()&&batch.completed.empty(),"exception joins and publishes no packet");
    unchanged();claims=0;pool.fill(batch,128,4096,claim,build);
    require(batch.pieces.size()==128,"pool reusable after preparation exception");
    for(uint64_t invalid:{UINT64_C(0),UINT64_C(4097)}) {
        claims=0;caught=false;
        try {pool.fill(batch,7,4096,claim,[&](const ChunkIdx&,u256 key,uint64_t& count){u256_from_u64(key,1);count=invalid;return true;});}
        catch(const std::exception&){caught=true;}
        require(caught&&batch.pieces.empty()&&batch.completed.empty(),"invalid logical block length rejected atomically");
    }
    claims=0;caught=false;
    try {pool.fill(batch,7,4096,claim,[](const ChunkIdx&,u256,uint64_t&){return false;});}
    catch(const std::exception&){caught=true;}
    require(caught&&batch.pieces.empty()&&batch.completed.empty(),"claimed ordinal build failure rejects packet");
    claims=0;caught=false;
    try {pool.fill(batch,7,4096,[&](ChunkIdx& id){if(claims==5)throw std::runtime_error("injected claim error");id.from_u64(claims++);return true;},build);}
    catch(const std::exception&){caught=true;}
    require(caught&&active==0&&batch.pieces.empty()&&batch.completed.empty(),"claim failure starts no metadata or completion publication");
    unchanged();
}
void prefetch_lifetime() {
    std::atomic<unsigned> active{0},calls{0};std::promise<void> entered,release;
    auto ready=entered.get_future();auto permission=release.get_future().share();
    auto permit=std::async(std::launch::async,[&]{ready.wait();release.set_value();});
    {
        WaveGpuParallel pool;unsigned supplied=0;
        WaveGpuPrefetch queue([&]{
            const unsigned call=++calls;WaveGpuBatch batch;
            pool.fill(batch,17,4096,[&](ChunkIdx& id){id.from_u64(supplied++);return true;},
                [&](const ChunkIdx& id,u256 key,uint64_t& count){
                    Active held(active);
                    if(call==2&&id.b[31]==17){entered.set_value();permission.wait();}
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    u256_from_u64(key,id.b[31]*4096+1);count=4096;return true;
                });return batch;
        },true);
        WaveGpuBatch first;queue.take(first);require(first.pieces.size()==17,"first prefetch packet exact");
        ready.wait();
        // Destruction order is production order: future first, metadata pool second.
    }
    permit.get();require(active==0&&calls==2,"prefetch and pool destructors join before captured state expires");unchanged();
}
}
int main() {try {
    require(SetPriorityClass(GetCurrentProcess(),BELOW_NORMAL_PRIORITY_CLASS)!=0,"low CPU priority");
    production_parity();controls();prefetch_lifetime();
    std::printf("PASS wave GPU metadata parallel CPU-only: cases=%u checks=%llu packets=%llu rows=%llu finite_keys=%llu; exact serial/production parity, finite uniqueness, tails, 256-bit IDs, HEX, anchor, stop, exception, join, worker cap; no GPU or production writes\n",
        cases,(unsigned long long)checks,(unsigned long long)packets,(unsigned long long)rows,(unsigned long long)finite_keys);
    return 0;
}catch(const std::exception& e){std::printf("FAIL %s checks=%llu case=%u\n",e.what(),(unsigned long long)checks,cases);return 1;}}
