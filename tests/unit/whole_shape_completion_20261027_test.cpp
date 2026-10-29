#include "whole_shape_traversal.h"
#include <cstdio>
#include <stdexcept>
#include <vector>
#include <limits>

using whole_shaped::Plan;
using shaped::Word;
using shaped::Result;
using shaped::Config;
using shaped::Kind;
namespace d=shaped::detail;
static uint64_t checks=0,cases=0,slots=0,windows=0;
static void require(bool x,const char* message){++checks;if(!x)throw std::runtime_error(message);}
static Word plus(const Word& a,uint64_t b){Word out{};require(d::add(a,d::from_u64(b),out),"bounded arithmetic");return out;}
static uint64_t small(const Word& x){uint64_t n=0;require(d::less_than_u64(x,UINT64_MAX,n),"small test domain");return n;}
static bool same(const Result& a,const Result& b){return a.start==b.start&&a.center==b.center&&a.phase_base==b.phase_base&&a.count==b.count&&a.phase==b.phase&&a.step==b.step&&a.first_skipped==b.first_skipped;}
static void exhaustive(Config c){
    Plan plan;require(plan.init(c),"init");++cases;
    const uint64_t family=small(plan.total()),complete=small(plan.complete_total());
    require(complete>=family,"complete domain contains original family");
    struct Window{Word begin{},end{};};std::vector<Window> emitted;
    uint64_t total_keys=0;
    for(uint64_t i=0;i<complete;++i){
        ++slots;Result r;
        require(plan.get_complete(d::from_u64(i),r),"every complete ordinal admitted");
        if(i<family){Result before;require(plan.get(d::from_u64(i),before),"family ordinal");require(same(r,before),"original family prefix byte fields preserved");}
        if(!r.count)continue;
        ++windows;require(d::compare(r.start,c.lower)>=0,"lower boundary");
        Word end=plus(r.start,r.count-1);
        require(d::compare(end,c.upper)<=0,"upper boundary");
        emitted.push_back({r.start,end});total_keys+=r.count;
    }
    std::sort(emitted.begin(),emitted.end(),[](const Window& a,const Window& b){return a.begin<b.begin;});
    require(!emitted.empty(),"nonempty interval covered");
    require(emitted.front().begin==c.lower,"coverage begins at exact lower bound");
    require(emitted.back().end==c.upper,"coverage reaches exact upper bound");
    for(size_t i=1;i<emitted.size();++i)require(plus(emitted[i-1].end,1)==emitted[i].begin,"no gaps or repeated keys between windows");
    require(total_keys==small(d::subtract(c.upper,c.lower))+1,"exact inclusive key cardinality");
    Result end;require(!plan.get_complete(plan.complete_total(),end),"exhaustion only after complete domain");
}
static Config bounded(Kind kind,unsigned n,unsigned top,uint64_t lower,uint64_t upper,uint64_t anchor,uint64_t seed,bool fixed=false){
    Config c;c.kind=kind;c.low_nibbles=n;c.top_index=top;c.lower=d::from_u64(lower);c.upper=d::from_u64(upper);c.anchor=d::from_u64(anchor);c.seed=seed;c.fixed_wave=fixed;return c;
}
int main(){try{
    for(Kind kind:{Kind::Wave,Kind::Zone})for(uint64_t seed:{0ULL,19ULL,8271ULL}){
        exhaustive(bounded(kind,1,61,1,1023,513,seed));
        exhaustive(bounded(kind,1,60,3,65530,0x8123,seed));
        exhaustive(bounded(kind,2,60,0x1801,0x4abd,0x2f29,seed));
        exhaustive(bounded(kind,3,60,0x8123,0xefba,0xa338,seed));
        exhaustive(bounded(kind,4,63,0x51231,0x51235,0x51233,seed));
        exhaustive(bounded(kind,15,63,0x1231,0x1239,0x1235,seed));
        Config high=bounded(kind,1,60,3,65530,0x8123,seed);
        for(unsigned p=0;p<60;++p){d::set_nibble(high.lower,p,15);d::set_nibble(high.upper,p,15);d::set_nibble(high.anchor,p,15);}
        exhaustive(high);
    }
    for(uint64_t seed:{0ULL,8271ULL}){
        exhaustive(bounded(Kind::Wave,1,61,1,1023,0x217,seed,true));
        exhaustive(bounded(Kind::Wave,2,60,3,65530,0x8123,seed,true));
        exhaustive(bounded(Kind::Wave,2,60,0x1801,0x4abd,0x2f29,seed,true));
        exhaustive(bounded(Kind::Wave,2,61,3,4000,37,seed,true));
        exhaustive(bounded(Kind::Wave,4,63,1,9,5,seed,true));
        Config near_max=bounded(Kind::Wave,2,60,3,65535,0x8123,seed,true);
        for(unsigned p=0;p<60;++p){d::set_nibble(near_max.lower,p,15);d::set_nibble(near_max.upper,p,15);d::set_nibble(near_max.anchor,p,15);}
        exhaustive(near_max);
    }
    uint64_t random=8271;
    for(unsigned i=0;i<240;++i){
        random=d::mix(random);const uint64_t lower=1+(random%UINT64_C(120000));
        random=d::mix(random);const uint64_t upper=lower+random%UINT64_C(16000);
        random=d::mix(random);const uint64_t anchor=lower+random%(upper-lower+1);
        random=d::mix(random);const unsigned low=1+unsigned(random%15);
        const Kind kind=(i&1)?Kind::Zone:Kind::Wave;
        Config c=bounded(kind,low,0,lower,upper,anchor,random,kind==Kind::Wave&&(i%4==0));
        c.top_index=63;
        for(unsigned p=0;p<64;++p)if(d::nibble(c.lower,p)!=d::nibble(c.upper,p)){c.top_index=p;break;}
        exhaustive(c);
    }
    // Exact 256-bit arithmetic: a domain whose slot count cannot fit uint64.
    for(Kind kind:{Kind::Wave,Kind::Zone}){
        Config c;c.kind=kind;c.low_nibbles=1;c.top_index=0;c.lower=Word{};c.upper.fill(255);c.anchor=d::from_u64(7);c.seed=19;
        Plan p;require(p.init(c),"full unsigned-256 interval init");
        require(d::compare(p.complete_total(),p.total())>0,"large completion suffix exists");
        Result r;require(p.get_complete(d::subtract(p.complete_total(),d::from_u64(1)),r),"last uint256 slot");
        require(d::compare(r.start,c.upper)<=0,"last unsigned window does not wrap");
        require(!p.get_complete(p.complete_total(),r),"uint256 exhaustion boundary");
        if(kind==Kind::Wave){c.fixed_wave=true;c.anchor=d::from_u64(7);Plan fixed;require(fixed.init(c),"full uint256 shifted init");require(fixed.get_complete(fixed.total(),r),"shifted prefix exists");require(r.start==Word{}&&r.count==7,"shifted prefix exact seven keys");}
    }
    std::printf("{\"result\":\"PASS\",\"checks\":%llu,\"cases\":%llu,\"slots\":%llu,\"windows\":%llu,\"gpu_executed\":false}\n",(unsigned long long)checks,(unsigned long long)cases,(unsigned long long)slots,(unsigned long long)windows);return 0;
}catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s after %llu checks, completed cases %llu\n",e.what(),(unsigned long long)checks,(unsigned long long)cases);return 1;}}
