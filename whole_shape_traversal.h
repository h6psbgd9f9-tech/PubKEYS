#pragma once

#include "shaped_traversal.h" // Only the checked unsigned arithmetic helpers/types.
#include <atomic>

// Whole original neighborhoods around distinct, widely varying base points.
// A small Z16 parity check separates complete patterns. No visited-key table,
// GPU, filesystem, IPC or application dependencies are used here.
namespace whole_shaped {
using Word = shaped::Word;
using Kind = shaped::Kind;
using Config = shaped::Config;
using Result = shaped::Result;
namespace detail = shaped::detail;

class Plan {
    struct SignatureState { Word signature{}; unsigned state=0; };
    Config config_{};
    Word total_{}, centers_{};
    Word complete_total_{}, grid_first_{}, grid_count_{}, low_offset_{};
    Result leading_{};
    std::array<SignatureState,946> signature_states_{};
    unsigned signature_count_=0;
    std::array<std::array<uint8_t,64>,64> check_{};
    std::array<uint32_t,8> multiplier_{};
    std::array<unsigned,16> outer_{};
    unsigned outer_count_=1, dimensions_=0, parity_=0, free_=0, last_=0;
    std::array<unsigned,16> wave_top_{};
    unsigned wave_top_count_=0;
    uint64_t block_=0, identity_=0;
    bool ready_=false, change_outer_=false;

    static uint64_t next_identity() noexcept {
        static std::atomic<uint64_t> counter{1};
        return counter.fetch_add(1,std::memory_order_relaxed);
    }

    static Word block_rank(const Word& value,unsigned low) noexcept {
        Word rank{};
        for(unsigned p=0;p<64U-low;++p)
            detail::set_nibble(rank,p+low,detail::nibble(value,p));
        return rank;
    }

    Word raw_for_rank(const Word& rank) const noexcept {
        Word raw=low_offset_;
        for(unsigned p=config_.low_nibbles;p<64U;++p)
            detail::set_nibble(raw,p-config_.low_nibbles,detail::nibble(rank,p));
        return raw;
    }

    void offset_for_state(unsigned state,std::array<unsigned,64>& offset) const noexcept {
        offset.fill(0);
        if(!state)return;
        const unsigned phase=(state-1U)/15U+1U,step=(state-1U)%15U+1U;
        if(config_.kind==Kind::Zone) {
            offset[dimensions_-phase]=step;
        } else {
            for(unsigned j=0;j<phase;++j)
                offset[j]=(step+15U*(phase-1U-j))&15U;
        }
    }

    void index_signatures() noexcept {
        signature_count_=pattern_states();
        for(unsigned state=0;state<signature_count_;++state) {
            auto& item=signature_states_[state];item=SignatureState{};item.state=state;
            std::array<unsigned,64> offset{};offset_for_state(state,offset);
            for(unsigned r=0;r<parity_;++r) {
                unsigned sum=0;
                for(unsigned j=0;j<dimensions_;++j)sum+=unsigned(check_[r][j])*offset[j];
                detail::set_nibble(item.signature,63U-r,sum);
            }
        }
        std::sort(signature_states_.begin(),signature_states_.begin()+signature_count_,
            [](const SignatureState& a,const SignatureState& b){return a.signature<b.signature;});
    }

    bool init_completion() noexcept {
        low_offset_=config_.anchor;
        for(unsigned p=0;p<64U-config_.low_nibbles;++p)detail::set_nibble(low_offset_,p,0);
        const Word zero{},one=detail::from_u64(1);
        grid_first_=block_rank(config_.lower,config_.low_nibbles);
        Word last=block_rank(config_.upper,config_.low_nibbles);
        if(detail::compare(raw_for_rank(grid_first_),config_.lower)>0) {
            if(grid_first_!=zero)grid_first_=detail::subtract(grid_first_,one);
            else {
                // A fixed unaligned wave has a shifted window grid. Its first
                // (conceptually negative) window contributes only this prefix.
                leading_.start=leading_.center=leading_.phase_base=config_.lower;
                Word end=detail::subtract(low_offset_,one);
                if(detail::compare(end,config_.upper)>0)end=config_.upper;
                uint64_t length=0;
                if(!detail::less_than_u64(detail::subtract(end,config_.lower),block_,length))return false;
                leading_.count=length+1U;
            }
        }
        bool grid=true;
        if(detail::compare(raw_for_rank(last),config_.upper)>0) {
            if(last==zero)grid=false;
            else last=detail::subtract(last,one);
        }
        grid_count_=Word{};
        if(grid&&detail::compare(grid_first_,last)<=0) {
            const Word difference=detail::subtract(last,grid_first_);
            if(!detail::add(difference,one,grid_count_))return false;
        }
        Word extra=grid_count_;
        if(leading_.count&&!detail::add(extra,one,extra))return false;
        return detail::add(total_,extra,complete_total_);
    }

