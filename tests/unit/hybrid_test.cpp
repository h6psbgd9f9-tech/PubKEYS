// Standalone CPU tests. Uses synthetic keys and an isolated temporary folder.
#include "hybrid_search.h"
#include "hash.h"
#include "platform.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

using namespace pkh_hybrid;
namespace {
int passed=0, failed=0;
#define CHECK(condition) do { if(condition) ++passed; else { ++failed; \
    std::cerr << "FAIL line " << __LINE__ << ": " << #condition << '\n'; } } while(0)
be256 number(uint64_t v) { be256 out; be256_from_u64(v,&out); return out; }
uint64_t small(const uint8_t b[32]) {
    uint64_t out=0; for(int i=24;i<32;++i) out=(out<<8)|b[i]; return out;
}
bool setup(HybridSearch& search,uint64_t low,uint64_t high,int n=3,
           const std::string& identity="synthetic-test",uint64_t seed=12345) {
    const auto lo=number(low),hi=number(high),s=number(seed); std::string why;
    return search.init(n,lo.b,hi.b,s.b,identity,why);
}
bool same(const HybridBatch& a,const HybridBatch& b) {
    if(a.keys!=b.keys||a.pieces.size()!=b.pieces.size()||
       a.next.offset!=b.next.offset||a.next.sequence!=b.next.sequence||
       a.next.exhausted!=b.next.exhausted||be256_cmp(&a.next.current,&b.next.current)||
       !std::equal(a.next.rng,a.next.rng+4,b.next.rng)) return false;
    for(size_t i=0;i<a.pieces.size();++i)
        if(a.pieces[i].count!=b.pieces[i].count||
           a.pieces[i].info.ordinal!=b.pieces[i].info.ordinal||
           std::memcmp(a.pieces[i].key,b.pieces[i].key,32)) return false;
    return true;
}
std::string read(const std::string& path) {
    std::ifstream file(plat_wide(path).c_str(),std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>());
}
void write(const std::string& path,const std::string& raw) {
    std::ofstream file(plat_wide(path).c_str(),std::ios::binary|std::ios::trunc); file<<raw;
}
std::string hex(const uint8_t* bytes,size_t size) {
    const char* alphabet="0123456789abcdef"; std::string out;
    for(size_t i=0;i<size;++i) { out+=alphabet[bytes[i]>>4]; out+=alphabet[bytes[i]&15]; }
    return out;
}
std::string mutate(std::string text,const std::string& name,const std::string& value) {
    const std::string prefix=name+'='; const auto begin=text.find(prefix);
    const auto end=text.find('\n',begin); text.replace(begin,end-begin,prefix+value);
    const auto checksum=text.find("checksum="); text.resize(checksum);
    uint8_t hash[32]; sha256(reinterpret_cast<const uint8_t*>(text.data()),text.size(),hash);
    return text+"checksum="+hex(hash,32)+'\n';
}
} // namespace

