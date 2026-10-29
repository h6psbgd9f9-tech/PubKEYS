#pragma once
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <future>
#include <string>
#include <utility>

// One speculative packet, as in BitPazl. The worker owns all its data and
// never touches CUDA, counters, output or persisted checkpoints.
template<class Search,class Batch> class BatchPrefetch {
    struct Result {Batch batch;std::string why;bool valid=false;};
    Search& search_;
    size_t capacity_;uint64_t piece_keys_;bool enabled_;
    std::future<Result> pending_;
public:
    BatchPrefetch(Search& search,size_t capacity,uint64_t keys,bool enabled)
        : search_(search),capacity_(capacity),piece_keys_(keys),enabled_(enabled) {}
    bool take(Batch& batch,std::string& why) {
        try {
            if(!pending_.valid())return search_.prepare_batch(capacity_,piece_keys_,batch,why);
            auto result=pending_.get();batch=std::move(result.batch);why=std::move(result.why);
            return result.valid;
        } catch(const std::exception& error) {why=error.what();return false;}
    }
    void start_after(const Batch& batch) {
        if(!enabled_ || batch.next.exhausted)return;
        const auto previous=batch.state_only();
        const size_t capacity=capacity_;const uint64_t keys=piece_keys_;
        Search* const search=&search_;
        try {
            pending_=std::async(std::launch::async,[search,previous,capacity,keys] {
                Result result;
                result.valid=search->prepare_after(previous,capacity,keys,result.batch,result.why);
                return result;
            });
        } catch(const std::exception&) {
            // Thread creation is optional. The next take uses committed state.
            enabled_=false;
        }
    }
};
