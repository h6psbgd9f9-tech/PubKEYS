// Compile this probe against either baseline_sources or the current root.
// Same synthetic inputs, actual checkpoint serializer and actual KXE mapping.
#define main inherited_invariants_not_run
#include "tests/unit/traversal_invariants_20261027_test.cpp"
#undef main
int main(int argc,char** argv){
    try{
        require(argc==2&&fs::current_path()==fs::absolute(fs::u8path(argv[1])),"exact isolated fixture CWD required");
        require(fs::directory_iterator(fs::current_path())==fs::directory_iterator(),"fixture starts empty");
        std::string evidence;
        for(int mode:{TRAV_SEQ,TRAV_KXE})for(int bits:{10,12})for(uint64_t width:{1,1023,1024,1025,3089}){
            reset(mode,0x10001,width,bits);PubEntry a{},b{};a.x[0]=1;a.par=1;b.x[0]=2;b.par=3;
            g_db.items={b,a,a};g_db.build();std::string why;
            require(checkpoint_prepare_identity(g_db,why),"target fingerprint prepared");
            wave_set_seed(7);const uint64_t limit=(width+(UINT64_C(1)<<bits)-1)>>bits;
            for(uint64_t i=0;i<=limit;++i){evidence+=checkpoint_path()+"\n"+checkpoint_payload(id(i),7);
                if(i<limit){u256 key{};uint64_t count=0;int phase=-1;std::mt19937_64 rng(1);
                    require(next_chunk(id(i),key,count,phase,rng),"legacy ordinal exists");evidence+=key_hex(key)+" "+std::to_string(count)+"\n";}}
        }
        uint8_t digest[32];sha256((const uint8_t*)evidence.data(),evidence.size(),digest);
        std::printf("legacy_sha256=%s cases=%llu GPU=0 notifications=0\n",to_hex(digest,32).c_str(),(unsigned long long)cases);return 0;
    }catch(const std::exception& error){std::fprintf(stderr,"FAIL legacy probe: %s\n",error.what());return 1;}
}
