// CPU-only tests of the production traversal route. No application entry point,
// configuration/target loader, engine, CUDA object, notifications or named IPC.
#include "traverse.h"
#include "search.h"
#include "wave_gpu_batch.h"
#include "wave_gpu_parallel.h"
#include "wave_gpu_prefetch.h"
#include "interval_progress.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>

#ifdef PKH_HAVE_CUDA
#error CPU-only fixture must not be linked with GPU support.
#endif

std::string tg_default_token(){return {};}
std::string tg_default_chat(){return {};}
Config g_cfg;
u256 g_step={1,0,0,0};
uint64_t g_total_chunks=0;
volatile sig_atomic_t g_stop_flag=0;
std::string now_string(){throw std::runtime_error("unexpected application logging");}
const char* mode_title(int){throw std::runtime_error("unexpected application logging");}
void secure_create(const std::string&){throw std::runtime_error("unexpected file permission operation");}
bool plat_fsync_file(const std::string&){throw std::runtime_error("unexpected checkpoint file sync");}
bool plat_fsync_dir(const std::string&){throw std::runtime_error("unexpected checkpoint directory sync");}
bool plat_replace_file(const std::string&,const std::string&){throw std::runtime_error("unexpected checkpoint replacement");}
std::wstring plat_wide(const std::string&){throw std::runtime_error("unexpected checkpoint file conversion");}

