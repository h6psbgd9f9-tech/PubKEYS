// Isolated CPU-only audit of current production traversal metadata.
// No app entry point, engine, database, GPU, notifications or checkpoint I/O.
#include "traverse.h"
#include "search.h"
#include "hybrid_search.h"
#include <algorithm>
#include <array>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>

#ifdef PKH_HAVE_CUDA
#error This metadata audit must never use a CUDA build.
#endif

std::string tg_default_token(){return {};}
std::string tg_default_chat(){return {};}
Config g_cfg;
u256 g_step={1,0,0,0};
uint64_t g_total_chunks=0;
volatile sig_atomic_t g_stop_flag=0;
std::string now_string(){throw std::runtime_error("unexpected application logging");}
const char* mode_title(int){throw std::runtime_error("unexpected application logging");}
void secure_create(const std::string&){throw std::runtime_error("unexpected permission operation");}
bool plat_fsync_file(const std::string&){throw std::runtime_error("unexpected checkpoint I/O");}
bool plat_fsync_dir(const std::string&){throw std::runtime_error("unexpected checkpoint I/O");}
bool plat_replace_file(const std::string&,const std::string&){throw std::runtime_error("unexpected checkpoint I/O");}
bool plat_remove_file(const std::string&){throw std::runtime_error("unexpected checkpoint I/O");}
std::wstring plat_wide(const std::string&){throw std::runtime_error("unexpected path conversion");}

