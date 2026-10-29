#include "gpu_info.h"
#include <cstdio>
#include <cstdlib>
int main(int argc, char **argv) {
    const int device = argc > 1 ? std::atoi(argv[1]) : 0;
    CudaScanLaunchInfo i{}; char why[512]{};
    if (!cuda_scan_query_launch(device, 256, &i, why, sizeof(why))) {
        std::printf("CUDA_QUERY_ERROR: %s\n", why); return 1;
    }
    const GpuPlan p = gpu_scan_plan(i);
    std::printf("QUERY_ONLY: no search kernel launched, no notifications\n"
        "SM=%d deviceMaxThreads=%d kernelMaxThreads=%d maxThreadsPerSM=%d warp=%d\n"
        "threadsPerBlock=%d residentBlocksPerSM=%d registers=%d queuedBlocks=%d totalThreads=%llu queueWaves=%d\n"
        "freeBytes=%zu totalBytes=%zu scratchPerThread=%zu allocationPerThread=%zu fixedBytes=%zu plannedBytes=%zu\n",
        i.sm_count, i.device_max_threads_per_block, i.kernel_max_threads_per_block,
        i.device_max_threads_per_sm, i.warp_size, p.threads_per_block, i.resident_blocks_per_sm,
        i.registers_per_thread, p.blocks, (unsigned long long)p.blocks * p.threads_per_block,
        p.queued_resident_waves, i.memory_free, i.memory_total, i.scratch_bytes_per_thread,
        i.allocation_bytes_per_thread, i.fixed_allocation_bytes, p.vram_needed);
    for (const auto &warning : p.warnings) std::printf("PLAN_ERROR: %s\n", warning.c_str());
    return p.blocks > 0 ? 0 : 1;
}