namespace {
using Key=std::array<uint8_t,32>;
uint64_t checks=0,cases=0,blocks=0,nonempty=0,kxe_vectors=0;
void require(bool ok,const char* message){++checks;if(!ok)throw std::runtime_error(message);}
Key bytes(const u256 value){Key out{};u256_to_be32(out.data(),value);return out;}
uint64_t fixture_u64(const uint8_t* value){uint64_t n=0;for(int i=0;i<24;++i)require(value[i]==0,"fixture ordinal must fit uint64");for(int i=24;i<32;++i)n=(n<<8)|value[i];return n;}
uint64_t fixture_u64(const u256 value){require(!(value[1]|value[2]|value[3]),"fixture scalar fits uint64");return value[0];}
std::string hex(const u256 value){const Key key=bytes(value);return to_hex(key.data(),32);}
void power(u256 value,unsigned bit){u256_zero(value);value[bit/64]=UINT64_C(1)<<(bit%64);}
ChunkIdx id64(uint64_t value){ChunkIdx id;id.from_u64(value);return id;}
ChunkIdx idpower(unsigned bit){u256 value;power(value,bit);ChunkIdx id;id.from_u256(value);return id;}
unsigned digit(const Key& key,unsigned pos){return pos%2?key[pos/2]&15:key[pos/2]>>4;}
void digit(Key& key,unsigned pos,unsigned value){if(pos%2)key[pos/2]=uint8_t((key[pos/2]&0xf0)|(value&15));else key[pos/2]=uint8_t((key[pos/2]&15)|((value&15)<<4));}
void configure(int mode,int low_bit,int high_bit,int nibbles,uint64_t seed,
               const u256 low,const u256 high,const std::string& anchor={}){
    g_cfg=Config{};g_cfg.mode=mode;g_cfg.range_start_bit=low_bit;g_cfg.range_end_bit=high_bit;
    g_cfg.det_hex=-1;g_cfg.wave_nibbles=nibbles;g_cfg.zone_nibbles=nibbles;
    g_cfg.chunk_bits=16;g_cfg.wave_start=anchor;g_cfg.mask_hex.clear();g_cfg.mask_froz=0;
    u256_set(g_range_start,low);u256_set(g_range_end,high);
    g_search.reset_progress();set_block_nibbles(nibbles);wave_set_seed(seed);compute_total_blocks256();
}
void tiny_config(int mode,int low_bit,int high_bit,int nibbles,uint64_t seed,
                 const std::string& anchor={}){
    u256 low,high;power(low,(unsigned)low_bit);power(high,(unsigned)high_bit);u256_sub_u64(high,high,1);
    configure(mode,low_bit,high_bit,nibbles,seed,low,high,anchor);
}
void bounds_and_sample(const u256 key,uint64_t count){
    if(!count)return;
    ++nonempty;require(count<=chunk_keys(),"clipped segment no larger than block");
    u256 last;u256_add_u64(last,key,count-1);
    require(u256_cmp(key,g_range_start)>=0&&u256_cmp(last,g_range_end)<=0&&u256_cmp(last,key)>=0,"actual next_chunk stays in range without wrap");
    const uint64_t offset=sample_offset(key,count);require(offset<count,"sample offset inside segment");
    u256 sample;u256_add_u64(sample,key,offset);
    require(u256_cmp(sample,key)>=0&&u256_cmp(sample,last)<=0,"actual sample helper remains inside exact segment");
}
std::vector<std::string> capture_kxe(){
    std::vector<std::string> rows;std::mt19937_64 rng(7);
    for(unsigned fixture=0;fixture<4;++fixture){
        u256 low{},high{};
        if(fixture==0){u256_from_u64(low,0x1234);u256_from_u64(high,0x9abcdef1);}
        else{power(low,fixture==2?250:40);u256_sub_u64(high,EC_N,1);}
        configure(TRAV_KXE,fixture==0?0:(fixture==2?250:40),fixture==0?32:256,3,71,low,high);
        g_cfg.chunk_bits=fixture==2?28:22;
        if(fixture==3){g_cfg.mask_hex=std::string(60,'0')+"1234";g_cfg.mask_froz=4;mask_compute();}
        compute_total_blocks256();
        std::vector<ChunkIdx> selected;
        for(uint64_t i=0;i<64;++i)selected.push_back(id64(i));
        if(fixture){for(unsigned bit:{63U,64U,128U,192U,200U}){selected.push_back(idpower(bit));ChunkIdx after=idpower(bit);after.inc();selected.push_back(after);}}
        ChunkIdx end;std::memcpy(end.b,g_total_blocks256,32);selected.push_back(end);
        u256 total;fe_from_be32(total,g_total_blocks256);u256_sub_u64(total,total,1);ChunkIdx last;last.from_u256(total);selected.push_back(last);
        for(const auto& id:selected){u256 key{};uint64_t count=0;int zone=-1;
            const bool exists=next_chunk(id,key,count,zone,rng);
            std::ostringstream row;row<<fixture<<' '<<id.hex()<<' '<<exists<<' '<<(exists?count:0)<<' '<<(exists?hex(key):std::string("end"));
            rows.push_back(row.str());++kxe_vectors;
        }
        require(mode_has_checkpoint(),"KXE checkpoint policy retained");
    }
    return rows;
}

#ifndef PKH_CAPTURE_KXE_ONLY
void exact_coverage(int mode,int nibbles,uint64_t seed){
    ++cases;tiny_config(mode,12,14,nibbles,seed);
    require(unique_blocks_mode()&&traversal_full_coverage(),"restored random modes retain exact full coverage");
    require(!mode_has_checkpoint()&&checkpoint_path().empty(),"wave/zone do not read persistent checkpoints");
    const uint64_t total=fixture_u64(g_total_blocks256);
    require(total>0&&total<=4096,"small logical grid bounded");
    std::vector<unsigned char> seen(0x3000,0);uint64_t sum=0,empty=0;
    std::mt19937_64 rng(3);g_search.reset_progress();
    for(uint64_t ordinal=0;ordinal<total;++ordinal){
        const auto id=id64(ordinal);u256 key{},other{};uint64_t count=99,count2=0;int zone=-1,zone2=-1;WaveInfo wave{};ZoneInfo info{};
        require(next_chunk(id,key,count,zone,rng),"every logical ordinal is recognized");
        require(restored_shape_block(id,other,count2,zone2,mode==TRAV_WAVE?&wave:nullptr,mode==TRAV_ZONE?&info:nullptr),"production metadata adapter recognizes same ordinal");
        require(bytes(key)==bytes(other)&&count==count2&&zone==zone2,"next_chunk and metadata use identical production block");
        bounds_and_sample(key,count);
        if(!count){++empty;debug_log_chunk(0,id,key,count);}
        else{const uint64_t first=fixture_u64(key);for(uint64_t i=0;i<count;++i){require(!seen[size_t(first+i-0x1000)],"actual production candidates never repeat");seen[size_t(first+i-0x1000)]=1;}sum+=count;g_count.fetch_add(count);}
        chunk_done(g_search,id);++blocks;
        require(checkpoint_seq(g_search)==id64(ordinal+1)&&chunks_in_flight(g_search)==0,"zero and nonzero states commit ordinal without gaps");
    }
    require(sum==0x3000&&g_count.load()==sum&&std::find(seen.begin(),seen.end(),0)==seen.end(),"actual production candidate total equals selected interval");
    u256 key{};uint64_t count=99;int zone=-1;require(!next_chunk(id64(total),key,count,zone,rng)&&count==0,"actual restored route terminates at exact logical end");
    if(mode==TRAV_WAVE&&nibbles==3)require(empty>0,"clipped-out wave shapes present and safely committed");
    require(!g_stop.load(),"CPU fixture never stops application state");
    ChunkIdx untouched=id64(19);save_checkpoint();require(!load_checkpoint(untouched)&&untouched==id64(19),"disabled checkpoint operations leave ordinal untouched");
}
void narrow_clip(int mode){
    ++cases;u256 lo,hi;u256_from_u64(lo,0x1801);u256_from_u64(hi,0x3abd);
    configure(mode,12,14,3,19,lo,hi);
    const uint64_t total=fixture_u64(g_total_blocks256);std::vector<unsigned char> seen(0x3abd-0x1801+1,0);uint64_t sum=0;std::mt19937_64 rng(1);
    for(uint64_t i=0;i<total;++i){u256 key{};uint64_t count=0;int zone;
        require(next_chunk(id64(i),key,count,zone,rng),"narrow logical state exists");bounds_and_sample(key,count);
        if(count){const uint64_t start=fixture_u64(key);for(uint64_t k=0;k<count;++k){require(!seen[size_t(start+k-0x1801)],"narrow clipping has no overlapping intersections");seen[size_t(start+k-0x1801)]=1;}sum+=count;}
    }
    require(sum==seen.size()&&std::find(seen.begin(),seen.end(),0)==seen.end(),"narrow partial boundary blocks cover exactly");
}
void fixed_wave(bool unaligned,int nibbles,bool wide){
    ++cases;u256 lo,hi,anchor;
    if(wide){power(lo,250);u256_sub_u64(hi,EC_N,1);power(anchor,251);u256_add_u64(anchor,anchor,unaligned?0x1234567:0x1234000);}
    else{u256_from_u64(lo,0x1000);u256_from_u64(hi,0x3fff);u256_from_u64(anchor,unaligned?0x2345:0x2000);}
    configure(TRAV_WAVE,wide?250:12,wide?256:14,nibbles,73,lo,hi,hex(anchor));
    require(!traversal_full_coverage(),"fixed wave reports finite shape rather than general full coverage");
    const uint64_t total=fixture_u64(g_total_blocks256);const unsigned m=unsigned(64-nibbles-g_top_ni);
    require(total==1+15*m,"fixed wave has one pattern only, never wraps to original anchor");
    const Key start=bytes(anchor);std::mt19937_64 rng(1);std::set<Key> intervals;
    for(uint64_t ordinal=0;ordinal<total;++ordinal){
        const unsigned p=ordinal?unsigned((ordinal-1)/15+1):0,t=ordinal?unsigned((ordinal-1)%15+1):0;
        Key expected=start,phase_base=start;
        for(unsigned i=0;i<m;++i){const unsigned add=p?15U*(p-1>i?p-1-i:0):0;
            const unsigned pos=63-unsigned(nibbles)-i;digit(phase_base,pos,digit(start,pos)+add);digit(expected,pos,digit(start,pos)+add+(i<p?t:0));}
        u256 actual{},direct{};uint64_t count=0,direct_count=0;int phase=-1,direct_phase=-1;WaveInfo info{};
        require(next_chunk(id64(ordinal),actual,count,phase,rng)&&restored_shape_block(id64(ordinal),direct,direct_count,direct_phase,&info),"fixed wave actual state exists");
        require(bytes(actual)==bytes(direct)&&count==direct_count&&phase==direct_phase,"fixed metadata route equals next_chunk");
        require(info.phase==int(p)&&info.step==int(t)&&bytes(info.start)==start&&bytes(info.base)==phase_base,"actual metadata matches independent original archive formula");
        // Independently intersect the raw original window with the interval.
        u256 raw;fe_from_be32(raw,expected.data());uint64_t expected_count=UINT64_C(1)<<(4*nibbles);
        if(u256_cmp(raw,hi)>0)expected_count=0;
        else if(u256_cmp(raw,lo)<0){u256 gap;u256_sub(gap,lo,raw);if(gap[1]||gap[2]||gap[3]||gap[0]>=expected_count)expected_count=0;else{expected_count-=gap[0];u256_set(raw,lo);}}
        if(expected_count){u256 room;u256_sub(room,hi,raw);if(!(room[1]|room[2]|room[3])&&room[0]<expected_count)expected_count=room[0]+1;}
        require(count==expected_count,"fixed wave clipping equals independent original window intersection");
        if(count){require(bytes(actual)==bytes(raw)&&intervals.insert(bytes(actual)).second,"fixed wave original phases never duplicate a window");bounds_and_sample(actual,count);}
    }
    u256 key{};uint64_t count=99;int phase=-1;require(!next_chunk(id64(total),key,count,phase,rng),"fixed anchor completes once instead of repeating pattern");
}
void zone_initial_shape(){
    ++cases;tiny_config(TRAV_ZONE,24,28,3,7137);
    u256 center{},key{};uint64_t count=0;int phase=-1;ZoneInfo first{};
    require(restored_shape_block(id64(0),center,count,phase,nullptr,&first),"zone initial center exists");
    const Key base=bytes(center);const unsigned m=unsigned(63-3-g_top_ni);
    std::set<unsigned> changed_positions;
    for(unsigned zone=0;zone<m;++zone)for(unsigned t=1;t<=15;++t){ZoneInfo info{};
        const uint64_t ordinal=1+uint64_t(zone)*15+t-1;
        require(restored_shape_block(id64(ordinal),key,count,phase,nullptr,&info),"zone original center state exists");
        Key expected=base;const unsigned pos=unsigned(g_top_ni)+zone+1;digit(expected,pos,digit(base,pos)+t);
        require(info.pos==int(pos)&&info.zone==int(zone+1)&&bytes(info.base)==base,"zone metadata starts at second significant nibble and advances right");
        // This 1...f range retains every same-top nibble mutation in bounds.
        require(count==4096&&bytes(key)==expected,"zone changes exactly one selected higher nibble");
        require(digit(bytes(key),unsigned(g_top_ni))==digit(base,unsigned(g_top_ni)),"zone cycle keeps most significant digit fixed");changed_positions.insert(pos);
    }
    require(changed_positions.size()==m,"all original zone positions visited");
}
void wide_samples(int mode,int nibbles){
    ++cases;u256 lo,hi;power(lo,250);u256_sub_u64(hi,EC_N,1);configure(mode,250,256,nibbles,8271,lo,hi);
    std::mt19937_64 rng(1);std::set<Key> seen;
    std::vector<ChunkIdx> selected;for(uint64_t i=0;i<2048;++i)selected.push_back(id64(i));
    for(unsigned bit:{63U,64U,128U,192U,200U})for(unsigned tail=0;tail<8;++tail){ChunkIdx id=idpower(bit);for(unsigned i=0;i<tail;++i)id.inc();selected.push_back(id);}
    for(const auto& id:selected){u256 key{};uint64_t count=0;int phase=-1;
        require(next_chunk(id,key,count,phase,rng),"wide ordinal including bits above 64 remains usable");
        if(count){bounds_and_sample(key,count);require(seen.insert(bytes(key)).second,"wide sampled ordinals do not repeat");}
    }
    require(seen.size()>1000,"wide shape sample contains substantial real in-range work");
}
void parallel_zeros(){
    ++cases;tiny_config(TRAV_WAVE,12,14,3,19);g_search.reset_progress();
    const uint64_t total=fixture_u64(g_total_blocks256);WaveGpuParallel parallel(4);WaveGpuBatch batch;
    parallel.fill(batch,32,4096,[&](ChunkIdx& id){g_chunk_seq.next(id);return std::memcmp(id.b,g_total_blocks256,32)<0;},
        [&](const ChunkIdx& id,u256 key,uint64_t& count){std::mt19937_64 rng(3);int phase;return next_chunk(id,key,count,phase,rng);});
    require(batch.completed.size()==total&&batch.pieces.size()==3,"parallel metadata builder accepts zero-key states and preserves completion IDs");
    uint64_t sum=0;for(const auto& piece:batch.pieces)sum+=piece.count;
    require(sum==0x3000,"parallel packet contains only exact in-range work");
    for(const auto& id:batch.completed)chunk_done(g_search,id);
    require(checkpoint_seq(g_search)==id64(total)&&chunks_in_flight(g_search)==0,"parallel zero states cannot leave watermark holes");
}
void cursor_rotation_drain(){
    ++cases;tiny_config(TRAV_WAVE,16,20,4,37);g_search.reset_progress();
    WaveGpuCursor cursor;WaveGpuBatch batch;bool timer=false;uint64_t claimed=0,processed=0,completed=0;
    auto next=[&](ChunkIdx& id,u256 key,uint64_t& count){if(timer)return false;id=id64(claimed++);std::mt19937_64 rng(1);int phase;return next_chunk(id,key,count,phase,rng);};
    for(unsigned launch=0;launch<32;++launch){cursor.fill(batch,2,4096,next);if(launch==0){require(claimed==1&&batch.completed.empty(),"large block remains unfinished when timer fires");timer=true;}
        for(const auto& piece:batch.pieces)processed+=piece.count;for(const auto& id:batch.completed){chunk_done(g_search,id);++completed;}
        if(batch.pieces.empty()&&batch.completed.empty())break;
    }
    require(claimed==1&&processed==65536&&completed==1,"timer stops new claims and drains all remaining pieces of the claimed block");
    require(checkpoint_seq(g_search)==id64(1)&&chunks_in_flight(g_search)==0,"drained large block commits before rotating away");
    VolatileIntervalProgress memory;memory.remember(7,checkpoint_seq(g_search),37,processed);
    ChunkIdx restored;uint64_t seed=0,checked=0;require(memory.recall(7,restored,seed,&checked)&&restored==id64(1)&&seed==37&&checked==processed,"volatile rotation preserves exact position, seed and checked count");
}
void prefetch_rotation_drain(){
    ++cases;u256 lo,hi;u256_from_u64(lo,1);u256_from_u64(hi,0xfffff);configure(TRAV_ZONE,0,20,3,37,lo,hi);g_search.reset_progress();
    WaveGpuCursor cursor;std::atomic<bool> timer{false};std::atomic<unsigned> prepares{0};uint64_t claimed=0,processed=0,completed=0;
    std::promise<void> ready,release;auto ready_future=ready.get_future();auto release_future=release.get_future().share();
    WaveGpuPrefetch prefetch([&](){WaveGpuBatch batch;const unsigned call=++prepares;
        cursor.fill(batch,4,4096,[&](ChunkIdx& id,u256 key,uint64_t& count){if(timer.load())return false;id=id64(claimed++);std::mt19937_64 rng(1);int phase;return next_chunk(id,key,count,phase,rng);});
        if(call==2){ready.set_value();release_future.wait();}return batch;},true);
    WaveGpuBatch batch;prefetch.take(batch);ready_future.wait();timer=true;release.set_value();
    auto publish=[&](){for(const auto& piece:batch.pieces)processed+=piece.count;for(const auto& id:batch.completed){chunk_done(g_search,id);++completed;}};
    publish();for(unsigned i=0;i<8;++i){prefetch.take(batch);publish();if(batch.pieces.empty()&&batch.completed.empty())break;}
    require(claimed==8&&completed==8&&processed>0,"already prepared look-ahead IDs are drained after timer fires");
    require(checkpoint_seq(g_search)==id64(8)&&chunks_in_flight(g_search)==0,"look-ahead drain prevents replay when returning to timed digit");
}
#endif
} // namespace

