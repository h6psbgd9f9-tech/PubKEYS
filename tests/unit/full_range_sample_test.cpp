// Offline inspection of the user's attachment plus a deliberately small CPU
// proof of full-width traversal/sample metadata. Never prints attached material.
#include "common.h"
#include "engine.h"
#include "hybrid_search.h"
#include "notify.h"
#include "packed_traversal.h"
#include "scan_secp.h"
#include "search.h"
#include "search_range.h"
#include "targets.h"
#include "traverse.h"
#include "wave_gpu_batch.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <sstream>
#include <stdexcept>

#ifdef PKH_HAVE_CUDA
#error This attachment inspection must be compiled without GPU support.
#endif

namespace {
uint64_t checks=0,modes=0,segments=0,large_ordinals=0,cpu_candidates=0,points=0;
int selected_lower_bit=0;
void require(bool valid,const char* label){++checks;if(!valid)throw std::runtime_error(label);}
struct Silence {
    std::ostringstream out,err;
    std::streambuf *old_out=std::cout.rdbuf(out.rdbuf()),*old_err=std::cerr.rdbuf(err.rdbuf());
    ~Silence(){std::cout.rdbuf(old_out);std::cerr.rdbuf(old_err);}
};
std::string lower(std::string text){std::transform(text.begin(),text.end(),text.begin(),[](unsigned char c){return char(std::tolower(c));});return text;}
std::string read(const std::filesystem::path& path){std::ifstream input(path,std::ios::binary);require(bool(input),"attachment opens read-only");return {std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};}
std::string scalar_hex(const u256 value){uint8_t bytes[32];u256_to_be32(bytes,value);return to_hex(bytes,32);}
std::string public_hex(const Point& value,bool compressed){uint8_t bytes[65]{};bytes[0]=compressed?uint8_t(2|(value.y[0]&1)):4;fe_to_be32(bytes+1,value.x);if(!compressed)fe_to_be32(bytes+33,value.y);return to_hex(bytes,compressed?33:65);}
std::string field(const std::string& source,const std::string& pattern){
    const std::regex expression(pattern,std::regex::icase);std::sregex_iterator item(source.begin(),source.end(),expression),end;
    require(item!=end,"attachment contains expected field");const auto value=lower((*item)[1].str());++item;require(item==end,"attachment field is unambiguous");return value;
}
void independent_point(const u256 key,const Point& expected){
    require(search_key_is_valid(key),"verified scalar inside group order");
    const Point actual=ec_mul_G(key);require(!actual.inf&&fe_eq(actual.x,expected.x)&&fe_eq(actual.y,expected.y),"private scalar regenerates complete point");
    uint8_t bytes[32],compressed[33],y[32];u256_to_be32(bytes,key);
    require(scan_secp_pubkey(bytes,compressed,y)!=0,"independent libsecp public derivation succeeds");
    require(to_hex(compressed,33)==public_hex(actual,true)&&to_hex(y,32)==scalar_hex(actual.y),"independent libsecp agrees on compressed point and full Y");
    ++points;
}
void full_range(int mode){
    g_cfg=Config{};g_cfg.mode=mode;g_cfg.range_start_bit=selected_lower_bit;g_cfg.range_end_bit=256;
    g_cfg.chunk_bits=16;g_cfg.zone_nibbles=g_cfg.wave_nibbles=g_cfg.hybrid_nibbles=4;
    g_cfg.det_hex=-1;g_cfg.digit_hours=0;g_cfg.want_nk=false;g_cfg.threads=1;
    g_cfg.mask_hex.clear();g_cfg.mask_froz=0;g_cfg.wave_start.clear();
    g_cfg.tg_token.clear();g_cfg.tg_chat.clear();g_cfg.signal_to.clear();g_cfg.signal_account.clear();g_cfg.notify_cmd.clear();g_cfg.archive_pass.clear();
    g_quiet=true;g_search.reset_progress();g_pause.reset(&g_stop);g_stop_flag=0;g_shutdown_requested=false;g_test_found.clear();g_last_found.clear();
    set_block_nibbles(4);compute_range();zone_set_seed(7137);wave_set_seed(7137);
    u256 minimum{},one,maximum;u256_from_u64(one,1);minimum[selected_lower_bit/64]=UINT64_C(1)<<(selected_lower_bit%64);u256_sub_u64(maximum,EC_N,1);
    require(u256_cmp(g_range_start,minimum)==0&&u256_cmp(g_range_end,maximum)==0,"menu bounds are exactly the selected power of two through n minus one");
    require(!mask_active()&&g_cfg.det_hex==-1,"full range has no mask or leading-digit narrowing");
    require(chunk_keys()==65536&&traversal_block_bits()==16,"four-nibble block remains 65536 keys");
    set_scan_step(one);g_db.items.clear();g_db.n_comp=g_db.n_uncomp=g_db.n_xonly=0;
}
void inspect_attachment(const std::filesystem::path& path,bool wave){
    const auto source=read(path);
    const auto key_text=field(source,"PRIVATE KEY\\s*:\\s*([0-9a-f]{64})");
    const auto x=field(source,"(?:^|[\\r\\n])\\s*X\\s*:\\s*([0-9a-f]{64})");
    const auto y=field(source,"(?:^|[\\r\\n])\\s*Y\\s*:\\s*([0-9a-f]{64})");
    const auto compressed=field(source,"PUBLIC KEY\\s*\\(\\s*c\\s*\\)\\s*:\\s*([0-9a-f]{66})");
    const auto uncompressed=field(source,"PUBLIC KEY\\s*\\(\\s*u\\s*\\)\\s*:\\s*([0-9a-f]{130})");
    u256 key;require(hex_to_u256(key_text.c_str(),key),"attached private field parses");
    require(search_key_in_range(key),"attached scalar is inside selected full group range");
    require(key[3]!=0,"attached example retains scalar bits above 192");
    const Point point=ec_mul_G(key);independent_point(key,point);
    require(scalar_hex(point.x)==x&&scalar_hex(point.y)==y,"attached X and Y both equal scalar multiplication");
    require(public_hex(point,true)==compressed&&public_hex(point,false)==uncompressed,"both attached public formats equal the same private key point");
    bool found_low=false,found_high=false;const auto high=scalar_hex(g_range_end);
    const std::regex hex_token("[0-9a-fA-F]{32,}");
    for(std::sregex_iterator item(source.begin(),source.end(),hex_token),end;item!=end;++item){const auto token=lower(item->str());if(token.size()==64){found_low|=token==scalar_hex(g_range_start);found_high|=token==high;}}
    require(found_low&&found_high,"displayed exact endpoints equal selected lower bound and n minus one");
    require(std::regex_search(source,std::regex(wave?"Блок волны\\s*:\\s*16\\^4":"Зонный блок\\s*:\\s*16\\^4")),"attachment identifies the selected four-nibble block mode");
    u256 relative,base,last;u256_sub(relative,key,g_range_start);const uint64_t offset=relative[0]&65535;
    u256_sub_u64(base,key,offset);u256_sub(relative,g_range_end,base);
    const uint64_t count=!(relative[1]|relative[2]|relative[3])&&relative[0]<65535?relative[0]+1:65536;
    u256_add_u64(last,base,count-1);
    require(search_key_in_range(base)&&search_key_in_range(last)&&offset>0&&offset<count,"attached key is internal to an allowed interval-relative zone block");
    require(sample_offset(base,count)==offset,"attached internal key matches production sample selection for its exact block");
}
void shr(u256 result,const u256 value,unsigned bits){
    for(unsigned limb=0;limb<4;++limb){const unsigned source=limb+bits/64;result[limb]=source<4?value[source]>>(bits%64):0;if(bits%64&&source+1<4)result[limb]|=value[source+1]<<(64-bits%64);}
}
ChunkIdx ordinal(const u256 value){ChunkIdx id;id.from_u256(value);return id;}
ChunkIdx ordinal_power(unsigned bit,bool previous=false){u256 value{};value[bit/64]=UINT64_C(1)<<(bit%64);if(previous)u256_sub_u64(value,value,1);return ordinal(value);}
std::vector<ChunkIdx> selected_ordinals(){
    ChunkIdx zero,one;one.from_u64(1);std::vector<ChunkIdx> result{zero,one};
    for(unsigned bit:{63u,64u,128u,192u,239u}){result.push_back(ordinal_power(bit,true));result.push_back(ordinal_power(bit));}
    u256 total,last;fe_from_be32(total,g_total_blocks256);u256_sub_u64(last,total,1);result.push_back(ordinal(last));return result;
}
void check_segment(const u256 start,uint64_t count,bool aligned){
    ++segments;require(count>0&&count<=65536,"selected segment length stays bounded");
    u256 last;u256_add_u64(last,start,count-1);
    require(search_key_in_range(start)&&search_key_in_range(last)&&u256_cmp(start,last)<=0,"selected full-width segment stays inside group bounds without wrap");
    if(aligned)require(((start[0]-g_range_start[0])&65535)==0,"finite block remains relative to the true range start");
    const uint64_t offset=sample_offset(start,count);u256 sample;u256_add_u64(sample,start,offset);
    require(offset<count&&search_key_in_range(sample)&&u256_cmp(sample,start)>=0&&u256_cmp(sample,last)<=0,"production sample offset remains inside selected segment");
    const Point point=ec_mul_G(sample);independent_point(sample,point);
}
ChunkIdx find_tail_ordinal(){
    u256 low{},high;fe_from_be32(high,g_total_blocks256);unsigned iterations=0;
    require(!permuted_tail_precedes(ordinal(low))&&permuted_tail_precedes(ordinal(high)),"inverse tail predicate spans the full finite domain");
    for(;;){u256 difference;u256_sub(difference,high,low);if(!(difference[1]|difference[2]|difference[3])&&difference[0]==1)break;
        require(++iterations<=256,"targeted tail lookup bounded to 256 comparisons");
        u256 sum,middle;u256_add(sum,low,high);shr(middle,sum,1);
        if(permuted_tail_precedes(ordinal(middle)))u256_set(high,middle);else u256_set(low,middle);
    }
    return ordinal(low);
}
void actual_cpu_samples(const u256 start){
    g_test_found.clear();g_count=0;g_found_num=0;g_sample_valid=false;
    u256 target;u256_add_u64(target,start,8);const Point point=ec_mul_G(target);
    g_db.items.clear();g_db.n_comp=g_db.n_uncomp=g_db.n_xonly=0;
    require(g_db.add_hex(public_hex(point,true))&&g_db.add_hex(public_hex(point,false)),"synthetic target accepts both public encodings");
    g_db.build();require(push_targets_to_backend(),"actual CPU engine accepts synthetic target");
    scan_chunk(start,17);cpu_candidates+=17;
    require(g_count.load()==17&&g_found_num.load()==1&&g_test_found.count(scalar_hex(target))==1&&!g_stop.load(),"limited real CPU scan finds exact high-width scalar and keeps running");
    require(g_sample_valid,"actual CPU engine publishes its sample");
    u256 expected;u256_add_u64(expected,start,sample_offset(start,17));
    require(u256_cmp(g_sample_key,expected)==0,"actual set_sample retains all scalar limbs");
    independent_point(g_sample_key,ec_mul_G(g_sample_key));
}
void finite_mode(int mode){
    ++modes;full_range(mode);
    u256 total;fe_from_be32(total,g_total_blocks256);require(total[3]!=0,"finite ordinal domain is wider than 192 bits");
    std::mt19937_64 random(7137);const auto ids=selected_ordinals();std::set<std::array<uint8_t,32>> unique;
    for(const auto& id:ids){u256 start;uint64_t count=0;int phase=-1;require(next_chunk(id,start,count,phase,random),"selected finite full-width ordinal produces a segment");
        if(std::any_of(id.b,id.b+24,[](uint8_t byte){return byte!=0;}))++large_ordinals;
        std::array<uint8_t,32> bytes;u256_to_be32(bytes.data(),start);require(unique.insert(bytes).second,"limited selected finite ordinals do not repeat a block");check_segment(start,count,true);}
    ChunkIdx end;memcpy(end.b,g_total_blocks256,32);u256 rejected;uint64_t rejected_count=0;int phase=-1;
    require(!next_chunk(end,rejected,rejected_count,phase,random),"finite traversal rejects ordinal at exact domain end");
    ChunkIdx tail;if(mode==TRAV_SEQ){u256 last;u256_sub_u64(last,total,1);tail=ordinal(last);}else tail=find_tail_ordinal();
    u256 tail_start,tail_last;uint64_t tail_count=0;
    require(next_chunk(tail,tail_start,tail_count,phase,random),"actual finite traversal reaches exact order-boundary tail");u256_add_u64(tail_last,tail_start,tail_count-1);
    u256 width;u256_sub(width,EC_N,g_range_start);const uint64_t expected_tail=width[0]&65535;
    require(tail_count==(expected_tail?expected_tail:65536)&&u256_cmp(tail_last,g_range_end)==0,"actual permuted tail clipped at n minus one without overflowing into invalid scalars");
    check_segment(tail_start,tail_count,true);
    if(mode==TRAV_WAVE||mode==TRAV_ZONE){PackedTraversalPlan legacy;require(!packed_traversal_prepare(legacy),"legacy translated packing is disabled for current finite mode");
        WaveGpuCursor cursor;WaveGpuBatch batch;size_t next=0;
        cursor.fill(batch,ids.size(),65536,[&](ChunkIdx& id,u256 key,uint64_t& count){if(next==ids.size())return false;id=ids[next++];int zone=-1;return next_chunk(id,key,count,zone,random);});
        require(batch.pieces.size()==ids.size()&&batch.completed.size()==ids.size(),"production exact-start GPU metadata cursor packs all selected full-width blocks on CPU");
        for(size_t i=0;i<ids.size();++i){u256 expected,actual;uint64_t count=0;int zone=-1;require(next_chunk(ids[i],expected,count,zone,random),"packed exact-start reference exists");fe_from_be32(actual,batch.pieces[i].start.data());require(u256_cmp(actual,expected)==0&&count==batch.pieces[i].count,"packed starts preserve every upper byte and original clipped length");}
    }
    u256 high;uint64_t count=0;require(next_chunk(ordinal_power(128),high,count,phase,random)&&count>=17,"high-width limited CPU sample block exists");actual_cpu_samples(high);
    u256 final_start;u256_sub_u64(final_start,EC_N,17);actual_cpu_samples(final_start);
}
void random_mode(){++modes;full_range(TRAV_RANDOM);std::mt19937_64 random(7137);u256 first{};bool high_seen=false;
    for(unsigned i=0;i<8;++i){ChunkIdx id;id.from_u64(i);u256 start;uint64_t count=0;int phase=-1;require(next_chunk(id,start,count,phase,random),"limited random route produces segment");check_segment(start,count,false);high_seen|=start[3]!=0;if(i==0)u256_set(first,start);}
    require(high_seen,"random route retains values above 192 bits");actual_cpu_samples(first);u256 final_start;u256_sub_u64(final_start,EC_N,17);actual_cpu_samples(final_start);
}
void hybrid_mode(){++modes;full_range(TRAV_HYBRID);uint8_t lo[32],hi[32],seed[32]{};u256_to_be32(lo,g_range_start);u256_to_be32(hi,g_range_end);seed[31]=91;
    pkh_hybrid::HybridSearch hybrid;pkh_hybrid::HybridBatch batch;std::string why;
    require(hybrid.init(4,lo,hi,seed,"full-width-offline-proof",why),"hybrid accepts exact full group bounds");
    require(hybrid.prepare_batch(8,65536,batch,why)&&!batch.pieces.empty(),"limited hybrid batch prepares without search or checkpoint writes");bool high_seen=false;u256 first{};
    for(size_t i=0;i<batch.pieces.size();++i){u256 start;fe_from_be32(start,batch.pieces[i].key);check_segment(start,batch.pieces[i].count,false);high_seen|=start[3]!=0;if(i==0)u256_set(first,start);}
    require(high_seen,"hybrid A component retains values above 192 bits");require(hybrid.commit_batch(batch,why),"limited hybrid batch commits only in memory");actual_cpu_samples(first);u256 final_start;u256_sub_u64(final_start,EC_N,17);actual_cpu_samples(final_start);
}
}
int wmain(int argc,wchar_t**argv){SetPriorityClass(GetCurrentProcess(),BELOW_NORMAL_PRIORITY_CLASS);SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_LOWEST);
    try{require(argc==4,"two attachments and empty scratch directory required");const auto attachment=std::filesystem::absolute(std::filesystem::path(argv[1]));const auto second_attachment=std::filesystem::absolute(std::filesystem::path(argv[2]));const auto scratch=std::filesystem::absolute(std::filesystem::path(argv[3]));
        require(std::filesystem::is_directory(scratch)&&std::filesystem::is_empty(scratch),"isolated scratch directory exists and is empty");
        {Silence silence;std::filesystem::current_path(scratch);init_colors();build_table();require(scan_secp_init()!=0,"independent libsecp CPU verifier initializes");
            for(int lower_bit:{0,40}){selected_lower_bit=lower_bit;full_range(lower_bit==0?TRAV_ZONE:TRAV_WAVE);inspect_attachment(lower_bit==0?attachment:second_attachment,lower_bit!=0);
                for(int mode:{TRAV_SEQ,TRAV_KXE,TRAV_WAVE,TRAV_ZONE})finite_mode(mode);random_mode();hybrid_mode();}
            require(std::filesystem::is_empty(scratch),"offline verification creates no result/config/checkpoint files");require(pending_found_postprocessing()==0&&pending_background_notifications()==0,"offline verification queues no postprocessing or notifications");}
        std::printf("PASS: two attachments and six-mode CPU proof checks=%llu mode_ranges=%llu segments=%llu large_ordinals=%llu points=%llu cpu_candidates=%llu; range0..256 and40..256 attached scalar/X/Y/both encodings/endpoints/internal sample agree, independent libsecp crosscheck, full-width selected ordinals and exact n-1 tails, exact-start packing metadata, limited real CPU comparisons; files=0 notifications=0 GPU=0\n",(unsigned long long)checks,(unsigned long long)modes,(unsigned long long)segments,(unsigned long long)large_ordinals,(unsigned long long)points,(unsigned long long)cpu_candidates);return 0;
    }catch(const std::exception& failure){std::fprintf(stderr,"FAIL: offline full-range sample proof: %s\n",failure.what());return 1;}
}
