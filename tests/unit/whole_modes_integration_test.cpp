// CPU-only production-route regression for complete original neighborhoods.
// Reuse the exact KXE capture fixture from the preceding release unchanged.
#include "whole_shape_traversal.h"
#define main previous_integration_fixture_main
#include "restored_modes_integration_test.cpp"
#undef main

#ifndef PKH_CAPTURE_KXE_ONLY
namespace {
uint64_t complete_patterns=0;
bool independent_clip(u256 raw,uint64_t block,uint64_t& count) {
    count=block;
    if(u256_cmp(raw,g_range_end)>0) {count=0;return false;}
    if(u256_cmp(raw,g_range_start)<0) {
        u256 gap;u256_sub(gap,g_range_start,raw);
        if(gap[1]||gap[2]||gap[3]||gap[0]>=count){count=0;return false;}
        count-=gap[0];u256_set(raw,g_range_start);
    }
    u256 room;u256_sub(room,g_range_end,raw);
    if(!(room[1]|room[2]|room[3])&&room[0]<count)count=room[0]+1;
    return count!=0;
}
void full_patterns(int mode,int nibbles,uint64_t seed,int low_bit=250) {
    ++cases;u256 low{},high{};power(low,(unsigned)low_bit);u256_sub_u64(high,EC_N,1);
    configure(mode,low_bit,256,nibbles,seed,low,high);
    require(unique_blocks_mode()&&!traversal_full_coverage(),"whole patterns are selective, never an assertion of entire range coverage");
    require(!mode_has_checkpoint()&&checkpoint_path().empty(),"whole shapes still have no persistent checkpoint I/O");
    const unsigned dimensions=unsigned(64-nibbles-g_top_ni-(mode==TRAV_ZONE?1:0));
    const uint64_t states=1+15*uint64_t(dimensions),block=UINT64_C(1)<<(4*nibbles);
    std::set<Key> intervals;std::set<std::string> high_prefixes;
    Key initial_center{},second_center{};
    std::mt19937_64 unused_rng(3);
    for(uint64_t base=0;base<64;++base) {
        Key center{};bool center_seen=false,admitted=false;
        for(uint64_t state=0;state<states;++state) {
            const auto id=id64(base*states+state);u256 key{},direct{};
            uint64_t count=0,direct_count=0;int phase=-1,direct_phase=-1;
            WaveInfo wave{};ZoneInfo zone{};
            require(next_chunk(id,key,count,phase,unused_rng),"every original state of every base exists");
            require(restored_shape_block(id,direct,direct_count,direct_phase,mode==TRAV_WAVE?&wave:nullptr,mode==TRAV_ZONE?&zone:nullptr),"metadata adapter recognizes complete pattern");
            require(bytes(key)==bytes(direct)&&count==direct_count&&phase==direct_phase,"metadata and actual production candidate agree");
            const Key actual_center=mode==TRAV_WAVE?bytes(wave.start):bytes(zone.base);
            if(!center_seen) {
                center=actual_center;center_seen=true;
                u256 base_window;fe_from_be32(base_window,center.data());uint64_t base_count=0;
                admitted=independent_clip(base_window,block,base_count);
                high_prefixes.insert(std::string(reinterpret_cast<const char*>(center.data()),10));
                if(base==0)initial_center=center;if(base==1)second_center=center;
            }
            require(actual_center==center,"base is unchanged throughout its COMPLETE original pattern");
            const unsigned p=state?unsigned((state-1)/15+1):0,t=state?unsigned((state-1)%15+1):0;
            require(phase==int(p),"no original phase skipped or substituted by ownership hierarchy");
            Key raw=center,phase_base=center;
            if(mode==TRAV_WAVE) {
                require(wave.phase==int(p)&&wave.step==int(t)&&wave.pass==1&&!wave.high,"every wave has all original growing phases");
                for(unsigned j=0;j<dimensions;++j) {
                    const unsigned completed=p>j+1?p-j-1:0;
                    const unsigned pos=63-unsigned(nibbles)-j;
                    digit(phase_base,pos,digit(center,pos)+15*completed);
                    digit(raw,pos,digit(center,pos)+15*completed+(j<p?t:0));
                }
                require(bytes(wave.base)==phase_base,"wave phase base equals independent archive formula");
            } else {
                require(zone.zone==int(p)&&zone.pos==(p?g_top_ni+int(p):-1),"zone visits every position high-to-low around every base");
                if(p) {const unsigned pos=unsigned(g_top_ni)+p;digit(raw,pos,digit(center,pos)+t);}
            }
            u256 expected;fe_from_be32(expected,raw.data());uint64_t expected_count=0;
            if(admitted)independent_clip(expected,block,expected_count);
            require(count==expected_count,"whole-pattern bounds equal independent intersection");
            if(count) {
                require(bytes(key)==bytes(expected),"actual candidate follows original complete pattern");
                bounds_and_sample(key,count);
                require(intervals.insert(bytes(key)).second,"complete patterns around distinct bases never overlap");
            }
            chunk_done(g_search,id);++blocks;
            require(checkpoint_seq(g_search)==id64(base*states+state+1),"empty or outside original states still commit correct ordinal");
        }
        ++complete_patterns;
    }
    require(std::memcmp(initial_center.data(),second_center.data(),10)!=0,"first two bases immediately change full high prefix, not just outer digit");
    require(high_prefixes.size()>=16,"many complete patterns use materially different high prefixes");
    require(intervals.size()>10000,"substantial real candidate sample within selected interval");
    u256 completed{};require(!checkpoint_completed_keys(id64(100),completed),"selective ordinal is not converted into false range progress");
}
void clipped_family(int mode,int nibbles) {
    ++cases;u256 low{},high{};u256_from_u64(low,0x1801);u256_from_u64(high,0x4abd);
    configure(mode,12,16,nibbles,19,low,high);
    require(!traversal_full_coverage(),"small family remains selective");
    const uint64_t total=fixture_u64(g_total_blocks256);require(total>0&&total<=4096,"small selective family finite");
    std::vector<unsigned char> seen(0x4abd-0x1801+1,0);std::mt19937_64 rng(1);uint64_t actual=0;
    for(uint64_t i=0;i<total;++i) {
        u256 key{};uint64_t count=0;int phase=-1;
        require(next_chunk(id64(i),key,count,phase,rng),"small clipped family state exists");
        bounds_and_sample(key,count);
        if(count) {const uint64_t first=fixture_u64(key);for(uint64_t j=0;j<count;++j){require(!seen[size_t(first+j-0x1801)],"partially clipped shapes never collapse into duplicate lower-bound interval");seen[size_t(first+j-0x1801)]=1;}actual+=count;}
    }
    require(actual>0&&actual<=seen.size(),"finite family emits only distinct selected candidates without promising all");
    u256 key{};uint64_t count=99;int phase=-1;require(!next_chunk(id64(total),key,count,phase,rng)&&count==0,"selective family ends at exact total");
}
void seed_domain_stable(int mode,int nibbles) {
    ++cases;u256 low{},high{};power(low,250);u256_sub_u64(high,EC_N,1);
    configure(mode,250,256,nibbles,0,low,high);const Key expected=std::array<uint8_t,32>{};
    (void)expected;Key total{};std::memcpy(total.data(),g_total_blocks256,32);
    for(uint64_t seed:{UINT64_C(1),UINT64_C(8271),UINT64_C(0x1020304050607080),UINT64_C(0xfeedbeef12345678)}) {
        wave_set_seed(seed);compute_total_blocks256();
        require(std::memcmp(total.data(),g_total_blocks256,32)==0,"public shape certificate and total independent of run seed initialization/recall");
        std::mt19937_64 rng(1);u256 key{};uint64_t count=0;int phase=-1;
        require(next_chunk(id64(0),key,count,phase,rng),"new run seed rebuilds valid plan cache");bounds_and_sample(key,count);
    }
}
void zeros_commit_in_parallel() {
    ++cases;tiny_config(TRAV_WAVE,12,14,3,19);g_search.reset_progress();
    const uint64_t total=fixture_u64(g_total_blocks256);WaveGpuParallel parallel(4);WaveGpuBatch batch;
    parallel.fill(batch,4096,4096,[&](ChunkIdx& id){g_chunk_seq.next(id);return std::memcmp(id.b,g_total_blocks256,32)<0;},
        [&](const ChunkIdx& id,u256 key,uint64_t& count){std::mt19937_64 rng(1);int phase;return next_chunk(id,key,count,phase,rng);});
    require(batch.completed.size()==total,"parallel completion includes every zero-state original shape");
    std::set<Key> starts;uint64_t sum=0;
    for(const auto& piece:batch.pieces){require(starts.insert(piece.start).second,"parallel packets contain unique actual pieces");sum+=piece.count;}
    for(const auto& id:batch.completed)chunk_done(g_search,id);
    require(sum>0&&sum<=0x3000&&checkpoint_seq(g_search)==id64(total),"no watermark holes or false counts from clipped-out states");
}
void small_wave_keeps_multiple_bases() {
    ++cases;tiny_config(TRAV_WAVE,34,35,3,173);
    const unsigned dimensions=unsigned(64-3-g_top_ni);
    const uint64_t states=1U+15U*dimensions,total=fixture_u64(g_total_blocks256);
    require(dimensions==6&&total>states*16U,"puzzle-35 geometry retains many complete waves rather than one identity-certificate base");
    std::set<Key> centers;std::mt19937_64 rng(1);
    for(uint64_t base=0;base<16;++base) {
        Key center{};
        for(uint64_t state=0;state<states;++state) {
            u256 key{};uint64_t count=0;int phase=-1;WaveInfo info{};
            require(restored_shape_block(id64(base*states+state),key,count,phase,&info),"small whole-wave state exists for every complete base");
            if(!state){center=bytes(info.start);require(centers.insert(center).second,"small-wave base family does not repeat center");}
            require(bytes(info.start)==center&&phase==(state?int((state-1)/15+1):0),"small wave keeps every original phase around the same base");
            bounds_and_sample(key,count);
        }
    }
    const Key total_before=bytes(g_range_end);(void)total_before;
    wave_set_seed(0);compute_total_blocks256();require(fixture_u64(g_total_blocks256)==total,"small wave count stays fixed before seed initialization");
    wave_set_seed(UINT64_C(0xfeedbeef12345678));compute_total_blocks256();require(fixture_u64(g_total_blocks256)==total,"small wave count stays fixed after seed restoration");
}
} // namespace
#endif

