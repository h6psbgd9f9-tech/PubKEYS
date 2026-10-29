#pragma once
#include "search.h"

// Called before workers start or after they have all joined. The parser or
// volatile cache has already validated this exact position/count pair.
inline void apply_shape_checkpoint_state(SearchContext& context,const ChunkIdx& seq,const u256 confirmed) {
    checkpoint_reset(context,seq,confirmed);
    context.chunk_seq.set(seq);
    context.resumed_items=u256_to_ld(confirmed);
}
