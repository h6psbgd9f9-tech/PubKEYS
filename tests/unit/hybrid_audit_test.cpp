// Independent synthetic CPU audit: no application globals, GPU, targets or network.
#include "hybrid_search.h"
#include "batch_prefetch.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>

using namespace pkh_hybrid;
namespace {
unsigned checks=0, failures=0;
uint64_t inspected_blocks=0, inspected_pieces=0;
double maximum_chi_square=0;
#define CHECK(test) do { ++checks; if(!(test)) { ++failures; \
    std::cerr << "FAIL " << __LINE__ << ": " << #test << '\n'; } } while(0)
be256 number(uint64_t value) { be256 result; be256_from_u64(value,&result); return result; }
be256 add(be256 value,uint64_t delta) { be256_add_u64(&value,delta); return value; }
bool equal(const be256& a,const be256& b) { return be256_cmp(&a,&b)==0; }
int digit(const be256& key,int nibble) {
    return (key.b[31-nibble/2]>>((nibble%2)*4))&15;
}
void put(be256& key,int nibble,int value) {
    const int index=31-nibble/2, shift=(nibble%2)*4;
    key.b[index]=static_cast<uint8_t>((key.b[index]&~(15<<shift))|(value<<shift));
}
be256 aligned(be256 key,int skip) { for(int i=0;i<skip;++i) put(key,i,0); return key; }
be256 pattern(int variant) {
    be256 value;
    for(int i=0;i<32;++i) value.b[i]=static_cast<uint8_t>(i*53+variant*19);
    value.b[0]=0x50; // Synthetic scalar safely below the curve order.
    return value;
}
std::string contents(const std::filesystem::path& path) {
    std::ifstream stream(path,std::ios::binary);
    return {std::istreambuf_iterator<char>(stream),std::istreambuf_iterator<char>()};
}
bool same_batch(const HybridBatch& a,const HybridBatch& b) {
    if(a.keys!=b.keys || a.full_starts!=b.full_starts || a.pieces.size()!=b.pieces.size() ||
       a.next.sequence!=b.next.sequence || a.next.offset!=b.next.offset ||
       a.next.started!=b.next.started || a.next.exhausted!=b.next.exhausted ||
       !equal(a.next.current,b.next.current) ||
       !std::equal(a.next.rng,a.next.rng+4,b.next.rng)) return false;
    for(size_t i=0;i<a.pieces.size();++i)
        if(a.pieces[i].count!=b.pieces[i].count ||
           a.pieces[i].info.ordinal!=b.pieces[i].info.ordinal ||
           !equal(a.pieces[i].info.raw,b.pieces[i].info.raw) ||
           std::memcmp(a.pieces[i].key,b.pieces[i].key,32)) return false;
    return true;
}
void validate(const HybridBatch& batch,const be256& low,const be256& high,uint64_t piece_keys) {
    uint64_t total=0;
    bool valid=true;
    std::vector<uint8_t> full;
    for(const auto& piece:batch.pieces) {
        ++inspected_pieces;
        be256 start; std::memcpy(start.b,piece.key,32);
        const be256 end=add(start,piece.count-1);
        valid=valid && piece.count>0 && piece.count<=piece_keys &&
            be256_cmp(&start,&low)>=0 && be256_cmp(&end,&high)<=0 &&
            be256_cmp(&end,&start)>=0 && equal(aligned(start,batch.next.skip),piece.info.raw) &&
            equal(aligned(end,batch.next.skip),piece.info.raw);
        total+=piece.count;
        if(piece.count==piece_keys) full.insert(full.end(),piece.key,piece.key+32);
    }
    CHECK(valid && total==batch.keys && full==batch.full_starts);
}
void shifts() {
    // Independent nibble indexing checks byte carries, odd shifts and aliases.
    for(int variant=0;variant<10;++variant) for(int skip=0;skip<=64;++skip) {
        const be256 input=pattern(variant);
        be256 right,left,expected_right{},expected_left{};
        for(int nib=0;nib<64;++nib) {
            if(nib+skip<64) put(expected_right,nib,digit(input,nib+skip));
            if(nib>=skip) put(expected_left,nib,digit(input,nib-skip));
        }
        HybridTraversal::shift_right(input,skip,right);
        HybridTraversal::shift_left(input,skip,left);
        CHECK(equal(right,expected_right) && equal(left,expected_left));
        right=left=input;
        HybridTraversal::shift_right(right,skip,right);
        HybridTraversal::shift_left(left,skip,left);
        CHECK(equal(right,expected_right) && equal(left,expected_left));
    }
}
void boundary_blocks() {
    for(int skip=3;skip<=15;++skip) {
        const uint64_t span=UINT64_C(1)<<(skip*4);
        for(int variant=0;variant<8;++variant) {
            const be256 prefix=aligned(pattern(variant),skip);
            const be256 seam=add(prefix,span), low=add(prefix,span-3), high=add(seam,4);
            const be256 exclusive=add(high,1), seed=number(123+variant*7+skip);
            HybridTraversal walk; walk.init(skip,low,exclusive,seed);
            unsigned hits[2]{}; bool valid=true;
            for(int sample=0;sample<2000;++sample) {
                be256 begin,end; HybridInfo info;
                const uint64_t count=walk.bounds(begin,end,info);
                const bool first=equal(walk.current,prefix);
                const bool second=equal(walk.current,seam);
                valid=valid && (first||second) && count==(first?3:5) &&
                    equal(begin,first?low:seam) && equal(end,first?seam:exclusive) &&
                    info.ordinal==static_cast<uint64_t>(sample);
                ++hits[first?0:1]; ++inspected_blocks;
                valid=valid && walk.advance();
            }
            CHECK(valid && hits[0]>850 && hits[0]<1150 && hits[1]>850 && hits[1]<1150);
            HybridSearch search; std::string why;
            CHECK(search.init(skip,low.b,high.b,seed.b,"hybrid-audit-seam",why));
            for(uint64_t piece_keys:{UINT64_C(1),UINT64_C(2),UINT64_C(3),UINT64_C(5),span,
                                      (std::numeric_limits<uint64_t>::max)()}) {
                HybridBatch batch;
                CHECK(search.prepare_batch(37,piece_keys,batch,why));
                validate(batch,low,high,piece_keys);
                CHECK(search.commit_batch(batch,why));
            }
        }
        for(const be256 key:{number(1),number(span-1),number(span),add(aligned(pattern(2),skip),span-1)}) {
            HybridSearch singleton; HybridBatch batch; const be256 seed=number(skip); std::string why;
            CHECK(singleton.init(skip,key.b,key.b,seed.b,"hybrid-audit-singleton",why));
            CHECK(singleton.prepare_batch(10,span,batch,why));
            CHECK(batch.keys==1 && batch.pieces.size()==1 && batch.next.exhausted &&
                  std::memcmp(batch.pieces[0].key,key.b,32)==0);
            CHECK(singleton.commit_batch(batch,why));
            CHECK(singleton.prepare_batch(10,1,batch,why) && batch.pieces.empty() && batch.keys==0);
        }
    }
}
void nibble_histograms() {
    // Each allowed A is equally likely. Here every non-leading prefix nibble
    // is unconstrained, while the leading nibble has exactly 14 allowed values.
    // 1.3M independent block selections test every selectable nibble for every B width.
    const unsigned samples=100000;
    be256 low{},exclusive{}; low.b[0]=0x10; exclusive.b[0]=0xf0;
    for(int skip=3;skip<=15;++skip) {
        HybridTraversal walk; walk.init(skip,low,exclusive,number(71000+skip));
        unsigned histogram[64][16]{}; unsigned repeats[64]{};
        be256 previous{}; bool valid=true;
        for(unsigned i=0;i<samples;++i) {
            be256 begin,end; HybridInfo info;
            valid=valid && walk.bounds(begin,end,info)==walk.block_keys() &&
                be256_cmp(&begin,&low)>=0 && be256_cmp(&end,&exclusive)<=0;
            for(int nib=0;nib<64;++nib) {
                const int value=digit(walk.current,nib);
                ++histogram[nib][value];
                if(i && value==digit(previous,nib)) ++repeats[nib];
            }
            previous=walk.current; ++inspected_blocks;
            valid=valid && walk.advance();
        }
        CHECK(valid);
        for(int nib=0;nib<64;++nib) {
            if(nib<skip) { CHECK(histogram[nib][0]==samples); continue; }
            const int first=nib==63?1:0, bins=nib==63?14:16;
            const double expected=double(samples)/bins;
            double chi=0;
            for(int value=0;value<16;++value) {
                if(value<first || value>=first+bins) CHECK(histogram[nib][value]==0);
                else { const double delta=histogram[nib][value]-expected; chi+=delta*delta/expected; }
            }
            maximum_chi_square=(std::max)(maximum_chi_square,chi);
            // Generous deterministic regression bound (df <=15); also reject
            // frozen/cyclic adjacent digits that marginal frequencies can hide.
            CHECK(chi<65.0);
            const double repeat_expected=double(samples-1)/bins;
            CHECK(std::abs(double(repeats[nib])-repeat_expected)<8*std::sqrt(repeat_expected));
        }
    }
}
void speculative_and_resume(const std::filesystem::path& directory) {
    for(int skip=3;skip<=15;++skip) {
        const uint64_t span=UINT64_C(1)<<(skip*4);
        const be256 prefix=aligned(pattern(skip),skip), low=add(prefix,span-19);
        const be256 high=add(prefix,span*3+23), seed=number(99+skip);
        HybridSearch serial,pipeline,resumed;
        std::string why;
        CHECK(serial.init(skip,low.b,high.b,seed.b,"hybrid-audit-pipeline",why));
        CHECK(pipeline.init(skip,low.b,high.b,seed.b,"hybrid-audit-pipeline",why));
        CHECK(resumed.init(skip,low.b,high.b,seed.b,"hybrid-audit-pipeline",why));
        const auto left=directory/("serial-"+std::to_string(skip)+".dat");
        const auto right=directory/("pipeline-"+std::to_string(skip)+".dat");
        const uint64_t piece_keys=span/3;
        BatchPrefetch<HybridSearch,HybridBatch> queue(pipeline,17,piece_keys,true);
        for(int attempt=0;attempt<15;++attempt) {
            HybridBatch a,b,c,d;
            CHECK(serial.prepare_batch(17,piece_keys,a,why) && queue.take(b,why) && same_batch(a,b));
            CHECK(pipeline.save(right.u8string(),why)); const auto before=contents(right);
            queue.start_after(b);
            validate(b,low,high,piece_keys);
            CHECK(pipeline.save(right.u8string(),why) && contents(right)==before);
            CHECK(serial.commit_batch(a,why) && pipeline.commit_batch(b,why));
            CHECK(!pipeline.commit_batch(b,why));
            CHECK(serial.save(left.u8string(),why) && pipeline.save(right.u8string(),why) &&
                  contents(left)==contents(right));
            CHECK(resumed.load(right.u8string(),why));
            CHECK(resumed.prepare_batch(31,span/7,c,why) &&
                  pipeline.prepare_batch(31,span/7,d,why) && same_batch(c,d));
            CHECK(!serial.commit_batch(d,why));
        }
        // Both reset and load must invalidate a speculative next generation,
        // even when its version equals the newly committed state's version.
        HybridBatch a,b;
        CHECK(pipeline.prepare_batch(1,piece_keys,a,why));
        CHECK(pipeline.prepare_after(a,1,piece_keys,b,why)); pipeline.reset();
        CHECK(!pipeline.commit_batch(b,why));
        CHECK(pipeline.prepare_batch(1,piece_keys,a,why));
        CHECK(pipeline.prepare_after(a,1,piece_keys,b,why));
        CHECK(pipeline.load(right.u8string(),why) && !pipeline.commit_batch(b,why));
    }
}
void overflow_rejection() {
    be256 low{},high{},seed=number(81); low.b[0]=0x10; high.b[0]=0x70;
    HybridSearch search; HybridBatch batch; std::string why;
    CHECK(search.init(15,low.b,high.b,seed.b,"hybrid-audit-overflow",why));
    CHECK(!search.prepare_batch(32,UINT64_C(1)<<60,batch,why));
    CHECK(batch.pieces.empty() && !search.status().started);
    CHECK(!search.prepare_batch((std::numeric_limits<size_t>::max)(),1,batch,why));
    CHECK(!search.status().started);
}
} // namespace

int main() {
    const auto directory=std::filesystem::temp_directory_path()/
        ("pkh-hybrid-audit-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory);
    shifts(); boundary_blocks(); nibble_histograms(); speculative_and_resume(directory); overflow_rejection();
    std::cout << "Hybrid audit: " << checks << " checks; " << failures << " failures; "
              << inspected_blocks << " synthetic blocks; " << inspected_pieces << " pieces\n"
              << "Worst nibble histogram chi-square: " << maximum_chi_square << " (limit 65)\n"
              << "Synthetic checkpoints: " << directory.u8string() << '\n';
    return failures?1:0;
}
