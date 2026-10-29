// Reuse the audited filesystem-only fixture primitives, not its old main.
// This compiles the actual current traverse.cpp and numeric traversal backend.
#define main inherited_invariants_not_run
#include "tests/unit/traversal_invariants_20261027_test.cpp"
#undef main

namespace {
struct SavedWindow {std::string key;uint64_t count;int phase;};
SavedWindow window_at(uint64_t ordinal) {
    u256 key{};uint64_t count=0;int phase=-1;std::mt19937_64 rng(17);
    require(next_chunk(id(ordinal),key,count,phase,rng),"shape ordinal exists");
    return {key_hex(key),count,phase};
}
void shape_setup(int mode,unsigned low=3,bool fixed=false) {
    reset(mode,0x1001,65536+91,12);
    g_cfg.wave_nibbles=g_cfg.zone_nibbles=int(low);set_block_nibbles(int(low));
    wave_set_seed(UINT64_C(0x123456789abcdef));
    if(fixed){u256 start;u256_from_u64(start,0x2345);g_cfg.wave_start=key_hex(start);}
    compute_top_nibble();compute_zone_plan();compute_total_blocks256();
    PubEntry a{},b{};a.x[0]=1;a.par=1;b.x[0]=2;b.par=3;
    g_db.items={b,a,a};g_db.build();std::string why;
    require(checkpoint_prepare_identity(g_db,why),"shape target identity prepared");
    checkpoint_clear_loaded_shape();checkpoint_reset(g_search,id(0));
    require(mode_has_checkpoint(),"shape checkpoint enabled");
    require(checkpoint_path().find("_balanced_v1_")!=std::string::npos,"new shape namespace");
}
uint64_t small_limit() {u256 value;fe_from_be32(value,g_total_blocks256);return scalar64(value);}
void commit_range(uint64_t first,uint64_t end) {
    for(uint64_t i=first;i<end;++i){const auto w=window_at(i);chunk_done(g_search,id(i),w.count);}
}
void cover_small_interval(uint64_t limit) {
    constexpr uint64_t width=65536+91,first=0x1001;
    std::vector<uint8_t> seen(size_t(width),0);
    for(uint64_t i=0;i<limit;++i){const auto window=window_at(i);u256 start{};
        require(hex_to_u256(window.key.c_str(),start),"window scalar parses");
        if(!window.count)continue;
        require(!(start[1]|start[2]|start[3])&&start[0]>=first&&start[0]-first<width,"nonempty window lies in interval");
        require(window.count<=width-(start[0]-first),"window tail clipped exactly");
        for(uint64_t k=0;k<window.count;++k)require(!seen[size_t(start[0]-first+k)]++,"each primary key occurs once across complete family and complement");
    }
    require(std::all_of(seen.begin(),seen.end(),[](uint8_t n){return n==1;}),"every eligible primary key covered");
}
void exact_resume(int mode,unsigned low,bool fixed) {
    shape_setup(mode,low,fixed);const uint64_t limit=small_limit();
    require(limit>1,"fixture has several logical slots");
    cover_small_interval(limit);
    const uint64_t cut=(std::min)(limit-1,(std::max)(UINT64_C(1),limit/2));
    commit_range(0,cut);
    ChunkIdx snap;u256 count{};require(checkpoint_snapshot(g_search,snap,count)&&snap==id(cut),"coherent prefix snapshot");
    std::vector<SavedWindow> expected;
    for(uint64_t i=cut;i<(std::min)(limit,cut+31);++i)expected.push_back(window_at(i));
    const auto identity=checkpoint_search_identity();const auto path=checkpoint_path();
    save_checkpoint();const auto text=read(path);
    require(text.rfind("PKHSCP1\n",0)==0&&text.find("confirmed_keys ")!=std::string::npos,"new complete shape payload");
    wave_set_seed(987654321);compute_total_blocks256();
    checkpoint_reset(g_search,id(0));ChunkIdx restored;
    require(load_checkpoint(restored)&&restored==id(cut),"real shape load succeeds");
    require(wave_get_seed()==UINT64_C(0x123456789abcdef),"saved seed restored");
    require(checkpoint_search_identity()==identity,"seed excluded from identity");
    require(checkpoint_seq(g_search)==id(0),"load has not applied acceptance to SearchContext");
    ChunkIdx staged;u256 staged_count{};uint64_t seed=0;
    require(checkpoint_loaded_shape(staged,staged_count,&seed)&&staged==restored&&u256_cmp(staged_count,count)==0,"loaded count staged exactly");
    for(size_t i=0;i<expected.size();++i){const auto actual=window_at(cut+i);
        require(actual.key==expected[i].key&&actual.count==expected[i].count&&actual.phase==expected[i].phase,"next mapping identical after seed restoration");}
    checkpoint_reset(g_search,restored,staged_count);
    commit_range(cut,limit);u256 final{};
    require(checkpoint_snapshot(g_search,snap,final)&&snap==id(limit),"continued to exact endpoint");
    require(scalar64(final)==65536+91,"resumed committed count equals actual interval cardinality");
    save_checkpoint();checkpoint_clear_loaded_shape();wave_set_seed(0);compute_total_blocks256();
    require(load_checkpoint(restored)&&restored==id(limit),"completed shape checkpoint reloads");
    require(checkpoint_loaded_shape(staged,staged_count)&&scalar64(staged_count)==65536+91,"completed payload exact count");
    u256 key{};uint64_t n=0;int phase=-1;std::mt19937_64 rng(1);
    require(!next_chunk(restored,key,n,phase,rng),"completed resume issues no further keys");
}
void weights() {
    shape_setup(TRAV_WAVE);ChunkIdx seq;u256 count{};
    chunk_done(g_search,id(2),7);require(checkpoint_snapshot(g_search,seq,count)&&seq==id(0)&&scalar64(count)==0,"ahead work not counted before gap");
    chunk_done(g_search,id(0),11);require(checkpoint_snapshot(g_search,seq,count)&&seq==id(1)&&scalar64(count)==11,"first prefix advances independently");
    chunk_done(g_search,id(1),0);require(checkpoint_snapshot(g_search,seq,count)&&seq==id(3)&&scalar64(count)==18,"empty slot merges weighted ahead completion");
    chunk_done(g_search,id(1),0);require(checkpoint_snapshot(g_search,seq,count)&&scalar64(count)==18,"duplicate counted once");
    checkpoint_reset(g_search,id(0));set_stop();
    if(!g_stop.load())chunk_done(g_search,id(0),13);
    require(checkpoint_snapshot(g_search,seq,count)&&seq==id(0)&&scalar64(count)==0,"caller leaves interrupted slot unpublished");
    g_stop=false;g_shutdown_requested=false;g_stop_flag=0;
    checkpoint_reset(g_search,id(0));chunk_done(g_search,id(0));
    require(!checkpoint_snapshot(g_search,seq,count),"unweighted shape commit cannot be persisted with invented count");
    checkpoint_reset(g_search,id(0));
    for(unsigned i=0;i<32;++i)chunk_done(g_search,id(i),UINT64_C(1)<<60);
    require(checkpoint_snapshot(g_search,seq,count)&&seq==id(32)&&count[1]==2&&count[0]==0,"weighted sum exceeds64 bits exactly");
}
void parallel_snapshots() {
    shape_setup(TRAV_ZONE);checkpoint_reset(g_search,id(0));
    constexpr uint64_t size=4000;std::atomic<bool> finished{false};std::atomic<unsigned> errors{0};
    std::thread reader([&]{while(!finished){ChunkIdx seq;u256 count{};if(!checkpoint_snapshot(g_search,seq,count)){++errors;continue;}
        u256 position{};seq.to_u256(position);const uint64_t pos=position[0];if(count[0]!=pos*(pos+1)/2||count[1]||count[2]||count[3])++errors;}});
    std::vector<std::thread> workers;
    for(unsigned t=0;t<4;++t)workers.emplace_back([&,t]{for(uint64_t i=t;i<size;i+=4)chunk_done(g_search,id(i),i+1);});
    for(auto& thread:workers)thread.join();finished=true;reader.join();
    ChunkIdx seq;u256 count{};
    require(errors==0&&checkpoint_snapshot(g_search,seq,count)&&seq==id(size)&&count[0]==size*(size+1)/2,"concurrent position/count snapshots never tear");
}
std::string signed_shape(const ChunkIdx& seq,uint64_t seed,const u256 count) {
    const auto body=checkpoint_payload(seq,seed,count);uint8_t digest[32];sha256((const uint8_t*)body.data(),body.size(),digest);
    return body+"checksum "+to_hex(digest,32)+"\n";
}
void rejection() {
    shape_setup(TRAV_WAVE);const auto path=checkpoint_path();
    checkpoint_reset(g_search,id(0));wave_set_seed(0);compute_total_blocks256();save_checkpoint();
    require(fs::exists(path),"seed zero checkpoint exists");wave_set_seed(789);compute_total_blocks256();ChunkIdx restored;
    require(load_checkpoint(restored)&&wave_get_seed()==0,"zero seed valid and restored");
    checkpoint_clear_loaded_shape();u256 cleared{};require(!checkpoint_loaded_shape(restored,cleared),"loaded accessor clears without context mutation");
    // Every failure must preserve the caller's sequence and not mutate the seed.
    auto reject=[&](const std::string& payload){put(path,payload);wave_set_seed(77);compute_total_blocks256();ChunkIdx seq=id(123);
        require(!load_checkpoint(seq)&&seq==id(123)&&wave_get_seed()==77,"invalid load transactional for seed and position");
        u256 count{};require(!checkpoint_loaded_shape(seq,count),"invalid load clears staged state");};
    reject("PKHCPU1\nidentity obsolete\n");
    reject(std::string(1025,'x'));
    u256 zero{};auto good=signed_shape(id(0),5,zero);good.back()='x';reject(good);
    u256 bad_count{};u256_from_u64(bad_count,1);reject(signed_shape(id(0),5,bad_count));
    u256_from_u64(bad_count,65536+92);reject(signed_shape(id(1),5,bad_count));
    u256_from_u64(bad_count,65536);reject(signed_shape(id(small_limit()),5,bad_count));
    reject(signed_shape(id(small_limit()+1),5,zero));
    const auto old_path=std::string("checkpoint_wave_unique_v1_")+checkpoint_search_identity()+".dat";
    put(old_path,"old finite state");fs::remove(path);
    require(!load_checkpoint(restored)&&read(old_path)=="old finite state","old namespace neither accepted nor overwritten");
}
void legacy_serialization() {
    for(int mode:{TRAV_SEQ,TRAV_KXE}){
        reset(mode,0x10001,3089,10);PubEntry a{};a.x[0]=1;a.par=1;g_db.items={a};g_db.build();std::string why;
        require(checkpoint_prepare_identity(g_db,why),"legacy identity prepared");
        const auto expected=std::string("PKHCP2\nidentity ")+checkpoint_search_identity()+"\nsequence "+id(1).hex()+"\nwave_seed 7\n";
        require(checkpoint_payload(id(1),7)==expected,"SEQ/KXE payload byte structure unchanged");
        require(checkpoint_path().find("_v2_")!=std::string::npos,"SEQ/KXE namespace retained");
    }
    canonical_targets_and_roundtrip();
    for(int mode:{TRAV_SEQ,TRAV_KXE})for(int bits:{10,12})for(uint64_t width:{1,1023,1024,1025,3089})metadata(mode,width,bits);
    watermark_and_carry();
}
}
int main(int argc,char** argv) {
    try{
        require(argc==2&&fs::current_path()==fs::absolute(fs::u8path(argv[1])),"exact isolated fixture CWD required");
        require(fs::directory_iterator(fs::current_path())==fs::directory_iterator(),"fixture starts empty");
        for(int mode:{TRAV_WAVE,TRAV_ZONE})for(unsigned low:{3U,4U,15U})exact_resume(mode,low,false);
        exact_resume(TRAV_WAVE,3,true);weights();parallel_snapshots();rejection();legacy_serialization();atomic_faults();
        std::printf("PASS shape checkpoint checks=%llu cases=%llu GPU=0 notifications=0\n",(unsigned long long)checks,(unsigned long long)cases);return 0;
    }catch(const std::exception& error){std::fprintf(stderr,"FAIL shape checkpoint: %s\n",error.what());return 1;}
}