int main(int argc,char** argv){
    SetPriorityClass(GetCurrentProcess(),BELOW_NORMAL_PRIORITY_CLASS);
    try{
        require(argc==2,"pass only isolated synthetic KXE-vector file");
        const auto baseline=std::filesystem::absolute(std::filesystem::u8path(argv[1]));
        std::filesystem::current_path(baseline.parent_path());
        const auto kxe=capture_kxe();
#ifdef PKH_CAPTURE_KXE_ONLY
        std::ofstream out(baseline,std::ios::binary|std::ios::trunc);require(bool(out),"open synthetic baseline output");for(const auto& row:kxe)out<<row<<'\n';
        std::cout<<"PASS: captured "<<kxe_vectors<<" synthetic baseline KXE vectors; no application or GPU.\n";
#else
        std::ifstream in(baseline,std::ios::binary);require(bool(in),"open isolated baseline vectors");std::vector<std::string> expected;std::string line;while(std::getline(in,line)){if(!line.empty()&&line.back()=='\r')line.pop_back();expected.push_back(line);}
        require(kxe==expected,"actual KXE output remains byte-identical to pre-change production source across all fixtures");
        for(int mode:{TRAV_WAVE,TRAV_ZONE}){for(int n:{3,4,5})for(uint64_t seed:{UINT64_C(0),UINT64_C(173)})exact_coverage(mode,n,seed);narrow_clip(mode);for(int n:{3,4})wide_samples(mode,n);}
        for(bool unaligned:{false,true})for(int n:{3,4})fixed_wave(unaligned,n,true);
        fixed_wave(true,3,false);zone_initial_shape();parallel_zeros();cursor_rotation_drain();prefetch_rotation_drain();
        require(!g_stop.load(),"test leaves its own stop flag clear");
        std::cout<<"PASS: "<<checks<<" checks; "<<cases<<" cases; "<<blocks<<" logical blocks; "<<nonempty<<" nonempty samples; "<<kxe_vectors<<" unchanged KXE vectors. CPU only, no configuration/database/GPU/IPC/network.\n";
#endif
        return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;}
}
