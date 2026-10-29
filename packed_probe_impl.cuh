// Isolated experiment. Arithmetic imported unchanged from the adjacent BitPazl
// GPU implementation, in a namespace; production search does not call this API.
#include "packed_probe.h"
#include <vector>
#include <algorithm>
#if defined(_MSC_VER)
#include <intrin.h>
#endif
namespace packed_bit {
#include "packed_bit_ec.cuh"
}
#undef HD
#undef MULHI

extern "C" int cuda_host_packed_bases(int, int, const uint8_t*, const uint8_t*, int, cpt*, uint8_t*);
extern "C" int cuda_host_packed_bytes(cpt*);
extern "C" int cuda_host_packed_offsets(int, int, cpt*);

static cpt *packed_nib=nullptr, *packed_roots=nullptr, *packed_offsets=nullptr;
static uint8_t *packed_starts=nullptr;
static size_t packed_capacity=0;
static int packed_offset_count=0;

static bool packed_cuda_ok(cudaError_t e){
    if(e==cudaSuccess)return true;
    fprintf(stderr,"PACKED_CUDA_ERROR: %s\n",cudaGetErrorString(e));return false;
}
static bool packed_workspace_need(size_t capacity){
    if(packed_capacity)return packed_capacity>=capacity;
    if (!capacity || capacity > SIZE_MAX / sizeof(cpt) || capacity > SIZE_MAX / 32) return false;
    struct PendingWorkspace {
        uint8_t* starts=nullptr;
        cpt *roots=nullptr, *offsets=nullptr, *nib=nullptr;
        ~PendingWorkspace(){
            if(starts)cudaFree(starts); if(roots)cudaFree(roots);
            if(offsets)cudaFree(offsets); if(nib)cudaFree(nib);
        }
    } pending;
    try {
        std::vector<cpt> nibbles(32*256);
        if(!cuda_host_packed_bytes(nibbles.data()))return false;
        if(!packed_cuda_ok(cudaMalloc(&pending.starts,capacity*32))
           ||!packed_cuda_ok(cudaMalloc(&pending.roots,capacity*sizeof(cpt)))
           ||!packed_cuda_ok(cudaMalloc(&pending.offsets,(65536/CUDA_SPAN)*sizeof(cpt)))
           ||!packed_cuda_ok(cudaMalloc(&pending.nib,32*256*sizeof(cpt))))return false;
        if(!packed_cuda_ok(cudaMemcpy(pending.nib,nibbles.data(),nibbles.size()*sizeof(cpt),cudaMemcpyHostToDevice)))return false;
    } catch (...) { return false; }
    packed_starts=pending.starts; pending.starts=nullptr;
    packed_roots=pending.roots; pending.roots=nullptr;
    packed_offsets=pending.offsets; pending.offsets=nullptr;
    packed_nib=pending.nib; pending.nib=nullptr;
    packed_capacity=capacity; return true;
}

__global__ void packed_root_init(const uint8_t* starts, int segments, const cpt* nib, cpt* roots) {
    const int s=blockIdx.x*blockDim.x+threadIdx.x;
    if(s>=segments) return;
    uint8_t k[32]; for(int i=0;i<32;++i) k[i]=starts[(size_t)s*32+i];
    unsigned carry=CUDA_HALF;
    for(int i=31;i>=0;--i){ const unsigned v=k[i]+carry; k[i]=(uint8_t)v; carry=v>>8; }
    packed_bit::gej acc; packed_bit::gej_set_infinity(&acc);
    for(int n=0;n<32;++n){
        const int v=k[31-n];
        if(!v) continue;
        const cpt p=nib[n*256+v]; packed_bit::ge e;
        for(int j=0;j<4;++j){e.x.d[j]=p.x[j];e.y.d[j]=p.y[j];}e.inf=0;
        packed_bit::gej_add_ge(&acc,&acc,&e);
    }
    packed_bit::ge out;packed_bit::ge_set_gej(&out,&acc);
    for(int j=0;j<4;++j){roots[s].x[j]=out.x.d[j];roots[s].y[j]=out.y.d[j];}
    roots[s].inf=out.inf;
}

__global__ void packed_center_expand(const uint8_t* starts,const cpt* roots,const cpt* offsets,
    int nthreads,int per_segment,cpt* bases,uint8_t* keys){
    const int t=blockIdx.x*blockDim.x+threadIdx.x;if(t>=nthreads)return;
    const int s=t/per_segment,j=t%per_segment;
    if(j==0)bases[t]=roots[s];else cpt_add(bases[t],roots[s],offsets[j]);
    unsigned carry=CUDA_HALF+j*CUDA_SPAN;
    for(int i=31;i>=0;--i){const unsigned v=starts[(size_t)s*32+i]+carry;keys[(size_t)t*32+i]=(uint8_t)v;carry=v>>8;}
}

