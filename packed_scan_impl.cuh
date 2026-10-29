#include "packed_scan.h"

static uint8_t *search_packed_deltas=nullptr,*search_packed_bases=nullptr,*search_packed_bounds=nullptr;
static PackedZoneOp* search_packed_zone=nullptr;
static PackedSegment* search_packed_segments=nullptr;
static int* search_packed_invalid=nullptr;
static size_t search_packed_base_capacity=0,search_packed_segment_capacity=0;
static uint64_t search_packed_rows=0,search_packed_keys=0;
static int search_packed_mode=0,search_packed_top=0,search_packed_min=0,search_packed_radix=16;

__device__ __forceinline__ bool search_packed_less(const uint8_t* a,const uint8_t* b){
    for(int i=0;i<32;++i){if(a[i]<b[i])return true;if(a[i]>b[i])return false;}return false;
}
__global__ void search_packed_generate(const uint8_t* bases,int base_count,const PackedSegment* segments,
    int count,int mode,const uint8_t* deltas,const PackedZoneOp* zone,uint64_t rows,
    int top,int minimum,int radix,const uint8_t* bounds,uint64_t keys,uint8_t* starts,int* invalid){
    const int s=blockIdx.x*blockDim.x+threadIdx.x;if(s>=count)return;
    const PackedSegment segment=segments[s];
    if(segment.base>=(uint32_t)base_count||segment.state>=rows){atomicAdd(invalid,1);return;}
    uint8_t k[32];for(int b=0;b<32;++b)k[b]=bases[(size_t)segment.base*32+b];
    bool valid=true;
    if(mode==PACKED_SCAN_WAVE){
        for(int p=0;p<64;++p){
            const unsigned d=deltas[(size_t)segment.state*64+p];if(!d)continue;
            const unsigned old=(p&1)?k[p/2]&15:k[p/2]>>4;
            unsigned value;
            if(p==top){
                if(old<(unsigned)minimum||old>=(unsigned)(minimum+radix)){valid=false;break;}
                value=(unsigned)minimum+(old-(unsigned)minimum+d)%(unsigned)radix;
            }else value=(old+d)&15;
            if(p&1)k[p/2]=(uint8_t)((k[p/2]&0xf0)|value);
            else k[p/2]=(uint8_t)((k[p/2]&15)|(value<<4));
        }
    }else{
        const PackedZoneOp op=zone[segment.state];
        if(op.pos>=0){
            const int p=op.pos;const unsigned old=(p&1)?k[p/2]&15:k[p/2]>>4;
            unsigned ordinal=0,value=16;
            for(unsigned delta=1;delta<16;++delta){
                const unsigned candidate=(old+delta)&15;
                if(!(op.mask&(1u<<candidate)))continue;
                if(ordinal++==op.offset){value=candidate;break;}
            }
            if(value>15)valid=false;
            else if(p&1)k[p/2]=(uint8_t)((k[p/2]&0xf0)|value);
            else k[p/2]=(uint8_t)((k[p/2]&15)|(value<<4));
        }
    }
    if(search_packed_less(k,bounds))valid=false;
    for(int i=0;i<32;++i)starts[(size_t)s*32+i]=k[i];
    uint64_t carry=keys-1;
    for(int i=31;i>=0;--i){const uint64_t v=k[i]+carry;k[i]=(uint8_t)v;carry=v>>8;}
    if(carry||search_packed_less(bounds+32,k))valid=false;
    if(!valid)atomicAdd(invalid,1);
}

extern "C" int cuda_scan_packed_configure(int mode,const uint8_t* deltas,const PackedZoneOp* zone_ops,
    uint64_t rows,int top,int minimum,int radix,const uint8_t lo[32],const uint8_t hi[32],uint64_t keys){
    search_packed_mode=0;
    if(!g_ready||!lo||!hi||memcmp(lo,hi,32)>0||!rows||rows>100000||keys>65536||keys<CUDA_SPAN
       ||keys%CUDA_SPAN||top<0||top>63||minimum<0||radix<1||minimum+radix>16)return 0;
    if(mode==PACKED_SCAN_WAVE){
        if(!deltas)return 0;
        // Starts need no alignment: every nibble is included in the root,
        // then centers receive their exact integer offset. A zero-delta row
        // also batches the host zone traversal without changing its starts.
        // Every complete segment is independently checked against the bounds.
        for(uint64_t r=0;r<rows;++r)for(int p=0;p<64;++p){
            const unsigned d=deltas[r*64+p];
            if(d>=(unsigned)(p==top?radix:16))return 0;
        }
        if(search_packed_deltas){if(!packed_cuda_ok(cudaFree(search_packed_deltas)))return 0;search_packed_deltas=nullptr;}
        if(!packed_cuda_ok(cudaMalloc(&search_packed_deltas,(size_t)rows*64))
           ||!packed_cuda_ok(cudaMemcpy(search_packed_deltas,deltas,(size_t)rows*64,cudaMemcpyHostToDevice)))return 0;
    }else if(mode==PACKED_SCAN_ZONE){
        if(!zone_ops)return 0;
        for(uint64_t r=0;r<rows;++r){const auto&op=zone_ops[r];
            if(op.pos < -1||op.pos>=60||op.mask>65535||op.offset>14)return 0;
        }
        if(search_packed_zone){if(!packed_cuda_ok(cudaFree(search_packed_zone)))return 0;search_packed_zone=nullptr;}
        if(!packed_cuda_ok(cudaMalloc(&search_packed_zone,(size_t)rows*sizeof(PackedZoneOp)))
           ||!packed_cuda_ok(cudaMemcpy(search_packed_zone,zone_ops,(size_t)rows*sizeof(PackedZoneOp),cudaMemcpyHostToDevice)))return 0;
    }else return 0;
    if(!search_packed_bounds&&!packed_cuda_ok(cudaMalloc(&search_packed_bounds,64)))return 0;
    if(!search_packed_invalid&&!packed_cuda_ok(cudaMalloc(&search_packed_invalid,sizeof(int))))return 0;
    if(!packed_cuda_ok(cudaMemcpy(search_packed_bounds,lo,32,cudaMemcpyHostToDevice))
       ||!packed_cuda_ok(cudaMemcpy(search_packed_bounds+32,hi,32,cudaMemcpyHostToDevice)))return 0;
    search_packed_mode=mode;search_packed_rows=rows;search_packed_top=top;
    search_packed_min=minimum;search_packed_radix=radix;search_packed_keys=keys;return 1;
}

