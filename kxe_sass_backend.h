#pragma once
#include "traverse.h"
#ifdef PKH_HAVE_CUDA
// Production fail-closed gate. Always false, including when the experimental
// environment variable is set; the caller scans the same ordinal normally.
bool kxe_sass_requested();
struct KxeSassStats {
    uint64_t keys=0, batches=0;
    double seconds=0, prepare_ms=0, points_ms=0, filter_ms=0;
    double denominator_ms=0, products_ms=0, inverse_ms=0, expand_ms=0,
           finish_ms=0, bloom_ms=0, summary_ms=0;
};
// Experimental coverage is not independently validated for production. Direct
// calls always return false, zero optional stats and publish no work. Never
// commit an ordinal on this result. No GPU or result-writer access occurs.
bool kxe_sass_scan(const ChunkIdx&, KxeSassStats* = nullptr,
                   void (*after_batch)(uint64_t) = nullptr);
void kxe_sass_reset();
#endif
