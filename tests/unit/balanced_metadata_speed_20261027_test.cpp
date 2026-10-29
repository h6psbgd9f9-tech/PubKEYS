// Bounded CPU scheduling throughput only. No scan/ECC/application/GPU/network.
#include "whole_shape_traversal.h"
#define PKH_CAPTURE_KXE_ONLY
#define main unused_numeric_fixture_main
#include "restored_modes_integration_test.cpp"
#undef main
#undef PKH_CAPTURE_KXE_ONLY
#include <chrono>
#include <iomanip>

namespace {
struct MetadataStat {uint64_t valid=0,keys=0;double seconds=0;};
template<class Build>
void measure(const char* scheduler,int mode,int n,unsigned workers,Build build) {
    const unsigned total=262144U*workers;
    std::vector<MetadataStat> stats(workers);
    std::vector<std::thread> threads;
    std::atomic<unsigned> ready{0};std::atomic<bool> go{false};
    for(unsigned worker=0;worker<workers;++worker)threads.emplace_back([&,worker]{
        for(unsigned i=0;i<128;++i){uint64_t count=0;build(worker+i*workers,count);}
        ++ready;while(!go.load(std::memory_order_acquire))std::this_thread::yield();
        const auto begin=std::chrono::steady_clock::now();
        auto& stat=stats[worker];
        for(unsigned id=worker;id<total;id+=workers){uint64_t count=0;build(id,count);if(count){++stat.valid;stat.keys+=count;}}
        stat.seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count();
    });
    while(ready.load()!=workers)std::this_thread::yield();
    go.store(true,std::memory_order_release);
    for(auto& thread:threads)thread.join();
    double elapsed=0;uint64_t valid=0,keys=0;
    for(const auto& stat:stats){elapsed=(std::max)(elapsed,stat.seconds);valid+=stat.valid;keys+=stat.keys;}
    std::cout<<scheduler<<','<<(mode==TRAV_WAVE?"wave":"zone")<<','<<n<<','<<workers<<','<<total<<','<<valid<<','<<keys<<','
             <<std::fixed<<std::setprecision(6)<<elapsed<<','<<std::setprecision(0)<<double(valid)/elapsed<<','<<double(keys)/elapsed<<'\n';
}
void run(int mode,int n,unsigned workers) {
    u256 lo{},hi{};power(lo,139);u256_sub_u64(hi,EC_N,1);
    configure(mode,139,256,n,173,lo,hi);
    measure("production_balanced_next_chunk",mode,n,workers,[&](uint64_t ordinal,uint64_t& count){
        static thread_local std::mt19937_64 rng(1);u256 key{};int phase=-1;
        if(!next_chunk(id64(ordinal),key,count,phase,rng))throw std::runtime_error("metadata ordinal ended early");
    });
    whole_shaped::Config cfg;cfg.lower=bytes(lo);cfg.upper=bytes(hi);cfg.anchor=bytes(lo);
    cfg.kind=mode==TRAV_WAVE?whole_shaped::Kind::Wave:whole_shaped::Kind::Zone;
    cfg.top_index=0;cfg.low_nibbles=unsigned(n);cfg.seed=173;
    whole_shaped::Plan baseline;require(baseline.init(cfg),"baseline whole plan initialized");
    measure("baseline_direct_whole_plan",mode,n,workers,[&](uint64_t ordinal,uint64_t& count){
        whole_shaped::Result result;if(!baseline.get_complete(whole_shaped::detail::from_u64(ordinal),result))throw std::runtime_error("baseline ordinal ended early");count=result.count;
    });
}
}
int main(int argc,char**) {
    SetPriorityClass(GetCurrentProcess(),BELOW_NORMAL_PRIORITY_CLASS);
    try {
        std::cout<<"scheduler,mode,nibbles,workers,logical_positions,nonempty_windows,represented_keys,seconds,valid_windows_per_second,represented_keys_per_second\n";
        for(int mode:{TRAV_WAVE,TRAV_ZONE})for(int n:{3,4})for(unsigned workers:{1U,4U})
            if(argc==1||workers==4)run(mode,n,workers);
        return 0;
    }catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}
}