namespace {
using Key=std::array<uint8_t,32>;
struct Segment {Key key{},last{};uint64_t count=0,ordinal=0;int phase=-1;};
struct Row {std::string name,scope;uint64_t states=0,nonempty=0,keys=0,repeated_keys=0;
    uint64_t first_repeat_ordinal=UINT64_MAX,first_previous_ordinal=UINT64_MAX,first_repeat_key=0;
    uint64_t replayed=0;};
uint64_t assertions=0;
std::vector<Row> rows;
void need(bool ok,const char* why){++assertions;if(!ok)throw std::runtime_error(why);}
Key bytes(const u256 x){Key out{};u256_to_be32(out.data(),x);return out;}
uint64_t small(const uint8_t* p){uint64_t n=0;for(int i=0;i<24;++i)need(!p[i],"synthetic value must fit uint64");for(int i=24;i<32;++i)n=(n<<8)|p[i];return n;}
uint64_t small(const u256 x){need(!(x[1]|x[2]|x[3]),"synthetic value must fit uint64");return x[0];}
ChunkIdx id(uint64_t n){ChunkIdx out;out.from_u64(n);return out;}
void power(u256 x,unsigned bit){u256_zero(x);x[bit/64]=UINT64_C(1)<<(bit%64);}
void configure(int mode,int lo_bit,int hi_bit,int n,uint64_t seed,const u256 lo,const u256 hi,int chunk_bits=12){
    g_cfg=Config{};g_cfg.mode=mode;g_cfg.range_start_bit=lo_bit;g_cfg.range_end_bit=hi_bit;
    g_cfg.det_hex=-1;g_cfg.wave_nibbles=n;g_cfg.zone_nibbles=n;g_cfg.chunk_bits=chunk_bits;
    g_cfg.wave_start.clear();g_cfg.mask_hex.clear();g_cfg.mask_froz=0;
    u256_set(g_range_start,lo);u256_set(g_range_end,hi);g_search.reset_progress();
    set_block_nibbles(n);wave_set_seed(seed);compute_total_blocks256();
}
Segment emit(uint64_t ordinal,std::mt19937_64& rng,bool& exists){
    Segment out;out.ordinal=ordinal;u256 key{},last{};
    exists=next_chunk(id(ordinal),key,out.count,out.phase,rng);out.key=bytes(key);
    if(!exists){out.count=0;return out;}
    if(out.count){u256_add_u64(last,key,out.count-1);out.last=bytes(last);
        need(u256_cmp(key,g_range_start)>=0&&u256_cmp(last,g_range_end)<=0&&u256_cmp(last,key)>=0,"production segment outside requested bounds");}
    return out;
}
bool same(const Segment&a,const Segment&b){return a.key==b.key&&a.last==b.last&&a.count==b.count&&a.phase==b.phase;}
void no_intersection(std::vector<Segment> segments){
    segments.erase(std::remove_if(segments.begin(),segments.end(),[](const Segment&s){return !s.count;}),segments.end());
    std::sort(segments.begin(),segments.end(),[](const Segment&a,const Segment&b){return a.key<b.key;});
    for(size_t i=1;i<segments.size();++i)need(segments[i-1].last<segments[i].key,"distinct production primary intervals overlap");
}
void mark(Row&row,const Segment&s,uint64_t low,std::vector<int64_t>&seen,bool require_unique){
    ++row.states;if(!s.count)return;++row.nonempty;row.keys+=s.count;const uint64_t first=small(s.key.data());
    need(first>=low&&s.count<=seen.size()&&first-low<=seen.size()-s.count,"bounded synthetic segment invalid");
    for(uint64_t k=0;k<s.count;++k){auto& prior=seen[size_t(first+k-low)];
        if(prior>=0){++row.repeated_keys;if(row.first_repeat_ordinal==UINT64_MAX){row.first_repeat_ordinal=s.ordinal;row.first_previous_ordinal=uint64_t(prior);row.first_repeat_key=first+k;}}
        if(require_unique)need(prior<0,"repeated primary key in mode with uniqueness guarantee");
        if(prior<0)prior=int64_t(s.ordinal);
    }
}
void replay(int mode,int lo_bit,int hi_bit,int n,uint64_t seed,const u256 lo,const u256 hi,const std::vector<Segment>&expected,Row&row){
    configure(mode,lo_bit,hi_bit,n,seed,lo,hi);std::mt19937_64 rng(7);
    for(size_t i=0;i<expected.size();++i){bool exists=false;const auto actual=emit(i,rng,exists);need(exists&&same(actual,expected[i]),"same seed and ordinal fail to replay production metadata");++row.replayed;}
}
void partition(int mode,const char*name){
    u256 lo{},hi{};u256_from_u64(lo,0x1801);u256_from_u64(hi,0x801abc);
    configure(mode,12,24,3,71,lo,hi);need(traversal_full_coverage(),"partition must report full range coverage");
    Row row{name,"first 1000 production blocks; 4096 keys per full block"};std::vector<Segment> emitted;
    std::vector<int64_t> seen(size_t(hi[0]-lo[0]+1),-1);std::mt19937_64 rng(7);
    for(uint64_t i=0;i<1000;++i){bool exists=false;auto s=emit(i,rng,exists);need(exists,"1000-block partition fixture unexpectedly ended");mark(row,s,lo[0],seen,true);emitted.push_back(s);}
    no_intersection(emitted);replay(mode,12,24,3,71,lo,hi,emitted,row);rows.push_back(row);
    u256_from_u64(hi,0x4abd);configure(mode,12,16,3,71,lo,hi);
    Row complete{std::string(name)+"_clipped_complete","all blocks of synthetic inclusive [0x1801,0x4abd]"};
    seen.assign(size_t(hi[0]-lo[0]+1),-1);const uint64_t total=small(g_total_blocks256);
    for(uint64_t i=0;i<total;++i){bool exists=false;auto s=emit(i,rng,exists);need(exists,"clipped partition ordinal absent");mark(complete,s,lo[0],seen,true);}
    need(complete.keys==seen.size()&&std::find(seen.begin(),seen.end(),-1)==seen.end(),"partition omitted a permitted synthetic key");
    bool exists=false;emit(total,rng,exists);need(!exists,"partition did not finish at exact total");rows.push_back(complete);
}
void shapes(int mode,const char*name){
    // Bound exhaustive key-level verification; this family is SELECTIVE.
    for(uint64_t seed:{UINT64_C(19),UINT64_C(8271)}){
        u256 lo{},hi{};u256_from_u64(lo,0x1000);u256_from_u64(hi,0xffffff);
        configure(mode,12,24,3,seed,lo,hi);need(unique_blocks_mode()&&!traversal_full_coverage(),"shape must report finite selective family");
        const uint64_t total=small(g_total_blocks256);need(total&&total<=4096,"bounded shape family too large");
        Row row{std::string(name)+"_bounded_seed_"+std::to_string(seed),"entire selective family, key-level overlap check"};
        std::vector<int64_t>seen(size_t(hi[0]-lo[0]+1),-1);std::vector<Segment>emitted;std::mt19937_64 rng(7);
        for(uint64_t i=0;i<total;++i){bool exists=false;auto s=emit(i,rng,exists);need(exists,"finite shape state absent");mark(row,s,lo[0],seen,true);emitted.push_back(s);}
        no_intersection(emitted);need(row.keys>0&&row.keys<=seen.size(),"selective family count invalid");
        bool exists=false;emit(total,rng,exists);need(!exists,"shape family does not end at exact total");
        replay(mode,12,24,3,seed,lo,hi,emitted,row);rows.push_back(row);
    }
    for(uint64_t seed:{UINT64_C(19),UINT64_C(20)}){
        u256 lo{},hi{};power(lo,251);u256_sub_u64(hi,EC_N,1);configure(mode,251,256,4,seed,lo,hi);
        const uint64_t per_pattern=1+15*uint64_t(64-4-g_top_ni-(mode==TRAV_ZONE?1:0));
        Row row{std::string(name)+"_wide_seed_"+std::to_string(seed),"4 complete original patterns, range 251..256, N=4; interval overlap check"};
        std::vector<Segment>emitted;std::mt19937_64 rng(7);
        for(uint64_t i=0;i<4*per_pattern;++i){bool exists=false;auto s=emit(i,rng,exists);need(exists,"wide original pattern absent");++row.states;if(s.count){++row.nonempty;row.keys+=s.count;}emitted.push_back(s);}
        no_intersection(emitted);need(row.nonempty>1000,"wide metadata fixture too sparse");
        replay(mode,251,256,4,seed,lo,hi,emitted,row);rows.push_back(row);
    }
}
void random_repeats(){
    u256 lo{},hi{};u256_from_u64(lo,0x1001);u256_from_u64(hi,0x4abd);configure(TRAV_RANDOM,12,16,3,19,lo,hi);
    Row row{"RANDOM","1000 actual next_chunk calls on synthetic inclusive [0x1001,0x4abd]"};
    std::vector<int64_t>seen(size_t(hi[0]-lo[0]+1),-1);std::vector<Segment>emitted;std::mt19937_64 rng(7);
    for(uint64_t i=0;i<1000;++i){bool exists=false;auto s=emit(i,rng,exists);need(exists,"random interval unexpectedly ended");mark(row,s,lo[0],seen,false);emitted.push_back(s);}
    need(row.repeated_keys&&row.first_repeat_ordinal<1000,"production RANDOM counterexample not observed");
    replay(TRAV_RANDOM,12,16,3,19,lo,hi,emitted,row);rows.push_back(row);
}
void hybrid_repeats(){
    using namespace pkh_hybrid;
    Key lo{},hi{},seed{};lo[30]=0x18;lo[31]=0x01;hi[30]=0x5a;hi[31]=0xbc;seed[31]=19;
    HybridSearch search;std::string why;need(search.init(3,lo.data(),hi.data(),seed.data(),"synthetic-repeat-audit",why),"hybrid fixture init failed");
    Row row{"HYBRID","1000 actual HybridSearch prepare/commit blocks, inclusive [0x1801,0x5abc], N=3"};
    std::vector<int64_t>seen(size_t(0x5abc-0x1801+1),-1);
    for(uint64_t i=0;i<1000;++i){HybridBatch batch,repeat;
        need(search.prepare_batch(1,4096,batch,why)&&batch.pieces.size()==1,"hybrid prepare failed");
        need(search.prepare_batch(1,4096,repeat,why)&&repeat.pieces.size()==1,"hybrid speculative replay failed");
        const auto&p=batch.pieces.front();const auto&r=repeat.pieces.front();
        need(p.count==r.count&&!std::memcmp(p.key,r.key,32)&&p.info.ordinal==r.info.ordinal,"uncommitted hybrid preparation advanced state");++row.replayed;
        Segment s;s.ordinal=i;std::memcpy(s.key.data(),p.key,32);s.count=p.count;mark(row,s,0x1801,seen,false);
        need(search.commit_batch(batch,why),"hybrid commit failed");
    }
    need(row.repeated_keys&&row.first_repeat_ordinal<1000,"production HYBRID counterexample not observed");rows.push_back(row);
    // This documented special case really terminates after one prefix A.
    hi=lo;hi[30]=0x1a;hi[31]=0xbc;
    need(search.init(3,lo.data(),hi.data(),seed.data(),"synthetic-single-prefix",why),"single-prefix hybrid init failed");
    need(search.status().single_prefix,"single-prefix interval not recognized");HybridBatch one,end;
    need(search.prepare_batch(1,4096,one,why)&&one.pieces.size()==1&&one.keys==0x1abc-0x1801+1,"single-prefix hybrid length invalid");
    need(search.commit_batch(one,why)&&search.prepare_batch(1,4096,end,why)&&end.pieces.empty()&&search.status().exhausted,"single-prefix hybrid repeated its only prefix");
    Row single{"HYBRID_single_prefix","complete synthetic [0x1801,0x1abc] special case"};single.states=single.nonempty=1;single.keys=one.keys;rows.push_back(single);
}
}
int main(){try{
    partition(TRAV_SEQ,"SEQ");partition(TRAV_KXE,"KXE");shapes(TRAV_WAVE,"WAVE");shapes(TRAV_ZONE,"ZONE");random_repeats();hybrid_repeats();
    std::cout<<"{\n  \"result\":\"PASS\",\n  \"cpu_only\":true,\n  \"assertions\":"<<assertions<<",\n  \"rows\":[\n";
    for(size_t i=0;i<rows.size();++i){const auto&r=rows[i];std::cout<<"    {\"mode\":\""<<r.name<<"\",\"scope\":\""<<r.scope<<"\",\"states\":"<<r.states<<",\"nonempty\":"<<r.nonempty<<",\"keys\":"<<r.keys<<",\"repeated_keys\":"<<r.repeated_keys<<",\"same_state_replays\":"<<r.replayed;
        if(r.first_repeat_ordinal!=UINT64_MAX)std::cout<<",\"first_repeat_block\":"<<r.first_repeat_ordinal<<",\"previous_block\":"<<r.first_previous_ordinal<<",\"synthetic_repeated_key\":"<<r.first_repeat_key;
        std::cout<<"}"<<(i+1==rows.size()?"\n":",\n");}
    std::cout<<"  ],\n  \"limits\":\"Finite samples plus source algebra; not a GPU or whole 256-bit-domain exhaustive test. Primary candidate uniqueness is distinct from n-k mirror overlap and finding deduplication. Fresh wave/zone runs have no exclusion of prior runs.\"\n}\n";return 0;
}catch(const std::exception&e){std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;}}
