#include "balanced_shape_traversal.h"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <vector>

using balanced_shaped::Word;
using balanced_shaped::Config;
using balanced_shaped::Result;
namespace d=whole_shaped::detail;
static uint64_t checks=0,cases=0,windows=0,wide_turns=0;
static void require(bool ok,const char* message){++checks;if(!ok)throw std::runtime_error(message);}
static Word word(uint64_t x){return d::from_u64(x);}
static uint64_t number(const Word& x){uint64_t result=0;for(unsigned i=0;i<24;++i)require(!x[i],"small word");for(unsigned i=24;i<32;++i)result=(result<<8)|x[i];return result;}
static unsigned length(uint64_t x){unsigned bits=0;while(x){++bits;x>>=1;}return bits;}
static Word pow2(unsigned bit){Word result{};result[31-bit/8]=uint8_t(1u<<(bit%8));return result;}
static Word add_small(Word x,uint64_t value){Word result{};require(d::add(x,word(value),result),"addition fits");return result;}
static unsigned read_digit(const Word& x,unsigned p){return (p&1)?(x[p/2]&15):(x[p/2]>>4);}
static void write_digit(Word& x,unsigned p,unsigned digit){digit%=16;if(p&1)x[p/2]=uint8_t((x[p/2]&240)|digit);else x[p/2]=uint8_t((x[p/2]&15)|(digit<<4));}

