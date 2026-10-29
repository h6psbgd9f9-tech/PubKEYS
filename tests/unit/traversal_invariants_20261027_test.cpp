// Actual traversal/checkpoint implementation, CPU only. Every file is local to
// an initially empty fixture CWD; never invokes the application entry point.
#define NOMINMAX
#include "traverse.h"
#include "search.h"
#include "engine.h"
#include "notify.h"
#include "platform.h"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>
#include <atomic>
#include <stdexcept>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstdio>

#ifdef PKH_HAVE_CUDA
#error This invariant fixture must not link CUDA.
#endif
namespace fs=std::filesystem;
std::atomic<bool> fail_close{false};
bool fail_file_sync=false,fail_dir_sync=false,fail_replace=false;
std::atomic<unsigned> privacy_calls{0};
int fixture_close(FILE* file){int result=std::fclose(file);return fail_close.exchange(false)?EOF:result;}
#define fclose fixture_close
#include "traverse.cpp"
#undef fclose

std::string tg_default_token(){return {};}
std::string tg_default_chat(){return {};}
Config g_cfg;
u256 g_step={1,0,0,0};
uint64_t g_total_chunks=0;
volatile sig_atomic_t g_stop_flag=0;
std::string now_string(){return "synthetic fixture";}
const char* mode_title(int){return "fixture";}
void secure_create(const std::string& path){if(!fs::exists(fs::u8path(path)))throw std::runtime_error("privacy must follow open");++privacy_calls;}
bool plat_fsync_file(const std::string& path){return !fail_file_sync&&fs::is_regular_file(fs::u8path(path));}
bool plat_fsync_dir(const std::string&){return !fail_dir_sync;}
std::wstring plat_wide(const std::string& path){return fs::u8path(path).wstring();}
bool plat_replace_file(const std::string& source,const std::string& target){
    return !fail_replace&&MoveFileExW(plat_wide(source).c_str(),plat_wide(target).c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);
}
namespace {
uint64_t checks=0,cases=0;
void require(bool condition,const char* why){++checks;if(!condition)throw std::runtime_error(why);}
ChunkIdx id(uint64_t n){ChunkIdx value;value.from_u64(n);return value;}
uint64_t scalar64(const u256 n){require(!(n[1]|n[2]|n[3]),"expected small number");return n[0];}
std::string read(const fs::path& path){std::ifstream in(path,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};}
void put(const fs::path& path,const std::string& text){std::ofstream out(path,std::ios::binary|std::ios::trunc);out<<text;out.close();require(bool(out),"write fixture input");}
void reset(int mode,uint64_t first,uint64_t width,int bits){
    ++cases;g_cfg=Config{};g_cfg.mode=mode;g_cfg.chunk_bits=bits;g_cfg.det_hex=-1;
    g_cfg.mask_hex.clear();g_cfg.mask_froz=0;g_cfg.wave_start.clear();
    u256_from_u64(g_range_start,first);u256_add_u64(g_range_end,g_range_start,width-1);
    g_search.reset_progress();g_stop=false;g_shutdown_requested=false;g_stop_flag=0;
    compute_total_blocks256();
}
void metadata(int mode,uint64_t width,int bits){
    reset(mode,0x10001,width,bits);std::mt19937_64 rng(1);
    const uint64_t total=(width+(UINT64_C(1)<<bits)-1)>>bits;
    std::vector<std::pair<uint64_t,uint64_t>> windows;uint64_t completed=0;
    for(uint64_t i=0;i<=total;++i){
        u256 n{};require(checkpoint_completed_keys(id(i),n),"completed prefix exists");
        require(scalar64(n)==completed,"tail accounting matches actual ordered pieces");
        if(i==total)break;
        u256 key{};uint64_t count=0;int phase=-1;
        require(next_chunk(id(i),key,count,phase,rng)&&count>0,"chunk exists");
        require(count<=chunk_keys(),"chunk maximum agrees");
        windows.push_back({scalar64(key),count});completed+=count;
    }
    require(completed==width,"prefix endpoint exact");
    std::sort(windows.begin(),windows.end());uint64_t next=0x10001;
    for(const auto& window:windows){require(window.first==next,"no gap or overlap");next+=window.second;}
    require(next==0x10001+width,"union endpoint exact");
    require(!permuted_tail_precedes(id(0)),"zero ordinal cannot have completed tail");
}
void atomic_faults(){
    put("atomic.dat","old");fail_close=true;
    require(!write_file_atomically("atomic.dat","close"),"fclose failure propagated");
    require(read("atomic.dat")=="old"&&!fs::exists("atomic.dat.tmp"),"failed close preserves prior target");
    fail_file_sync=true;require(!write_file_atomically("atomic.dat","sync"),"file sync failure propagated");fail_file_sync=false;
    require(read("atomic.dat")=="old","failed file sync preserves target");
    fail_replace=true;require(!write_file_atomically("atomic.dat","replace"),"replace failure propagated");fail_replace=false;
    require(read("atomic.dat")=="old","failed replace preserves target");
    fail_dir_sync=true;require(!write_file_atomically("atomic.dat","renamed"),"directory sync failure propagated");fail_dir_sync=false;
    require(read("atomic.dat")=="renamed","rename may be visible before durability confirmed");
    require(write_file_atomically("atomic.dat","ok")&&read("atomic.dat")=="ok","normal atomic update succeeds");
    const auto cwd=fs::current_path();fs::create_directory(fs::u8path("кириллица"));fs::current_path(fs::u8path("кириллица"));
    require(write_file_atomically("checkpoint.dat","unicode cwd"),"ASCII checkpoint name in Unicode directory");fs::current_path(cwd);
}
void canonical_targets_and_roundtrip(){
    reset(TRAV_KXE,0x10001,3*1024+17,10);PubEntry a{},b{};a.x[0]=1;a.par=1;b.x[0]=2;b.par=2;
    g_db.items={b,a,a};g_db.build();std::string why;
    require(checkpoint_prepare_identity(g_db,why),"first canonical DB prepared");const auto first=checkpoint_search_identity();
    g_db.items={a,b,b};g_db.build();require(checkpoint_prepare_identity(g_db,why),"reordered DB prepared");
    require(checkpoint_search_identity()==first,"order and duplicates do not alter target identity");
    wave_set_seed(3);std::mt19937_64 rng(1);u256 before{};uint64_t count=0;int phase=-1;
    require(next_chunk(id(1),before,count,phase,rng),"KXE reference position");
    checkpoint_reset(g_search,id(1));save_checkpoint();wave_set_seed(9999);ChunkIdx restored;
    require(load_checkpoint(restored)&&restored==id(1),"real KXE checkpoint round trip");
    u256 after{};require(next_chunk(restored,after,count,phase,rng),"restored KXE position");
    require(u256_cmp(before,after)==0,"KXE mapping independent of serialized wave seed");
}
void watermark_and_carry(){
    reset(TRAV_SEQ,1,5000,10);chunk_done(g_search,id(1));chunk_done(g_search,id(2));
    require(checkpoint_seq(g_search)==id(0),"withheld partial first chunk prevents advance");
    set_stop();require(checkpoint_seq(g_search)==id(0),"stop does not fill uncommitted gap");g_stop=false;g_shutdown_requested=false;g_stop_flag=0;
    chunk_done(g_search,id(1));require(checkpoint_seq(g_search)==id(0),"duplicate ahead is idempotent");
    chunk_done(g_search,id(0));require(checkpoint_seq(g_search)==id(3),"closing gap merges completed ahead");
    chunk_done(g_search,id(0));require(checkpoint_seq(g_search)==id(3),"duplicate below is idempotent");
    g_cfg.mode=TRAV_WAVE;g_cfg.wave_nibbles=3;set_block_nibbles(3);compute_total_blocks256();
    std::mt19937_64 rng(1);bool empty=false;
    for(uint64_t i=0;i<10000;++i){u256 key{};uint64_t n=0;int phase=-1;if(!next_chunk(id(i),key,n,phase,rng))break;if(n==0){checkpoint_reset(g_search,id(i));chunk_done(g_search,id(i));require(checkpoint_seq(g_search)==id(i+1),"proven empty ordinal advances watermark");empty=true;break;}}
    require(empty,"fixture reaches a real empty shaped ordinal");
    g_cfg.mode=TRAV_SEQ;g_cfg.chunk_bits=10;u256_from_u64(g_range_start,1);u256_zero(g_range_end);g_range_end[1]=1;
    u256 key{};uint64_t n=0;require(chunk_bounds(id(0),key,n)&&n==1024,"left low limb max does not create zero length");
    u256_zero(g_range_start);g_range_start[0]=UINT64_MAX-511;u256_add_u64(g_range_end,g_range_start,2047);
    require(chunk_bounds(id(1),key,n)&&key[1]==1&&key[0]==512&&n==1024,"chunk offset carries across 64-bit limb");
}
void debug_limits(){
    g_cfg.mode=TRAV_WAVE;g_cfg.range_start_bit=0;g_cfg.range_end_bit=16;set_block_nibbles(3);
    u256 zero{};WaveInfo info{};info.width=1;info.phase=1;info.step=1;info.top=63;
    g_dbg_written=0;debug_log_wave(0,id(0),zero,info,1);const auto secured=privacy_calls.load();
    fs::remove("debug_chunks.txt");debug_log_wave(0,id(1),zero,info,1);require(privacy_calls>secured,"recreated log secured before write");
    fs::remove("debug_chunks.txt");g_dbg_written=0;std::vector<std::thread> threads;
    for(unsigned t=0;t<4;++t)threads.emplace_back([&,t]{for(unsigned i=0;i<3000;++i)debug_log_wave(t,id(t*3000+i),zero,info,1);});
    for(auto& thread:threads)thread.join();const auto text=read("debug_chunks.txt");size_t rows=0,pos=0;
    while((pos=text.find("участок ",pos))!=std::string::npos){++rows;pos+=8;}
    require(rows==DBG_LIMIT,"exactly DBG_LIMIT rows");const auto marker=text.find("===== предел ");
    require(marker!=std::string::npos&&text.find("участок ",marker)==std::string::npos,"no row follows limit notice");
    require(text.find("===== предел ",marker+1)==std::string::npos,"one limit notice");
    require(hex_mark_wave(zero,3,false,4,60)==std::string(57,'0')+"[0][0][0]"+std::string(4,'0'),"low front marks exactly three positions before four block digits");
    require(hex_mark_wave(zero,3,true,4,60)==std::string(3,'0')+"[0][0][0]"+std::string(58,'0'),"high front top is numbered from right, position three from left");
    require(hex_mark_wave(zero,3,false,65,60)==std::string(64,'0'),"invalid skip safely yields plain key");
    require(hex_mark_wave(zero,3,true,4,0)==std::string(64,'0'),"out-of-string high front safely yields plain key");
    std::set<size_t> previous;
    for(int width=1;width<=60;++width){
        const auto marked=hex_mark_wave(zero,width,false,4,60);std::set<size_t> positions;size_t raw=0;
        for(char ch:marked){if(ch=='[')positions.insert(raw);else if(ch!=']')++raw;}
        require(positions.size()==size_t(width),"front width equals exact bracket count");
        require(std::includes(positions.begin(),positions.end(),previous.begin(),previous.end()),"expanding phase retains all prior marked positions");previous=positions;
    }
    for(int skip=-2;skip<=66;++skip)for(int top=-2;top<=65;++top)for(int width=-1;width<=66;++width)for(bool high:{false,true}){
        const auto marked=hex_mark_wave(zero,width,high,skip,top);require(marked.size()>=64,"hex mark handles all boundary parameters");
        require(std::count(marked.begin(),marked.end(),'[')==std::count(marked.begin(),marked.end(),']'),"balanced mark brackets");
    }
}
}
int main(int argc,char** argv){
    try{
        require(argc==2,"requires isolated fixture directory");require(fs::current_path()==fs::absolute(fs::u8path(argv[1])),"fixture CWD matches exact requested directory");
        require(fs::directory_iterator(fs::current_path())==fs::directory_iterator(),"fixture starts empty");
        atomic_faults();for(int mode:{TRAV_SEQ,TRAV_KXE})for(int bits:{10,12})for(uint64_t width:{1,1023,1024,1025,3089})metadata(mode,width,bits);
        canonical_targets_and_roundtrip();watermark_and_carry();debug_limits();
        std::printf("PASS invariant checks=%llu cases=%llu GPU=0 notifications=0\n",(unsigned long long)checks,(unsigned long long)cases);return 0;
    }catch(const std::exception& error){std::fprintf(stderr,"FAIL invariant: %s\n",error.what());return 1;}
}
