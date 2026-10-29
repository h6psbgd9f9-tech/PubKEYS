// Actual current-interval sample provider and six CPU database routes only.
// No menu, GPU, network, real configuration, database or result files.
#include "common.h"
#include "engine.h"
#include "hybrid_search.h"
#include "notify.h"
#include "search.h"
#include "search_range.h"
#include "search_status_sample.h"
#include "targets.h"
#include "traverse.h"
#include <windows.h>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>

#ifdef PKH_HAVE_CUDA
#error This provider regression must link only CPU application objects.
#endif
namespace {
uint64_t checks=0,modes=0,scanned=0;
void require(bool value,const char* name){++checks;if(!value)throw std::runtime_error(name);}
std::string scalar(const u256 key){uint8_t bytes[32];u256_to_be32(bytes,key);return to_hex(bytes,32);}
void sample(const u256 key,bool valid){std::lock_guard<std::mutex> lock(g_mtx_sample);u256_set(g_sample_key,key);g_sample_valid=valid;}
void setup(int mode){
    g_cfg=Config{};g_cfg.mode=mode;g_cfg.range_start_bit=40;g_cfg.range_end_bit=256;g_cfg.chunk_bits=16;
    g_cfg.wave_nibbles=g_cfg.zone_nibbles=g_cfg.hybrid_nibbles=4;g_cfg.det_hex=-1;g_cfg.mask_hex.clear();g_cfg.mask_froz=0;g_cfg.want_nk=false;g_cfg.threads=1;
    g_cfg.tg_token.clear();g_cfg.tg_chat.clear();g_cfg.signal_to.clear();g_cfg.signal_account.clear();g_cfg.notify_cmd.clear();g_cfg.archive_pass.clear();
    g_quiet=true;g_search.reset_progress();g_pause.reset(&g_stop);g_stop_flag=0;g_shutdown_requested=false;g_test_found.clear();g_last_found.clear();
    set_block_nibbles(4);compute_range();zone_set_seed(7137);wave_set_seed(7137);u256 one;u256_from_u64(one,1);set_scan_step(one);
    g_db.items.clear();g_db.n_comp=g_db.n_uncomp=g_db.n_xonly=0;u256 empty{};sample(empty,false);
}
void actual_mode(int mode){
    ++modes;setup(mode);require(search_status_sample().empty(),"new session has no stale sample before a worker publishes one");
    u256 start;uint64_t count=0;int phase=-1;std::mt19937_64 random(7137);pkh_hybrid::HybridSearch hybrid;pkh_hybrid::HybridBatch batch;std::string why;
    if(mode==TRAV_HYBRID){uint8_t lo[32],hi[32],seed[32]{};u256_to_be32(lo,g_range_start);u256_to_be32(hi,g_range_end);seed[31]=91;
        require(hybrid.init(4,lo,hi,seed,"local-status-provider",why)&&hybrid.prepare_batch(1,65536,batch,why)&&batch.pieces.size()==1,"actual hybrid provider source interval prepared");
        fe_from_be32(start,batch.pieces[0].key);count=batch.pieces[0].count;
    }else{u256 index{};index[3]=UINT64_C(1)<<47;ChunkIdx id;id.from_u256(index);require(next_chunk(id,start,count,phase,random),"actual traversal source interval prepared");}
    require(count>=17&&start[3]!=0,"sampled interval retains high scalar limbs");u256 target;u256_add_u64(target,start,8);const Point point=ec_mul_G(target);
    uint8_t compressed[33];compressed[0]=uint8_t(2|(point.y[0]&1));fe_to_be32(compressed+1,point.x);
    require(g_db.add_hex(to_hex(compressed,33)),"synthetic target accepted");g_db.build();require(push_targets_to_backend(),"actual CPU backend receives synthetic target");
    scan_chunk(start,17);scanned+=17;
    require(g_count.load()==17&&g_found_num.load()==1&&g_test_found.count(scalar(target))==1&&!g_stop.load(),"real CPU comparison and candidate count continue normally");
    u256 expected;u256_add_u64(expected,start,sample_offset(start,17));const auto text=search_status_sample();
    require(text.size()==64&&text==scalar(expected)&&g_sample_valid,"real provider exposes the exact fixed-width current-interval sample");
    require(search_key_in_range(expected),"exposed sample is valid in selected search range");
    sample(expected,false);require(search_status_sample().empty(),"cleared validity flag suppresses a previous session scalar");
}
void invalid_and_width(){
    setup(TRAV_SEQ);u256 key{},below;u256_sub_u64(below,g_range_start,1);
    for(const auto* invalid:std::vector<const uint64_t*>{key,EC_N,below}){sample(invalid,true);require(search_status_sample().empty(),"zero group-order or out-of-range sample is suppressed");}
    u256_from_u64(g_range_start,1);u256_from_u64(key,1);sample(key,true);
    require(search_status_sample()==std::string(63,'0')+"1","provider preserves leading zeros to exactly 64 hex digits");
    u256 after;{std::lock_guard<std::mutex> lock(g_mtx_sample);u256_set(after,g_sample_key);}require(u256_cmp(after,key)==0,"reading the provider does not alter the search sample");
}
void coherent_snapshot(){
    u256 first={UINT64_C(0x1111111111111111),UINT64_C(0x2222222222222222),UINT64_C(0x3333333333333333),UINT64_C(0x4444444444444444)};
    u256 second={UINT64_C(0x5555555555555555),UINT64_C(0x6666666666666666),UINT64_C(0x7777777777777777),UINT64_C(0x8888888888888888)};
    const auto a=scalar(first),b=scalar(second);sample(first,true);std::atomic<bool> go{false};
    std::thread publisher([&]{while(!go.load(std::memory_order_acquire))std::this_thread::yield();for(unsigned i=0;i<20000;++i)sample((i&1)?first:second,true);});
    go.store(true,std::memory_order_release);bool coherent=true;for(unsigned i=0;i<20000;++i){const auto snapshot=search_status_sample();if(snapshot!=a&&snapshot!=b)coherent=false;}
    publisher.join();require(coherent,"sample mutex yields complete 256-bit snapshots during concurrent publication");
}
}
int wmain(int argc,wchar_t**argv){SetPriorityClass(GetCurrentProcess(),BELOW_NORMAL_PRIORITY_CLASS);SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_LOWEST);
    try{require(argc==2,"fresh isolated scratch argument required");const auto scratch=std::filesystem::absolute(std::filesystem::path(argv[1]));require(std::filesystem::is_directory(scratch)&&std::filesystem::is_empty(scratch),"scratch directory exists and is empty");std::filesystem::current_path(scratch);
        std::ostringstream output,errors;auto* old_out=std::cout.rdbuf(output.rdbuf());auto* old_err=std::cerr.rdbuf(errors.rdbuf());
        try{init_colors();build_table();for(int mode:{TRAV_SEQ,TRAV_KXE,TRAV_WAVE,TRAV_ZONE,TRAV_RANDOM,TRAV_HYBRID})actual_mode(mode);invalid_and_width();coherent_snapshot();
            require(std::filesystem::is_empty(scratch),"CPU provider proof creates no configuration results or checkpoints");require(pending_found_postprocessing()==0&&pending_background_notifications()==0,"CPU provider proof queues no jobs or notifications");
        }catch(...){std::cout.rdbuf(old_out);std::cerr.rdbuf(old_err);throw;}std::cout.rdbuf(old_out);std::cerr.rdbuf(old_err);
        std::printf("PASS: actual private status provider checks=%llu modes=%llu cpu_candidates=%llu concurrent_snapshots=20000; exact current-interval sample, full 256-bit limbs, fixed64 leading zeros, invalid/stale suppression, coherent mutex snapshot; files=0 notifications=0 GPU=0\n",(unsigned long long)checks,(unsigned long long)modes,(unsigned long long)scanned);return 0;
    }catch(const std::exception& failure){std::fprintf(stderr,"FAIL: actual private status provider: %s\n",failure.what());return 1;}
}