extern "C" uint64_t cuda_scan_packed_batch(const uint8_t* bases,int base_count,const PackedSegment* segments,
    int count,uint8_t sample[32],uint8_t* out,int max_out,int* found){
    if(found)*found=0;
    if(!g_ready||!search_packed_mode||!bases||!segments||!sample||!out||!found||max_out<=0||base_count<=0||count<=0)return 0;
    const uint64_t nthr=(uint64_t)count*(search_packed_keys/CUDA_SPAN);
    const size_t capacity=(size_t)g_blocks*g_threads;
    if(!nthr||nthr>capacity||count>INT32_MAX-256)return 0;
    const auto begin=std::chrono::steady_clock::now();
    if(!packed_workspace_need(capacity))return 0;
    if(search_packed_base_capacity<(size_t)base_count){
        if(search_packed_bases&&!packed_cuda_ok(cudaFree(search_packed_bases)))return 0;
        if(!packed_cuda_ok(cudaMalloc(&search_packed_bases,(size_t)base_count*32)))return 0;
        search_packed_base_capacity=(size_t)base_count;
    }
    if(search_packed_segment_capacity<(size_t)count){
        if(search_packed_segments&&!packed_cuda_ok(cudaFree(search_packed_segments)))return 0;
        if(!packed_cuda_ok(cudaMalloc(&search_packed_segments,(size_t)count*sizeof(PackedSegment))))return 0;
        search_packed_segment_capacity=(size_t)count;
    }
    if(!packed_cuda_ok(cudaMemcpy(search_packed_bases,bases,(size_t)base_count*32,cudaMemcpyHostToDevice))
       ||!packed_cuda_ok(cudaMemcpy(search_packed_segments,segments,(size_t)count*sizeof(PackedSegment),cudaMemcpyHostToDevice))
       ||!packed_cuda_ok(cudaMemset(search_packed_invalid,0,sizeof(int))))return 0;
    search_packed_generate<<<(count+127)/128,128>>>(search_packed_bases,base_count,search_packed_segments,count,
        search_packed_mode,search_packed_deltas,search_packed_zone,search_packed_rows,search_packed_top,
        search_packed_min,search_packed_radix,search_packed_bounds,search_packed_keys,packed_starts,search_packed_invalid);
    if(!packed_cuda_ok(cudaGetLastError()))return 0;
    int invalid=0;if(!packed_cuda_ok(cudaMemcpy(&invalid,search_packed_invalid,sizeof(int),cudaMemcpyDeviceToHost)))return 0;
    // Any clipped/invalid segment goes through the original exact whole-batch replay.
    if(invalid)return 0;
    uint8_t accepted_sample[32];
    if(!packed_cuda_ok(cudaMemcpy(accepted_sample,packed_starts,32,cudaMemcpyDeviceToHost)))return 0;
    const double generated_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
    const uint64_t checked=packed_run_impl(nullptr,count,search_packed_keys,1,0,out,max_out,found,true);
    if(checked!=(uint64_t)count*search_packed_keys||*found>=max_out||*found>=g_maxout){*found=0;return 0;}
    memcpy(sample,accepted_sample,32);g_last_prep_ms+=generated_ms;return checked;
}

static void packed_buffers_release(){
    cudaFree(packed_nib);cudaFree(packed_roots);cudaFree(packed_offsets);cudaFree(packed_starts);
    packed_nib=packed_roots=packed_offsets=nullptr;packed_starts=nullptr;packed_capacity=0;packed_offset_count=0;
    cudaFree(packed_wave_deltas);cudaFree(packed_wave_bases);cudaFree(packed_wave_invalid);
    packed_wave_deltas=packed_wave_bases=nullptr;packed_wave_invalid=nullptr;
    packed_wave_states=packed_wave_segment_keys=0;packed_wave_base_capacity=0;
    cudaFree(search_packed_deltas);cudaFree(search_packed_bases);cudaFree(search_packed_bounds);
    cudaFree(search_packed_zone);cudaFree(search_packed_segments);cudaFree(search_packed_invalid);
    search_packed_deltas=search_packed_bases=search_packed_bounds=nullptr;
    search_packed_zone=nullptr;search_packed_segments=nullptr;search_packed_invalid=nullptr;
    search_packed_base_capacity=search_packed_segment_capacity=0;
    search_packed_rows=search_packed_keys=0;search_packed_mode=0;
}
