#define main unused_first_gpu_fixture_main
#include "balanced_gpu_20261027_test.cpp"
#undef main

template<class Plan> static Timing small_measure(Plan& p,const char* mode,const char* label,unsigned run,
        std::vector<uint8_t>& out,const std::vector<uint8_t>* identical=nullptr){
    uint64_t ordinal=run>=2?40000:0,keys=0;double kernels=0;const auto begin=Clock::now();
    for(unsigned batch=0;batch<3;++batch){
        auto generated=perf_input(p,ordinal);int found=0;double ms=0;
        keys+=launch(identical?*identical:generated,out,found,ms);
        require(found==0,"empty control Bloom returns no candidates");kernels+=ms;
    }
    return {mode,label,run,keys,std::chrono::duration<double>(Clock::now()-begin).count(),kernels/1000.0};
}
int main(int argc,char**argv){
    if(argc!=2)return 2;bool initialized=false;
    try{
        auto wc=config(whole_shaped::Kind::Wave),zc=config(whole_shaped::Kind::Zone);
        balanced_shaped::Plan wave,zone;whole_shaped::Plan old_wave,old_zone;
        require(wave.init(wc)&&zone.init(zc)&&old_wave.init(wc)&&old_zone.init(zc),"control numeric plans initialized");
        uint64_t ordinal=0;const auto identical=perf_input(old_wave,ordinal);
        char why[256]{};CudaScanLaunchInfo limits{};
        require(cuda_scan_query_launch(0,256,&limits,why,sizeof why)!=0,"control device limits");
        const size_t requested=limits.fixed_allocation_bytes+size_t(656)*256*limits.allocation_bytes_per_thread;
        require(limits.memory_free>requested+(size_t(1)<<30),"control free VRAM margin");
        require(cuda_scan_init(0,656,256,why,sizeof why)!=0,"control scanner init");initialized=true;
        uint8_t step[32]{};step[31]=1;require(cuda_scan_set_step(step,why,sizeof why)!=0,"control step");
        uint8_t delta[64]{};require(cuda_scan_packed_configure(PACKED_SCAN_WAVE,delta,nullptr,1,0,0,16,wc.lower.data(),wc.upper.data(),65536)!=0,"control packed configuration");
        std::vector<uint64_t> empty(32768*BLOOM_BLOCK_WORDS,0);
        require(cuda_scan_set_targets(nullptr,1,empty.data(),32767,why,sizeof why)!=0,"control empty Bloom");
        std::vector<uint8_t> out(CUDA_MAX_OUT*32);const auto gpu_begin=Clock::now();
        deadline=gpu_begin+std::chrono::milliseconds(6500);unsigned warm_batches=0;
        do{int found=0;double km=0;launch(identical,out,found,km);++warm_batches;}
        while(Clock::now()-gpu_begin<std::chrono::seconds(2));
        std::vector<Timing> rows;
        for(unsigned run=0;run<4;++run){const bool newer=run==1||run==2;rows.push_back(newer?small_measure(wave,"identical-control","balanced",run,out,&identical):small_measure(old_wave,"identical-control","baseline",run,out,&identical));}
        for(unsigned run=0;run<4;++run){const bool newer=run==1||run==2;rows.push_back(newer?small_measure(wave,"wave","balanced",run,out):small_measure(old_wave,"wave","baseline",run,out));}
        for(unsigned run=0;run<4;++run){const bool newer=run==1||run==2;rows.push_back(newer?small_measure(zone,"zone","balanced",run,out):small_measure(old_zone,"zone","baseline",run,out));}
        const double wall=std::chrono::duration<double>(Clock::now()-gpu_begin).count();
        cuda_scan_shutdown();initialized=false;
        std::ofstream file(argv[1]);require(bool(file),"control evidence writable");
        file<<"{\n\"Result\":\"PASS\",\"GPUPhaseWallSeconds\":"<<wall<<",\"WarmupBatches\":"<<warm_batches<<",\"NoHostPowerThrottle\":true,\"MetadataInsideWallTimer\":true,\"ProductionSearchControlled\":false,\"Timings\":[\n";
        for(size_t i=0;i<rows.size();++i){const auto&r=rows[i];if(i)file<<",\n";file<<"{\"mode\":\""<<r.mode<<"\",\"scheduler\":\""<<r.scheduler<<"\",\"run\":"<<r.run<<",\"keys\":"<<r.keys<<",\"wall_seconds\":"<<r.wall<<",\"kernel_seconds\":"<<r.kernel<<",\"wall_keys_per_second\":"<<double(r.keys)/r.wall<<"}";}
        file<<"\n]}\n";std::printf("PASS control GPU_phase_seconds=%.3f warmup_batches=%u\n",wall,warm_batches);return 0;
    }catch(const std::exception&e){if(initialized)cuda_scan_shutdown();std::fprintf(stderr,"FAIL GPU control: %s\n",e.what());return 1;}
}