static uint64_t packed_run_impl(const uint8_t* starts,int segments,
    uint64_t segment_keys,int gpu_init,int verify_centers,uint8_t* out,int max_out,int* found,bool device_starts){
    if(found)*found=0;
    g_chain_ok=false;g_ndegen=0;g_last_prep_ms=g_last_kern_ms=0;
    if(!g_ready||g_ntargets<=0||(!starts&&!device_starts)||!out||!found||segments<=0||max_out<=0
       ||segment_keys<CUDA_SPAN||segment_keys%CUDA_SPAN||segment_keys>65536)return 0;
    for(int i=0;i<31;++i)if(g_step[i])return 0;
    if(g_step[31]!=1)return 0;
    const uint64_t per=segment_keys/CUDA_SPAN,nthr=(uint64_t)segments*per;
    const size_t capacity=(size_t)g_blocks*g_threads;
    if(!nthr||nthr>capacity||nthr>INT32_MAX)return 0;
    const uint64_t total=(uint64_t)segments*segment_keys;
    const auto begin=std::chrono::steady_clock::now();
    cpt stride{};uint8_t skey[32]{};
    const bool tables=g_tables_ok&&g_tables_nthr==nthr&&memcmp(g_tables_step,g_step,32)==0;
    auto fail=[](cudaError_t e){fprintf(stderr,"PACKED_CUDA_ERROR: %s\n",cudaGetErrorString(e));return uint64_t{0};};
#define PACKED_CUDA(call) do {const cudaError_t e=(call);if(e!=cudaSuccess)return fail(e);}while(0)
    if(tables){stride=g_tables_stride;memcpy(skey,g_tables_skey,32);}
    else{
        // All three device tables form one cache entry. A partial upload must
        // never leave the old entry reusable by the ordinary scan path.
        g_tables_ok=false;
        if(!cuda_host_tables((int)nthr,g_step,h_gt_pin,CUDA_HALF,&stride,skey))return 0;
        PACKED_CUDA(cudaMemcpy(d_gt,h_gt_pin,CUDA_HALF*sizeof(cpt),cudaMemcpyHostToDevice));
        PACKED_CUDA(cudaMemcpy(d_skey_p,skey,32,cudaMemcpyHostToDevice));
        PACKED_CUDA(cudaMemcpy(d_step_p,g_step,32,cudaMemcpyHostToDevice));
        g_tables_nthr=(size_t)nthr;g_tables_stride=stride;
        memcpy(g_tables_step,g_step,32);memcpy(g_tables_skey,skey,32);
        g_tables_ok=true;
    }
    if(gpu_init){
        if(!packed_workspace_need(capacity))return 0;
        if(packed_offset_count!=(int)per){
            std::vector<cpt> offsets((size_t)per);
            if(!cuda_host_packed_offsets((int)per,CUDA_HALF,offsets.data()))return 0;
            PACKED_CUDA(cudaMemcpy(packed_offsets,offsets.data(),offsets.size()*sizeof(cpt),cudaMemcpyHostToDevice));
            packed_offset_count=(int)per;
        }
        if(!device_starts)PACKED_CUDA(cudaMemcpy(packed_starts,starts,(size_t)segments*32,cudaMemcpyHostToDevice));
        packed_root_init<<<(segments+127)/128,128>>>(packed_starts,segments,packed_nib,packed_roots);
        PACKED_CUDA(cudaGetLastError());
        packed_center_expand<<<((int)nthr+127)/128,128>>>(packed_starts,packed_roots,packed_offsets,(int)nthr,(int)per,d_base,d_basekey);
        PACKED_CUDA(cudaGetLastError());
        PACKED_CUDA(cudaDeviceSynchronize());
        if(verify_centers){
            if(!cuda_host_packed_bases(segments,(int)per,starts,g_step,CUDA_HALF,h_base_pin,h_bkey_pin))return 0;
            std::vector<cpt> actual((size_t)nthr);std::vector<uint8_t> actual_keys((size_t)nthr*32);
            PACKED_CUDA(cudaMemcpy(actual.data(),d_base,actual.size()*sizeof(cpt),cudaMemcpyDeviceToHost));
            PACKED_CUDA(cudaMemcpy(actual_keys.data(),d_basekey,actual_keys.size(),cudaMemcpyDeviceToHost));
            for(size_t t=0;t<nthr;++t)if(memcmp(actual[t].x,h_base_pin[t].x,32)||memcmp(actual[t].y,h_base_pin[t].y,32)
                ||actual[t].inf!=h_base_pin[t].inf||memcmp(actual_keys.data()+t*32,h_bkey_pin+t*32,32)){
                fprintf(stderr,"PACKED_CENTER_MISMATCH thread=%zu\n",t);return 0;
            }
        }
    }else{
        if(!cuda_host_packed_bases(segments,(int)per,starts,g_step,CUDA_HALF,h_base_pin,h_bkey_pin))return 0;
        PACKED_CUDA(cudaMemcpy(d_base,h_base_pin,(size_t)nthr*sizeof(cpt),cudaMemcpyHostToDevice));
        PACKED_CUDA(cudaMemcpy(d_basekey,h_bkey_pin,(size_t)nthr*32,cudaMemcpyHostToDevice));
    }
    PACKED_CUDA(cudaMemset(d_found,0,sizeof(*d_found)));
    PACKED_CUDA(cudaMemset(d_ndegen,0,sizeof(int)));
    const auto prepared=std::chrono::steady_clock::now();
    kern_scan<<<((int)nthr+g_threads-1)/g_threads,g_threads>>>(d_gt,d_work,d_base,d_basekey,stride,d_skey_p,
        0,1,nthr,total,d_targ,g_ntargets,d_bloom,g_bmask,d_step_p,d_out,std::min(max_out,g_maxout),d_found,
        d_degen,CUDA_MAX_DEGEN,d_ndegen);
    PACKED_CUDA(cudaGetLastError());PACKED_CUDA(cudaDeviceSynchronize());
    const auto completed=std::chrono::steady_clock::now();
    unsigned long long raw=0;int degenerate=0;
    PACKED_CUDA(cudaMemcpy(&raw,d_found,sizeof(raw),cudaMemcpyDeviceToHost));
    PACKED_CUDA(cudaMemcpy(&degenerate,d_ndegen,sizeof(int),cudaMemcpyDeviceToHost));
    if(degenerate){fprintf(stderr,"PACKED_DEGENERATE: %d; benchmark refuses partial work\n",degenerate);return 0;}
    *found=cuda_candidate_count_clamped(raw,g_maxout,max_out);
    if(*found)PACKED_CUDA(cudaMemcpy(out,d_out,(size_t)*found*32,cudaMemcpyDeviceToHost));
    g_last_prep_ms=std::chrono::duration<double,std::milli>(prepared-begin).count();
    g_last_kern_ms=std::chrono::duration<double,std::milli>(completed-prepared).count();
    g_last_threads=nthr;g_last_rounds=1;
#undef PACKED_CUDA
    return total;
}