    bool emitted_by_family(const Word& raw) const noexcept {
        // The base enumeration covers the entire kernel of H=[I|A]. Therefore
        // H(raw-anchor) identifies at most one original pattern offset. Recover
        // its center and apply the same admission rule as get(), including
        // clipped boundary centers and a fixed-start wave's single exact base.
        for(unsigned p=0;p<config_.top_index;++p)
            if(detail::nibble(raw,p)!=detail::nibble(config_.anchor,p))return false;
        for(unsigned p=64U-config_.low_nibbles;p<64U;++p)
            if(detail::nibble(raw,p)!=detail::nibble(config_.anchor,p))return false;
        if(change_outer_) {
            const unsigned outer=detail::nibble(raw,config_.top_index);
            bool allowed=false;
            for(unsigned i=0;i<outer_count_;++i)allowed=allowed||outer_[i]==outer;
            if(!allowed)return false;
        }
        Word signature{};
        for(unsigned r=0;r<parity_;++r) {
            unsigned sum=0;
            for(unsigned j=0;j<dimensions_;++j) {
                const unsigned diff=(detail::nibble(raw,last_-j)+16U
                    -detail::nibble(config_.anchor,last_-j))&15U;
                sum+=unsigned(check_[r][j])*diff;
            }
            detail::set_nibble(signature,63U-r,sum);
        }
        unsigned lo=0,hi=signature_count_;
        while(lo<hi) {const unsigned mid=(lo+hi)/2U;
            if(signature_states_[mid].signature<signature)lo=mid+1U;else hi=mid;}
        if(lo==signature_count_||signature_states_[lo].signature!=signature)return false;
        std::array<unsigned,64> offset{};
        offset_for_state(signature_states_[lo].state,offset);
        Word center=raw;
        for(unsigned j=0;j<dimensions_;++j)
            detail::set_nibble(center,last_-j,detail::nibble(center,last_-j)+16U-offset[j]);
        if(config_.fixed_wave&&center!=config_.anchor)return false;
        Result admitted{};admitted.start=center;intersect(admitted);
        return admitted.count!=0;
    }

    bool signatures_unique() const noexcept {
        // Compare entire syndromes, never a hash of them. At most 946 rows are
        // possible with the supported positive low-digit count (m<=63).
        std::array<Word,946> signatures{};
        unsigned count=1;
        if(config_.kind==Kind::Zone) {
            for(unsigned j=0;j<dimensions_;++j) {
                for(unsigned t=1;t<=15;++t) {
                    Word& signature=signatures[count++];
                    for(unsigned r=0;r<parity_;++r)
                        detail::set_nibble(signature,63U-r,t*check_[r][j]);
                }
            }
        } else {
            // H(S_p,t)=phase_base+t*prefix; completed phases each add15*prefix.
            // This is the archive's independent digitwise +1 rule.
            std::array<unsigned,64> prefix{},base{};
            for(unsigned p=1;p<=dimensions_;++p) {
                for(unsigned r=0;r<parity_;++r)
                    prefix[r]=(prefix[r]+check_[r][p-1U])&15U;
                for(unsigned t=1;t<=15;++t) {
                    Word& signature=signatures[count++];
                    for(unsigned r=0;r<parity_;++r)
                        detail::set_nibble(signature,63U-r,base[r]+t*prefix[r]);
                }
                for(unsigned r=0;r<parity_;++r)
                    base[r]=(base[r]+15U*prefix[r])&15U;
            }
        }
        std::sort(signatures.begin(),signatures.begin()+count);
        for(unsigned i=1;i<count;++i) if(signatures[i]==signatures[i-1U]) return false;
        return true;
    }

    void identity_columns() noexcept {
        for(auto& row:check_) row.fill(0);
        for(unsigned r=0;r<parity_;++r) check_[r][r]=1;
    }

