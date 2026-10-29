#pragma once
#include <cstdint>
#include <cstring>

namespace pkh_completion {
// A claimed ordinal can run ahead of unfinished work. Only the committed
// watermark proves that every finite block has finished. Rotated intervals
// retain their existing summary until completion of all digits is proven.
inline bool range_fully_checked(bool finite, bool interrupted, bool rotating,
                                const std::uint8_t* committed,
                                const std::uint8_t* total) {
    if (!finite || interrupted || rotating) return false;
    bool nonempty = false;
    for (int i = 0; i < 32; ++i) nonempty |= total[i] != 0;
    return nonempty && std::memcmp(committed, total, 32) == 0;
}
}