int main() {
    const auto root=std::filesystem::temp_directory_path()/
        ("pkh-hybrid-cpu-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(root);
    const std::string path=(root/"checkpoint.dat").u8string();
    std::string why;
    {
        HybridSearch s; HybridBatch b;
        CHECK(!s.prepare_batch(1,1,b,why));
        CHECK(!setup(s,1,100,2)); CHECK(!setup(s,1,100,16));
        CHECK(!setup(s,0,100)); CHECK(!setup(s,100,1));
        CHECK(!setup(s,1,100,3,"")); CHECK(setup(s,1,100));
        CHECK(!s.prepare_batch(0,1,b,why)); CHECK(!s.prepare_batch(1,0,b,why));
        CHECK(!s.commit_batch(HybridBatch{},why));
    }
    {
        // One A, even when B is much wider than the whole requested interval.
        HybridSearch s; CHECK(setup(s,123,10122,15));
        CHECK(s.status().skip==15 && s.status().single_prefix &&
              s.status().block_keys==(UINT64_C(1)<<60));
        HybridBatch b; CHECK(s.prepare_batch(200,73,b,why));
        uint64_t expected=123; bool contiguous=true;
        for(const auto& piece:b.pieces) {
            contiguous=contiguous && small(piece.key)==expected && piece.info.ordinal==0;
            expected+=piece.count;
        }
        CHECK(contiguous && expected==10123 && b.keys==10000 && b.next.exhausted);
        CHECK(!s.status().started); CHECK(s.commit_batch(b,why));
        CHECK(s.prepare_batch(1,1,b,why) && b.pieces.empty());
        CHECK(s.save(path,why)); HybridSearch restored; CHECK(setup(restored,123,10122,15));
        CHECK(restored.load(path,why) && restored.status().exhausted);
    }
    {
        // A remains fixed across as many scan launches as B requires.
        HybridSearch s; CHECK(setup(s,0x1000,0xffff));
        HybridBatch a,b; CHECK(s.prepare_batch(1,1000,a,why));
        const auto first=small(a.pieces[0].key);
        CHECK(first%4096==0 && a.next.offset==1000 && a.next.sequence==1);
        CHECK(s.commit_batch(a,why)); CHECK(s.prepare_batch(3,1000,b,why));
        bool stable=true; uint64_t next=first+1000;
        for(const auto& piece:b.pieces) {
            stable=stable && small(piece.key)==next && piece.info.ordinal==0;
            next+=piece.count;
        }
        CHECK(stable && b.next.offset==4000 && b.next.sequence==1);
        CHECK(s.commit_batch(b,why)); CHECK(s.prepare_batch(1,1000,b,why));
        CHECK(b.keys==96 && small(b.pieces[0].key)==first+4000 &&
              b.next.sequence==2 && b.next.offset==0);
    }
    {
        // Every block is selected equally, even if one edge contains one key.
        HybridTraversal walk; walk.init(3,number(0x1fff),number(0x4001),number(173));
        unsigned counts[4]{}; bool bounds=true;
        for(int i=0;i<40000;++i) {
            be256 begin,end; HybridInfo info; const uint64_t count=walk.bounds(begin,end,info);
            const uint64_t prefix=small(walk.current.b)>>12;
            if(prefix<1||prefix>4) { bounds=false; break; }
            ++counts[prefix-1];
            const uint64_t expected_begin=(std::max)(UINT64_C(0x1fff),prefix*4096);
            const uint64_t expected_end=(std::min)(UINT64_C(0x4001),(prefix+1)*4096);
            bounds=bounds && small(begin.b)==expected_begin && small(end.b)==expected_end &&
                count==expected_end-expected_begin;
            walk.advance();
        }
        CHECK(bounds); bool uniform=true;
        for(unsigned count:counts) uniform=uniform && count>9500 && count<10500;
        CHECK(uniform);
        CHECK(counts[0]>0 && counts[3]>0);
    }
    {
        // High bits of A are regenerated too; not a one-digit zonal mutation.
        be256 lo{},hi{},seed=number(9981); lo.b[12]=1; hi.b[12]=3;
        HybridTraversal walk; walk.init(15,lo,hi,seed);
        be256 previous{}; bool many=false,in_range=true;
        for(int i=0;i<100;++i) {
            be256 begin,end; HybridInfo info; walk.bounds(begin,end,info);
            in_range=in_range && be256_cmp(&begin,&lo)>=0 && be256_cmp(&end,&hi)<=0;
            int changed=0;
            if(i) for(int nib=15;nib<64;++nib)
                changed+=pkh_zone::ZoneTraversal::digit_at(previous,nib)!=
                         pkh_zone::ZoneTraversal::digit_at(walk.current,nib);
            many=many || changed>1; previous=walk.current; walk.advance();
        }
        CHECK(in_range && many);
    }
    {
        // Scalar order upper edge must be clipped, even with a 60-bit B.
        static const uint8_t last_bytes[32]={
            0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xfe,
            0xba,0xae,0xdc,0xe6,0xaf,0x48,0xa0,0x3b,0xbf,0xd2,0x5e,0x8c,0xd0,0x36,0x41,0x40};
        be256 hi{},lo{},delta=number(500),seed=number(81);
        std::memcpy(hi.b,last_bytes,32); be256_sub(&hi,&delta,&lo);
        HybridSearch s; CHECK(s.init(15,lo.b,hi.b,seed.b,"order-edge",why));
        HybridBatch b; CHECK(s.prepare_batch(100,128,b,why));
        CHECK(b.keys==501 && b.next.exhausted && std::memcmp(b.pieces[0].key,lo.b,32)==0);
        be256_add_u64(&hi,1);
        CHECK(!s.init(15,lo.b,hi.b,seed.b,"order-edge",why));
    }
    {
        // prepare/drop does not move committed A, suffix offset, or RNG.
        HybridSearch s; CHECK(setup(s,0x1ffb,0x123456));
        CHECK(s.save(path,why)); const std::string before=read(path);
        HybridBatch speculative,again; CHECK(s.prepare_batch(20,777,speculative,why));
        CHECK(s.save(path,why) && read(path)==before);
        CHECK(s.prepare_batch(20,777,again,why) && same(speculative,again));
        CHECK(s.commit_batch(speculative,why)); CHECK(!s.commit_batch(again,why));
        HybridSearch other; CHECK(setup(other,0x1ffb,0x123456));
        CHECK(!other.commit_batch(speculative,why));
        CHECK(s.save(path,why)); CHECK(other.load(path,why));
        HybridBatch continued,resumed;
        CHECK(s.prepare_batch(200,900,continued,why));
        CHECK(other.prepare_batch(200,900,resumed,why) && same(continued,resumed));
        CHECK(s.status().offset>0);
        s.reset(); CHECK(!s.status().started); CHECK(!s.commit_batch(continued,why));
        CHECK(s.load(path,why));
        CHECK(s.prepare_batch(200,900,continued,why) && same(continued,resumed));
    }
    {
        // Restore initial state and one clipped prefix before checking tampering.
        HybridSearch s,copy; CHECK(setup(s,0x1fff,0x2000,3,"checkpoint"));
        CHECK(setup(copy,0x1fff,0x2000,3,"checkpoint",999));
        CHECK(s.save(path,why) && copy.load(path,why));
        HybridBatch a,b; CHECK(s.prepare_batch(1,1,a,why));
        CHECK(copy.prepare_batch(1,1,b,why) && same(a,b));
        CHECK(s.commit_batch(a,why) && s.save(path,why));
        const std::string valid=read(path);
        HybridSearch wrong_target,wrong_size,wrong_interval;
        CHECK(setup(wrong_target,0x1fff,0x2000,3,"different"));
        CHECK(!wrong_target.load(path,why));
        CHECK(setup(wrong_size,0x1fff,0x2000,4,"checkpoint"));
        CHECK(!wrong_size.load(path,why));
        CHECK(setup(wrong_interval,0x1ffe,0x2000,3,"checkpoint"));
        CHECK(!wrong_interval.load(path,why));
        CHECK(s.checkpoint_path()!=wrong_target.checkpoint_path() &&
              s.checkpoint_path()!=wrong_size.checkpoint_path() &&
              s.checkpoint_path()!=wrong_interval.checkpoint_path());
        write(path,valid.substr(0,valid.size()/2)); CHECK(!copy.load(path,why));
        std::string corrupted=valid; corrupted[corrupted.find("offset=")+7]='9';
        write(path,corrupted); CHECK(!copy.load(path,why));
        write(path,mutate(valid,"offset","4096")); CHECK(!copy.load(path,why));
        auto invalid=number(0x1001); write(path,mutate(valid,"current",hex(invalid.b,32)));
        CHECK(!copy.load(path,why));
        invalid=number(0x3000); write(path,mutate(valid,"current",hex(invalid.b,32)));
        CHECK(!copy.load(path,why));
        std::string zeros=valid;
        for(int i=0;i<4;++i) zeros=mutate(zeros,"rng"+std::to_string(i),"0000000000000000");
        write(path,zeros); CHECK(!copy.load(path,why));
        write(path,mutate(valid,"exhausted","1")); CHECK(!copy.load(path,why));
        write(path,valid); CHECK(copy.load(path,why));
        CHECK(s.prepare_batch(20,1,a,why) && copy.prepare_batch(20,1,b,why) && same(a,b));
    }
    {
        // Concurrent saves capture committed state, never a partially prepared batch.
        HybridSearch s,copy; CHECK(setup(s,0x1000,0xabcdef));
        CHECK(setup(copy,0x1000,0xabcdef));
        std::atomic<bool> saved_ok{true};
        std::thread saver([&]() { for(int i=0;i<50;++i) {
            std::string error; if(!s.save(path,error)) saved_ok=false;
        }});
        bool commits=true;
        for(int i=0;i<100;++i) {
            HybridBatch b; commits=commits && s.prepare_batch(8,113,b,why) && s.commit_batch(b,why);
        }
        saver.join(); CHECK(commits && saved_ok);
        CHECK(s.save(path,why) && copy.load(path,why));
        HybridBatch a,b; CHECK(s.prepare_batch(100,789,a,why) &&
            copy.prepare_batch(100,789,b,why) && same(a,b));
    }
    std::cout << "Hybrid CPU checks: " << passed << " passed, " << failed << " failed\n"
              << "Synthetic test artifacts: " << root.u8string() << '\n';
    return failed ? 1 : 0;
}
