#include "zone_search.h"
#include "hybrid_search.h"
#include "batch_prefetch.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cstring>
#include <chrono>

static size_t checked=0;static int failures=0;
#define CHECK(x) do {if(!(x)){if(failures<15)std::cerr<<"FAIL "<<__LINE__<<"\n";++failures;}} while(0)
static void bytes(uint8_t out[32],uint64_t n){std::memset(out,0,32);for(int i=31;i>=24;--i){out[i]=(uint8_t)n;n>>=8;}}
static uint64_t value(const uint8_t in[32]){uint64_t n=0;for(int i=0;i<24;++i)CHECK(in[i]==0);for(int i=24;i<32;++i)n=(n<<8)|in[i];return n;}
static std::string read(const std::filesystem::path& p){std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
template<class Batch>static bool same(const Batch& a,const Batch& b){
    if(a.keys!=b.keys||a.full_starts!=b.full_starts||a.pieces.size()!=b.pieces.size())return false;
    for(size_t i=0;i<a.pieces.size();++i)if(a.pieces[i].count!=b.pieces[i].count||std::memcmp(a.pieces[i].key,b.pieces[i].key,32))return false;
    return a.next.sequence==b.next.sequence&&a.next.offset==b.next.offset&&
        !std::memcmp(a.next.current.b,b.next.current.b,32)&&!std::memcmp(a.next.rng,b.next.rng,sizeof(a.next.rng));
}
template<class Search,class Batch>static void run(uint64_t low,uint64_t high,int n,size_t capacity,uint64_t piece,const std::filesystem::path& dir){
    Search serial,pipeline,resumed;uint8_t lo[32],hi[32],seed[32];bytes(lo,low);bytes(hi,high);bytes(seed,71);
    std::string why;CHECK(serial.init(n,lo,hi,seed,"prefetch-test",why));
    CHECK(pipeline.init(n,lo,hi,seed,"prefetch-test",why));CHECK(resumed.init(n,lo,hi,seed,"prefetch-test",why));
    BatchPrefetch<Search,Batch> queue(pipeline,capacity,piece,true);
    for(int step=0;step<7;++step){
        Batch a,b;CHECK(serial.prepare_batch(capacity,piece,a,why));CHECK(queue.take(b,why));CHECK(same(a,b));
        const auto before=pipeline.status();queue.start_after(b);
        CHECK(pipeline.status().sequence==before.sequence&&pipeline.status().version==before.version);
        uint64_t total=0;std::vector<uint8_t> starts;
        for(const auto& item:b.pieces){
            const auto first=value(item.key);CHECK(first>=low&&first<=high&&item.count&&item.count-1<=high-first);
            total+=item.count;++checked;
            if(item.count==piece)starts.insert(starts.end(),item.key,item.key+32);
        }
        CHECK(total==b.keys&&starts==b.full_starts);
        CHECK(serial.commit_batch(a,why));CHECK(pipeline.commit_batch(b,why));CHECK(!pipeline.commit_batch(b,why));
        CHECK(serial.save((dir/"serial.dat").string(),why));CHECK(pipeline.save((dir/"pipeline.dat").string(),why));
        CHECK(read(dir/"serial.dat")==read(dir/"pipeline.dat"));
        CHECK(resumed.load((dir/"pipeline.dat").string(),why));
        Batch c,d;CHECK(resumed.prepare_batch(19,137,c,why));CHECK(pipeline.prepare_batch(19,137,d,why));CHECK(same(c,d));
        if(b.next.exhausted)break;
    }
    // Speculation must never become valid because reset/load happens to
    // increment the version to the value expected after an ordinary commit.
    Batch parent,next;CHECK(pipeline.prepare_batch(1,piece,parent,why));
    CHECK(pipeline.prepare_after(parent,1,piece,next,why));pipeline.reset();CHECK(!pipeline.commit_batch(next,why));
    CHECK(pipeline.prepare_batch(1,piece,parent,why));CHECK(pipeline.prepare_after(parent,1,piece,next,why));
    CHECK(pipeline.load((dir/"pipeline.dat").string(),why));CHECK(!pipeline.commit_batch(next,why));
    CHECK(!serial.commit_batch(parent,why));
}
int main(){
    auto dir=std::filesystem::temp_directory_path()/("pkh-prefetch-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(dir);
    for(int n:{3,4,10}){
        run<pkh_zone::ZoneSearch,pkh_zone::ZoneBatch>(UINT64_C(1)<<48,(UINT64_C(1)<<49)-1,n,41984,4096,dir);
        run<pkh_hybrid::HybridSearch,pkh_hybrid::HybridBatch>(UINT64_C(1)<<48,(UINT64_C(1)<<49)-1,n,41984,4096,dir);
    }
    for(auto range:{std::pair<uint64_t,uint64_t>{123,171},{4000,4200},{1,1048575}}){
        run<pkh_zone::ZoneSearch,pkh_zone::ZoneBatch>(range.first,range.second,3,71,73,dir);
        run<pkh_hybrid::HybridSearch,pkh_hybrid::HybridBatch>(range.first,range.second,3,71,73,dir);
    }
    std::cout<<"Prefetch checks: "<<checked<<" pieces; failures="<<failures<<"\n";
    return failures?1:0;
}
