#pragma once
#include "wave_gpu_batch.h"
#include <stdexcept>

inline void require_shape_completion_counts(const WaveGpuBatch& batch) {
    if(batch.completed.size()!=batch.completed_counts.size())
        throw std::logic_error("Не совпали позиции и подтверждённые размеры блоков");
}

// This function is called only after every piece in the batch succeeded and
// every finding is durable. Validate the entire vector before publishing any ID.
inline void commit_completed_batch(SearchContext& context,const WaveGpuBatch& batch,bool weighted) {
    if(weighted)require_shape_completion_counts(batch);
    for(size_t i=0;i<batch.completed.size();++i) {
        if(weighted)chunk_done(context,batch.completed[i],batch.completed_counts[i]);
        else chunk_done(context,batch.completed[i]);
    }
}
