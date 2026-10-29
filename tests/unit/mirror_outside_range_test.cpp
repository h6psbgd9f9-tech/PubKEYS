// CPU-only integration regression: actual traversal -> ECC -> synthetic DB ->
// quiet result commit. No live files, configuration, GPU or notifications.
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
#error This integration fixture must be linked against CPU-only application objects.
#endif

extern void slow_confirm(const u256 center, int index, bool plus);
#ifdef USE_SECP
extern void secp_hit(const uint8_t center[32], int index, int plus, void*);
#endif

namespace {
uint64_t checks=0, engine_cases=0, scanned=0;
using Bytes=std::array<uint8_t,32>;
void require(bool ok,const char* label) {
    ++checks;
    if(!ok)throw std::runtime_error(label);
}
Bytes bytes(const u256 value) { Bytes result{};u256_to_be32(result.data(),value);return result; }
std::string hex(const u256 value) {const auto data=bytes(value);return to_hex(data.data(),data.size());}
Point negative(Point value) {if(!value.inf)fe_neg(value.y,value.y);return value;}
bool same(const Point& a,const Point& b) {
    return a.inf==b.inf&&(a.inf||(fe_eq(a.x,b.x)&&fe_eq(a.y,b.y)));
}
std::string public_key(const Point& point,bool compressed) {
    uint8_t data[65]{};
    data[0]=compressed?uint8_t(2|(point.y[0]&1)):4;
    fe_to_be32(data+1,point.x);
    if(!compressed)fe_to_be32(data+33,point.y);
    return to_hex(data,compressed?33:65);
}
void verify_wif(const std::string& wif,const u256 expected,bool compressed) {
    std::vector<uint8_t> decoded;
    require(base58_decode(wif,decoded),"WIF decodes");
    require(decoded.size()==(compressed?38:37),"WIF length");
    require(decoded[0]==0x80,"WIF network");
    const auto expected_bytes=bytes(expected);
    require(std::equal(expected_bytes.begin(),expected_bytes.end(),decoded.begin()+1),"WIF actual scalar");
    if(compressed)require(decoded[33]==1,"WIF compressed flag");
    uint8_t once[32],twice[32];const size_t payload=decoded.size()-4;
    sha256(decoded.data(),payload,once);sha256(once,sizeof once,twice);
    require(std::equal(twice,twice+4,decoded.begin()+payload),"WIF checksum");
}
void verify_record(const u256 base,const Point& point,int kind) {
    u256 actual;u256_set(actual,base);if(kind==2)u256_sub(actual,EC_N,base);
    const Point expected=kind==2?negative(point):point;
    const auto record=make_found(base,point.x,point.y,kind);
    require(record.mirror==(kind==2),"result mirror flag");
    require(record.priv_hex==hex(actual),"result actual scalar");
    require(record.pub_c==public_key(expected,true),"result compressed parity");
    require(record.pub_u==public_key(expected,false),"result uncompressed Y");
    require(record.x_hex==hex(expected.x)&&record.y_hex==hex(expected.y),"result point coordinates");
    require(same(ec_mul_G(actual),expected),"actual saved scalar independently regenerates point");
    verify_wif(record.wif_c,actual,true);verify_wif(record.wif_u,actual,false);
}
void clear_found() {g_test_found.clear();g_last_found.clear();g_count=0;g_found_num=0;g_stop=false;g_stop_flag=0;}
void targets(const Point& point,int wanted,bool compressed) {
    g_db.items.clear();g_db.n_comp=g_db.n_uncomp=g_db.n_xonly=0;
    if(wanted&1)require(g_db.add_hex(public_key(point,compressed)),"direct synthetic public target");
    if(wanted&2)require(g_db.add_hex(public_key(negative(point),compressed)),"mirror synthetic public target");
    g_db.build();require(push_targets_to_backend(),"synthetic DB backend upload");
    require(g_db.confirm_mask(point.x,int(point.y[0]&1))==wanted,"DB matches both point signs independently");
}
std::set<std::string> expected_set(const u256 key,int wanted,bool mirror) {
    std::set<std::string> result;if(wanted&1)result.insert(hex(key));
    if((wanted&2)&&mirror){u256 reversed;u256_sub(reversed,EC_N,key);result.insert(hex(reversed));}
    return result;
}
void found_is(const std::set<std::string>& expected) {
    require(g_test_found==expected,"actual engine committed exact direct/mirror scalars");
    require(g_found_num==expected.size(),"found counter includes each matched sign");
    if(expected.empty())require(g_last_found.empty(),"no match has no saved scalar");
    else require(expected.count(g_last_found)!=0,"saved scalar is an actual match");
}
void reset(int mode,int location) {
    g_cfg=Config{};g_cfg.mode=mode;g_cfg.threads=1;g_cfg.chunk_bits=12;
    g_cfg.wave_nibbles=g_cfg.zone_nibbles=g_cfg.hybrid_nibbles=3;
    g_cfg.det_hex=-1;g_cfg.mask_hex.clear();g_cfg.mask_froz=0;
    g_cfg.tg_token.clear();g_cfg.tg_chat.clear();g_cfg.signal_to.clear();
    g_cfg.notify_cmd.clear();g_cfg.archive_pass.clear();
    g_quiet=true;g_pause.reset(&g_stop);clear_found();
    if(location==0) {
        u256_zero(g_range_start);g_range_start[1]=UINT64_C(1)<<6;
        u256_add_u64(g_range_start,g_range_start,7);u256_add_u64(g_range_end,g_range_start,131070);
    } else if(location==1) {
        u256_sub_u64(g_range_start,EC_N,131071);u256_sub_u64(g_range_end,EC_N,1);
    } else {
        g_cfg.range_start_bit=252;g_cfg.range_end_bit=256;g_cfg.det_hex=8;compute_range();
        require((g_range_start[3]>>60)==8&&(g_range_end[3]>>60)==8,"effective leading HEX digit narrowed");
    }
    set_block_nibbles(3);compute_total_blocks256();compute_top_nibble();compute_zone_plan();
    zone_set_seed(7137);wave_set_seed(7137);u256 one;u256_from_u64(one,1);set_scan_step(one);
}
void engine_case(int mode,int location) {
    reset(mode,location);std::mt19937_64 random(7137);u256 start{};uint64_t count=0;int phase=-1;
    pkh_hybrid::HybridSearch hybrid;pkh_hybrid::HybridBatch hybrid_batch;std::string why;
    if(mode==TRAV_HYBRID) {
        uint8_t seed[32]{};seed[31]=91;const auto low=bytes(g_range_start),high=bytes(g_range_end);
        require(hybrid.init(3,low.data(),high.data(),seed,"synthetic-mirror-outside-range",why),"actual hybrid setup");
        require(hybrid.prepare_batch(1,4096,hybrid_batch,why)&&hybrid_batch.pieces.size()==1,"actual hybrid produces piece");
        fe_from_be32(start,hybrid_batch.pieces[0].key);count=hybrid_batch.pieces[0].count;
    } else {
        ChunkIdx first;
        require(next_chunk(first,start,count,phase,random),"actual mode produces first block");
    }
    require(count>0&&count<=4096,"small actual traversal length");
    u256 key,mirror;u256_add_u64(key,start,count-1);u256_sub(mirror,EC_N,key);
    require(search_key_in_range(start)&&search_key_in_range(key),"actual base piece within effective bounds");
    require(!search_key_in_range(mirror),"mirror really outside effective bounds");
    const Point point=ec_mul_G(key);require(!point.inf,"base candidate is finite");
    verify_record(key,point,1);verify_record(key,point,2);
    for(bool compressed:{true,false})for(int wanted:{1,2,3})for(bool enabled:{false,true}) {
        g_cfg.want_nk=enabled;targets(point,wanted,compressed);const auto expected=expected_set(key,wanted,enabled);
        clear_found();scan_chunk(start,count);scanned+=count;++engine_cases;
        found_is(expected);require(g_count==count,"engine count remains base candidates only");
        clear_found();scan_slow_range(key,1,false);found_is(expected);
        clear_found();slow_confirm(key,0,true);found_is(expected);
#ifdef USE_SECP
        clear_found();const auto data=bytes(key);secp_hit(data.data(),0,1,nullptr);found_is(expected);
#endif
        clear_found();if(wanted&1)report_found(key,point.x,point.y,1);if(wanted&2)report_found(key,point.x,point.y,2);found_is(expected);
    }
    if(mode==TRAV_HYBRID)require(hybrid.commit_batch(hybrid_batch,why),"actual hybrid safely commits piece");
}
void invalid_base_guards() {
    reset(TRAV_SEQ,0);u256 key;u256_add_u64(key,g_range_start,99);const Point point=ec_mul_G(key);
    g_cfg.want_nk=true;targets(point,3,true);clear_found();
    u256 bad{},below,above;u256_sub_u64(below,g_range_start,1);u256_add_u64(above,g_range_end,1);
    const uint64_t* invalid_candidates[]={bad,EC_N,below,above};
    for(const auto* candidate:invalid_candidates) {
        require(!search_match_allowed(candidate,1)&&!search_match_allowed(candidate,2),"invalid/outside base rejected for both signs");
        report_found(candidate,point.x,point.y,1);report_found(candidate,point.x,point.y,2);
    }
    report_found(key,point.x,point.y,0);report_found(key,point.x,point.y,3);
    found_is({});
    // Even an outside base whose mirror is inside cannot claim an inside
    // scalar: the base must be a member of the scheduled search range.
    u256 mirror;u256_sub(mirror,EC_N,key);const Point reversed=negative(point);
    report_found(mirror,reversed.x,reversed.y,2);found_is({});
    const uint64_t* outside_candidates[]={below,above};
    for(const auto* candidate:outside_candidates) {
        const Point own=ec_mul_G(candidate);targets(own,3,true);clear_found();
        scan_slow_range(candidate,1,false);slow_confirm(candidate,0,true);found_is({});
#ifdef USE_SECP
        const auto data=bytes(candidate);secp_hit(data.data(),0,1,nullptr);found_is({});
#endif
    }
}
void mask_grid_case() {
    reset(TRAV_SEQ,0);
    g_cfg.range_start_bit=70;g_cfg.range_end_bit=71;
    g_cfg.mask_hex=std::string(63,'0')+"a";g_cfg.mask_froz=1;compute_range();
    require(mask_active()&&g_mask_step[0]==16,"actual frozen-suffix mask active");
    set_scan_step(g_mask_step);ChunkIdx first;u256 start,key,mirror;uint64_t offered=0;int phase=-1;
    std::mt19937_64 random(317);
    require(next_chunk(first,start,offered,phase,random)&&offered>=1029,"actual mask route produces sufficient candidates");
    const uint64_t count=1029;u256_add_u64(key,start,(count-1)*16);u256_sub(mirror,EC_N,key);
    require((key[0]&15)==10&&(mirror[0]&15)!=10,"mirror allowed to differ from frozen suffix");
    require(!search_key_in_range(mirror),"masked mirror really outside numeric range");
    const Point point=ec_mul_G(key);verify_record(key,point,2);
    for(bool compressed:{true,false})for(bool enabled:{false,true}) {
        g_cfg.want_nk=enabled;targets(point,3,compressed);clear_found();
        scan_chunk(start,count);scanned+=count;++engine_cases;
        found_is(expected_set(key,3,enabled));require(g_count==count,"mask counter remains source candidates only");
    }
}
void explicit_range_commit() {
    reset(TRAV_SEQ,0);u256 key;u256_add_u64(key,g_range_start,77);const Point point=ec_mul_G(key);
    u256 mirror;u256_sub(mirror,EC_N,key);
    for(bool enabled:{false,true})for(int kind:{1,2}) {
        g_cfg.want_nk=enabled;clear_found();
        const bool allowed=kind==1||enabled;
        require(report_found_in_range(key,point.x,point.y,kind,g_range_start,g_range_end)==allowed,"kangaroo explicit-range commit matches policy");
        std::set<std::string> expected;if(allowed)expected.insert(hex(kind==2?mirror:key));found_is(expected);
    }
    g_cfg.want_nk=true;clear_found();u256 invalid{},above;
    u256_add_u64(above,g_range_end,1);Point bad_point=point;bad_point.y[0]^=1;
    require(!report_found_in_range(key,bad_point.x,bad_point.y,2,g_range_start,g_range_end),"kangaroo invalid public point rejected");
    require(!report_found_in_range(invalid,point.x,point.y,2,g_range_start,g_range_end),"kangaroo zero rejected");
    require(!report_found_in_range(EC_N,point.x,point.y,2,g_range_start,g_range_end),"kangaroo order rejected");
    const Point above_point=ec_mul_G(above);
    require(!report_found_in_range(above,above_point.x,above_point.y,1,g_range_start,g_range_end),"kangaroo out-of-range base direct rejected");
    require(!report_found_in_range(above,above_point.x,above_point.y,2,g_range_start,g_range_end),"kangaroo out-of-range base mirror rejected");
    const Point reversed=negative(point);
    require(!report_found_in_range(mirror,reversed.x,reversed.y,2,g_range_start,g_range_end),"kangaroo inside mirror cannot excuse outside base");
    require(!report_found_in_range(key,point.x,point.y,0,g_range_start,g_range_end),"kangaroo invalid kind rejected");
    require(!report_found_in_range(key,point.x,point.y,3,g_range_start,g_range_end),"kangaroo invalid kind rejected");
    found_is({});
}
}
int main(int argc,char** argv) {
    SetPriorityClass(GetCurrentProcess(),BELOW_NORMAL_PRIORITY_CLASS);
    SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_LOWEST);
    try {
        require(argc==2,"supply isolated empty directory");
        const auto fixture=std::filesystem::absolute(std::filesystem::u8path(argv[1]));
        require(std::filesystem::is_directory(fixture)&&std::filesystem::is_empty(fixture),"test directory exists and is empty");
        std::filesystem::current_path(fixture);
        for(int mode:{TRAV_SEQ,TRAV_KXE,TRAV_WAVE,TRAV_ZONE,TRAV_RANDOM,TRAV_HYBRID})
            for(int location:{0,1,2})engine_case(mode,location);
        mask_grid_case();invalid_base_guards();explicit_range_commit();
        require(std::filesystem::is_empty(fixture),"quiet results create no files or checkpoints");
        require(pending_background_notifications()==0,"quiet results launch no background notifications");
        std::printf("PASS: mirror-outside-range checks=%llu engine_cases=%llu base_candidates=%llu; six actual CPU routes, both target formats/parities, enable/disable, low/high/HEX bounds, saved scalar/public/WIF and kangaroo commit; zero files/notifications/GPU\n",
                    static_cast<unsigned long long>(checks),static_cast<unsigned long long>(engine_cases),static_cast<unsigned long long>(scanned));
        return 0;
    }catch(const std::exception& error){std::fprintf(stderr,"FAIL: mirror-outside-range: %s\n",error.what());return 1;}
}
