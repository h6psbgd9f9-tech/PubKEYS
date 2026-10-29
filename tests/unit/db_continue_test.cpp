// Actual finite wave/zone traversal -> CPU ECC -> synthetic database -> quiet
// result commit. A hit must not stop later blocks or conceal normal exhaustion.
#include "common.h"
#include "engine.h"
#include "notify.h"
#include "search.h"
#include "search_range.h"
#include "targets.h"
#include "traverse.h"
#include <windows.h>
#include <array>
#include <cstdio>
#include <filesystem>
#include <random>
#include <stdexcept>

#ifdef PKH_HAVE_CUDA
#error The continuation fixture must use CPU-only objects.
#endif

namespace {
uint64_t checks=0,cases=0,scanned=0;
constexpr uint64_t width=4*UINT64_C(4096)+37;
struct Piece { std::array<uint8_t,32> start{};uint64_t count=0; };
void require(bool condition,const char* message) {
    ++checks;if(!condition)throw std::runtime_error(message);
}
ChunkIdx ordinal(uint64_t value) {ChunkIdx result;result.from_u64(value);return result;}
std::string scalar_hex(const u256 key) {uint8_t data[32];u256_to_be32(data,key);return to_hex(data,32);}
std::string public_key(const Point& point,bool compressed) {
    uint8_t data[65]{};data[0]=compressed?uint8_t(2|(point.y[0]&1)):4;
    fe_to_be32(data+1,point.x);if(!compressed)fe_to_be32(data+33,point.y);
    return to_hex(data,compressed?33:65);
}
void setup(int mode,uint64_t seed,bool high) {
    g_cfg=Config{};g_cfg.mode=mode;g_cfg.threads=1;g_cfg.chunk_bits=12;
    g_cfg.wave_nibbles=g_cfg.zone_nibbles=3;g_cfg.det_hex=-1;
    g_cfg.want_nk=false;g_cfg.mask_hex.clear();g_cfg.mask_froz=0;
    g_cfg.tg_token.clear();g_cfg.tg_chat.clear();g_cfg.signal_to.clear();
    g_cfg.notify_cmd.clear();g_cfg.archive_pass.clear();
    g_quiet=true;g_search.reset_progress();g_pause.reset(&g_stop);g_stop_flag=0;
    g_test_found.clear();g_last_found.clear();
    if(high) {u256_zero(g_range_start);g_range_start[1]=UINT64_C(1)<<6;u256_add_u64(g_range_start,g_range_start,7);}
    else u256_from_u64(g_range_start,UINT64_C(1)<<34);
    u256_add_u64(g_range_end,g_range_start,width-1);
    set_block_nibbles(3);compute_total_blocks256();compute_top_nibble();compute_zone_plan();
    zone_set_seed(seed);wave_set_seed(seed);u256 one;u256_from_u64(one,1);set_scan_step(one);
    g_db.items.clear();g_db.n_comp=g_db.n_uncomp=g_db.n_xonly=0;
    require(unique_blocks_mode(),"finite disjoint-block mode active");
    require(std::memcmp(g_total_blocks256,ordinal(5).b,32)==0,"five finite blocks including short tail");
    require(!mode_has_checkpoint(),"wave/zone remain without on-disk checkpoint");
}
void one_case(int mode,uint64_t seed,bool high,int format) {
    ++cases;setup(mode,seed,high);
    std::mt19937_64 preview_rng(seed);std::vector<Piece> pieces;
    for(uint64_t index=0;index<5;++index) {
        Piece piece;u256 start;int phase=-1;
        require(next_chunk(ordinal(index),start,piece.count,phase,preview_rng),"production traversal previews every block");
        require(piece.count>0&&piece.count<=4096,"actual block length positive and bounded");
        u256_to_be32(piece.start.data(),start);pieces.push_back(piece);
    }
    std::array<std::string,3> expected;
    const unsigned target_blocks[]={0,2,4};
    for(unsigned hit=0;hit<3;++hit) {
        const auto& piece=pieces[target_blocks[hit]];u256 base,key;fe_from_be32(base,piece.start.data());
        uint64_t offset=piece.count/2;Point point;
        do {
            require(offset<piece.count,"fixture can choose requested point parity inside block");
            u256_add_u64(key,base,offset++);point=ec_mul_G(key);
        }while((point.y[0]&1)!=(hit&1));
        expected[hit]=scalar_hex(key);
        const bool compressed=format==0||(format==2&&hit!=1);
        require(g_db.add_hex(public_key(point,compressed)),"synthetic target public key accepted");
    }
    g_db.build();require(g_db.items.size()==3,"three distinct synthetic database targets");
    require(push_targets_to_backend(),"synthetic public database delivered to actual CPU backend");
    std::mt19937_64 actual_rng(seed);std::vector<uint8_t> visited(width,0);
    uint64_t completed=0;
    for(uint64_t index=0;index<5;++index) {
        u256 start,offset;uint64_t count=0;int phase=-1;
        require(next_chunk(ordinal(index),start,count,phase,actual_rng),"actual traversal still issues later blocks after a hit");
        uint8_t data[32];u256_to_be32(data,start);
        require(std::equal(data,data+32,pieces[index].start.begin())&&count==pieces[index].count,
                "scan schedule matches actual preview exactly");
        u256_sub(offset,start,g_range_start);
        require(!(offset[1]|offset[2]|offset[3])&&offset[0]<width&&count<=width-offset[0],"block remains inside exact range");
        for(uint64_t j=0;j<count;++j)require(visited[offset[0]+j]++==0,"no candidate repeated between blocks");
        scan_chunk(start,count);completed+=count;scanned+=count;
        require(!g_stop.load()&&g_stop_flag==0,"database match does not request stop");
        require(g_count.load()==completed,"actual engine candidate counter advances after every block");
        const uint64_t found=index/2+1;
        std::set<std::string> wanted;
        for(uint64_t j=0;j<found;++j)wanted.insert(expected[j]);
        require(g_test_found==wanted&&g_found_num.load()==found,"every early and later hit committed exactly once");
        require(wanted.count(g_last_found)==1,"latest committed scalar belongs to a verified hit");
        if(index==0)require(found==1&&completed<width,"first hit happens with unfinished work remaining");
        chunk_done(g_search,ordinal(index));
        require(checkpoint_seq(g_search)==ordinal(index+1),"completed watermark advances after a hit");
        u256 accounted;
        require(checkpoint_completed_keys(checkpoint_seq(g_search),accounted)&&
                accounted[0]==completed&&!(accounted[1]|accounted[2]|accounted[3]),"watermark has exact count including permuted tail");
    }
    require(completed==width&&g_count.load()==width,"scan reaches exact finite range end");
    require(std::all_of(visited.begin(),visited.end(),[](uint8_t value){return value==1;}),"every candidate covered exactly once without gaps");
    require(g_test_found.size()==3&&g_found_num.load()==3,"all three hits survive normal completion");
    u256 next;uint64_t count=99;int phase=-1;
    require(!next_chunk(ordinal(5),next,count,phase,actual_rng),"finite traversal reports exhaustion only after all blocks");
    require(!g_stop.load()&&g_stop_flag==0,"natural exhaustion leaves user stop flag clear");
    require(checkpoint_seq(g_search)==ordinal(5)&&chunks_in_flight(g_search)==0,"full watermark equals total with no unfinished chunks");
    require(pending_background_notifications()==0,"quiet synthetic hits launch no notifications");
}
}
int main(int argc,char**argv) {
    SetPriorityClass(GetCurrentProcess(),BELOW_NORMAL_PRIORITY_CLASS);
    SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_LOWEST);
    try {
        require(argc==2,"isolated empty fixture directory argument required");
        const auto folder=std::filesystem::absolute(std::filesystem::u8path(argv[1]));
        require(std::filesystem::is_directory(folder)&&std::filesystem::is_empty(folder),"scratch directory exists and is empty");
        std::filesystem::current_path(folder);init_colors();g_quiet=true;build_table();
        for(int mode:{TRAV_WAVE,TRAV_ZONE})for(uint64_t seed:{UINT64_C(7137),UINT64_C(91731)})
            for(bool high:{false,true})for(int format:{0,1,2})one_case(mode,seed,high,format);
        require(std::filesystem::is_empty(folder),"actual quiet scans create no files/results/checkpoints");
        std::printf("PASS: database continuation checks=%llu cases=%llu scanned=%llu; actual wave/zone traversal and CPU ECC, three hits in first/middle/final blocks, both public formats/parities, no repeats/gaps, exact exhaustion watermark, no hit stop; zero GPU/files/notifications\n",
            static_cast<unsigned long long>(checks),static_cast<unsigned long long>(cases),static_cast<unsigned long long>(scanned));
        return 0;
    }catch(const std::exception& error){std::fprintf(stderr,"FAIL: database continuation: %s\n",error.what());return 1;}
}