    bool construct_check() noexcept {
        if(!dimensions_) { parity_=free_=0; return true; }
        if(config_.kind==Kind::Zone) {
            // Distinct nonzero columns modulo2 separate all15 multiples of
            // different columns over Z16. Systematic unit columns come first.
            parity_=1;
            while(((1U<<parity_)-1U)<dimensions_) ++parity_;
            parity_=(std::min)(parity_,dimensions_);
            identity_columns();
            unsigned candidate=1;
            for(unsigned j=parity_;j<dimensions_;++j) {
                while((candidate&(candidate-1U))==0) ++candidate; // Skip unit columns.
                for(unsigned r=0;r<parity_;++r) check_[r][j]=uint8_t((candidate>>r)&1U);
                ++candidate;
            }
            if(!signatures_unique()) return false;
        } else {
            // Six parity digits normally separate the <=916 wave states.
            // Bounded deterministic attempts, then more digits; the identity
            // matrix is an exact final fallback, so no unverified plan is used.
            unsigned first=(std::min)(6U,dimensions_);
            if(dimensions_<=6U) {
                // A small interval can support many disjoint complete waves.
                // Do not force an identity certificate (one base) merely
                // because the wide-domain default has six parity digits.
                first=1U;unsigned capacity=16U;
                while(capacity<pattern_states()) {++first;capacity*=16U;}
            }
            bool found=false;
            for(parity_=first;parity_<=dimensions_&&!found;++parity_) {
                const unsigned attempts=parity_==dimensions_?1U:128U;
                for(unsigned trial=0;trial<attempts;++trial) {
                    identity_columns();
                    // The public certificate depends only on the shape, not on
                    // the per-run seed. Domain size therefore stays identical
                    // when the application initializes/restores its run seed.
                    uint64_t state=detail::mix(UINT64_C(0x57484f4c455f5631)
                        ^ (uint64_t(dimensions_)<<48) ^ (uint64_t(config_.low_nibbles)<<56)
                        ^ (uint64_t(parity_)<<40) ^ uint64_t(trial));
                    for(unsigned j=parity_;j<dimensions_;++j)
                        for(unsigned r=0;r<parity_;++r) {
                            state=detail::mix(state);
                            check_[r][j]=uint8_t(state&15U);
                        }
                    if(signatures_unique()) { found=true; break; }
                }
            }
            if(!found) return false;
            --parity_; // The successful for-loop iteration advanced it once.
        }
        free_=dimensions_-parity_;
        return true;
    }

    void intersect(Result& result) const noexcept {
        const Word raw=result.start;
        result.count=0;
        if(detail::compare(raw,config_.upper)>0) return;
        uint64_t length=block_,small=0;
        if(detail::compare(raw,config_.lower)<0) {
            const Word gap=detail::subtract(config_.lower,raw);
            if(!detail::less_than_u64(gap,block_,small)) return;
            result.start=config_.lower; length-=small;
        }
        const Word remaining=detail::subtract(config_.upper,result.start);
        if(detail::less_than_u64(remaining,length,small)) length=small+1U;
        result.count=length;
    }

    Word center_for(const Word& rank,unsigned outer_index) const noexcept {
        // Constant-size per-thread cache only; the immutable plan can still be
        // shared across preparation workers without a mutable-data race.
        struct Local {
            uint64_t identity=0;
            Word rank{},center{};
            unsigned outer_index=0;
        };
        static thread_local Local cache;
        if(cache.identity==identity_&&cache.rank==rank&&cache.outer_index==outer_index)
            return cache.center;
        Word center=config_.anchor;
        if(change_outer_) detail::set_nibble(center,config_.top_index,outer_[outer_index]);
        const std::array<uint32_t,8> zero{};
        Word free_values;
        if(wave_top_count_) {
            unsigned selected=0;
            const Word lower_rank=detail::divide_small(rank,wave_top_count_,selected);
            free_values=detail::affine_power16(lower_rank,multiplier_,zero,free_-1U);
            detail::set_nibble(free_values,64U-free_,
                wave_top_[selected]+16U-detail::nibble(config_.anchor,config_.top_index));
        } else free_values=detail::affine_power16(rank,multiplier_,zero,free_);
        std::array<unsigned,64> kernel{};
        for(unsigned j=parity_;j<dimensions_;++j)
            kernel[j]=detail::nibble(free_values,63U-(j-parity_));
        for(unsigned r=0;r<parity_;++r) {
            unsigned sum=0;
            for(unsigned j=parity_;j<dimensions_;++j) sum+=unsigned(check_[r][j])*kernel[j];
            kernel[r]=(16U-(sum&15U))&15U;
        }
        for(unsigned j=0;j<dimensions_;++j) {
            const unsigned position=last_-j;
            detail::set_nibble(center,position,detail::nibble(center,position)+kernel[j]);
        }
        cache.identity=identity_;cache.rank=rank;cache.outer_index=outer_index;cache.center=center;
        return center;
    }

