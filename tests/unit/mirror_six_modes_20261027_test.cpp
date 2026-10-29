// Isolated CPU integration: actual current traversal -> ECC -> synthetic DB ->
// quiet result commit. No application entry, live files, GPU or notifications.
#include "common.h"
#include "engine.h"
#include "hybrid_search.h"
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
#error This fixture is CPU-only.
#endif
std::string tg_default_token(){return {};}
std::string tg_default_chat(){return {};}
Config g_cfg;
const char* mode_title(int){throw std::runtime_error("unexpected application UI route");}
extern void slow_confirm(const u256 center,int index,bool plus);
#ifdef USE_SECP
extern void secp_hit(const uint8_t center[32],int index,int plus,void*);
#endif

namespace {
uint64_t checks=0,engine_cases=0,scanned=0,route_cases=0;
using Bytes=std::array<uint8_t,32>;
void require(bool ok,const char* label){++checks;if(!ok)throw std::runtime_error(label);}
Bytes bytes(const u256 value){Bytes result{};u256_to_be32(result.data(),value);return result;}
std::string hex(const u256 value){const auto data=bytes(value);return to_hex(data.data(),data.size());}
Point negative(Point value){if(!value.inf)fe_neg(value.y,value.y);return value;}
bool same(const Point& a,const Point& b){return a.inf==b.inf&&(a.inf||(fe_eq(a.x,b.x)&&fe_eq(a.y,b.y)));}
std::string public_key(const Point& point,bool compressed){
    uint8_t data[65]{};data[0]=compressed?uint8_t(2|(point.y[0]&1)):4;
    fe_to_be32(data+1,point.x);if(!compressed)fe_to_be32(data+33,point.y);
    return to_hex(data,compressed?33:65);
}
void verify_wif(const std::string& wif,const u256 expected,bool compressed){
    std::vector<uint8_t> decoded;require(base58_decode(wif,decoded),"WIF decodes");
    require(decoded.size()==(compressed?38:37),"WIF length");require(decoded[0]==0x80,"WIF network");
    const auto key=bytes(expected);require(std::equal(key.begin(),key.end(),decoded.begin()+1),"WIF actual scalar");
    if(compressed)require(decoded[33]==1,"WIF compressed marker");
    uint8_t first[32],second[32];const size_t payload=decoded.size()-4;
    sha256(decoded.data(),payload,first);sha256(first,sizeof first,second);
    require(std::equal(second,second+4,decoded.begin()+payload),"WIF checksum");
}
void verify_record(const u256 base,const Point& point,int kind){
    u256 actual;u256_set(actual,base);if(kind==2)u256_sub(actual,EC_N,base);
    const Point expected=kind==2?negative(point):point;const auto record=make_found(base,point.x,point.y,kind);
    require(record.mirror==(kind==2),"result mirror flag");require(record.priv_hex==hex(actual),"result actual scalar");
    require(record.pub_c==public_key(expected,true),"result compressed parity");
    require(record.pub_u==public_key(expected,false),"result uncompressed Y");
    require(record.x_hex==hex(expected.x)&&record.y_hex==hex(expected.y),"result coordinates");
    require(same(ec_mul_G(actual),expected),"saved scalar independently regenerates actual point");
    verify_wif(record.wif_c,actual,true);verify_wif(record.wif_u,actual,false);
}
void clear_found(){g_test_found.clear();g_last_found.clear();g_count=0;g_found_num=0;g_stop=false;g_stop_flag=0;}
void targets(const Point& point,int wanted,bool compressed){
    g_db.items.clear();g_db.n_comp=g_db.n_uncomp=g_db.n_xonly=0;
    if(wanted&1)require(g_db.add_hex(public_key(point,compressed)),"direct synthetic target");
    if(wanted&2)require(g_db.add_hex(public_key(negative(point),compressed)),"mirror synthetic target");
    g_db.build();require(push_targets_to_backend(),"CPU synthetic backend setup");
    require(g_db.confirm_mask(point.x,int(point.y[0]&1))==wanted,"DB preserves both signs independently");
}
std::set<std::string> expected_set(const u256 key,int wanted,bool mirror){
    std::set<std::string> result;if(wanted&1)result.insert(hex(key));
    if((wanted&2)&&mirror){u256 reversed;u256_sub(reversed,EC_N,key);result.insert(hex(reversed));}return result;
}
void found_is(const std::set<std::string>& expected){
    require(g_test_found==expected,"engine committed exact direct/mirror scalars");
    require(g_found_num==expected.size(),"each matched sign counted exactly once");
    require(expected.empty()?g_last_found.empty():expected.count(g_last_found)!=0,"last saved scalar matches result");
    require(!g_stop.load()&&g_stop_flag==0,"finding does not stop search");
}
void power(u256 out,unsigned bit){u256_zero(out);out[bit/64]=UINT64_C(1)<<(bit%64);}
void reset(int mode,int location){
    g_cfg=Config{};g_cfg.mode=mode;g_cfg.threads=1;g_cfg.chunk_bits=12;
    const int nib=location==2?4:3;
    g_cfg.wave_nibbles=g_cfg.zone_nibbles=g_cfg.hybrid_nibbles=nib;
    g_cfg.wave_start.clear();g_cfg.det_hex=-1;g_cfg.mask_hex.clear();g_cfg.mask_froz=0;
    g_cfg.tg_token.clear();g_cfg.tg_chat.clear();g_cfg.signal_to.clear();g_cfg.notify_cmd.clear();g_cfg.archive_pass.clear();
    g_cfg.range_start_bit=location==0?70:location==1?255:251;g_cfg.range_end_bit=location==0?71:256;
    if(location==0){power(g_range_start,70);u256_add_u64(g_range_start,g_range_start,7);u256_add_u64(g_range_end,g_range_start,131070);}
    else if(location==1){u256_sub_u64(g_range_start,EC_N,131071);u256_sub_u64(g_range_end,EC_N,1);}
    else{power(g_range_start,251);u256_sub_u64(g_range_end,EC_N,1);}
    g_quiet=true;g_pause.reset(&g_stop);clear_found();g_search.reset_progress();
    set_block_nibbles(nib);compute_top_nibble();zone_set_seed(7137);wave_set_seed(7137);compute_total_blocks256();
    require(mask_compute(),"unmasked synthetic range setup");u256 one;u256_from_u64(one,1);set_scan_step(one);
}
void engine_case(int mode,int location){
    reset(mode,location);++route_cases;std::mt19937_64 random(7137);u256 start{},key{},mirror{};uint64_t count=0,offered=0;int phase=-1;
    pkh_hybrid::HybridSearch hybrid;pkh_hybrid::HybridBatch hybrid_batch;std::string why;bool selected=false;
    // The wide range contains most of its own mirrors. Select an actual emitted
    // short segment near n, whose mirror is verifiably below 2^251. No candidates
    // from the enormous range are scanned in order to select this fixture.
    for(uint64_t ordinal=0;ordinal<100000&&!selected;++ordinal){
        if(mode==TRAV_HYBRID){
            uint8_t seed[32]{};for(int j=0;j<8;++j)seed[31-j]=uint8_t((ordinal+91)>>(8*j));
            const auto low=bytes(g_range_start),high=bytes(g_range_end);
            require(hybrid.init(g_cfg.hybrid_nibbles,low.data(),high.data(),seed,"synthetic-mirror-six-modes-20261027",why),"real hybrid initializes");
            require(hybrid.prepare_batch(1,1031,hybrid_batch,why)&&hybrid_batch.pieces.size()==1,"real hybrid emits short piece");
            fe_from_be32(start,hybrid_batch.pieces[0].key);offered=hybrid_batch.pieces[0].count;
        }else{
            ChunkIdx id;id.from_u64(ordinal);
            if(location==2&&mode==TRAV_SEQ){u256 total;fe_from_be32(total,g_total_blocks256);u256_sub_u64(total,total,ordinal+1);id.from_u256(total);}
            if(!next_chunk(id,start,offered,phase,random))break;
        }
        count=(std::min)(offered,UINT64_C(1031));if(count<33)continue;
        u256_add_u64(key,start,count/2);u256_sub(mirror,EC_N,key);
        selected=search_key_in_range(start)&&search_key_in_range(key)&&!search_key_in_range(mirror);
    }
    require(selected,"actual traversal supplies in-range base with mirror outside");
    u256 end;u256_add_u64(end,start,count-1);require(search_key_in_range(end),"entire tested segment within selected range");
    const Point point=ec_mul_G(key);require(!point.inf,"base point finite");verify_record(key,point,1);verify_record(key,point,2);
    for(bool compressed:{true,false})for(int wanted:{1,2,3})for(bool enabled:{false,true}){
        g_cfg.want_nk=enabled;targets(point,wanted,compressed);const auto expected=expected_set(key,wanted,enabled);
        clear_found();scan_chunk(start,count);scanned+=count;++engine_cases;
        found_is(expected);require(g_count==count,"full segment continues beyond middle finding; count is base candidates only");
        // A subsequent real scan after the finding must remain runnable. One
        // nonmatching endpoint avoids quietly rediscovering the same target.
        scan_chunk(start,1);++scanned;found_is(expected);require(g_count==count+1,"next scan runs after finding");
        clear_found();scan_slow_range(key,1,false);found_is(expected);
        clear_found();slow_confirm(key,0,true);found_is(expected);
#ifdef USE_SECP
        clear_found();const auto data=bytes(key);secp_hit(data.data(),0,1,nullptr);found_is(expected);
#endif
        clear_found();if(wanted&1)report_found(key,point.x,point.y,1);if(wanted&2)report_found(key,point.x,point.y,2);found_is(expected);
    }
    if(mode==TRAV_HYBRID)require(hybrid.commit_batch(hybrid_batch,why),"real hybrid batch commits without checkpoint I/O");
    std::printf("PASS route: mode=%d range_case=%d block_nibbles=%d short_candidates=%llu\n",mode,location,g_cfg.wave_nibbles,(unsigned long long)count);
}
void invalid_base_guards(){
    reset(TRAV_SEQ,0);u256 key;u256_add_u64(key,g_range_start,99);const Point point=ec_mul_G(key);
    g_cfg.want_nk=true;targets(point,3,true);clear_found();u256 zero{},below,above;
    u256_sub_u64(below,g_range_start,1);u256_add_u64(above,g_range_end,1);
    const uint64_t* invalid[]={zero,EC_N,below,above};
    for(const auto* candidate:invalid){
        require(!search_match_allowed(candidate,1)&&!search_match_allowed(candidate,2),"invalid/outside base rejected for both signs");
        report_found(candidate,point.x,point.y,1);report_found(candidate,point.x,point.y,2);
    }
    report_found(key,point.x,point.y,0);report_found(key,point.x,point.y,3);found_is({});
    u256 mirror;u256_sub(mirror,EC_N,key);const Point reverse=negative(point);report_found(mirror,reverse.x,reverse.y,2);found_is({});
}
}
int main(int argc,char** argv){
    SetPriorityClass(GetCurrentProcess(),BELOW_NORMAL_PRIORITY_CLASS);SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_LOWEST);
    try{
        require(argc==2,"supply isolated empty fixture directory");
        const auto fixture=std::filesystem::absolute(std::filesystem::u8path(argv[1]));
        require(std::filesystem::is_directory(fixture)&&std::filesystem::is_empty(fixture),"fixture exists and is empty");
        require(std::filesystem::equivalent(std::filesystem::current_path(),fixture),"process starts inside isolated fixture directory");
        // Do not change current directory: process/chdir IPC and network calls
        // are trapped in all production objects; assert this directory stays empty.
        for(int mode:{TRAV_SEQ,TRAV_KXE,TRAV_RANDOM,TRAV_ZONE,TRAV_WAVE,TRAV_HYBRID})
            for(int location:{0,1,2})engine_case(mode,location);
        invalid_base_guards();require(std::filesystem::is_empty(fixture),"quiet fixture has zero files/checkpoints");
        require(pending_background_notifications()==0,"quiet finding creates zero notifications");
        std::printf("PASS mirror-six-modes: checks=%llu traversal_cases=%llu engine_cases=%llu base_candidates=%llu; actual CPU batch+tail, slow, callback, quiet commit, WIF(c/u), continuation; no durable-file claim, no GPU\n",(unsigned long long)checks,(unsigned long long)route_cases,(unsigned long long)engine_cases,(unsigned long long)scanned);
        return 0;
    }catch(const std::exception& error){std::fprintf(stderr,"FAIL mirror-six-modes: %s\n",error.what());return 1;}
}
