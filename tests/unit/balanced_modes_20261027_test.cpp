// Only numeric traversal metadata. No application, GPU, target DB or notifications.
#define PKH_CAPTURE_KXE_ONLY
#define main unused_old_numeric_fixture_main
#include "restored_modes_integration_test.cpp"
#undef main
#undef PKH_CAPTURE_KXE_ONLY
#include "packed_traversal.h"

namespace {
uint64_t complete_cases=0,complete_windows=0;
struct Window { Key first{},last{};uint64_t count=0; };
void verify_union(std::vector<Window>& windows,const u256 lower,const u256 upper) {
    std::sort(windows.begin(),windows.end(),[](const Window& a,const Window& b){return a.first<b.first;});
    require(!windows.empty(),"nonempty interval has work");
    require(windows.front().first==bytes(lower),"coverage begins exactly at selected lower bound");
    require(windows.back().last==bytes(upper),"coverage ends exactly at selected upper bound");
    for(size_t i=1;i<windows.size();++i) {
        u256 prior{},following{};fe_from_be32(prior,windows[i-1].last.data());
        u256_add_u64(following,prior,1);
        require(bytes(following)==windows[i].first,"sorted windows have no gaps or repeats");
    }
}
void exact_numeric(int mode,uint64_t lo,uint64_t hi,int n,uint64_t seed,uint64_t fixed=0) {
    ++complete_cases;u256 lower{},upper{},anchor{};u256_from_u64(lower,lo);u256_from_u64(upper,hi);
    u256_from_u64(anchor,fixed);unsigned highbit=1;while(highbit<64&&(hi>>highbit))++highbit;
    configure(mode,0,int(highbit),n,seed,lower,upper,fixed?hex(anchor):std::string());
    require(traversal_full_coverage(),"production wave/zone promises exact interval coverage");
    require(!mode_has_checkpoint(),"no persistent checkpoint is read or written");
    PackedTraversalPlan packed;require(!packed_traversal_prepare(packed),"legacy GPU row mapping stays disabled for complete shapes");
    const uint64_t total=fixture_u64(g_total_blocks256);
    require(total>0&&total<2000000,"finite fixture metadata count is bounded");
    std::vector<Window> windows;std::mt19937_64 rng(7);uint64_t sum=0;
    for(uint64_t i=0;i<total;++i) {
        u256 key{},last{};uint64_t count=0;int phase=-1;
        require(next_chunk(id64(i),key,count,phase,rng),"all complete logical positions remain available");
        if(count) {
            require(count<=chunk_keys(),"window fits configured block");
            u256_add_u64(last,key,count-1);
            require(u256_cmp(key,lower)>=0&&u256_cmp(last,upper)<=0,"window clipped to exact interval");
            windows.push_back({bytes(key),bytes(last),count});sum+=count;++complete_windows;
        }
        if(i<20||i+20>=total) {
            u256 repeated{};uint64_t rc=0;int rp=-1;
            require(restored_shape_block(id64(i),repeated,rc,rp),"reference adapter recognizes same complete position");
            require(count==rc&&phase==rp&&(!count||bytes(key)==bytes(repeated)),"production and metadata agree on full-pass position");
        }
        chunk_done(g_search,id64(i));
    }
    verify_union(windows,lower,upper);
    require(sum==hi-lo+1,"every scalar appears exactly once, including both endpoints");
    require(checkpoint_seq(g_search)==id64(total),"completion watermark includes empty complement slots");
    u256 end{};uint64_t end_count=99;int end_phase=-1;
    require(!next_chunk(id64(total),end,end_count,end_phase,rng)&&end_count==0,"only full interval exhaustion ends issuance");
    require(!g_stop.load(),"traversal does not request stopping on candidate or exhaustion");
}
void parallel_numeric(int mode) {
    ++complete_cases;u256 lower{},upper{};u256_from_u64(lower,0x1801);u256_from_u64(upper,0x24abd);
    configure(mode,0,18,3,19,lower,upper);
    const uint64_t total=fixture_u64(g_total_blocks256);std::vector<Window> windows;
    WaveGpuParallel parallel(4);WaveGpuBatch batch;g_search.reset_progress();
    for(;;) {
        parallel.fill(batch,17,4096,[&](ChunkIdx& id){g_chunk_seq.next(id);return std::memcmp(id.b,g_total_blocks256,32)<0;},
            [&](const ChunkIdx& id,u256 key,uint64_t& count){std::mt19937_64 rng(1);int phase=-1;return next_chunk(id,key,count,phase,rng);});
        if(batch.completed.empty())break;
        for(const auto& piece:batch.pieces){u256 key{},last{};fe_from_be32(key,piece.start.data());u256_add_u64(last,key,piece.count-1);windows.push_back({piece.start,bytes(last),piece.count});}
        for(const auto& id:batch.completed)chunk_done(g_search,id);
    }
    verify_union(windows,lower,upper);
    require(checkpoint_seq(g_search)==id64(total),"parallel metadata commits every full-coverage ordinal");
}
void wide_endpoints(int mode) {
    ++complete_cases;u256 lower{},upper{};power(lower,250);u256_sub_u64(upper,EC_N,1);
    configure(mode,250,256,4,8271,lower,upper);u256 total{};fe_from_be32(total,g_total_blocks256);
    require(total[1]||total[2]||total[3],"complete total retains widths above uint64");
    u256 last_number{};u256_sub_u64(last_number,total,1);ChunkIdx last;last.from_u256(last_number);
    std::mt19937_64 rng(1);u256 key{},edge{};uint64_t count=0;int phase=-1;
    require(next_chunk(last,key,count,phase,rng),"last full-domain ordinal exists");
    if(count){u256_add_u64(edge,key,count-1);require(u256_cmp(edge,upper)==0,"last nonempty completion window ends exactly at upper scalar");}
    ChunkIdx limit;limit.from_u256(total);require(!next_chunk(limit,key,count,phase,rng),"256-bit completion domain terminates at its exact total");
}
void stale_mask_ignored(int mode,int n,int frozen) {
    ++complete_cases;u256 lower{},upper{};u256_from_u64(lower,0x1801);u256_from_u64(upper,0x4abd);
    configure(mode,0,16,n,173,lower,upper);const uint64_t total=fixture_u64(g_total_blocks256);
    std::vector<Window> expected;std::mt19937_64 rng(11);
    for(uint64_t i=0;i<total;++i){u256 key{};uint64_t count=0;int phase=-1;require(next_chunk(id64(i),key,count,phase,rng),"reference full-pass position exists");expected.push_back({bytes(key),Key{},count});}
    g_cfg.mask_hex=std::string(60,'0')+"1234";g_cfg.mask_froz=frozen;
    require(!mask_active(),"mask is a deterministic-mode feature; stale fields do not activate it in wave/zone");
    require(mask_compute(),"inactive mask remains a harmless no-op");compute_total_blocks256();
    require(total==fixture_u64(g_total_blocks256),"stale mask settings leave production full-pass count unchanged");
    for(uint64_t i=0;i<total;++i){u256 key{};uint64_t count=0;int phase=-1;require(next_chunk(id64(i),key,count,phase,rng),"position still exists with stale mask settings");require(count==expected[size_t(i)].count&&(!count||bytes(key)==expected[size_t(i)].first),"stale mask fields leave actual scalar schedule byte-identical");}
}

unsigned key_bits(const u256 key) {
    const Key value=bytes(key);
    for(unsigned i=0;i<32;++i)if(value[i]){
        unsigned leading=0,v=value[i];while(v){++leading;v>>=1;}
        return (31U-i)*8U+leading;
    }
    return 0;
}
uint64_t balanced_groups=0,balanced_states=0;
void wide_balanced_patterns(int mode) {
    ++complete_cases;u256 lower{},upper{};power(lower,139);u256_sub_u64(upper,EC_N,1);
    configure(mode,139,256,4,173,lower,upper);
    std::array<unsigned,257> visits{};std::vector<unsigned> first_order;
    uint64_t ordinal=0;std::mt19937_64 rng(1);
    for(unsigned round=0;round<2;++round)for(unsigned turn=0;turn<117;++turn) {
        u256 first{};uint64_t first_count=0;int first_phase=-1;WaveInfo first_wave{};ZoneInfo first_zone{};
        require(restored_shape_block(id64(ordinal),first,first_count,first_phase,
            mode==TRAV_WAVE?&first_wave:nullptr,mode==TRAV_ZONE?&first_zone:nullptr),"balanced base position exists");
        require(first_count&&first_phase==0,"every wide-bucket visit starts with an admitted complete base");
        const unsigned bits=key_bits(first),top=64U-(bits+3U)/4U;
        require(bits>=140&&bits<=256,"configured exponent139 retains actual bit lengths140..256");
        require(visits[bits]==round,"one whole-pattern turn per nonexhausted bucket per round");++visits[bits];
        if(!round)first_order.push_back(bits);else require(first_order[turn]==bits,"round-robin bucket order remains stable");
        u256 bucket_lo{},bucket_hi{};power(bucket_lo,bits-1);
        if(bits==256)u256_sub_u64(bucket_hi,EC_N,1);else{power(bucket_hi,bits);u256_sub_u64(bucket_hi,bucket_hi,1);}
        const unsigned dimensions=mode==TRAV_WAVE?60U-top:59U-top;
        const unsigned states=1U+15U*dimensions;
        const Key base=mode==TRAV_WAVE?bytes(first_wave.start):bytes(first_zone.base);
        for(unsigned state=0;state<states;++state,++ordinal) {
            const unsigned p=state?(state-1U)/15U+1U:0U,t=state?(state-1U)%15U+1U:0U;
            u256 key{},next{},raw{},last{};uint64_t count=0,next_count=0;int phase=-1,next_phase=-1;WaveInfo wi{};ZoneInfo zi{};
            require(restored_shape_block(id64(ordinal),key,count,phase,mode==TRAV_WAVE?&wi:nullptr,mode==TRAV_ZONE?&zi:nullptr),"every original pattern state remains contiguous");
            require(next_chunk(id64(ordinal),next,next_count,next_phase,rng),"production balanced next_chunk returns same pattern state");
            require(count==next_count&&phase==next_phase&&(!count||bytes(key)==bytes(next)),"metadata and production scheduler agree");
            require(phase==int(p),"bit rotation never cuts an original pattern phase");
            Key expected=base;
            if(mode==TRAV_WAVE) {
                require(bytes(wi.start)==base&&wi.step==int(t)&&wi.top==int(63U-top),"wave base, step and local top remain exact");
                for(unsigned j=0;j<dimensions;++j){const unsigned add=p?15U*(p-1U>j?p-1U-j:0U):0U;
                    const unsigned pos=59U-j;digit(expected,pos,digit(base,pos)+add+(j<p?t:0U));}
            }else {
                require(bytes(zi.base)==base&&zi.pos==(p?int(top+p):-1),"zone keeps one base and reports bucket-local position");
                if(p)digit(expected,top+p,digit(base,top+p)+t);
            }
            fe_from_be32(raw,expected.data());uint64_t expected_count=65536;
            if(u256_cmp(raw,bucket_hi)>0)expected_count=0;
            else if(u256_cmp(raw,bucket_lo)<0){u256 gap{};u256_sub(gap,bucket_lo,raw);if(gap[1]||gap[2]||gap[3]||gap[0]>=expected_count)expected_count=0;else{expected_count-=gap[0];u256_set(raw,bucket_lo);}}
            if(expected_count){u256 room{};u256_sub(room,bucket_hi,raw);if(!(room[1]|room[2]|room[3])&&room[0]<expected_count)expected_count=room[0]+1;}
            require(count==expected_count,"unchanged archive shape is clipped only at bit-bucket boundaries");
            if(count){require(bytes(key)==bytes(raw),"actual state matches independent original nibble formula");u256_add_u64(last,key,count-1);require(u256_cmp(key,bucket_lo)>=0&&u256_cmp(last,bucket_hi)<=0,"pattern state never leaves its admitted bit bucket");}
            ++balanced_states;
        }
        ++balanced_groups;
    }
    for(unsigned bits=140;bits<=256;++bits)require(visits[bits]==2,"all117 active bit buckets visited in both initial rounds");
}
}