    unsigned outer_for(const Word& rank,unsigned cycle) const noexcept {
        if(outer_count_==1U) return 0;
        struct Local {
            uint64_t identity=0;
            Word rank{};
            unsigned rotation=0;
        };
        static thread_local Local cache;
        if(cache.identity!=identity_||cache.rank!=rank) {
            uint64_t folded=detail::mix(config_.seed^UINT64_C(0x6f757465725f726b));
            bool nonzero=false;
            for(unsigned i=0;i<32;++i) {
                folded=detail::mix(folded^uint64_t(rank[i])^(uint64_t(i)<<8));
                nonzero=nonzero||rank[i]!=0;
            }
            cache.identity=identity_;cache.rank=rank;
            cache.rotation=nonzero?unsigned(folded%outer_count_):0U;
        }
        return (cycle+cache.rotation)%outer_count_;
    }

public:
    bool init(const Config& input,bool admit_wave_top=false) noexcept {
        ready_=false;total_=Word{};centers_=Word{};outer_count_=1;dimensions_=0;
        complete_total_=grid_first_=grid_count_=low_offset_=Word{};leading_=Result{};signature_count_=0;
        parity_=free_=0;change_outer_=false;identity_=0;wave_top_count_=0;
        if(input.low_nibbles==0||input.low_nibbles>15||input.top_index>=64
            ||(input.kind!=Kind::Wave&&input.kind!=Kind::Zone)
            ||(input.fixed_wave&&input.kind!=Kind::Wave)
            ||detail::compare(input.lower,input.upper)>0
            ||detail::compare(input.anchor,input.lower)<0
            ||detail::compare(input.anchor,input.upper)>0) return false;
        for(unsigned p=0;p<input.top_index;++p)
            if(detail::nibble(input.lower,p)!=detail::nibble(input.upper,p)
                ||detail::nibble(input.anchor,p)!=detail::nibble(input.lower,p)) return false;
        config_=input;last_=63U-config_.low_nibbles;
        block_=UINT64_C(1)<<(4U*config_.low_nibbles);
        if(!config_.fixed_wave)
            for(unsigned p=64U-config_.low_nibbles;p<64;++p) detail::set_nibble(config_.anchor,p,0);
        if(config_.kind==Kind::Wave) {
            dimensions_=config_.top_index<=last_?last_-config_.top_index+1U:0U;
        } else {
            dimensions_=config_.top_index<last_?last_-config_.top_index:0U;
            change_outer_=config_.top_index<=last_;
            if(change_outer_) {
                const unsigned lo=detail::nibble(config_.lower,config_.top_index);
                const unsigned hi=detail::nibble(config_.upper,config_.top_index);
                const unsigned anchor=detail::nibble(config_.anchor,config_.top_index);
                if(lo>hi||anchor<lo||anchor>hi) return false;
                outer_count_=0;outer_[outer_count_++]=anchor;
                for(unsigned v=lo;v<=hi;++v) if(v!=anchor) outer_[outer_count_++]=v;
                uint64_t seed=detail::mix(config_.seed^UINT64_C(0x77686f6c655f746f));
                for(unsigned i=outer_count_;i>2;--i) {
                    seed=detail::mix(seed);
                    const unsigned j=1U+unsigned(seed%(i-1U));
                    std::swap(outer_[i-1U],outer_[j]);
                }
            }
        }
        if(!construct_check()) return false;
        centers_=config_.fixed_wave?detail::from_u64(1):detail::power16(free_);
        // Optional admission used only by the balanced bit-length wrapper.
        // The highest free coordinate is the actual leading variable digit.
        // Enumerating its permitted values directly removes wholly empty base
        // turns without changing the parity matrix or any shape offset/phase.
        if(admit_wave_top&&config_.kind==Kind::Wave&&!config_.fixed_wave&&free_) {
            const unsigned lo=detail::nibble(config_.lower,config_.top_index);
            const unsigned hi=detail::nibble(config_.upper,config_.top_index);
            const unsigned anchor=detail::nibble(config_.anchor,config_.top_index);
            if(lo>hi||anchor<lo||anchor>hi)return false;
            wave_top_[wave_top_count_++]=anchor;
            for(unsigned value=lo;value<=hi;++value)
                if(value!=anchor)wave_top_[wave_top_count_++]=value;
            centers_=detail::multiply_small(detail::power16(free_-1U),wave_top_count_);
        }
        total_=detail::multiply_small(centers_,pattern_states()*outer_count_);
        uint64_t seed=detail::mix(config_.seed ^ UINT64_C(0x77686f6c655f726b)
            ^ (config_.kind==Kind::Wave?UINT64_C(0x57415645):UINT64_C(0x5a4f4e45)));
        for(unsigned i=0;i<8;++i) {seed=detail::mix(seed);multiplier_[i]=uint32_t(seed);}
        multiplier_[0]|=1U;
        index_signatures();
        if(!init_completion())return false;
        identity_=next_identity();
        ready_=true;return true;
    }

