#pragma once
#include "traverse.h"
#include <array>

// Rotation can revisit a timed-out digit during one application run. Keep its
// confirmed position in memory. This can be newer than its disk checkpoint.
class VolatileIntervalProgress {
    struct Entry { bool valid=false; ChunkIdx position{}; uint64_t seed=0; u256 checked{}; };
    std::array<Entry,16> entries_{};
public:
    void remember(int digit,const ChunkIdx& position,uint64_t seed,uint64_t checked=0) {
        u256 exact;u256_from_u64(exact,checked);remember_exact(digit,position,seed,exact);
    }
    void remember_exact(int digit,const ChunkIdx& position,uint64_t seed,const u256 checked) {
        if(digit<0||digit>=16)return;
        auto& entry=entries_[(size_t)digit];entry.valid=true;entry.position=position;entry.seed=seed;
        u256_set(entry.checked,checked);
    }
    bool recall(int digit,ChunkIdx& position,uint64_t& seed,uint64_t* checked=nullptr) const {
        if(digit<0||digit>=16||!entries_[(size_t)digit].valid)return false;
        position=entries_[(size_t)digit].position;seed=entries_[(size_t)digit].seed;
        if(checked){const auto& value=entries_[(size_t)digit].checked;
            *checked=(value[1]||value[2]||value[3])?UINT64_MAX:value[0];}
        return true;
    }
    bool recall_exact(int digit,ChunkIdx& position,uint64_t& seed,u256 checked) const {
        if(digit<0||digit>=16||!entries_[(size_t)digit].valid)return false;
        const auto& entry=entries_[(size_t)digit];position=entry.position;seed=entry.seed;
        u256_set(checked,entry.checked);return true;
    }
};
