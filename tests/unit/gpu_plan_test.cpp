#include "gpu_info.h"
#include <cstdio>
#include <climits>

static int checks = 0, failures = 0;
static void check(bool condition, const char *label) {
    ++checks;
    if (!condition) { ++failures; std::printf("FAIL: %s\n", label); }
}
static CudaScanLaunchInfo fixture(int sm, size_t free_bytes) {
    CudaScanLaunchInfo i{};
    i.sm_count = sm; i.warp_size = 32;
    i.device_max_threads_per_block = 1024;
    i.device_max_threads_per_sm = 1536;
    i.kernel_max_threads_per_block = 1024;
    i.max_grid_x = INT_MAX;
    i.threads_per_block = 256; i.resident_blocks_per_sm = 2;
    i.registers_per_thread = 128;
    i.memory_free = free_bytes; i.memory_total = free_bytes;
    i.scratch_bytes_per_thread = (size_t)CUDA_HALF * 32;
    i.allocation_bytes_per_thread = i.scratch_bytes_per_thread + 104;
    i.fixed_allocation_bytes = (size_t)2 * 1024 * 1024;
    return i;
}
int main() {
    const size_t GiB = (size_t)1024 * 1024 * 1024;
    for (int sm : {82, 128, 188}) {
        for (int gib : {8, 16, 24, 96}) {
            auto i = fixture(sm, (size_t)gib * GiB);
            auto p = gpu_scan_plan(i);
            const uint64_t old_memory_blocks = (i.memory_free / 4)
                / (2 * i.scratch_bytes_per_thread) / 256;
            uint64_t expected = old_memory_blocks < (uint64_t)sm * 16
                ? old_memory_blocks : (uint64_t)sm * 16;
            expected -= expected % sm;
            check(p.blocks == expected, "SM-scaled resident queue preserves conservative scratch quota");
            check(p.threads_per_block == 256 && p.resident_blocks_per_sm == 2, "queried kernel dimensions retained");
            check(p.vram_needed == i.fixed_allocation_bytes + (size_t)p.blocks * 256 * i.allocation_bytes_per_thread,
                  "actual single scratch, bases, keys and fixed bytes accounted");
            check(p.vram_needed <= i.memory_free - 64 * 1024 * 1024, "allocation remains within free memory");
        }
    }
    auto i = fixture(82, 24 * GiB);
    check(gpu_scan_plan(i).blocks == 738, "24-GiB/82-SM fixture retains prior 738 blocks");
    i.resident_blocks_per_sm = 1; check(gpu_scan_plan(i).blocks == 656, "lower actual occupancy lowers queue cap");
    i = fixture(188, 96 * GiB); check(gpu_scan_plan(i).blocks == 3008, "large-memory 188-SM fixture scales by actual occupancy");
    i = fixture(128, 24 * GiB); i.threads_per_block = 128; i.kernel_max_threads_per_block = 128;
    check(gpu_scan_plan(i).threads_per_block == 128, "smaller legal kernel block retained");
    i.threads_per_block = 256; check(!gpu_scan_plan(i).blocks, "reject block above kernel maximum");
    i = fixture(82, 24 * GiB); i.device_max_threads_per_block = 128;
    check(!gpu_scan_plan(i).blocks, "reject block above device maximum");
    i = fixture(82, 24 * GiB); i.memory_free = 0;
    check(!gpu_scan_plan(i).blocks, "unknown/free-zero memory never replaced with total memory");
    i = fixture(82, 24 * GiB); i.memory_total = 0;
    check(!gpu_scan_plan(i).blocks, "unknown total memory rejected");
    i = fixture(0, 24 * GiB); check(!gpu_scan_plan(i).blocks, "unknown SM count rejected");
    i = fixture(82, 24 * GiB); i.resident_blocks_per_sm = 0;
    check(!gpu_scan_plan(i).blocks, "unknown kernel occupancy rejected");
    i = fixture(82, 24 * GiB); i.warp_size = 0;
    check(!gpu_scan_plan(i).blocks, "unknown warp size rejected");
    i = fixture(82, 24 * GiB); i.scratch_bytes_per_thread = 0;
    check(!gpu_scan_plan(i).blocks, "zero scratch size rejected");
    i = fixture(82, 24 * GiB); i.max_grid_x = 10;
    check(gpu_scan_plan(i).blocks == 10, "device grid limit honored below one SM wave");
    i = fixture(82, 24 * GiB);
    const size_t targets = 512 * 1024 * 1024;
    auto reserved = gpu_scan_plan(i, targets);
    check(reserved.vram_needed == gpu_scan_plan(i).vram_needed + targets, "future target reservation accounted");
    check(!gpu_scan_plan(i, SIZE_MAX).blocks, "target reservation cannot overflow");
    i.memory_free = 64 * 1024 * 1024 + i.fixed_allocation_bytes + 256 * i.allocation_bytes_per_thread;
    check(gpu_scan_plan(i).blocks == 1, "low memory permits one block without forcing full SM wave");
    --i.memory_free;
    check(!gpu_scan_plan(i).blocks, "one byte below legal block memory rejects plan");
    i = fixture(INT_MAX, SIZE_MAX); i.threads_per_block = 32; i.resident_blocks_per_sm = 1;
    i.scratch_bytes_per_thread = 1; i.allocation_bytes_per_thread = 1; i.fixed_allocation_bytes = 0;
    auto extreme = gpu_scan_plan(i);
    check(extreme.blocks > 0 && (uint64_t)extreme.blocks * 32 <= INT_MAX, "extreme inputs bounded by signed host thread count");
    size_t bytes = 0;
    check(cuda_scan_checked_array_bytes(1234, 32, &bytes) && bytes == 39488, "target allocation multiplication exact");
    check(!cuda_scan_checked_array_bytes(UINT64_MAX, 32, &bytes), "UINT64 target allocation overflow rejected");
    check(!cuda_scan_checked_array_bytes(SIZE_MAX / 8 + 1, 8, &bytes), "SIZE_MAX boundary target allocation rejected");
    check(!cuda_scan_checked_array_bytes(1, 0, &bytes), "zero-sized element rejected");
    std::printf("GPU planner CPU checks: %d, failures: %d\n", checks, failures);
    return failures ? 1 : 0;
}
