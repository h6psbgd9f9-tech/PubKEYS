// Isolated CPU-only coverage/checkpoint tests of the actual production route.
// No engine, ECC search, external targets, configuration, GPU or notifications.
#include "traverse.h"
#include "search.h"
#include <windows.h>
#include <array>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <set>
#include <stdexcept>
#include <vector>

// Minimal application-independent fixture. Deliberately do not link config,
// engine, target loaders, notifications or the ordinary search entry point.
std::string tg_default_token() { return {}; }
std::string tg_default_chat() { return {}; }
Config g_cfg;
u256 g_step = {1,0,0,0};
volatile sig_atomic_t g_stop_flag = 0;
std::string now_string() { throw std::runtime_error("unexpected application log"); }
const char* mode_title(int) { throw std::runtime_error("unexpected application log"); }
void secure_create(const std::string&) { throw std::runtime_error("unexpected application log"); }

namespace {
uint64_t checks=0,cases=0,scanned_blocks=0;
using Key=std::array<uint8_t,32>;
void require(bool ok,const char* message) {
    ++checks;
    if(!ok)throw std::runtime_error(message);
}
Key bytes(const u256 value) {Key out{};u256_to_be32(out.data(),value);return out;}
uint64_t low_u64(const uint8_t* value) {
    uint64_t out=0;for(int i=24;i<32;++i)out=(out<<8)|value[i];return out;
}
void configure(int mode,const u256 low,const u256 high,int nibbles,uint64_t seed,
               const std::string& anchor={}) {
    g_cfg=Config{};g_cfg.mode=mode;g_cfg.det_hex=-1;
    g_cfg.wave_nibbles=nibbles;g_cfg.zone_nibbles=nibbles;
    g_cfg.wave_start=anchor;g_cfg.chunk_bits=22;
    u256_set(g_range_start,low);u256_set(g_range_end,high);
    set_block_nibbles(nibbles);wave_set_seed(seed);compute_total_blocks256();
    require(unique_blocks_mode(),"new modes use finite route");
    require(traversal_block_bits()==4*nibbles,"independent requested block bits");
    require(chunk_keys()==(UINT64_C(1)<<(4*nibbles)),"effective chunk size");
}
void coverage_case(int mode,const u256 low,uint64_t width,int nibbles,uint64_t seed,
                   const std::string& anchor={}) {
    ++cases;u256 high;u256_add_u64(high,low,width-1);
    configure(mode,low,high,nibbles,seed,anchor);
    const uint64_t block=chunk_keys(),total=(width-1)/block+1;
    require(low_u64(g_total_blocks256)==total,"exact finite domain");
    for(int i=0;i<24;++i)require(g_total_blocks256[i]==0,"small domain upper bytes");
    std::vector<uint8_t> seen(static_cast<size_t>(total),0);
    uint64_t sum=0;std::mt19937_64 unused_rng(3);
    for(uint64_t ordinal=0;ordinal<total;++ordinal) {
        ChunkIdx id;id.from_u64(ordinal);u256 key,done;uint64_t count=0;int phase=-1;
        require(checkpoint_completed_keys(id,done)&&done[0]==sum&&!done[1]&&!done[2]&&!done[3],
                "exact prefix including permuted short tail");
        require(next_chunk(id,key,count,phase,unused_rng),"every finite ordinal exists");
        u256 difference;u256_sub(difference,key,low);
        require(!difference[1]&&!difference[2]&&!difference[3]&&difference[0]<width,
                "block starts inside interval including carries");
        const uint64_t offset=difference[0];
        require(offset%block==0,"partition is relative to inclusive low bound");
        const uint64_t block_id=offset/block;
        require(block_id<total&&!seen[static_cast<size_t>(block_id)],"no repeated block");
        seen[static_cast<size_t>(block_id)]=1;
        require(count==(std::min)(block,width-offset)&&count>0,"exact block/tail length");
        u256 last;u256_add_u64(last,key,count-1);
        require(u256_cmp(last,high)<=0&&u256_cmp(last,key)>=0,"inclusive endpoint");
        sum+=count;++scanned_blocks;
    }
    require(sum==width&&std::find(seen.begin(),seen.end(),uint8_t(0))==seen.end(),
            "complete exact coverage");
    ChunkIdx end;end.from_u64(total);u256 key,done;uint64_t count=99;int phase=-1;
    require(!next_chunk(end,key,count,phase,unused_rng),"finite route ends");
    require(checkpoint_completed_keys(end,done)&&done[0]==width&&!done[1]&&!done[2]&&!done[3],
            "completed endpoint counts every candidate exactly once");
    end.inc();require(!checkpoint_completed_keys(end,done),"reject watermark beyond end");
}
void seed_and_anchor() {
    u256 low,high;u256_from_u64(low,123);u256_add_u64(high,low,UINT64_C(65536)*257+112);
    for(int mode:{TRAV_WAVE,TRAV_ZONE}) {
        configure(mode,low,high,4,0);
        std::vector<Key> first;std::mt19937_64 random(7);
        for(uint64_t i=0;i<258;++i){ChunkIdx id;id.from_u64(i);u256 key;uint64_t count;int phase;
            require(next_chunk(id,key,count,phase,random),"seed fixture ordinal");first.push_back(bytes(key));}
        configure(mode,low,high,4,0);
        for(uint64_t i=0;i<258;++i){ChunkIdx id;id.from_u64(i);u256 key;uint64_t count;int phase;
            require(next_chunk(id,key,count,phase,random)&&bytes(key)==first[static_cast<size_t>(i)],
                    "zero seed deterministic replay");}
        configure(mode,low,high,4,173);bool differs=false;
        for(uint64_t i=0;i<258;++i){ChunkIdx id;id.from_u64(i);u256 key;uint64_t count;int phase;
            require(next_chunk(id,key,count,phase,random),"other seed fixture ordinal");
            differs|=bytes(key)!=first[static_cast<size_t>(i)];}
        require(differs,"seed changes permutation");
    }
    u256 anchor;u256_add_u64(anchor,low,3*UINT64_C(65536)+17);
    configure(TRAV_WAVE,low,high,4,173,to_hex(bytes(anchor).data(),32));
    ChunkIdx zero;u256 key;uint64_t count;int phase;std::mt19937_64 random(1);
    require(next_chunk(zero,key,count,phase,random),"fixed-start first block");
    u256_sub(key,key,low);require(key[0]==3*UINT64_C(65536)&&!key[1]&&!key[2]&&!key[3],
            "fixed anchor chooses containing first block without repeating it");
}
void wide_domain() {
    for(int mode:{TRAV_WAVE,TRAV_ZONE}) {
        u256 low,high;u256_from_u64(low,1);u256_zero(high);high[2]=1;
        configure(mode,low,high,4,173);
        require(g_total_blocks256[17]!=0,"block domain exceeds 64 bits");
        std::set<Key> seen;std::mt19937_64 random(13);
        for(uint64_t base:{UINT64_C(0),UINT64_C(10000),UINT64_C(1)<<40})
        for(uint64_t i=0;i<64;++i){ChunkIdx id;id.from_u64(base+i);
            u256 key,last;uint64_t count;int phase;require(next_chunk(id,key,count,phase,random),"wide low ordinal");
            require(seen.insert(bytes(key)).second,"wide low ordinals are distinct");
            u256_add_u64(last,key,count-1);require(u256_cmp(key,low)>=0&&u256_cmp(last,high)<=0,"wide bounds");}
        for(uint64_t i=0;i<64;++i){ChunkIdx id;id.b[23]=1;id.b[31]=static_cast<uint8_t>(i);
            u256 key;uint64_t count;int phase;require(next_chunk(id,key,count,phase,random),"ordinal beyond 2^64");
            require(seen.insert(bytes(key)).second,"high 256-bit ordinal is not truncated");}
    }
    // Synthetic arithmetic edge: a 2^256-wide domain has a representable
    // block count and proper prefixes, but its final candidate count does not
    // fit in u256. It must not wrap to zero or shorten the finite block domain.
    for(int mode:{TRAV_WAVE,TRAV_ZONE}) {
        u256 low{},high;for(auto& limb:high)limb=UINT64_MAX;
        configure(mode,low,high,4,173);
        u256 total;fe_from_be32(total,g_total_blocks256);
        require(total[3]==(UINT64_C(1)<<48)&&!total[0]&&!total[1]&&!total[2],"full-width domain block count");
        ChunkIdx end;end.from_u256(total);u256 completed;
        require(!checkpoint_completed_keys(end,completed),"2^256 completed candidates cannot wrap to zero");
        u256 before;u256_sub_u64(before,total,1);ChunkIdx prefix;prefix.from_u256(before);
        require(checkpoint_completed_keys(prefix,completed),"full-width penultimate prefix representable");
        require(completed[0]==UINT64_MAX-65535&&completed[1]==UINT64_MAX&&completed[2]==UINT64_MAX&&completed[3]==UINT64_MAX,
                "full-width penultimate prefix exact");
        std::mt19937_64 random(1);u256 key;uint64_t count;int phase;
        require(next_chunk(prefix,key,count,phase,random)&&count==65536,"full-width last ordinal exists");
        require(!next_chunk(end,key,count,phase,random),"full-width route finite endpoint");
    }
}
void ten_trillion_sample() {
    constexpr uint64_t width=UINT64_C(10000000000000),block=65536,total=(width-1)/block+1;
    for(int mode:{TRAV_WAVE,TRAV_ZONE}) {
        u256 low,high;u256_from_u64(low,1);u256_from_u64(high,width);configure(mode,low,high,4,173);
        require(low_u64(g_total_blocks256)==total,"10T exact block domain");
        std::set<Key> seen;std::mt19937_64 random(13);
        for(uint64_t ordinal=0;ordinal<8192;++ordinal) {
            const uint64_t selected=ordinal<4096?ordinal:total-8192+ordinal;
            ChunkIdx id;id.from_u64(selected);u256 key,offset;uint64_t count;int phase;
            require(next_chunk(id,key,count,phase,random),"10T sampled ordinal");u256_sub(offset,key,low);
            require(offset[0]<width&&offset[0]%block==0&&!offset[1]&&!offset[2]&&!offset[3],"10T relative grid");
            require(count==(std::min)(block,width-offset[0])&&seen.insert(bytes(key)).second,"10T sampled lengths and uniqueness");
        }
        ChunkIdx end;end.from_u64(total);u256 completed,key;uint64_t count=0;int phase;
        require(checkpoint_completed_keys(end,completed)&&completed[0]==width&&!completed[1]&&!completed[2]&&!completed[3],"10T exact complete count");
        require(!next_chunk(end,key,count,phase,random),"10T finite end");
    }
}
void checkpoint_case(int mode,uint64_t seed,const std::filesystem::path& root) {
    ++cases;const auto folder=root/(std::string(mode==TRAV_SEQ?"seq_":"kxe_")+std::to_string(seed));
    std::filesystem::create_directories(folder);std::filesystem::current_path(folder);
    u256 low,high;u256_from_u64(low,0x107);u256_add_u64(high,low,9*UINT64_C(65536)+117);
    g_cfg=Config{};g_cfg.mode=mode;g_cfg.det_hex=-1;g_cfg.chunk_bits=16;
    u256_set(g_range_start,low);u256_set(g_range_end,high);wave_set_seed(seed);compute_total_blocks256();
    require(mode_has_checkpoint(),"checkpoint enabled for sequential/KXE");
    PubDB synthetic;PubEntry entry{};entry.x[0]=1;entry.par=1;synthetic.items.push_back(entry);
    std::string why;require(checkpoint_prepare_identity(synthetic,why),"synthetic target fingerprint");
    const std::string path=checkpoint_path();
    require(path.find(mode==TRAV_SEQ?"checkpoint_seq_v2_":"checkpoint_kxe_v2_")==0,"established checkpoint namespace");
    g_search.reset_progress();ChunkIdx two;two.from_u64(2);chunk_done(g_search,two);
    const std::string old_path="checkpoint_old_infinite_mode.dat";
    {std::ofstream old(old_path);old<<"old-format fixture, must remain untouched\n";}
    ChunkIdx zero;chunk_done(g_search,zero);ChunkIdx one;one.from_u64(1);
    require(checkpoint_seq(g_search)==one&&chunks_in_flight(g_search)==1,"watermark never crosses unfinished work");
    std::mt19937_64 random(1);u256 expected;uint64_t expected_count;int phase;
    require(next_chunk(one,expected,expected_count,phase,random),"next block before save");
    u256 completed_before;require(checkpoint_completed_keys(one,completed_before),"prefix before save");
    save_checkpoint();require(std::filesystem::exists(path),"isolated checkpoint created");
    {std::ifstream old(old_path);std::string line;std::getline(old,line);
        require(line=="old-format fixture, must remain untouched","old checkpoint preserved");}
    {std::ifstream file(path);std::string magic;file>>magic;require(magic=="PKHCP2","established checkpoint format");}
    const uint64_t changed_seed=seed^UINT64_C(0x71a509bf);wave_set_seed(changed_seed);ChunkIdx loaded;
    require(load_checkpoint(loaded)&&loaded==one&&wave_get_seed()==changed_seed,"resume loads sequential/KXE without unrelated seed mutation");
    u256 actual,completed_after;uint64_t actual_count;
    require(next_chunk(loaded,actual,actual_count,phase,random)&&bytes(actual)==bytes(expected)&&actual_count==expected_count,
            "resume restores exact next block");
    require(checkpoint_completed_keys(loaded,completed_after)&&bytes(completed_before)==bytes(completed_after),"resume prefix exact");
    synthetic.items[0].x[0]=2;require(checkpoint_prepare_identity(synthetic,why)&&checkpoint_path()!=path,"targets bound to checkpoint");
    synthetic.items[0].x[0]=1;require(checkpoint_prepare_identity(synthetic,why)&&checkpoint_path()==path,"target identity replay");
    // Identity changes cannot overwrite another mapping's progress.
    u256_add_u64(g_range_end,g_range_end,1);compute_total_blocks256();require(checkpoint_path()!=path,"bounds bound to checkpoint");
    u256_sub_u64(g_range_end,g_range_end,1);g_cfg.mode=mode==TRAV_SEQ?TRAV_KXE:TRAV_SEQ;
    require(checkpoint_path()!=path,"mode bound to checkpoint");
    g_cfg.mode=mode;g_cfg.chunk_bits=12;compute_total_blocks256();
    require(checkpoint_path()!=path,"block size bound to checkpoint");
}
using Files=std::map<std::string,std::pair<std::string,std::filesystem::file_time_type>>;
Files fixture_files() {
    Files result;
    for(const auto& entry:std::filesystem::directory_iterator(std::filesystem::current_path())) {
        require(entry.is_regular_file(),"isolated fixture contains only files");
        std::ifstream file(entry.path(),std::ios::binary);
        result.emplace(entry.path().filename().u8string(),std::make_pair(
            std::string(std::istreambuf_iterator<char>(file),{}),entry.last_write_time()));
    }
    return result;
}
void disabled_checkpoint_case(int mode,const std::filesystem::path& root) {
    ++cases;const auto folder=root/("disabled_"+std::to_string(mode));
    std::filesystem::create_directories(folder);std::filesystem::current_path(folder);
    g_search.reset_progress();u256 low,high;u256_from_u64(low,1);u256_from_u64(high,1000000);
    if(mode==TRAV_WAVE||mode==TRAV_ZONE)configure(mode,low,high,4,173);
    else{g_cfg=Config{};g_cfg.mode=mode;u256_set(g_range_start,low);u256_set(g_range_end,high);wave_set_seed(173);}
    PubDB synthetic;PubEntry entry{};entry.x[0]=1;entry.par=1;synthetic.items.push_back(entry);
    std::string why;require(checkpoint_prepare_identity(synthetic,why),"disabled mode fingerprint prepared");
    require(!mode_has_checkpoint()&&checkpoint_path().empty(),"disabled mode exposes no checkpoint path");
    for(const auto* name:{"checkpoint.dat","checkpoint_wave_v2_fixture.dat","checkpoint_wave_unique_v1_fixture.dat",
                         "checkpoint_zone_block2_fixture.dat","checkpoint_zone_unique_v1_fixture.dat","checkpoint_hybrid_fixture.dat"}) {
        std::ofstream file(name,std::ios::binary);file<<"old and new checkpoint fixture; preserve exactly\n";
    }
    const auto before=fixture_files();const uint64_t saved_seed=wave_get_seed();
    ChunkIdx watermark;watermark.from_u64(3);checkpoint_reset(g_search,watermark);
    save_checkpoint();require(fixture_files()==before,"disabled save creates and changes no files");
    ChunkIdx attempted;attempted.from_u64(19);const ChunkIdx unchanged=attempted;
    require(!load_checkpoint(attempted)&&attempted==unchanged,"disabled load rejects without changing ordinal");
    require(wave_get_seed()==saved_seed&&checkpoint_seq(g_search)==watermark,"disabled load/save leave in-memory seed and watermark unchanged");
    require(fixture_files()==before,"all existing checkpoint files and timestamps preserved");
    require(!g_stop.load(),"disabled checkpoint operation stays silent and does not stop search");
}
}
int main(int argc,char** argv) {
    SetPriorityClass(GetCurrentProcess(),BELOW_NORMAL_PRIORITY_CLASS);SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_LOWEST);
    try {
        if(argc!=2)throw std::runtime_error("pass isolated fixture directory");
        const auto root=std::filesystem::absolute(std::filesystem::u8path(argv[1]));
        std::filesystem::create_directories(root);std::filesystem::current_path(root);
        for(int mode:{TRAV_WAVE,TRAV_ZONE}) {
            for(uint64_t blocks:std::array<uint64_t,18>{1,2,3,4,7,8,15,16,17,31,32,33,255,256,257,65535,65536,65537}) {
                u256 low;u256_from_u64(low,1);coverage_case(mode,low,blocks*4096-13,3,173);
            }
            u256 low;u256_from_u64(low,1);coverage_case(mode,low,1,4,0);coverage_case(mode,low,65536,4,173);
            low[0]=UINT64_MAX-101;low[1]=17;coverage_case(mode,low,4*65536+17,4,173);
            u256_sub_u64(low,EC_N,65537);coverage_case(mode,low,65537,4,173);
            for(int nib:{3,4,8,10,15}){u256_from_u64(low,7);coverage_case(mode,low,65537,nib,173);}
        }
        for(int mode:{TRAV_SEQ,TRAV_KXE})for(uint64_t seed:{UINT64_C(0),UINT64_C(173)})checkpoint_case(mode,seed,root);
        for(int mode:{TRAV_WAVE,TRAV_ZONE,TRAV_HYBRID,TRAV_RANDOM})disabled_checkpoint_case(mode,root);
        seed_and_anchor();wide_domain();ten_trillion_sample();
        std::cout<<"PASS: cases="<<cases<<" checks="<<checks<<" exact_coverage_blocks="<<scanned_blocks
                 <<"; finite coverage, tails, carry, >64-bit ordinals, seed/anchor; SEQ/KXE durable resume, other modes no checkpoint I/O; 10T boundary/sample checks\n";
        return 0;
    }catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}
}
