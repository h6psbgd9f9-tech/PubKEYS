// Standalone CPU audit of the current one-nibble zone walk.
// Synthetic inputs only; links with zone_search.cpp and platform.cpp.
#include "zone_search.h"
#include "batch_prefetch.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <set>

using namespace pkh_zone;
namespace {
uint64_t checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { if (failures < 20) std::cerr << "FAIL " << __LINE__ << ": " << #x << '\n'; ++failures; } } while (0)
be256 number(uint64_t value) { be256 key; be256_from_u64(value, &key); return key; }
uint64_t small(const be256& key) {
    uint64_t value = 0; for (int i = 24; i < 32; ++i) value = (value << 8) | key.b[i]; return value;
}
uint64_t small(const uint8_t* key) { be256 k; std::memcpy(k.b,key,32); return small(k); }
int digit(const be256& key, int nib) {
    return (key.b[31-nib/2] >> (nib%2 ? 4 : 0)) & 15;
}
void replace(be256& key, int nib, int value) {
    const int byte=31-nib/2, shift=nib%2?4:0;
    key.b[byte]=static_cast<uint8_t>((key.b[byte]&~(15<<shift))|(value<<shift));
}
bool setup(ZoneSearch& search, uint64_t low, uint64_t high, int skip, uint64_t seed=173) {
    auto l=number(low),h=number(high),s=number(seed); std::string why;
    return search.init(skip,l.b,h.b,s.b,"synthetic-zone-audit",why);
}
bool same(const ZoneBatch& a,const ZoneBatch& b) {
    if (a.keys!=b.keys || a.full_starts!=b.full_starts || a.pieces.size()!=b.pieces.size() ||
        a.next.sequence!=b.next.sequence || a.next.offset!=b.next.offset ||
        a.next.exhausted!=b.next.exhausted || be256_cmp(&a.next.current,&b.next.current) ||
        !std::equal(a.next.rng,a.next.rng+4,b.next.rng)) return false;
    for (size_t i=0;i<a.pieces.size();++i)
        if (a.pieces[i].count!=b.pieces[i].count ||
            std::memcmp(a.pieces[i].key,b.pieces[i].key,32) ||
            a.pieces[i].info.ordinal!=b.pieces[i].info.ordinal) return false;
    return true;
}
void masks_and_steps() {
    std::mt19937_64 random(0x20261022);
    for (int sample=0;sample<900;++sample) {
        be256 lo{},hi{},seed{};
        for (int j=0;j<28;++j) lo.b[j]=static_cast<uint8_t>(random());
        hi=lo; seed=number(random());
        const uint64_t low=1+random()%60000, high=low+1+random()%5000;
        for (int j=31;j>=28;--j) {lo.b[j]=static_cast<uint8_t>(low >> (8*(31-j))); hi.b[j]=static_cast<uint8_t>(high >> (8*(31-j)));}
        ZoneTraversal walk; walk.init(sample%5,lo,hi,seed);
        for (int round=0;round<24;++round) {
            be256 begin,end;ZoneInfo info;
            const uint64_t count=walk.bounds(begin,end,info);
            CHECK(count && be256_cmp(&begin,&lo)>=0 && be256_cmp(&end,&hi)<=0);
            be256 width; be256_sub(&end,&begin,&width); CHECK(small(width)==count);
            for(int n=0;n<walk.skip;++n) CHECK(digit(walk.current,n)==0);
            uint16_t masks[64]{};const uint64_t available=walk.replacement_masks(masks);
            be256 anchor=lo;for(int n=0;n<walk.skip;++n)replace(anchor,n,0);
            uint64_t expected_positions=0;
            for(int nib=0;nib<64;++nib) {
                uint16_t expected=0;
                for(int value=0;value<16;++value) {
                    if(value==digit(walk.current,nib))continue;
                    be256 candidate=walk.current;replace(candidate,nib,value);
                    if(be256_cmp(&candidate,&anchor)>=0 && be256_cmp(&candidate,&hi)<0)
                        expected|=uint16_t(1u<<value);
                }
                CHECK(masks[nib]==expected);
                if(expected)expected_positions|=UINT64_C(1)<<nib;
            }
            CHECK(available==expected_positions);
            // The optimized rectangle/interior path must make the exact same
            // RNG draws and return the exact same neighbour as brute masks.
            ZoneTraversal reference=walk;
            const uint64_t eligible=available&~((UINT64_C(1)<<walk.skip)-1);
            be256 expected=walk.current;
            if(eligible) {
                const int nib=ZoneTraversal::selected_bit(eligible,reference.random_below(ZoneTraversal::popcount(eligible)));
                const int value=ZoneTraversal::selected_bit(masks[nib],reference.random_below(ZoneTraversal::popcount(masks[nib])));
                replace(expected,nib,value);
            }
            const be256 previous=walk.current;
            const bool moved=walk.advance(); CHECK(moved==bool(eligible));
            if(!moved)break;
            CHECK(be256_cmp(&expected,&walk.current)==0);
            CHECK(std::equal(reference.rng,reference.rng+4,walk.rng));
            int differences=0;for(int nib=0;nib<64;++nib)differences+=digit(previous,nib)!=digit(walk.current,nib);
            CHECK(differences==1 && walk.last_position>=0 && walk.last_position<64-walk.skip);
            CHECK(be256_cmp(&previous,&walk.last_base)==0);
        }
    }
}
void lower_edge_regression() {
    // Every canonical block must remain reachable, including the lower partial
    // block. The old clamp-to-lo permanently removed it for most random seeds.
    for(uint64_t seed=1;seed<=80;++seed) {
        ZoneTraversal walk;walk.init(1,number(0x107),number(0x200),number(seed));
        bool saw_lower=false;std::set<uint64_t> bases;
        for(int round=0;round<300;++round) {
            be256 begin,end;ZoneInfo info;const uint64_t count=walk.bounds(begin,end,info);
            CHECK(count && small(begin)>=0x107 && small(end)<=0x200);
            CHECK(small(walk.current)%16==0);
            bases.insert(small(walk.current));
            if(small(begin)==0x107) {saw_lower=true;CHECK(count==9 && small(end)==0x110);}
            CHECK(walk.advance());
        }
        CHECK(saw_lower && bases.size()==16);
    }
}
void distribution(int skip) {
    // 64 mutable positions, including both end positions of the 256-bit key.
    be256 lo{},hi{};hi.b[0]=0xf0;
    ZoneTraversal walk;walk.init(skip,lo,hi,number(173));CHECK(walk.step());
    std::array<uint64_t,64> positions{};
    uint64_t values[64][16]{};
    const uint64_t samples=1000000;
    for(uint64_t round=0;round<samples;++round) {
        CHECK(walk.advance());const int nib=63-walk.last_position;
        CHECK(nib>=0 && nib<64 && walk.last_value>=0 && walk.last_value<16);
        ++positions[nib];++values[nib][walk.last_value];
    }
    uint64_t smallest=samples,largest=0;
    double chi2=0,max_sigma=0,value_chi2=0,value_max_sigma=0;
    uint64_t value_min=samples,value_max=0;
    unsigned value_df=0;
    const int count=64-skip;
    const double expected=double(samples)/count;
    for(int nib=0;nib<skip;++nib)CHECK(positions[nib]==0);
    for(int nib=skip;nib<64;++nib) {
        smallest=(std::min)(smallest,positions[nib]);largest=(std::max)(largest,positions[nib]);
        const double error=double(positions[nib])-expected;
        chi2+=error*error/expected;
        max_sigma=(std::max)(max_sigma,std::abs(error)/std::sqrt(expected*(1-1.0/count)));
        CHECK(positions[nib]>samples/count*92/100 && positions[nib]<samples/count*108/100);
        const int alphabet=nib==63?15:16;
        value_df+=alphabet-1;
        for(int value=0;value<alphabet;++value) {
            const double mean=double(positions[nib])/alphabet,delta=double(values[nib][value])-mean;
            value_chi2+=delta*delta/mean;
            value_max_sigma=(std::max)(value_max_sigma,std::abs(delta)/std::sqrt(mean*(1-1.0/alphabet)));
            value_min=(std::min)(value_min,values[nib][value]);value_max=(std::max)(value_max,values[nib][value]);
            CHECK(values[nib][value]>positions[nib]/alphabet*70/100 &&
                  values[nib][value]<positions[nib]/alphabet*130/100);
        }
        if(nib==63)CHECK(values[nib][15]==0);
    }
    std::cout<<"Position distribution: 1000000 steps / "<<count<<" positions (skip="<<skip<<"); min="<<smallest<<", max="<<largest
             <<"; expected="<<expected<<"; chi2/df="<<chi2<<'/'<<count-1<<"; max_sigma="<<max_sigma<<'\n';
    std::cout<<"Replacement values (top 0..e, other "<<count-1<<" positions 0..f): min="<<value_min<<", max="<<value_max
             <<"; per-position conditional expected=position_count/alphabet; chi2/df="<<value_chi2<<'/'<<value_df<<"; max_sigma="<<value_max_sigma<<'\n';
}
void irregular_distribution() {
    // Freeze a nonrectangular state so all draws see the same domains: three
    // positions with 15, 10 and 1 possible replacements. Position weight must
    // stay equal even though the replacement-domain sizes are different.
    ZoneTraversal walk;walk.init(0,number(0x107),number(0x2b5),number(173));
    walk.current=number(0x215);walk.started=true;walk.sequence=1;
    const auto fixed=walk.current;uint64_t positions[3]{},values[3][16]{};
    uint16_t masks[64];const auto valid=walk.replacement_masks(masks);CHECK(valid==7);
    CHECK(ZoneTraversal::popcount(masks[0])==15 && ZoneTraversal::popcount(masks[1])==10 && ZoneTraversal::popcount(masks[2])==1);
    for(int round=0;round<300000;++round) {
        walk.current=fixed;CHECK(walk.step());const int nib=63-walk.last_position;CHECK(nib>=0 && nib<3);
        if(nib>=0 && nib<3){++positions[nib];++values[nib][walk.last_value];}
    }
    double chi2=0,max_sigma=0;
    for(int nib=0;nib<3;++nib) {
        const double error=double(positions[nib])-100000;chi2+=error*error/100000;
        max_sigma=(std::max)(max_sigma,std::abs(error)/std::sqrt(100000*2.0/3));
        CHECK(positions[nib]>98000 && positions[nib]<102000);
        const int alphabet=ZoneTraversal::popcount(masks[nib]);
        for(int value=0;value<16;++value) {
            if(masks[nib]&(1u<<value))CHECK(values[nib][value]>positions[nib]/alphabet*90/100 && values[nib][value]<positions[nib]/alphabet*110/100);
            else CHECK(values[nib][value]==0);
        }
    }
    std::cout<<"Irregular-state positions (domains 15/10/1): "<<positions[0]<<'/'<<positions[1]<<'/'<<positions[2]
             <<"; expected=100000; chi2/df="<<chi2<<"/2; max_sigma="<<max_sigma<<'\n';
}
void batches_and_checkpoints(const std::filesystem::path& directory) {
    std::string why;
    for(auto interval:{std::pair<uint64_t,uint64_t>{1,1},{0x107,0x1ff},{123,171},{4000,4200},{1,1048575}})
    for(int skip:{0,1,3,15}) {
        ZoneSearch serial,pipeline,resumed;
        CHECK(setup(serial,interval.first,interval.second,skip));
        CHECK(setup(pipeline,interval.first,interval.second,skip));
        CHECK(setup(resumed,interval.first,interval.second,skip));
        const auto path=(directory/"checkpoint.dat").u8string();
        const bool saved=serial.save(path,why); if(!saved)std::cerr<<"Checkpoint save: "<<why<<'\n';CHECK(saved);CHECK(resumed.load(path,why));
        BatchPrefetch<ZoneSearch,ZoneBatch> queue(pipeline,11,7,true);
        for(int round=0;round<12;++round) {
            ZoneBatch a,b;CHECK(serial.prepare_batch(11,7,a,why));CHECK(queue.take(b,why));CHECK(same(a,b));
            const auto state=pipeline.status();queue.start_after(b);
            CHECK(pipeline.status().sequence==state.sequence && pipeline.status().version==state.version);
            uint64_t total=0;std::vector<uint8_t> full;
            for(const auto& piece:b.pieces) {
                const uint64_t first=small(piece.key);
                CHECK(piece.count && first>=interval.first && first<=interval.second && piece.count-1<=interval.second-first);
                total+=piece.count;if(piece.count==7)full.insert(full.end(),piece.key,piece.key+32);
            }
            CHECK(total==b.keys && full==b.full_starts);
            CHECK(serial.commit_batch(a,why));CHECK(pipeline.commit_batch(b,why));CHECK(!pipeline.commit_batch(b,why));
            CHECK(pipeline.save(path,why));CHECK(resumed.load(path,why));
            ZoneBatch c,d;CHECK(resumed.prepare_batch(17,11,c,why));CHECK(pipeline.prepare_batch(17,11,d,why));CHECK(same(c,d));
            if(b.next.exhausted)break;
        }
        ZoneBatch a,b;CHECK(pipeline.prepare_batch(1,3,a,why));CHECK(pipeline.prepare_after(a,1,3,b,why));
        pipeline.reset();CHECK(!pipeline.commit_batch(b,why));CHECK(!serial.commit_batch(a,why));
    }
    // Inclusive high endpoint and the curve-order ceiling are accepted exactly.
    ZoneSearch search;ZoneBatch batch;CHECK(setup(search,100,100,15));
    CHECK(search.prepare_batch(4,16,batch,why));CHECK(batch.keys==1 && batch.pieces.size()==1 && small(batch.pieces[0].key)==100 && batch.next.exhausted);
    be256 maximum{};const uint8_t order_minus_one[32]={0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xfe,0xba,0xae,0xdc,0xe6,0xaf,0x48,0xa0,0x3b,0xbf,0xd2,0x5e,0x8c,0xd0,0x36,0x41,0x40};
    std::memcpy(maximum.b,order_minus_one,32);auto seed=number(1);
    CHECK(search.init(15,maximum.b,maximum.b,seed.b,"ceiling",why));
    CHECK(search.prepare_batch(1,16,batch,why));CHECK(batch.keys==1 && std::memcmp(batch.pieces[0].key,maximum.b,32)==0);
    be256 invalid=maximum;be256_add_u64(&invalid,1);CHECK(!search.init(1,maximum.b,invalid.b,seed.b,"ceiling",why));
    // Standard full private-key interval [1,n-1]: a zero canonical block base
    // is internal state only. The emitted first key remains one; both clipped
    // edge blocks and the maximum configured block size stay within the curve.
    ZoneTraversal full;full.init(15,number(1),invalid,seed);
    full.current=number(0);full.started=true;full.sequence=1;
    be256 begin,end;ZoneInfo info;const uint64_t low_count=full.bounds(begin,end,info);
    CHECK(low_count==(UINT64_C(1)<<60)-1 && small(begin)==1 && small(end)==(UINT64_C(1)<<60));
    CHECK(full.step());for(int nib=0;nib<15;++nib)CHECK(digit(full.current,nib)==0);
    full.current=maximum;for(int nib=0;nib<15;++nib)replace(full.current,nib,0);
    const uint64_t high_count=full.bounds(begin,end,info);
    CHECK(high_count && be256_cmp(&end,&invalid)==0 && be256_cmp(&begin,&maximum)<=0);
    CHECK(full.step());CHECK(be256_cmp(&full.current,&invalid)<0);
    // The changed lower-block grid deliberately has a new checkpoint namespace.
    // A synthetic old-format file is rejected; no existing user file is read.
    CHECK(setup(search,0x107,0x1ff,1));CHECK(search.checkpoint_path().find("checkpoint_zone_block2_")==0);
    const auto current_path=(directory/"current-format.dat").u8string(),old_path=(directory/"old-format.dat").u8string();
    CHECK(search.save(current_path,why));
    std::ifstream source(current_path,std::ios::binary);std::string old((std::istreambuf_iterator<char>(source)),{});source.close();
    const auto magic=old.find("PKHZONEBLOCK2");CHECK(magic!=std::string::npos);if(magic!=std::string::npos)old[magic+12]='1';
    {std::ofstream target(old_path,std::ios::binary);target<<old;}
    CHECK(!search.load(old_path,why));CHECK(why=="Unsupported checkpoint format");
}
} // namespace
int main(int argc,char** argv) {
    if(argc>1 && std::string(argv[1])=="--edge-only") {lower_edge_regression();}
    else {
        const auto directory=std::filesystem::temp_directory_path()/("pkh-zone-audit-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(directory);
        lower_edge_regression();masks_and_steps();distribution(0);distribution(4);distribution(10);distribution(15);irregular_distribution();batches_and_checkpoints(directory);
        std::cout<<"Temporary synthetic checkpoint: "<<directory.u8string()<<'\n';
    }
    std::cout<<"Zone correctness checks="<<checks<<"; failures="<<failures<<'\n';
    return failures?1:0;
}
