// Isolated CPU microbenchmark: no targets, GPU, user checkpoints or notifications.
// Compile identically against either the current sources or the reconstructed
// exact pre-audit zone sources. The build helper records their provenance.
#include "zone_search.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace pkh_zone;
namespace {
volatile uint64_t sink = 0;
using Clock = std::chrono::steady_clock;
be256 from_hex(const char* s) {
    be256 out{};
    for (int i=0;i<64;++i) {
        const char c=s[i]; const int d=c>='a'?c-'a'+10:c-'0';
        out.b[i/2]|=static_cast<uint8_t>(d << (i%2?0:4));
    }
    return out;
}
struct Range { const char* name; be256 lo, high; };
double seconds(Clock::time_point start) { return std::chrono::duration<double>(Clock::now()-start).count(); }
uint64_t checksum_key(const be256& k) {
    uint64_t sum=0; for(auto c:k.b)sum=(sum*UINT64_C(1099511628211))^c; return sum;
}
uint64_t checksum_walk(const ZoneTraversal& t) {
    uint64_t sum=checksum_key(t.current)^t.sequence^t.offset;
    for(auto v:t.rng)sum^=v; return sum;
}
struct Result { double rate; uint64_t sum; };
Result walk(const Range& range,int n,uint64_t count) {
    be256 hi=range.high;be256_add_u64(&hi,1); be256 seed{};seed.b[0]=0x53;seed.b[31]=0x91;
    ZoneTraversal t;t.init(n,range.lo,hi,seed);
    uint64_t sum=0; const auto start=Clock::now();
    for(uint64_t i=0;i<count;++i) {
        be256 first,last;ZoneInfo info;
        const auto keys=t.bounds(first,last,info);
        if(!keys)throw std::runtime_error("empty block in walk benchmark");
        sum+=keys+first.b[i%32]+info.ordinal;
        if(!t.advance())throw std::runtime_error("walk exhausted");
    }
    const auto elapsed=seconds(start);sum^=checksum_walk(t);sink=sum;return{count/elapsed,sum};
}
Result batches(const Range& range,int n,uint64_t count,size_t capacity) {
    be256 seed{};seed.b[0]=0x53;seed.b[31]=0x91;ZoneSearch search;std::string why;
    if(!search.init(n,range.lo.b,range.high.b,seed.b,"synthetic-speed-only",why))throw std::runtime_error(why);
    const uint64_t piece_keys=(std::min)(UINT64_C(65536),UINT64_C(1)<<(4*n));
    uint64_t sum=0,done=0;const auto start=Clock::now();
    while(done<count) {
        ZoneBatch b;
        if(!search.prepare_batch(capacity,piece_keys,b,why))throw std::runtime_error(why);
        if(b.pieces.empty())throw std::runtime_error("empty batch");
        done+=b.pieces.size();sum+=b.keys+b.full_starts.size()+b.pieces.front().key[0]+b.pieces.back().key[31];
        sum^=checksum_walk(b.next);
        if(!search.commit_batch(b,why))throw std::runtime_error(why);
    }
    const auto elapsed=seconds(start);sink=sum;return{done/elapsed,sum};
}
void paths(const Range& r,int n) {
    be256 hi=r.high;be256_add_u64(&hi,1);be256 seed{};seed.b[0]=0x53;seed.b[31]=0x91;
    ZoneTraversal t;t.init(n,r.lo,hi,seed);t.step();uint64_t nominal=0,masked=0;
    for(int i=0;i<100000;++i){if(t.rectangle||t.nominal_neighbors_inside())++nominal;else ++masked;t.advance();}
    std::cout<<"path,"<<r.name<<','<<n<<",rectangle="<<t.rectangle<<",nominal="<<nominal<<",masked="<<masked<<'\n';
}
}
int main(int argc,char**argv) { try {
    const uint64_t operations=argc>1?std::strtoull(argv[1],nullptr,10):2000000;
    const int repeats=argc>2?std::atoi(argv[2]):5;
    std::vector<Range> ranges{
        {"configured_250_256",from_hex("0400000000000000000000000000000000000000000000000000000000000000"),from_hex("fffffffffffffffffffffffffffffffebaaedce6af48a03bbfd25e8cd0364140")},
        {"rectangle_252_255",from_hex("1000000000000000000000000000000000000000000000000000000000000000"),from_hex("7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff")},
        {"lower_partial",from_hex("0000000000000000000000000000000000000000000000000000000000100007"),from_hex("00000000000000000000000000000000000000000000000000000000001ffffc")}
    };
    std::cout<<"kind,range,n,repeat,operations_per_second,checksum\n"<<std::fixed<<std::setprecision(3);
    for(const auto&r:ranges)for(int n:{3,4,10}) {
        paths(r,n);
        (void)walk(r,n,10000);(void)batches(r,n,16384,4096);
        for(int repeat=0;repeat<repeats;++repeat) {
            const auto a=walk(r,n,operations);std::cout<<"walk,"<<r.name<<','<<n<<','<<repeat<<','<<a.rate<<','<<a.sum<<'\n';
            const auto b=batches(r,n,operations,4096);std::cout<<"batch,"<<r.name<<','<<n<<','<<repeat<<','<<b.rate<<','<<b.sum<<'\n';
        }
    }
    return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;} }
