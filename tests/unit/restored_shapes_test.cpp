// Isolated CPU test of restored traversal shapes. No application, file-based
// targets/configuration, IPC, networking, CUDA objects or device libraries.
#include "shaped_traversal.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using shaped::Word;
using shaped::Kind;
using shaped::Config;
using shaped::Plan;
using shaped::Result;
uint64_t cases=0, states=0;

void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
Word small(uint64_t value){Word w{};for(int i=31;i>=0;--i){w[i]=uint8_t(value);value>>=8;}return w;}
Word power(unsigned bit){Word w{};require(bit<256,"test power overflow");w[31-bit/8]=uint8_t(1u<<(bit%8));return w;}
Word hex(const char* s){
    require(std::strlen(s)==64,"test hex length");Word w{};
    auto nib=[](char c)->unsigned{if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;throw std::runtime_error("test hex digit");};
    for(unsigned i=0;i<32;++i)w[i]=uint8_t(nib(s[2*i])*16+nib(s[2*i+1]));return w;
}
bool equal(const Word&a,const Word&b){return a==b;}
uint64_t integer(const Word&w){for(unsigned i=0;i<24;++i)require(!w[i],"test value does not fit uint64");uint64_t n=0;for(unsigned i=24;i<32;++i)n=(n<<8)|w[i];return n;}
unsigned digit(const Word&w,unsigned position){return (w[position/2]>>(position%2?0:4))&15;}
void set_digit(Word&w,unsigned position,unsigned value){unsigned shift=position%2?0:4;w[position/2]=uint8_t((w[position/2]&~(15u<<shift))|((value&15)<<shift));}
Word add(Word w,uint64_t amount,bool* overflow=nullptr){
    uint64_t carry=amount;
    for(int i=31;i>=0;--i){const unsigned n=unsigned(w[i])+unsigned(carry&255);w[i]=uint8_t(n);carry=(carry>>8)+(n>>8);}
    if(overflow)*overflow=carry!=0;return w;
}
Word plus(const Word&a,const Word&b){Word out{};unsigned carry=0;for(int i=31;i>=0;--i){unsigned n=unsigned(a[i])+b[i]+carry;out[i]=uint8_t(n);carry=n>>8;}require(!carry,"test addition overflow");return out;}
Word times(Word w,unsigned factor){Word out{};unsigned carry=0;for(int i=31;i>=0;--i){unsigned n=unsigned(w[i])*factor+carry;out[i]=uint8_t(n);carry=n>>8;}require(!carry,"test product overflow");return out;}
uint64_t distance(const Word&a,const Word&b){
    require(!(a<b),"test subtraction underflow");Word w{};int borrow=0;
    for(int i=31;i>=0;--i){int n=int(a[i])-int(b[i])-borrow;borrow=n<0;if(n<0)n+=256;w[i]=uint8_t(n);}return integer(w);
}
unsigned dimensions(const Config&c){const unsigned end=64-c.low_nibbles;const unsigned begin=c.top_index+(c.kind==Kind::Zone?1:0);return end>begin?end-begin:0;}
uint64_t block_size(const Config&c){return uint64_t(1)<<(4*c.low_nibbles);}

// Direct transcription of the archive's mathematical description, independent
// of the new ownership/rank implementation: phase width grows; digits each
// receive completed phases*15 plus the current step, modulo 16 without carry.
Word old_wave(const Word&anchor,unsigned phase,unsigned step,const Config&c){
    Word out=anchor;const unsigned m=dimensions(c);
    for(unsigned j=0;j<m;++j){
        const unsigned previous=phase>j+1?phase-j-1:0;
        const unsigned active=j<phase?step:0;
        const unsigned position=63-c.low_nibbles-j;
        set_digit(out,position,digit(anchor,position)+15*previous+active);
    }
    return out;
}
Word old_zone(const Word&center,unsigned phase,unsigned step,const Config&c){
    Word out=center;
    if(phase){const unsigned position=c.top_index+phase;require(position<64-c.low_nibbles,"zone phase outside original positions");set_digit(out,position,digit(center,position)+step);}
    return out;
}
struct Interval{Word start{};uint64_t count=0;};
Interval clipped(const Word&raw,const Config&c){
    Interval out;if(c.upper<raw)return out;bool overflow=false;
    Word end=add(raw,block_size(c)-1,&overflow);if(overflow)end.fill(255);
    if(c.upper<end)end=c.upper;out.start=raw<c.lower?c.lower:raw;
    if(end<out.start){out.count=0;return out;}out.count=distance(end,out.start)+1;return out;
}
void shape_matches(const Result&r,const Config&c){
    const unsigned m=dimensions(c);
    require(r.first_skipped<=m,"invalid ownership threshold");
    if(!r.phase){require(!r.step,"initial state has step");require(!r.first_skipped,"initial state repeats a previously-owned center");}
    else{require(r.step>=1&&r.step<=15,"step not one of 15 original alternatives");require(r.phase>=1&&r.phase<=m,"phase outside original pattern");
        if(c.kind==Kind::Wave)require(r.phase>r.first_skipped,"wave emitted owned lower phase");
        else require(r.phase<=m-r.first_skipped,"zone emitted owned lower position");
    }
    const Word raw=c.kind==Kind::Wave?old_wave(r.center,r.phase,r.step,c):old_zone(r.center,r.phase,r.step,c);
    const Word phase_base=c.kind==Kind::Wave?old_wave(r.center,r.phase,0,c):r.center;
    const Interval expected=clipped(raw,c);
    require(r.count==expected.count,"shape count does not match independent clipping");
    if(r.count)require(equal(r.start,expected.start),"block does not follow original shape");
    require(equal(r.phase_base,phase_base),"phase base does not follow original formula");
}