int main(int argc,char** argv) {
    SetPriorityClass(GetCurrentProcess(),BELOW_NORMAL_PRIORITY_CLASS);
    try {
        require(argc==2,"pass isolated KXE-vector baseline file");
        const auto actual=capture_kxe();std::ifstream input(argv[1]);require(bool(input),"read immutable numeric KXE baseline");
        std::vector<std::string> expected;std::string line;while(std::getline(input,line)){if(!line.empty()&&line.back()=='\r')line.pop_back();expected.push_back(line);}
        require(actual==expected,"KXE remains byte-identical across294 reference vectors");
        for(int mode:{TRAV_WAVE,TRAV_ZONE}) {
            for(int n:{3,4,5,15})for(uint64_t seed:{UINT64_C(0),UINT64_C(173)}) {
                exact_numeric(mode,1,1,n,seed);exact_numeric(mode,0x1801,0x4abd,n,seed);
                exact_numeric(mode,0x1000,0x2ffff,n,seed);
            }
            exact_numeric(mode,UINT64_C(1)<<34,(UINT64_C(1)<<35)-1,4,173);
            parallel_numeric(mode);wide_endpoints(mode);wide_balanced_patterns(mode);
            for(int n:{3,4})for(int frozen:{1,4,64})stale_mask_ignored(mode,n,frozen);
        }
        for(int n:{3,4}){
            exact_numeric(TRAV_WAVE,0x1801,0x24abd,n,19,0x12345);
            exact_numeric(TRAV_WAVE,1,0x24abd,n,8271,0x183);
        }
        std::cout<<"PASS: "<<checks<<" assertions; "<<complete_cases<<" balanced full interval cases; "<<complete_windows<<" actual windows; "<<balanced_groups<<" complete balanced base visits; "<<balanced_states<<" unchanged original states; "<<kxe_vectors<<" unchanged KXE vectors. CPU metadata only.\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;}
}