extern "C" uint64_t cuda_scan_run_packed(const uint8_t* starts,int segments,
    uint64_t segment_keys,int gpu_init,int verify_centers,uint8_t* out,int max_out,int* found){
    return packed_run_impl(starts,segments,segment_keys,gpu_init,verify_centers,out,max_out,found,false);
}

static uint8_t *packed_wave_deltas=nullptr,*packed_wave_bases=nullptr;
static int *packed_wave_invalid=nullptr;
static uint64_t packed_wave_states=0,packed_wave_segment_keys=0;
static size_t packed_wave_base_capacity=0;
__device__ __constant__ uint8_t packed_wave_hi[32]={
    0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xfe,
    0xba,0xae,0xdc,0xe6,0xaf,0x48,0xa0,0x3b,0xbf,0xd2,0x5e,0x8c,0xd0,0x36,0x41,0x40};

__global__ void packed_wave_generate(const uint8_t* bases,const uint8_t* deltas,uint64_t states,
    uint64_t first_state,int segments,uint64_t segment_keys,uint8_t* starts,int* invalid){
    const int s=blockIdx.x*blockDim.x+threadIdx.x;if(s>=segments)return;
    const uint64_t state=(first_state+(uint64_t)s)%states;
    const uint64_t base=(first_state+(uint64_t)s)/states;
    uint8_t k[32];
    for(int b=0;b<32;++b){
        const unsigned original=bases[base*32+b];
        const unsigned dhi=deltas[state*64+2*b],dlo=deltas[state*64+2*b+1];
        const unsigned hi=b==0?1+((original>>4)-1+dhi)%15:((original>>4)+dhi)&15;
        const unsigned lo=((original&15)+dlo)&15;k[b]=(uint8_t)((hi<<4)|lo);
        starts[(size_t)s*32+b]=k[b];
    }
    bool valid=k[0]>=0x10;
    uint64_t carry=segment_keys-1;
    for(int i=31;i>=0;--i){const uint64_t v=k[i]+carry;k[i]=(uint8_t)v;carry=v>>8;}
    if(carry)valid=false;
    for(int i=0;i<32;++i){if(k[i]<packed_wave_hi[i])break;if(k[i]>packed_wave_hi[i]){valid=false;break;}}
    if(!valid)atomicAdd(invalid,1);
}