// Independent archive arithmetic; does not call whole_shaped offset/get helpers.
static Word original_raw(const Word& center,unsigned top,unsigned low,
                         whole_shaped::Kind kind,unsigned state) {
    Word out=center;if(!state)return out;
    const unsigned phase=(state-1)/15+1,step=(state-1)%15+1,last=63-low;
    if(kind==whole_shaped::Kind::Zone){const unsigned p=top+phase;write_digit(out,p,read_digit(out,p)+step);}
    else for(unsigned j=0;j<phase;++j){const unsigned p=last-j;write_digit(out,p,read_digit(out,p)+step+15*(phase-1-j));}
    return out;
}
static void verify_pattern(const whole_shaped::Plan& plan,const Config& cfg,
                           unsigned top,const Word& base_ordinal) {
    Result center;require(plan.get(base_ordinal,center),"base exists");
    for(unsigned state=0;state<plan.pattern_states();++state){
        Result got;require(plan.get(add_small(base_ordinal,state),got),"state exists");
        require(got.center==center.center,"one complete base stays intact");
        require(got.phase==(state?(state-1)/15+1:0),"phase sequence original");
        require(got.step==(state?(state-1)%15+1:0),"15 independent digit steps");
        const Word raw=original_raw(center.center,top,cfg.low_nibbles,cfg.kind,state);
        Word start=raw;uint64_t count=0,block=UINT64_C(1)<<(4*cfg.low_nibbles);
        if(d::compare(raw,cfg.upper)<=0){
            if(d::compare(start,cfg.lower)<0)start=cfg.lower;
            const Word gap=d::subtract(start,raw);uint64_t clipped=0;
            if(d::less_than_u64(gap,block,clipped)){
                count=block-clipped;uint64_t rest=0;
                if(d::less_than_u64(d::subtract(cfg.upper,start),count,rest))count=rest+1;
            }
        }
        require(got.count==count,"archive shape clipping count");
        if(count)require(got.start==start,"archive digit arithmetic preserved");
    }
}
static Config config(uint64_t lo,uint64_t hi,unsigned low,whole_shaped::Kind kind,uint64_t seed){
    Config cfg;cfg.lower=word(lo);cfg.upper=word(hi);cfg.anchor=word(lo+(hi-lo)/2);
    cfg.low_nibbles=low;cfg.top_index=length(hi)?64-(length(hi)+3)/4:63;cfg.kind=kind;cfg.seed=seed;return cfg;
}
static void bounded(uint64_t lo,uint64_t hi,unsigned low,whole_shaped::Kind kind,uint64_t seed){
    balanced_shaped::Plan plan;const Config cfg=config(lo,hi,low,kind,seed);
    require(plan.init(cfg),"balanced init");require(!plan.fixed_start_passthrough(),"balanced enabled");
    const uint64_t count=number(plan.complete_total());
    struct Interval{uint64_t lo,hi;};std::vector<Interval> emitted;
    for(uint64_t i=0;i<count;++i){Result r;unsigned bits=999;require(plan.get_complete(word(i),r,&bits),"ordinal valid");if(!r.count)continue;
        const uint64_t a=number(r.start),b=a+r.count-1;
        require(a>=lo&&b<=hi,"inside original range");require(length(a)==bits&&length(b)==bits,"inside declared bit length");
        emitted.push_back({a,b});++windows;
    }
    std::sort(emitted.begin(),emitted.end(),[](const Interval&a,const Interval&b){return a.lo<b.lo;});
    uint64_t next=lo;for(const auto& r:emitted){require(r.lo==next,"exact union no gaps or repeats");next=r.hi+1;}
    require(next==hi+1,"whole interval covered");Result end;require(!plan.get_complete(word(count),end),"exhaustion exact");
    for(size_t b=0;b<plan.bucket_count();++b){Config local=cfg;plan.bucket_bounds(b,local.lower,local.upper);verify_pattern(plan.bucket_plan(b),local,plan.bucket_top_index(b),word(0));}
    ++cases;
}
static void wide(whole_shaped::Kind kind){
    Config cfg;cfg.lower=pow2(139);cfg.upper.fill(255);cfg.anchor=cfg.lower;
    cfg.low_nibbles=4;cfg.top_index=0;cfg.kind=kind;cfg.seed=UINT64_C(0x574156455f313339);
    balanced_shaped::Plan plan;require(plan.init(cfg),"wide init");require(plan.bucket_count()==117,"actual139exponent means140bit first");
    require(plan.bucket_bit_length(0)==140&&plan.bucket_bit_length(116)==256,"no off by one");
    Word global{};
    for(unsigned round=0;round<3;++round)for(size_t b=0;b<plan.bucket_count();++b){
        Result first;unsigned bits=0,top=99;require(plan.get_complete(global,first,&bits,&top),"wide turn");
        require(bits==140+b,"equal cyclic bucket visits");require(top==plan.bucket_top_index(b),"top metadata");
        require(first.phase==0&&first.step==0,"switch only complete base boundary");require(first.count>0,"wide base turn nonempty");
        Config local=cfg;plan.bucket_bounds(b,local.lower,local.upper);
        const Word local_base=word(uint64_t(round)*plan.bucket_pattern_states(b));
        verify_pattern(plan.bucket_plan(b),local,top,local_base);
        for(unsigned state=0;state<plan.bucket_pattern_states(b);++state){
            Result got,direct;unsigned observed=0;
            require(plan.get_complete(add_small(global,state),got,&observed),"whole turn valid");
            require(observed==bits,"no bit switch within pattern");
            require(plan.bucket_plan(b).get_complete(add_small(local_base,state),direct),"local state");
            require(got.start==direct.start&&got.count==direct.count&&got.phase==direct.phase&&got.step==direct.step,"wrapper preserves local order");
        }
        global=add_small(global,plan.bucket_pattern_states(b));++wide_turns;
    }
    // The wrapper is stateless for callers. Preparation threads may request
    // disjoint ordinals concurrently and receive identical answers.
    std::array<Result,512> serial{},parallel{};
    for(unsigned i=0;i<512;++i)require(plan.get_complete(word(i),serial[i]),"serial lookup");
    std::vector<std::thread> helpers;
    for(unsigned t=0;t<4;++t)helpers.emplace_back([&,t]{for(unsigned i=t;i<512;i+=4)plan.get_complete(word(i),parallel[i]);});
    for(auto& thread:helpers)thread.join();
    for(unsigned i=0;i<512;++i)require(serial[i].start==parallel[i].start&&serial[i].count==parallel[i].count,"four-thread deterministic lookup");
    ++cases;
}
static void fixed(){
    Config cfg=config(0x1013,0xaabd,2,whole_shaped::Kind::Wave,7);cfg.fixed_wave=true;cfg.anchor=word(0x3456);
    whole_shaped::Plan original;balanced_shaped::Plan wrapper;
    require(original.init(cfg)&&wrapper.init(cfg),"fixed init");require(wrapper.fixed_start_passthrough(),"explicit fixed-start policy");
    require(original.complete_total()==wrapper.complete_total(),"fixed total unchanged");
    for(uint64_t i=0;i<number(original.complete_total());++i){Result a,b;require(original.get_complete(word(i),a)&&wrapper.get_complete(word(i),b),"fixed states");require(a.start==b.start&&a.count==b.count&&a.phase==b.phase&&a.step==b.step,"fixed exact passthrough");}
    ++cases;
}
int main(int argc,char**argv){
    try{
        for(auto kind:{whole_shaped::Kind::Wave,whole_shaped::Kind::Zone})for(unsigned low=1;low<=4;++low)for(uint64_t seed:{0,19,8271}){
            bounded(1,4095,low,kind,seed);bounded(53,10037,low,kind,seed);bounded(0,273,low,kind,seed);
        }
        wide(whole_shaped::Kind::Wave);wide(whole_shaped::Kind::Zone);fixed();
        const std::string report="{\n  \"Result\":\"PASS\",\n  \"Checks\":"+std::to_string(checks)+",\n  \"Cases\":"+std::to_string(cases)+",\n  \"NonemptyWindows\":"+std::to_string(windows)+",\n  \"WideWholePatternTurns\":"+std::to_string(wide_turns)+",\n  \"WideBuckets\":117,\n  \"FirstBitLength\":140,\n  \"LastBitLength\":256,\n  \"GPUExecuted\":false,\n  \"OriginalDigitArithmeticIndependentlyChecked\":true,\n  \"FixedStartExplicitPassthrough\":true\n}\n";
        if(argc>1){std::ofstream out(argv[1]);out<<report;if(!out)throw std::runtime_error("report write");}
        std::printf("PASS cases=%llu checks=%llu windows=%llu whole-turns=%llu GPU=0\n",(unsigned long long)cases,(unsigned long long)checks,(unsigned long long)windows,(unsigned long long)wide_turns);return 0;
    }catch(const std::exception&e){std::fprintf(stderr,"FAIL %s\n",e.what());return 1;}
}
