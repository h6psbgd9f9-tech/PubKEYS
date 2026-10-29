// Independent CPU-only review of complete original nibble neighborhoods.
// No application entry point, GPU, files, settings, database or notifications.
#include "whole_shape_traversal.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace W = whole_shaped;
using Word = W::Word;
uint64_t checks=0, cases=0, states=0, accepted=0;
void require(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
unsigned digit(const Word& key,unsigned p){return p&1U?key[p/2]&15U:key[p/2]>>4;}
void digit(Word& key,unsigned p,unsigned value){value&=15U;if(p&1U)key[p/2]=uint8_t((key[p/2]&240U)|value);else key[p/2]=uint8_t((key[p/2]&15U)|(value<<4));}
int compare(const Word& a,const Word& b){for(unsigned i=0;i<32;++i){if(a[i]<b[i])return -1;if(a[i]>b[i])return 1;}return 0;}
Word number(uint64_t value){Word out{};for(int p=31;p>=24;--p){out[unsigned(p)]=uint8_t(value);value>>=8;}return out;}
bool plus(Word& value,uint64_t amount){unsigned carry=0;for(int p=31;p>=0;--p){const unsigned add=unsigned(amount&255U);amount>>=8;const unsigned x=value[unsigned(p)]+add+carry;value[unsigned(p)]=uint8_t(x);carry=x>>8;}return !carry&&!amount;}
Word times(const Word& value,unsigned multiplier){Word out{};unsigned carry=0;for(int p=31;p>=0;--p){const unsigned v=value[unsigned(p)]*multiplier+carry;out[unsigned(p)]=uint8_t(v);carry=v>>8;}require(!carry,"synthetic ordinal overflow");return out;}
Word shifted(unsigned bit){Word out{};out[31U-bit/8U]=uint8_t(1U<<(bit%8U));return out;}
uint64_t small(const Word& value){for(unsigned i=0;i<24;++i)require(!value[i],"fixture count exceeds uint64");uint64_t n=0;for(unsigned i=24;i<32;++i)n=(n<<8)|value[i];return n;}
Word minus(const Word& a,const Word& b){Word out{};int borrow=0;for(int p=31;p>=0;--p){const int v=int(a[unsigned(p)])-b[unsigned(p)]-borrow;out[unsigned(p)]=uint8_t(v);borrow=v<0;}require(!borrow,"negative synthetic subtraction");return out;}
Word divide(const Word& a,unsigned divisor,unsigned& remainder){Word out{};unsigned r=0;for(unsigned i=0;i<32;++i){const unsigned v=r*256U+a[i];out[i]=uint8_t(v/divisor);r=v%divisor;}remainder=r;return out;}
Word hex(const char* text){Word out{};for(unsigned i=0;i<64;++i){const char c=text[i];const unsigned v=c>='0'&&c<='9'?unsigned(c-'0'):unsigned(c-'a'+10);require(v<16,"synthetic hex digit");digit(out,i,v);}return out;}
std::string prefix(const Word& value,unsigned n){std::string out;for(unsigned i=0;i<n;++i)out.push_back("0123456789abcdef"[digit(value,i)]);return out;}
unsigned dimensions(const W::Config& cfg){const unsigned last=63U-cfg.low_nibbles;if(cfg.kind==W::Kind::Wave)return cfg.top_index<=last?last-cfg.top_index+1U:0U;return cfg.top_index<last?last-cfg.top_index:0U;}
unsigned shape_size(const W::Config& cfg){return 1U+15U*dimensions(cfg);}
uint64_t block_size(const W::Config& cfg){return UINT64_C(1)<<(4U*cfg.low_nibbles);}

struct Window {Word start{};uint64_t count=0;};
Window intersect(Word raw,uint64_t length,const W::Config& cfg){
    Word end=raw;if(!plus(end,length-1U))end.fill(255);
    if(compare(end,cfg.lower)<0||compare(raw,cfg.upper)>0)return {raw,0};
    Word first=compare(raw,cfg.lower)<0?cfg.lower:raw;
    Word last=compare(end,cfg.upper)>0?cfg.upper:end;
    return {first,small(minus(last,first))+1U};
}
bool center_in_range(const Word& center,const W::Config& cfg){return intersect(center,block_size(cfg),cfg).count!=0;}
Word original(const Word& center,const W::Config& cfg,unsigned phase,unsigned step){
    Word out=center;if(!phase)return out;
    if(cfg.kind==W::Kind::Zone){
        const unsigned pos=cfg.top_index+phase;
        digit(out,pos,digit(center,pos)+step);
    }else{
        for(unsigned j=0;j<phase;++j){
            const unsigned pos=63U-cfg.low_nibbles-j;
            digit(out,pos,digit(center,pos)+15U*(phase-1U-j)+step);
        }
    }
    return out;
}
void check_state(const W::Config& cfg,const W::Result& result,unsigned within){
    ++states;
    const unsigned phase=within?1U+(within-1U)/15U:0U;
    const unsigned step=within?1U+(within-1U)%15U:0U;
    if(!center_in_range(result.center,cfg)){
        require(result.count==0,"outside base must not selectively issue its neighboring states");return;
    }
    require(result.phase==phase&&result.step==step,"complete original phase/step sequence");
    require(result.first_skipped==0,"whole neighborhoods must not skip growth phases");
    const Word raw=original(result.center,cfg,phase,step);
    const Word phase_base=original(result.center,cfg,phase,0);
    require(result.phase_base==phase_base,"original zero-step phase base");
    const Window expected=intersect(raw,block_size(cfg),cfg);
    require(result.count==expected.count,"independent range intersection count");
    if(result.count){
        ++accepted;require(result.start==expected.start,"independent original nibble movement");
        Word last=result.start;require(plus(last,result.count-1U),"accepted segment cannot wrap");
        require(compare(result.start,cfg.lower)>=0&&compare(last,cfg.upper)<=0,"accepted segment within closed range");
    }
}
void disjoint(std::vector<Window>& windows){
    std::sort(windows.begin(),windows.end(),[](const Window&a,const Window&b){return compare(a.start,b.start)<0;});
    for(size_t i=1;i<windows.size();++i){Word end=windows[i-1].start;require(plus(end,windows[i-1].count-1U),"window end wrap");require(compare(end,windows[i].start)<0,"duplicate/overlapping candidate windows");}
}
W::Config wide(W::Kind kind,unsigned n,uint64_t seed){
    W::Config cfg;cfg.kind=kind;cfg.low_nibbles=n;cfg.top_index=0;cfg.seed=seed;
    cfg.lower=shifted(250);cfg.upper=hex("fffffffffffffffffffffffffffffffebaaedce6af48a03bbfd25e8cd0364140");
    for(unsigned p=0;p<64;++p)digit(cfg.anchor,p,(p*7U+9U)%16U);
    digit(cfg.anchor,0,7);for(unsigned p=64U-n;p<64;++p)digit(cfg.anchor,p,0);
    return cfg;
}
void complete_bases(const W::Config& cfg,unsigned bases){
    ++cases;W::Plan plan;require(plan.init(cfg),"wide plan initialization");
    const unsigned s=shape_size(cfg);require(compare(times(number(bases),s),plan.total())<=0,"sufficient complete bases");
    std::vector<Window> windows;windows.reserve(size_t(bases)*s);
    std::set<Word> centers;std::set<std::string> high5,high10;
    for(unsigned b=0;b<bases;++b){
        Word ordinal=times(number(b),s);W::Result first;
        require(plan.get(ordinal,first),"first state of base exists");
        require(centers.insert(first.center).second,"distinct complete base centers");
        if(b==0)require(first.center==cfg.anchor,"first complete neighborhood keeps anchor");
        if(b<16){high5.insert(prefix(first.center,5));high10.insert(prefix(first.center,10));}
        for(unsigned i=0;i<s;++i){
            W::Result state;require(plan.get(ordinal,state),"complete base state exists");
            require(state.center==first.center,"one center persists for whole original pattern");
            check_state(cfg,state,i);if(state.count)windows.push_back({state.start,state.count});
            require(plus(ordinal,1),"ordinal increment");
        }
    }
    require(high5.size()>=4&&high10.size()>=4,"early bases must mix upper HEX prefixes instead of clustering");
    W::Result second;require(plan.get(number(s),second),"second full base exists");
    require(prefix(second.center,5)!=prefix(cfg.anchor,5)&&prefix(second.center,10)!=prefix(cfg.anchor,10),"second base changes high prefix for fixed review seeds");
    if(cfg.kind==W::Kind::Zone)require(prefix(second.center,10).substr(1)!=prefix(cfg.anchor,10).substr(1),"zone must change inner upper prefix after one base, not just outer digit");
    disjoint(windows);
}
void seed_invariance(){
    constexpr uint64_t seeds[]={0,1,UINT64_C(0x1020304050607080),UINT64_C(0xffffffffffffffff)};
    for(auto kind:{W::Kind::Wave,W::Kind::Zone})for(unsigned n=3;n<=(kind==W::Kind::Wave?10U:15U);++n){
        ++cases;Word reference{};bool first=true;
        for(auto seed:seeds){W::Plan plan;const auto cfg=wide(kind,n,seed);require(plan.init(cfg),"all production sizes initialize");if(first){reference=plan.total();first=false;}else require(plan.total()==reference,"session seed must not change logical limit");}
    }
}
void large_ranks(){
    for(auto kind:{W::Kind::Wave,W::Kind::Zone})for(unsigned n:{3U,4U,5U,10U}){
        ++cases;auto cfg=wide(kind,n,UINT64_C(0x1020304050607080));W::Plan plan;require(plan.init(cfg),"large rank init");const unsigned s=shape_size(cfg);
        std::set<Word> centers;std::vector<Window>windows;
        for(unsigned bit:{0U,64U,128U}){
            const Word base=bit?shifted(bit):number(0);Word ordinal=times(base,s);require(compare(ordinal,plan.total())<0,"large rank is within family");
            W::Result first;require(plan.get(ordinal,first),"large rank accepted");require(centers.insert(first.center).second,"large counter must not truncate");
            for(unsigned i=0;i<s;++i){W::Result result;require(plan.get(ordinal,result),"large rank full pattern");require(result.center==first.center,"large rank center remains fixed");check_state(cfg,result,i);if(result.count)windows.push_back({result.start,result.count});require(plus(ordinal,1),"large ordinal increment");}
        }
        disjoint(windows);
    }
}
W::Config tiny(W::Kind kind,unsigned m,unsigned outer,unsigned n=3){
    W::Config cfg;cfg.kind=kind;cfg.low_nibbles=n;cfg.seed=UINT64_C(0x1020304050607080);
    const unsigned last=63U-n;cfg.top_index=kind==W::Kind::Wave?last-m+1U:last-m;
    cfg.lower=number(1);cfg.upper=number(0);
    for(unsigned p=cfg.top_index;p<64;++p)digit(cfg.upper,p,15);
    if(kind==W::Kind::Zone){digit(cfg.lower,cfg.top_index,4);digit(cfg.upper,cfg.top_index,4+outer-1);}
    cfg.anchor=cfg.lower;require(plus(cfg.anchor,157),"tiny anchor");return cfg;
}
void tiny_families(){
    for(auto kind:{W::Kind::Wave,W::Kind::Zone})for(unsigned m=1;m<=4;++m){
        ++cases;auto cfg=tiny(kind,m,kind==W::Kind::Zone?4U:1U);W::Plan plan;require(plan.init(cfg),"tiny family init");const uint64_t total=small(plan.total());const unsigned s=shape_size(cfg);require(total&&total%s==0,"whole families have complete shapes only");require(total<=UINT64_C(1048576),"tiny matrix family is bounded");
        std::vector<Window>windows;Word ordinal{};Word center{};
        for(uint64_t i=0;i<total;++i){W::Result result;require(plan.get(ordinal,result),"all tiny family states exist");if(i%s==0)center=result.center;require(result.center==center,"tiny whole center");check_state(cfg,result,unsigned(i%s));if(result.count)windows.push_back({result.start,result.count});require(plus(ordinal,1),"tiny ordinal increment");}
        W::Result end;require(!plan.get(plan.total(),end),"tiny family finite end");disjoint(windows);
    }
}
void fixed_patterns(){
    for(unsigned n:{3U,4U,5U,10U})for(bool unaligned:{false,true}){
        ++cases;auto cfg=wide(W::Kind::Wave,n,UINT64_C(0x1020304050607080));cfg.fixed_wave=true;if(unaligned){cfg.anchor[31]=0x93;cfg.anchor[30]|=1;}
        W::Plan plan;require(plan.init(cfg),"fixed wave init");require(small(plan.total())==shape_size(cfg),"fixed wave exactly one original pattern");
        std::vector<Window>windows;Word ordinal{};for(unsigned i=0;i<shape_size(cfg);++i){W::Result result;require(plan.get(ordinal,result),"fixed state exists");require(result.center==cfg.anchor,"fixed wave must not change center");check_state(cfg,result,i);if(result.count)windows.push_back({result.start,result.count});require(plus(ordinal,1),"fixed ordinal increment");}
        W::Result end;require(!plan.get(ordinal,end),"fixed pattern must finish instead of repeat");disjoint(windows);
    }
}
void narrow_wave_multiple_bases(){
    for(unsigned n:{3U,4U})for(unsigned m=3;m<=6;++m){
        ++cases;auto cfg=tiny(W::Kind::Wave,m,1,n);W::Plan plan;require(plan.init(cfg),"narrow wave certificate init");
        require(plan.free_digits()>0&&plan.parity_digits()<m,"certifiable narrow wave must not be restricted to its first base");
        const uint64_t centers=small(plan.center_count());require(centers>1,"narrow wave has multiple complete centers");
        const unsigned s=shape_size(cfg);require(small(plan.total())==centers*s,"narrow family total derives from complete shapes");
        const W::Config original_cfg=cfg;Word reference=plan.total();for(uint64_t seed:{UINT64_C(0),UINT64_C(1),UINT64_C(0xffffffffffffffff)}){cfg.seed=seed;W::Plan another;require(another.init(cfg)&&another.total()==reference,"narrow wave certificate/domain independent of run seed");}
        std::set<Word>seen_centers;std::vector<Window>windows;
        const unsigned sample_bases=unsigned((std::min)(centers,UINT64_C(32)));
        for(unsigned b=0;b<sample_bases;++b){Word ordinal=times(number(b),s);W::Result first;require(plan.get(ordinal,first),"narrow whole center exists");require(seen_centers.insert(first.center).second,"narrow whole centers distinct");for(unsigned i=0;i<s;++i){W::Result result;require(plan.get(ordinal,result),"narrow whole state exists");require(result.center==first.center,"narrow complete pattern center");check_state(original_cfg,result,i);if(result.count)windows.push_back({result.start,result.count});require(plus(ordinal,1),"narrow ordinal increment");}}
        disjoint(windows);
    }
}
void clipping_cases(){
    for(auto kind:{W::Kind::Wave,W::Kind::Zone}){
        ++cases;auto cfg=tiny(kind,3,kind==W::Kind::Zone?4U:1U);
        require(plus(cfg.lower,32),"partial lower");Word trim=number(67);cfg.upper=minus(cfg.upper,trim);cfg.anchor=cfg.lower;require(plus(cfg.anchor,29),"partial anchor");
        W::Plan plan;require(plan.init(cfg),"partial boundaries init");const auto total=small(plan.total());std::vector<Window>windows;
        for(uint64_t i=0;i<total;++i){W::Result result;require(plan.get(number(i),result),"partial state exists");check_state(cfg,result,unsigned(i%shape_size(cfg)));if(result.count)windows.push_back({result.start,result.count});}disjoint(windows);
    }
    for(auto kind:{W::Kind::Wave,W::Kind::Zone}){
        ++cases;W::Config cfg;cfg.kind=kind;cfg.low_nibbles=4;cfg.top_index=62;cfg.lower=number(123);cfg.upper=number(199);cfg.anchor=number(150);W::Plan plan;require(plan.init(cfg),"range entirely within one low block");require(small(plan.total())==1,"one low block has one state");W::Result result;require(plan.get(number(0),result),"zero-dimension state");require(result.start==cfg.lower&&result.count==77,"zero-dimension partial exact");
    }
}
void edge_boundaries(){
    for(auto kind:{W::Kind::Wave,W::Kind::Zone})for(bool high:{false,true}){
        ++cases;auto cfg=wide(kind,3,UINT64_C(0x1020304050607080));
        cfg.anchor=high?minus(cfg.upper,number(23)):cfg.lower;
        if(!high)require(plus(cfg.anchor,17),"near-low anchor");
        cfg.fixed_wave=kind==W::Kind::Wave;
        W::Plan plan;require(plan.init(cfg),"order/04 boundary initialization");
        W::Result first;require(plan.get(number(0),first)&&first.count,"order/04 initial segment");
        if(high)require(first.count==(cfg.fixed_wave?24U:321U),"n-minus-one clips exactly at order boundary");
        else require(digit(first.start,0)==0&&digit(first.start,1)==4,"leading zero is valid only from 04 boundary here");
        std::vector<Window>windows;
        for(unsigned i=0;i<shape_size(cfg);++i){W::Result result;require(plan.get(number(i),result),"boundary pattern state");check_state(cfg,result,i);if(result.count)windows.push_back({result.start,result.count});}
        disjoint(windows);
    }
}
}
int main(){
    try{
        for(auto kind:{W::Kind::Wave,W::Kind::Zone})for(unsigned n:{3U,4U,5U,10U})for(uint64_t seed:{UINT64_C(0x1020304050607080),UINT64_C(0xfeedbeef12345678)})complete_bases(wide(kind,n,seed),100);
        seed_invariance();large_ranks();tiny_families();fixed_patterns();clipping_cases();edge_boundaries();narrow_wave_multiple_bases();
        std::printf("PASS: %llu checks, %llu cases, %llu complete-pattern states, %llu accepted windows\n",(unsigned long long)checks,(unsigned long long)cases,(unsigned long long)states,(unsigned long long)accepted);return 0;
    }catch(const std::exception&e){std::fprintf(stderr,"FAIL after %llu checks: %s\n",(unsigned long long)checks,e.what());return 1;}
}
