#pragma once
#include "whole_shape_traversal.h"
#include <vector>
#include <stdexcept>

// Balance visits to complete original base patterns across disjoint bit-length
// intervals. A visit is an indivisible pattern, not an equal wall-clock slice.
// Each bucket then continues through its own exact remaining-window complement.
namespace balanced_shaped {
using Word=whole_shaped::Word;
using Config=whole_shaped::Config;
using Result=whole_shaped::Result;
namespace detail=whole_shaped::detail;

class Plan {
    struct Bucket {
        whole_shaped::Plan plan;
        Word lower{},upper{},total{},full_turns{};
        unsigned bits=0,top=0,quantum=1;
    };
    // Plan instances contain parity certificates; allocate them on the heap,
    // never as a 256-element stack array on a Windows worker.
    std::vector<Bucket> buckets_;
    Word total_{},rounds_{};
    uint64_t identity_=0;
    bool fixed_=false,ready_=false;
    inline static std::atomic<uint64_t> identities_{1};

    static Word power2(unsigned exponent) noexcept {
        Word word{};word[31U-exponent/8U]=uint8_t(1U<<(exponent&7U));return word;
    }
    static unsigned bit_length(const Word& word) noexcept {
        for(unsigned i=0;i<32;++i)if(word[i]) {
            unsigned top=word[i],bits=0;while(top){++bits;top>>=1U;}
            return (31U-i)*8U+bits;
        }
        return 0;
    }
    static Word choose_anchor(const Word& lo,const Word& hi,uint64_t seed) noexcept {
        const Word span=detail::subtract(hi,lo);const unsigned bits=bit_length(span);
        Word offset{},result{};
        for(unsigned attempt=0;attempt<128;++attempt) {
            for(unsigned i=0;i<32;i+=8) {
                seed=detail::mix(seed);
                for(unsigned j=0;j<8;++j)offset[i+j]=uint8_t(seed>>(56U-8U*j));
            }
            const unsigned whole=bits/8U,partial=bits&7U;
            for(unsigned i=0;i<32U-whole-(partial?1U:0U);++i)offset[i]=0;
            if(partial)offset[31U-whole]&=uint8_t((1U<<partial)-1U);
            if(detail::compare(offset,span)<=0){detail::add(lo,offset,result);return result;}
        }
        return lo;
    }
    Word consumed(const Word& round,const Bucket& bucket) const noexcept {
        if(detail::compare(round,bucket.full_turns)>0)return bucket.total;
        Word product=detail::multiply_small(round,bucket.quantum);
        return detail::compare(product,bucket.total)>0?bucket.total:product;
    }
    Word prefix(const Word& round) const noexcept {
        Word sum{};
        for(const auto& bucket:buckets_){Word next{};detail::add(sum,consumed(round,bucket),next);sum=next;}
        return sum;
    }
    struct Turn {size_t bucket=0;Word local{};unsigned end=0;};
    struct Cache {uint64_t identity=0;Word begin{},end{};std::vector<Turn> turns;};
    void prepare_cache(const Word& ordinal,Cache& cache) const {
        Word lo{},hi=rounds_;const Word one=detail::from_u64(1);
        // Largest whole round whose prefix does not exceed this ordinal.
        while(detail::compare(lo,hi)<0) {
            unsigned rem=0;Word half=detail::divide_small(detail::subtract(hi,lo),2,rem);
            if(rem){Word next{};detail::add(half,one,next);half=next;}
            Word middle{};detail::add(lo,half,middle);
            if(detail::compare(prefix(middle),ordinal)<=0)lo=middle;
            else hi=detail::subtract(middle,one);
        }
        cache.turns.clear();cache.turns.reserve(buckets_.size());
        const Word begin=prefix(lo);unsigned cumulative=0;
        for(size_t i=0;i<buckets_.size();++i) {
            const auto& bucket=buckets_[i];const Word local=consumed(lo,bucket);
            if(detail::compare(local,bucket.total)>=0)continue;
            const Word left=detail::subtract(bucket.total,local);uint64_t small=0;
            const unsigned count=detail::less_than_u64(left,bucket.quantum,small)?unsigned(small):bucket.quantum;
            cumulative+=count;cache.turns.push_back(Turn{i,local,cumulative});
        }
        Word end{};detail::add(begin,detail::from_u64(cumulative),end);
        if(!cumulative||detail::compare(ordinal,begin)<0||detail::compare(ordinal,end)>=0)
            throw std::runtime_error("Invalid balanced traversal round");
        cache.begin=begin;cache.end=end;cache.identity=identity_;
    }
public:
    bool init(const Config& input) {
        ready_=false;fixed_=input.fixed_wave;total_=rounds_=Word{};buckets_.clear();identity_=0;
        if(detail::compare(input.lower,input.upper)>0)return false;
        try {
            buckets_.reserve(257);
            if(fixed_) {
                buckets_.emplace_back();auto& bucket=buckets_.back();
                if(!bucket.plan.init(input))return false;
                bucket.lower=input.lower;bucket.upper=input.upper;bucket.top=input.top_index;
                bucket.total=bucket.plan.complete_total();total_=bucket.total;
            } else {
                const Word one=detail::from_u64(1);
                for(unsigned bits=0;bits<=256;++bits) {
                    Word lo{},hi{};
                    if(bits){lo=power2(bits-1U);if(bits==256)hi.fill(255);else hi=detail::subtract(power2(bits),one);}
                    if(detail::compare(lo,input.lower)<0)lo=input.lower;
                    if(detail::compare(hi,input.upper)>0)hi=input.upper;
                    if(detail::compare(lo,hi)>0)continue;
                    buckets_.emplace_back();auto& bucket=buckets_.back();
                    bucket.bits=bits;bucket.top=bits?64U-(bits+3U)/4U:63U;
                    bucket.lower=lo;bucket.upper=hi;
                    Config cfg=input;cfg.lower=lo;cfg.upper=hi;cfg.top_index=bucket.top;
                    cfg.seed=detail::mix(input.seed^(UINT64_C(0x6269745f7475726e)+bits));
                    cfg.anchor=choose_anchor(lo,hi,cfg.seed);
                    if(!bucket.plan.init(cfg,true))return false;
                    bucket.total=bucket.plan.complete_total();bucket.quantum=bucket.plan.pattern_states();
                    unsigned rem=0;bucket.full_turns=detail::divide_small(bucket.total,bucket.quantum,rem);
                    Word next{};
                    if(!detail::add(total_,bucket.total,next))return false;total_=next;
                    Word count=bucket.full_turns;
                    if(rem){detail::add(count,one,next);count=next;}
                    if(detail::compare(count,rounds_)>0)rounds_=count;
                }
            }
            if(buckets_.empty())return false;
            identity_=identities_.fetch_add(1,std::memory_order_relaxed);ready_=true;return true;
        }catch(...){ready_=false;return false;}
    }
    const Word& complete_total() const noexcept{return total_;}
    bool fixed_start_passthrough() const noexcept{return fixed_;}
    size_t bucket_count() const noexcept{return buckets_.size();}
    unsigned bucket_bit_length(size_t i) const{return buckets_.at(i).bits;}
    unsigned bucket_top_index(size_t i) const{return buckets_.at(i).top;}
    unsigned bucket_pattern_states(size_t i) const{return buckets_.at(i).quantum;}
    const Word& bucket_total(size_t i) const{return buckets_.at(i).total;}
    const whole_shaped::Plan& bucket_plan(size_t i) const{return buckets_.at(i).plan;}
    void bucket_bounds(size_t i,Word& lo,Word& hi) const{lo=buckets_.at(i).lower;hi=buckets_.at(i).upper;}
    bool get_complete(const Word& ordinal,Result& result,unsigned* bits=nullptr,unsigned* top=nullptr) const {
        result=Result{};if(!ready_||detail::compare(ordinal,total_)>=0)return false;
        if(fixed_){if(bits)*bits=0;if(top)*top=buckets_[0].top;return buckets_[0].plan.get_complete(ordinal,result);}
        static thread_local Cache cache;
        if(cache.identity!=identity_||detail::compare(ordinal,cache.begin)<0||detail::compare(ordinal,cache.end)>=0)
            prepare_cache(ordinal,cache);
        uint64_t offset=0;
        if(!detail::less_than_u64(detail::subtract(ordinal,cache.begin),UINT64_C(1)<<32,offset))return false;
        const auto it=std::upper_bound(cache.turns.begin(),cache.turns.end(),unsigned(offset),
            [](unsigned value,const Turn& turn){return value<turn.end;});
        if(it==cache.turns.end())return false;
        const unsigned previous=it==cache.turns.begin()?0U:(it-1)->end;
        Word local{};detail::add(it->local,detail::from_u64(offset-previous),local);
        const auto& bucket=buckets_[it->bucket];if(bits)*bits=bucket.bits;if(top)*top=bucket.top;
        return bucket.plan.get_complete(local,result);
    }
};
} // namespace balanced_shaped