void full_space(Kind kind,unsigned m,uint64_t seed){
    ++cases;Config c;c.kind=kind;c.low_nibbles=3;c.seed=seed;
    const unsigned prefix_digits=m+(kind==Kind::Zone?1:0);
    c.top_index=64-c.low_nibbles-prefix_digits;
    const uint64_t blocks=uint64_t(1)<<(4*prefix_digits),size=block_size(c);
    c.lower=small(0);c.upper=small(blocks*size-1);c.anchor=small(((seed*37+11)%blocks)*size);
    Plan plan;require(plan.init(c),"full-space init failed");require(equal(plan.total(),small(blocks)),"full-space ticket total incorrect");
    std::vector<uint64_t> seen;seen.reserve(size_t(blocks));
    for(uint64_t i=0;i<blocks;++i){Result r;require(plan.get(small(i),r),"valid full-space ordinal rejected");++states;
        shape_matches(r,c);require(r.count==size,"full block was truncated");const uint64_t start=integer(r.start);require(start%size==0,"unshifted grid is unaligned");seen.push_back(start/size);
    }
    std::sort(seen.begin(),seen.end());for(uint64_t i=0;i<blocks;++i)require(seen[size_t(i)]==i,"primary repetition or coverage hole");
    Result invalid;require(!plan.get(plan.total(),invalid),"ordinal equal to total accepted");require(!plan.get(add(plan.total(),1),invalid),"ordinal beyond total accepted");
}

void clip_fixture(Kind kind,bool fixed,bool unaligned){
    ++cases;Config c;c.kind=kind;c.low_nibbles=3;c.top_index=60;c.seed=0x99b17;c.fixed_wave=fixed;
    c.lower=small(0x1801);c.upper=small(0x3abd);c.anchor=small(unaligned?0x2345:0x2000);
    Plan plan;require(plan.init(c),"clipping fixture init failed");const uint64_t tickets=integer(plan.total());require(tickets<=256,"unexpected narrow ticket count");
    struct Part{uint64_t start,count;};std::vector<Part> parts;
    for(uint64_t i=0;i<tickets;++i){Result r;require(plan.get(small(i),r),"clipping ordinal rejected");++states;shape_matches(r,c);
        if(r.count){require(!(r.start<c.lower),"below lower bound");Word end=add(r.start,r.count-1);require(!(c.upper<end),"above upper bound");parts.push_back({integer(r.start),r.count});}
    }
    std::sort(parts.begin(),parts.end(),[](const Part&a,const Part&b){return a.start<b.start;});
    require(!parts.empty(),"all selected keys skipped");
    for(size_t i=1;i<parts.size();++i)require(parts[i-1].start+parts[i-1].count<=parts[i].start,"clipped blocks overlap");
    // This tiny interval has one mutable wave digit (or zone top digit), so
    // its one-pattern/family sweep must also cover both clipped edges exactly.
    require(parts.front().start==0x1801,"lower partial block omitted");
    for(size_t i=1;i<parts.size();++i)require(parts[i-1].start+parts[i-1].count==parts[i].start,"hole between clipped blocks");
    require(parts.back().start+parts.back().count==0x3abe,"upper partial block omitted");
    Result invalid;require(!plan.get(plan.total(),invalid),"clipped pass wraps and repeats");
}