    const Word& total() const noexcept {return total_;}
    // Complete traversal preserves the original family as an exact prefix.
    // The suffix visits every aligned (or fixed-offset) window once and emits
    // only those not already admitted by that family. Empty suffix slots are
    // intentional; they still advance the caller's completion watermark.
    const Word& complete_total() const noexcept {return complete_total_;}
    const Word& center_count() const noexcept {return centers_;}
    unsigned pattern_states() const noexcept {return 1U+15U*dimensions_;}
    unsigned parity_digits() const noexcept {return parity_;}
    unsigned free_digits() const noexcept {return free_;}
    unsigned outer_count() const noexcept {return outer_count_;}

    bool get_complete(const Word& ordinal,Result& result) const noexcept {
        if(detail::compare(ordinal,total_)<0)return get(ordinal,result);
        result=Result{};
        if(!ready_||detail::compare(ordinal,complete_total_)>=0)return false;
        Word index=detail::subtract(ordinal,total_);
        const Word zero{},one=detail::from_u64(1);
        if(leading_.count) {
            if(index==zero){result=leading_;return true;}
            index=detail::subtract(index,one);
        }
        if(detail::compare(index,grid_count_)>=0)return false;
        Word rank{};
        if(!detail::add(grid_first_,index,rank))return false;
        result.start=result.center=result.phase_base=raw_for_rank(rank);
        if(!emitted_by_family(result.start))intersect(result);
        return true;
    }

    bool get(const Word& ordinal,Result& result) const noexcept {
        result=Result{};
        if(!ready_||detail::compare(ordinal,total_)>=0) return false;
        unsigned state=0;
        const Word base_sequence=detail::divide_small(ordinal,pattern_states(),state);
        Word rank=base_sequence;
        // First vary the full free-coordinate rank. Do not keep the entire
        // inner prefix fixed while cycling the outer digit on the first bases.
        // base_sequence<outer_count*16^free, so its quotient fits one nibble.
        const unsigned outer_cycle=detail::nibble(base_sequence,63U-free_);
        for(unsigned p=0;p<64U-free_;++p) detail::set_nibble(rank,p,0);
        const unsigned outer_index=outer_for(rank,outer_cycle);
        result.center=center_for(rank,outer_index);
        result.phase_base=result.start=result.center;
        if(state) {
            result.phase=(state-1U)/15U+1U;result.step=(state-1U)%15U+1U;
            if(config_.kind==Kind::Wave) {
                const unsigned width=result.phase;
                for(unsigned j=0;j+1U<width;++j) {
                    const unsigned position=last_-j;
                    detail::set_nibble(result.phase_base,position,
                        detail::nibble(result.center,position)+15U*(width-1U-j));
                }
                result.start=result.phase_base;
                for(unsigned j=0;j<width;++j) {
                    const unsigned position=last_-j;
                    detail::set_nibble(result.start,position,
                        detail::nibble(result.start,position)+result.step);
                }
            } else {
                const unsigned position=config_.top_index+result.phase;
                detail::set_nibble(result.start,position,
                    detail::nibble(result.start,position)+result.step);
            }
        }
        // Preserve the original requirement of a base inside the selected
        // interval (an aligned boundary block may intersect only partially).
        Result admitted{};admitted.start=result.center;intersect(admitted);
        if(!admitted.count) return true;
        intersect(result);return true;
    }
};

// Proof: all arithmetic on higher digits is in Z16. H=[I|A] has full row
// rank, and each center is anchor+c with c in ker(H). We verify that H maps
// every complete original shape offset S (including zero) to a distinct full
// syndrome. Equality of two outputs gives H(S_i)=H(S_j), hence i=j and c_i=c_j.
// Free digits enumerate the kernel bijectively; multiplication by an odd
// full-width integer is invertible modulo16^free. Distinct outer zone digits
// own disjoint spaces. All emitted windows share the same low-digit offset
// and length, so strict range intersection cannot merge distinct windows.
// get()/total() retain the original finite selective family. get_complete()
// appends its exact window complement, so their disjoint union is the whole
// inclusive interval. No history table or per-key memory is necessary.
} // namespace whole_shaped