int main(int argc,char** argv) {
    SetPriorityClass(GetCurrentProcess(),BELOW_NORMAL_PRIORITY_CLASS);
    try {
        require(argc==2,"pass only isolated synthetic KXE vector file");
        const auto baseline=std::filesystem::absolute(std::filesystem::u8path(argv[1]));
        std::filesystem::current_path(baseline.parent_path());
        const auto kxe=capture_kxe();
#ifdef PKH_CAPTURE_KXE_ONLY
        std::ofstream out(baseline,std::ios::binary|std::ios::trunc);require(bool(out),"open isolated KXE output");
        for(const auto& row:kxe)out<<row<<'\n';
        std::cout<<"PASS: captured "<<kxe_vectors<<" exact baseline KXE vectors.\n";
#else
        std::ifstream in(baseline,std::ios::binary);require(bool(in),"read isolated KXE baseline");
        std::vector<std::string> expected;std::string line;
        while(std::getline(in,line)){if(!line.empty()&&line.back()=='\r')line.pop_back();expected.push_back(line);}
        require(kxe==expected,"all KXE outputs byte-identical to installed F58 source before new fix");
        for(int mode:{TRAV_WAVE,TRAV_ZONE}) {
            for(int n:{3,4}) {full_patterns(mode,n,8271);full_patterns(mode,n,173,251);seed_domain_stable(mode,n);}
            clipped_family(mode,3);clipped_family(mode,4);
            for(int n:{3,4})wide_samples(mode,n);
        }
        for(bool unaligned:{false,true})for(int n:{3,4})fixed_wave(unaligned,n,true);
        fixed_wave(true,3,false);zone_initial_shape();zeros_commit_in_parallel();small_wave_keeps_multiple_bases();
        cursor_rotation_drain();prefetch_rotation_drain();
        require(!g_stop.load(),"fixture never requests real application stop");
        std::cout<<"PASS: "<<checks<<" checks; "<<cases<<" cases; "<<blocks<<" production states; "
            <<nonempty<<" nonempty segment/sample checks; "<<complete_patterns<<" complete base patterns; "
            <<kxe_vectors<<" unchanged KXE vectors. CPU only, no app/GPU/config/db/IPC/network.\n";
#endif
        return 0;
    } catch(const std::exception& e) {std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;}
}