Config large_config(Kind kind,unsigned low=3){
    Config c;c.kind=kind;c.low_nibbles=low;c.top_index=0;c.seed=0x78a3c21;
    c.lower=power(250);c.upper=hex("fffffffffffffffffffffffffffffffebaaedce6af48a03bbfd25e8cd0364140");
    c.anchor[0]=0x7a;c.anchor[1]=0x3c;c.anchor[28]=0x12;c.anchor[29]=0x30;
    for(unsigned j=64-low;j<64;++j)set_digit(c.anchor,j,0);
    return c;
}
void fixed_original_wave(unsigned low){
    ++cases;Config c=large_config(Kind::Wave,low);c.fixed_wave=true;Plan p;require(p.init(c),"fixed original wave init failed");
    const unsigned m=dimensions(c);const uint64_t count=1+15*m;require(equal(p.total(),small(count)),"fixed wave does not end after original pattern");
    std::vector<Word> valid;
    for(uint64_t i=0;i<count;++i){Result r;require(p.get(small(i),r),"fixed wave state missing");++states;
        const unsigned phase=i?unsigned((i-1)/15)+1:0,step=i?unsigned((i-1)%15)+1:0;
        require(r.phase==phase&&r.step==step,"fixed wave phase/step order changed");require(equal(r.center,c.anchor),"fixed wave silently changes anchor");
        require(!r.first_skipped,"fixed wave unexpectedly skips original phase");shape_matches(r,c);
        const Interval expected=clipped(old_wave(c.anchor,phase,step,c),c);require(expected.count==r.count,"fixed wave wrong edge handling");if(r.count)valid.push_back(r.start);
    }
    std::sort(valid.begin(),valid.end());require(std::adjacent_find(valid.begin(),valid.end())==valid.end(),"original fixed wave repeats a block");
    Result invalid;require(!p.get(small(count),invalid),"fixed wave repeats its pattern");require(!p.get(small(count*2),invalid),"fixed wave repeats second start");
}

void large_ordinals(Kind kind){
    ++cases;Config c=large_config(kind);Plan p;require(p.init(c),"large range init failed");
    require(small(UINT64_MAX)<p.total(),"large logical space was narrowed to64 bits");
    std::vector<Word> samples={small(0),small(1),power(64),add(power(64),1),power(127),power(200),add(power(200),123)};
    std::vector<Word> valid;
    for(const Word&i:samples){Result r;require(i<p.total(),"test ordinal outside expected large domain");require(p.get(i,r),"large ordinal rejected");++states;shape_matches(r,c);
        if(r.count){require(!(r.start<c.lower),"large state below2^250");require(!(c.upper<add(r.start,r.count-1)),"large state above curve order");valid.push_back(r.start);}
    }
    std::sort(valid.begin(),valid.end());require(std::adjacent_find(valid.begin(),valid.end())==valid.end(),"high ordinal was truncated or repeated");
    Result invalid;require(!p.get(p.total(),invalid),"end of large pass accepted");
    Word max;max.fill(255);require(!p.get(max,invalid),"maximum256-bit ordinal accepted outside finite pass");
}

void same_group_high_counter(Kind kind,unsigned h,unsigned separating_bit){
    ++cases;Config c=large_config(kind);Plan p;require(p.init(c),"same-group init failed");
    const unsigned m=dimensions(c),outer=kind==Kind::Zone?16:1;
    require(h+1<m&&separating_bit<4*h,"bad same-group test parameters");
    // Independent closed-form group prefix. Both ordinals use the SAME
    // highest-coordinate group, phase, step, outer digit and nonzero digit;
    // only the lower center rank differs beyond64/128 bits.
    const Word begin=times(power(4*h),outer*(1+15*(m-h)));
    const unsigned weight=15*(m-h-1);
    const Word delta=times(power(separating_bit),15*outer*weight);
    const Word other=plus(begin,delta);require(other<p.total(),"same-group ordinal exceeds total");
    Result first,second;require(p.get(begin,first)&&p.get(other,second),"same-group ordinal rejected");states+=2;
    require(first.first_skipped==h+1&&second.first_skipped==h+1,"same-group fixture hit wrong ownership group");
    require(first.phase==second.phase&&first.step==second.step,"same-group phase shifted");
    require(!equal(first.center,second.center),"center rank truncated to64/128 bits");
    shape_matches(first,c);shape_matches(second,c);
}

void first_original_zone(unsigned low){
    ++cases;Config c=large_config(Kind::Zone,low);Plan p;require(p.init(c),"zone original-pattern init failed");
    const unsigned m=dimensions(c),width=1+15*m;
    std::vector<bool> seen(width,false);unsigned anchor_states=0;
    // The first zero-center family interleaves the16 possible outer digits.
    // Find the requested anchor's states by their metadata rather than relying
    // on the chosen order of those independent outer domains.
    for(uint64_t i=0;i<uint64_t(width)*16;++i){Result r;require(p.get(small(i),r),"zone first-family state missing");++states;
        if(!equal(r.center,c.anchor))continue;
        require(!r.first_skipped,"first zone center skips a position");
        const unsigned slot=r.phase?(r.phase-1)*15+r.step:0;
        require(slot<width&&!seen[slot],"zone repeats an original anchor state");seen[slot]=true;++anchor_states;
        shape_matches(r,c);
    }
    require(anchor_states==width,"zone did not preserve every original position/alternative");
    require(std::find(seen.begin(),seen.end(),false)==seen.end(),"zone original pattern has missing state");
}