extern "C" int cuda_scan_set_packed_wave_plan(const uint8_t* deltas,uint64_t states,uint64_t segment_keys){
    if(!g_ready||!deltas||!states||states>100000||segment_keys<CUDA_SPAN||segment_keys>65536||segment_keys%CUDA_SPAN)return 0;
    // A wave translation never modifies any digit occupied by a segment.
    int low=0;uint64_t n=segment_keys;while(n>1&&n%16==0){n/=16;++low;}if(n!=1)return 0;
    for(uint64_t state=0;state<states;++state)for(int p=0;p<64;++p){
        const unsigned d=deltas[state*64+p];
        if(d>=(p==0?15:16)||(p>=64-low&&d))return 0;
    }
    if(packed_wave_deltas){if(!packed_cuda_ok(cudaFree(packed_wave_deltas)))return 0;packed_wave_deltas=nullptr;}
    if(!packed_cuda_ok(cudaMalloc(&packed_wave_deltas,(size_t)states*64))
       ||!packed_cuda_ok(cudaMemcpy(packed_wave_deltas,deltas,(size_t)states*64,cudaMemcpyHostToDevice)))return 0;
    if(!packed_wave_invalid&&!packed_cuda_ok(cudaMalloc(&packed_wave_invalid,sizeof(int))))return 0;
    packed_wave_states=states;packed_wave_segment_keys=segment_keys;return 1;
}

extern "C" uint64_t cuda_scan_run_packed_wave(const uint8_t* bases,int base_count,
    uint64_t first_state,int segments,const uint8_t* expected_starts,uint8_t* out,int max_out,int* found){
    if(found)*found=0;
    if(!g_ready||!bases||!packed_wave_states||!packed_wave_deltas||segments<=0||base_count<=0
       ||first_state>=packed_wave_states||!out||!found||max_out<=0)return 0;
    const uint64_t needed=(first_state+(uint64_t)segments-1)/packed_wave_states+1;
    const uint64_t nthr=(uint64_t)segments*(packed_wave_segment_keys/CUDA_SPAN);
    const size_t capacity=(size_t)g_blocks*g_threads;
    if((uint64_t)base_count!=needed||nthr>capacity)return 0;
    const auto begin=std::chrono::steady_clock::now();
    if(!packed_workspace_need(capacity))return 0;
    if(packed_wave_base_capacity<(size_t)base_count){
        if(packed_wave_bases&&!packed_cuda_ok(cudaFree(packed_wave_bases)))return 0;
        if(!packed_cuda_ok(cudaMalloc(&packed_wave_bases,(size_t)base_count*32)))return 0;
        packed_wave_base_capacity=(size_t)base_count;
    }
    if(!packed_cuda_ok(cudaMemcpy(packed_wave_bases,bases,(size_t)base_count*32,cudaMemcpyHostToDevice))
       ||!packed_cuda_ok(cudaMemset(packed_wave_invalid,0,sizeof(int))))return 0;
    packed_wave_generate<<<(segments+127)/128,128>>>(packed_wave_bases,packed_wave_deltas,packed_wave_states,
        first_state,segments,packed_wave_segment_keys,packed_starts,packed_wave_invalid);
    if(!packed_cuda_ok(cudaGetLastError()))return 0;
    int invalid=0;if(!packed_cuda_ok(cudaMemcpy(&invalid,packed_wave_invalid,sizeof(int),cudaMemcpyDeviceToHost)))return 0;
    if(invalid){fprintf(stderr,"PACKED_WAVE_RANGE_FAIL segments=%d\n",invalid);return 0;}
    if(expected_starts){
        std::vector<uint8_t> actual((size_t)segments*32);
        if(!packed_cuda_ok(cudaMemcpy(actual.data(),packed_starts,actual.size(),cudaMemcpyDeviceToHost)))return 0;
        if(memcmp(actual.data(),expected_starts,actual.size())){fprintf(stderr,"PACKED_WAVE_START_MISMATCH\n");return 0;}
    }
    const double generated_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
    const uint64_t checked=packed_run_impl(expected_starts,segments,packed_wave_segment_keys,1,expected_starts!=nullptr,out,max_out,found,true);
    g_last_prep_ms+=generated_ms;return checked;
}
