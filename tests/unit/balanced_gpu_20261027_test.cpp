// Isolated CUDA fixture: no application, transports, production data or IPC.
#include "balanced_shape_traversal.h"
#include "cuda_scan.h"
#include "packed_scan.h"
#include "scan_secp.h"
#include "secp256k1.h"
#include "bloom.h"
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using Word=whole_shaped::Word;
using Clock=std::chrono::steady_clock;
static constexpr unsigned PERF_SEGMENTS=2624;
static unsigned checks=0;
static Clock::time_point deadline;
static void require(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
static void budget(){require(Clock::now()<deadline,"bounded GPU test budget exhausted");}
static Word add(const Word& a,uint64_t value){Word r{};require(shaped::detail::add(a,shaped::detail::from_u64(value),r),"scalar addition overflow");return r;}
static whole_shaped::Config config(whole_shaped::Kind kind){
    whole_shaped::Config c;c.kind=kind;c.seed=173;c.low_nibbles=4;c.top_index=0;
    c.lower[14]=8; // 2^139, big endian
    u256 upper{};u256_sub_u64(upper,EC_N,1);u256_to_be32(c.upper.data(),upper);
    c.anchor.fill(0x67);c.anchor[31]=0;
    return c;
}
struct Input {std::vector<uint8_t> starts;std::set<Word> wanted;};
static Input correctness_input(balanced_shaped::Plan& p){
    Input result;uint64_t ordinal=0;
    require(p.bucket_count()==117,"all 117 selected exponent buckets exist");
    for(size_t b=0;b<p.bucket_count();++b){
        unsigned accepted=0;
        for(unsigned state=0;state<p.bucket_pattern_states(b)&&accepted<2;++state){
            whole_shaped::Result r;unsigned bits=0;
            require(p.get_complete(shaped::detail::from_u64(ordinal+state),r,&bits),"balanced position exists");
            require(bits==140+b,"balanced bit-length routing");
            if(r.count!=65536)continue;
            result.starts.insert(result.starts.end(),r.start.begin(),r.start.end());
            const uint64_t offsets[]={0,CUDA_HALF,65535};
            require(result.wanted.insert(add(r.start,offsets[(b+accepted)%3])).second,"unique synthetic target");
            ++accepted;
        }
        require(accepted==2,"two full segments available in each selected bit bucket");
        ordinal+=p.bucket_pattern_states(b);
    }
    return result;
}
static uint64_t launch(const std::vector<uint8_t>& starts,std::vector<uint8_t>& out,int& found,double& kernel_ms){
    budget();const int count=int(starts.size()/32);std::vector<PackedSegment> segments(count);
    for(int i=0;i<count;++i)segments[i]={0,uint32_t(i)};
    uint8_t sample[32]{};
    const uint64_t got=cuda_scan_packed_batch(starts.data(),count,segments.data(),count,sample,out.data(),CUDA_MAX_OUT,&found);
    require(got==uint64_t(count)*65536,"exact GPU checked count");
    require(found>=0&&found<CUDA_MAX_OUT,"candidate output capacity not reached");
    require(std::memcmp(sample,starts.data(),32)==0,"GPU sample matches host start");
    cuda_scan_last_cost(nullptr,&kernel_ms,nullptr,nullptr);
    return got;
}
static unsigned verify_hits(const Input& input,std::vector<uint8_t>& out,char* why){
    std::vector<uint64_t> bloom(32768*BLOOM_BLOCK_WORDS,0);
    std::vector<uint8_t> targets;targets.reserve(input.wanted.size()*32);
    for(const auto& key:input.wanted){uint8_t pc[33]{},py[32]{};require(scan_secp_pubkey(key.data(),pc,py)!=0,"independent CPU target public point");targets.insert(targets.end(),pc+1,pc+33);u256 x{};fe_from_be32(x,pc+1);bloom_add(bloom.data(),32767,x);}
    require(cuda_scan_set_targets(targets.data(),int(input.wanted.size()),bloom.data(),32767,why,256)!=0,"synthetic Bloom uploaded");
    int found=0;double kernel=0;launch(input.starts,out,found,kernel);
    std::set<Word> seen;
    for(int i=0;i<found;++i){Word key{};std::memcpy(key.data(),out.data()+size_t(i)*32,32);if(input.wanted.count(key))require(seen.insert(key).second,"known target returned once");}
    require(seen==input.wanted,"all GPU targets agree with CPU oracle");
    return unsigned(seen.size());
}
struct Timing {const char* mode;const char* scheduler;unsigned run;uint64_t keys;double wall,kernel;};
template<class Plan> static std::vector<uint8_t> perf_input(Plan& p,uint64_t& ordinal){
    std::vector<uint8_t> starts;starts.reserve(PERF_SEGMENTS*32);
    while(starts.size()<PERF_SEGMENTS*32){whole_shaped::Result r;require(p.get_complete(shaped::detail::from_u64(ordinal++),r),"benchmark metadata position exists");if(r.count==65536)starts.insert(starts.end(),r.start.begin(),r.start.end());}
    return starts;
}
template<class Plan> static Timing measure(Plan& p,const char* mode,const char* scheduler,unsigned run,std::vector<uint8_t>& out){
    uint64_t ordinal=run>=2?40000:0,keys=0;double kernels=0;const auto begin=Clock::now();
    for(unsigned batch=0;batch<12;++batch){auto starts=perf_input(p,ordinal);int found=0;double ms=0;keys+=launch(starts,out,found,ms);require(found==0,"empty synthetic Bloom returns no candidates");kernels+=ms;}
    return Timing{mode,scheduler,run,keys,std::chrono::duration<double>(Clock::now()-begin).count(),kernels/1000.0};
}
int main(int argc,char**argv){
    if(argc!=2)return 2;bool initialized=false;
    try{
        auto wc=config(whole_shaped::Kind::Wave),zc=config(whole_shaped::Kind::Zone);
        balanced_shaped::Plan wave,zone;whole_shaped::Plan old_wave,old_zone;
        require(wave.init(wc)&&zone.init(zc)&&old_wave.init(wc)&&old_zone.init(zc),"all numeric plans initialized");
        const auto wave_input=correctness_input(wave),zone_input=correctness_input(zone);
        require(scan_secp_init()!=0,"independent CPU oracle initialized");
        char why[256]{};
        CudaScanLaunchInfo limits{};
        require(cuda_scan_query_launch(0,256,&limits,why,sizeof why)!=0,"device launch limits read");
        const size_t requested=limits.fixed_allocation_bytes+size_t(656)*256*limits.allocation_bytes_per_thread;
        require(limits.memory_free>requested+(size_t(1)<<30),"free VRAM exceeds isolated test plus safety margin");
        require(cuda_scan_init(0,656,256,why,sizeof why)!=0,"isolated CUDA scanner initialized");initialized=true;
        int blocks=0,threads=0;size_t vram=0;cuda_scan_layout(&blocks,&threads,&vram);
        require(vram<(size_t(4)<<30),"bounded scanner device allocation");
        uint8_t step[32]{};step[31]=1;require(cuda_scan_set_step(step,why,sizeof why)!=0,"unit step configured");
        uint8_t delta[64]{};require(cuda_scan_packed_configure(PACKED_SCAN_WAVE,delta,nullptr,1,0,0,16,wc.lower.data(),wc.upper.data(),65536)!=0,"same production identity-state packet configuration");
        std::vector<uint8_t> out(CUDA_MAX_OUT*32);
        const auto gpu_begin=Clock::now();deadline=gpu_begin+std::chrono::seconds(12);
        const unsigned hits=verify_hits(wave_input,out,why)+verify_hits(zone_input,out,why);
        std::vector<uint64_t> empty(32768*BLOOM_BLOCK_WORDS,0);require(cuda_scan_set_targets(nullptr,1,empty.data(),32767,why,sizeof why)!=0,"empty performance Bloom configured");
        uint64_t w=0,z=0,ow=0,oz=0;int f=0;double km=0;
        for(auto starts:{perf_input(old_wave,ow),perf_input(wave,w),perf_input(old_zone,oz),perf_input(zone,z)})launch(starts,out,f,km);
        std::vector<Timing> rows;
        for(unsigned run=0;run<4;++run){const bool newer=run==1||run==2;rows.push_back(newer?measure(wave,"wave","balanced",run,out):measure(old_wave,"wave","baseline",run,out));}
        for(unsigned run=0;run<4;++run){const bool newer=run==1||run==2;rows.push_back(newer?measure(zone,"zone","balanced",run,out):measure(old_zone,"zone","baseline",run,out));}
        const double gpu_wall=std::chrono::duration<double>(Clock::now()-gpu_begin).count();
        cuda_scan_shutdown();initialized=false;
        std::ofstream file(argv[1]);require(bool(file),"fixture evidence path writable");
        file<<"{\n\"Result\":\"PASS\",\"KnownHits\":"<<hits<<",\"Checks\":"<<checks<<",\"ScannerVRAMBytes\":"<<vram<<",\"Blocks\":"<<blocks<<",\"Threads\":"<<threads<<",\"GPUPhaseWallSeconds\":"<<gpu_wall<<",\"ProductionSearchControlled\":false,\"Notifications\":0,\"Timings\":[\n";
        for(size_t i=0;i<rows.size();++i){const auto&r=rows[i];if(i)file<<",\n";file<<"{\"mode\":\""<<r.mode<<"\",\"scheduler\":\""<<r.scheduler<<"\",\"run\":"<<r.run<<",\"keys\":"<<r.keys<<",\"wall_seconds\":"<<r.wall<<",\"kernel_seconds\":"<<r.kernel<<",\"wall_keys_per_second\":"<<double(r.keys)/r.wall<<"}";}
        file<<"\n]}\n";file.close();std::printf("PASS real CUDA known_hits=%u checks=%u GPU_phase_seconds=%.3f scanner_MiB=%.2f\n",hits,checks,gpu_wall,double(vram)/(1<<20));return 0;
    }catch(const std::exception&e){if(initialized)cuda_scan_shutdown();std::fprintf(stderr,"FAIL isolated CUDA fixture: %s\n",e.what());return 1;}
}