void nonzero_fixed_prefix(Kind kind){
    ++cases;Config c;c.kind=kind;c.low_nibbles=3;c.top_index=59;c.seed=0xabc77;
    c.lower=small(0xabc12000);c.upper=small(0xabc2afff);c.anchor=small(0xabc17000);
    Plan p;require(p.init(c),"nonzero common-prefix init failed");
    struct Part{uint64_t start,count;};std::vector<Part> parts;
    const uint64_t total=integer(p.total());require(total<=256,"nonzero prefix logical domain too large");
    for(uint64_t i=0;i<total;++i){Result r;require(p.get(small(i),r),"nonzero prefix ordinal rejected");++states;shape_matches(r,c);if(r.count)parts.push_back({integer(r.start),r.count});}
    std::sort(parts.begin(),parts.end(),[](const Part&a,const Part&b){return a.start<b.start;});
    require(!parts.empty()&&parts.front().start==0xabc12000,"nonzero prefix lower keys omitted");
    for(size_t i=1;i<parts.size();++i)require(parts[i-1].start+parts[i-1].count==parts[i].start,"nonzero prefix repetition or hole");
    require(parts.back().start+parts.back().count==0xabc2b000,"nonzero prefix upper keys omitted");
}

void zero_leading_nibble(){
    ++cases;Config c=large_config(Kind::Wave);c.fixed_wave=true;c.anchor=c.lower;
    Plan p;require(p.init(c),"lower-bound anchor rejected");Result r;require(p.get(small(0),r),"lower-bound initial state rejected");++states;
    require(r.count==4096&&equal(r.start,c.lower),"lower-bound block is incorrect");require(digit(r.start,0)==0&&digit(r.start,1)==4,"valid leading0/second4 lost");shape_matches(r,c);
}
void tail_only(){
    for(Kind kind:{Kind::Wave,Kind::Zone}){++cases;Config c;c.kind=kind;c.low_nibbles=3;c.top_index=61;c.lower=small(1);c.upper=small(0xffe);c.anchor=small(1);
        Plan p;require(p.init(c),"tail-only init failed");require(equal(p.total(),small(1)),"tail-only range emits duplicate outer top groups");Result r;require(p.get(small(0),r),"tail-only state missing");++states;
        require(equal(r.start,c.lower)&&r.count==0xffe,"tail-only clipping wrong");shape_matches(r,c);require(!p.get(small(1),r),"tail-only pass repeats");}
}

void invalid_configurations(){
    ++cases;Config c=large_config(Kind::Wave);Plan p;c.low_nibbles=16;require(!p.init(c),"64-bit blocksize overflow accepted");
    c=large_config(Kind::Wave);c.top_index=64;require(!p.init(c),"invalid top nibble accepted");
    c=large_config(Kind::Wave);std::swap(c.lower,c.upper);require(!p.init(c),"reversed bounds accepted");
    c=large_config(Kind::Wave);c.low_nibbles=0;require(!p.init(c),"zero block digits accepted");
    c=large_config(Kind::Wave);c.anchor=small(1);require(!p.init(c),"anchor outside selected interval accepted");
    c=large_config(Kind::Zone);c.fixed_wave=true;require(!p.init(c),"fixed-wave flag accepted for zone");
    c=large_config(Kind::Wave);c.top_index=1;require(!p.init(c),"inconsistent immutable prefix accepted");
    Result r;require(!p.get(small(0),r),"failed init retains a prior active plan");
}
}

int main(){
    try{
        for(Kind kind:{Kind::Wave,Kind::Zone}){
            for(unsigned m=0;m<=4;++m)full_space(kind,m,0x37);
            for(unsigned m=1;m<=3;++m){full_space(kind,m,1);full_space(kind,m,0xfedcba);}
            clip_fixture(kind,false,false);large_ordinals(kind);nonzero_fixed_prefix(kind);
            same_group_high_counter(kind,20,64);same_group_high_counter(kind,40,128);
        }
        clip_fixture(Kind::Wave,true,false);clip_fixture(Kind::Wave,true,true);
        fixed_original_wave(3);fixed_original_wave(4);first_original_zone(3);first_original_zone(4);zero_leading_nibble();tail_only();invalid_configurations();
        std::cout<<"PASS cases="<<cases<<" logical_states="<<states<<" gpu_calls=0 app_calls=0\n";return 0;
    }catch(const std::exception&e){std::cerr<<"FAIL case="<<cases<<" after_states="<<states<<": "<<e.what()<<"\n";return 1;}
}
