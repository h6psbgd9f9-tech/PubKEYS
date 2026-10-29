#pragma once
#include "wave_gpu_batch.h"
#include <condition_variable>
#include <exception>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <thread>

// CPU metadata only, for one-piece logical blocks. Claim stays serial; helpers
// fill assigned slots without changing ordinal order or publishing completion.
// One caller may fill at a time. Its owner joins look-ahead before destruction.
class WaveGpuParallel {
    struct Row { PackedKey start{}; uint64_t count=0; };
    std::vector<std::thread> helpers_;
    std::vector<ChunkIdx> ids_;
    std::vector<Row> rows_;
    std::mutex mutex_;
    std::condition_variable ready_, finished_;
    bool stopping_=false;
    uint64_t generation_=0;
    size_t count_=0, completed_=0;
    std::function<void(size_t)>* task_=nullptr;
    std::exception_ptr error_;

    void remember_error() {
        std::lock_guard<std::mutex> lock(mutex_);
        if(!error_)error_=std::current_exception();
    }
    void partition(unsigned worker,unsigned workers,size_t count,
                   std::function<void(size_t)>& task) {
        try {
            const size_t begin=count*worker/workers,end=count*(worker+1)/workers;
            for(size_t i=begin;i<end;++i)task(i);
        } catch(...) { remember_error(); }
    }
    void helper(unsigned worker) {
        uint64_t seen=0;
        for(;;) {
            std::unique_lock<std::mutex> lock(mutex_);
            ready_.wait(lock,[&]{return stopping_||generation_!=seen;});
            if(stopping_)return;
            seen=generation_;
            const size_t count=count_;
            const unsigned workers=(unsigned)helpers_.size()+1;
            auto* task=task_;
            lock.unlock();
            partition(worker,workers,count,*task);
            lock.lock();
            ++completed_;
            if(completed_==helpers_.size())finished_.notify_one();
        }
    }
    void join_helpers() noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_=true;
        }
        ready_.notify_all();
        for(auto& helper:helpers_)if(helper.joinable())helper.join();
        helpers_.clear();
    }
public:
    explicit WaveGpuParallel(unsigned requested=4) {
        const unsigned hardware=std::thread::hardware_concurrency();
        const unsigned workers=(std::max)(1u,(std::min)((std::min)(requested,4u),hardware?hardware:1u));
        // All helper creation is before any ordinal is claimed. If creation
        // fails, join partial construction and keep exact synchronous behavior.
        try {
            helpers_.reserve(workers-1);
            for(unsigned worker=1;worker<workers;++worker)
                helpers_.emplace_back([this,worker]{helper(worker);});
        } catch(...) {
            join_helpers();
            stopping_=false;
        }
    }
    ~WaveGpuParallel(){join_helpers();}
    WaveGpuParallel(const WaveGpuParallel&)=delete;
    WaveGpuParallel& operator=(const WaveGpuParallel&)=delete;
    unsigned worker_count() const noexcept{return (unsigned)helpers_.size()+1;}

    template<class Claim,class Build>
    void fill(WaveGpuBatch& batch,size_t capacity,uint64_t piece,Claim claim,Build build) {
        batch.pieces.clear();batch.full_starts.clear();batch.completed.clear();
        batch.completed_counts.clear();
        if(!capacity||!piece)return;
        // Container allocation and thread creation finish before claiming.
        // Buffers use the same bounded launch capacity as the serial cursor.
        batch.pieces.reserve(capacity);batch.full_starts.reserve(capacity);
        batch.completed.reserve(capacity);batch.completed_counts.reserve(capacity);
        ids_.resize(capacity);rows_.resize(capacity);
        std::function<void(size_t)> work=[&](size_t i){
            u256 key{};uint64_t count=0;
            if(!build(ids_[i],key,count)||count>piece)
                throw std::runtime_error("invalid one-piece traversal block");
            if(count)u256_to_be32(rows_[i].start.data(),key);
            rows_[i].count=count;
        };
        size_t count=0;
        while(count<capacity&&claim(ids_[count]))++count;
        if(!count)return;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            count_=count;completed_=0;error_=nullptr;task_=&work;++generation_;
        }
        ready_.notify_all();
        partition(0,worker_count(),count,work);
        std::exception_ptr error;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            finished_.wait(lock,[&]{return completed_==helpers_.size();});
            task_=nullptr;error=error_;
        }
        if(error)std::rethrow_exception(error);
        for(size_t i=0;i<count;++i){
            if(rows_[i].count){
                WaveGpuPiece part;part.start=rows_[i].start;part.count=rows_[i].count;
                batch.pieces.push_back(part);
                if(part.count==piece)batch.full_starts.push_back(part.start);
            }
            batch.completed.push_back(ids_[i]);
            batch.completed_counts.push_back(rows_[i].count);
        }
    }
};
