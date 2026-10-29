#pragma once

#include <array>
#include <cstdint>
#include <algorithm>

// The original nibble shapes, with exact ownership of overlapping windows.
// This file has no GPU, filesystem, checkpoint or application dependencies.
namespace shaped {

using Word = std::array<uint8_t, 32>; // unsigned, big endian
enum class Kind { Zone, Wave };

struct Config {
    Word lower{}, upper{}, anchor{};
    unsigned low_nibbles = 3;
    unsigned top_index = 0; // zero-based HEX position, counted from the left
    Kind kind = Kind::Wave;
    bool fixed_wave = false;
    uint64_t seed = 0;
};

struct Result {
    Word start{}, center{}, phase_base{};
    uint64_t count = 0;
    unsigned phase = 0, step = 0, first_skipped = 0;
};

namespace detail {
inline int compare(const Word& a, const Word& b) noexcept {
    for (unsigned i = 0; i < 32; ++i) {
        if (a[i] < b[i]) return -1;
        if (a[i] > b[i]) return 1;
    }
    return 0;
}
inline bool add(const Word& a, const Word& b, Word& out) noexcept {
    unsigned carry = 0;
    for (int i = 31; i >= 0; --i) {
        const unsigned v = unsigned(a[unsigned(i)]) + b[unsigned(i)] + carry;
        out[unsigned(i)] = uint8_t(v); carry = v >> 8;
    }
    return carry == 0;
}
inline Word subtract(const Word& a, const Word& b) noexcept {
    Word out{}; unsigned borrow = 0;
    for (int i = 31; i >= 0; --i) {
        const int v = int(a[unsigned(i)]) - b[unsigned(i)] - int(borrow);
        out[unsigned(i)] = uint8_t(v); borrow = v < 0 ? 1U : 0U;
    }
    return out;
}
inline Word from_u64(uint64_t value) noexcept {
    Word out{};
    for (int i = 31; i >= 24; --i) { out[unsigned(i)] = uint8_t(value); value >>= 8; }
    return out;
}
inline Word multiply_small(const Word& value, unsigned factor) noexcept {
    Word out{}; unsigned carry = 0;
    for (int i = 31; i >= 0; --i) {
        const unsigned v = unsigned(value[unsigned(i)]) * factor + carry;
        out[unsigned(i)] = uint8_t(v); carry = v >> 8;
    }
    return out;
}
inline Word divide_small(const Word& value, unsigned divisor, unsigned& remainder) noexcept {
    Word out{}; unsigned rem = 0;
    for (unsigned i = 0; i < 32; ++i) {
        const unsigned v = rem * 256U + value[i];
        out[i] = uint8_t(v / divisor); rem = v % divisor;
    }
    remainder = rem; return out;
}
inline unsigned nibble(const Word& word, unsigned pos) noexcept {
    return (pos & 1U) ? unsigned(word[pos / 2] & 15U) : unsigned(word[pos / 2] >> 4);
}
inline void set_nibble(Word& word, unsigned pos, unsigned value) noexcept {
    value &= 15U;
    if (pos & 1U) word[pos / 2] = uint8_t((word[pos / 2] & 0xf0U) | value);
    else word[pos / 2] = uint8_t((word[pos / 2] & 15U) | (value << 4));
}
inline Word power16(unsigned exponent) noexcept {
    Word out{}; set_nibble(out, 63U - exponent, 1); return out;
}
inline uint64_t mix(uint64_t x) noexcept {
    x += UINT64_C(0x9e3779b97f4a7c15);
    x = (x ^ (x >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    x = (x ^ (x >> 27)) * UINT64_C(0x94d049bb133111eb);
    return x ^ (x >> 31);
}
inline uint32_t limb(const Word& word, unsigned index) noexcept {
    const unsigned pos = 28U - index * 4U;
    return (uint32_t(word[pos]) << 24) | (uint32_t(word[pos + 1]) << 16)
        | (uint32_t(word[pos + 2]) << 8) | uint32_t(word[pos + 3]);
}
inline void set_limb(Word& word, unsigned index, uint32_t value) noexcept {
    const unsigned pos = 28U - index * 4U;
    word[pos] = uint8_t(value >> 24); word[pos + 1] = uint8_t(value >> 16);
    word[pos + 2] = uint8_t(value >> 8); word[pos + 3] = uint8_t(value);
}
inline Word affine_power16(const Word& value, const std::array<uint32_t, 8>& multiplier,
                           const std::array<uint32_t, 8>& offset, unsigned nibbles) noexcept {
    // An odd multiplier is invertible modulo 2^(4*nibbles). Multiplication and
    // addition use all active limbs; no truncation of the center counter to 64 bits.
    std::array<uint32_t, 8> x{}, out{};
    const unsigned limbs = (nibbles + 7U) / 8U;
    for (unsigned i = 0; i < limbs; ++i) x[i] = limb(value, i);
    for (unsigned i = 0; i < limbs; ++i) {
        uint64_t carry = 0;
        for (unsigned j = 0; i + j < limbs; ++j) {
            const uint64_t v = uint64_t(x[i]) * multiplier[j] + out[i + j] + carry;
            out[i + j] = uint32_t(v); carry = v >> 32;
        }
    }
    uint64_t carry = 0;
    for (unsigned i = 0; i < limbs; ++i) {
        const uint64_t v = uint64_t(out[i]) + offset[i] + carry;
        out[i] = uint32_t(v); carry = v >> 32;
    }
    const unsigned partial_bits = (nibbles & 7U) * 4U;
    if (partial_bits) out[limbs - 1U] &= (uint32_t(1) << partial_bits) - 1U;
    Word result{};
    for (unsigned i = 0; i < limbs; ++i) set_limb(result, i, out[i]);
    return result;
}
inline bool less_than_u64(const Word& value, uint64_t limit, uint64_t& small) noexcept {
    for (unsigned i = 0; i < 24; ++i) if (value[i]) return false;
    small = 0;
    for (unsigned i = 24; i < 32; ++i) small = (small << 8) | value[i];
    return small < limit;
}
} // namespace detail

class Plan {
    struct Group {
        Word begin{}, end{};
        int highest = -1;
        unsigned states = 1, digit_shift = 0;
        std::array<uint32_t, 8> multiplier{}, offset{};
    };
    Config config_{};
    Word total_{};
    std::array<Group, 64> groups_{};
    std::array<unsigned, 16> outer_{};
    unsigned group_count_ = 0, outer_count_ = 1, dimensions_ = 0, last_ = 0;
    uint64_t block_ = 0;
    bool ready_ = false, change_outer_ = false;

    bool append_group(int highest, unsigned states, const Word& centers) noexcept {
        if (group_count_ >= groups_.size()) return false;
        Group& g = groups_[group_count_]; g = Group{};
        g.begin = total_; g.highest = highest; g.states = states;
        Word length = detail::multiply_small(centers, states * outer_count_);
        if (!detail::add(total_, length, g.end)) return false;
        total_ = g.end;
        uint64_t seed = detail::mix(config_.seed ^ (uint64_t(highest + 2) << 32)
                                   ^ (config_.kind == Kind::Wave ? UINT64_C(0x57415645) : UINT64_C(0x5a4f4e45)));
        g.digit_shift = unsigned(seed % 15U);
        for (unsigned i = 0; i < 8; ++i) {
            seed = detail::mix(seed); g.multiplier[i] = uint32_t(seed);
            seed = detail::mix(seed); g.offset[i] = uint32_t(seed >> 32);
        }
        g.multiplier[0] |= 1U;
        ++group_count_; return true;
    }

    void intersect(Result& result) const noexcept {
        // Distinct raw windows all have the same low-digit offset and length.
        // Intersection can shorten them, but can never merge two empty windows
        // into the lower bound (the old collapsing-to-lower failure).
        const Word raw = result.start;
        result.count = 0;
        if (detail::compare(raw, config_.upper) > 0) return;
        uint64_t length = block_;
        if (detail::compare(raw, config_.lower) < 0) {
            const Word gap = detail::subtract(config_.lower, raw);
            uint64_t small = 0;
            if (!detail::less_than_u64(gap, block_, small)) return;
            result.start = config_.lower; length -= small;
        }
        const Word remaining = detail::subtract(config_.upper, result.start);
        uint64_t small = 0;
        if (detail::less_than_u64(remaining, length, small)) length = small + 1;
        result.count = length;
    }

public:
    bool init(const Config& input) noexcept {
        ready_ = false; total_ = Word{}; group_count_ = 0;
        outer_count_ = 1; dimensions_ = 0; change_outer_ = false;
        if (input.low_nibbles == 0 || input.low_nibbles > 15 || input.top_index >= 64
            || (input.kind != Kind::Wave && input.kind != Kind::Zone)
            || (input.fixed_wave && input.kind != Kind::Wave)
            || detail::compare(input.lower, input.upper) > 0
            || detail::compare(input.anchor, input.lower) < 0
            || detail::compare(input.anchor, input.upper) > 0) return false;
        for (unsigned p = 0; p < input.top_index; ++p)
            if (detail::nibble(input.lower, p) != detail::nibble(input.upper, p)
                || detail::nibble(input.anchor, p) != detail::nibble(input.lower, p)) return false;
        config_ = input; last_ = 63U - config_.low_nibbles;
        block_ = UINT64_C(1) << (4U * config_.low_nibbles);
        if (!config_.fixed_wave)
            for (unsigned p = 64U - config_.low_nibbles; p < 64; ++p)
                detail::set_nibble(config_.anchor, p, 0);
        if (config_.kind == Kind::Wave) {
            dimensions_ = config_.top_index <= last_ ? last_ - config_.top_index + 1U : 0U;
        } else {
            dimensions_ = config_.top_index < last_ ? last_ - config_.top_index : 0U;
            // When top itself is inside the scanned low-digit block, changing
            // it would create overlapping windows. One clipped block suffices.
            change_outer_ = config_.top_index <= last_;
            if (change_outer_) {
                const unsigned lo = detail::nibble(config_.lower, config_.top_index);
                const unsigned hi = detail::nibble(config_.upper, config_.top_index);
                const unsigned anchor = detail::nibble(config_.anchor, config_.top_index);
                if (lo > hi || anchor < lo || anchor > hi) return false;
                outer_count_ = 0; outer_[outer_count_++] = anchor;
                for (unsigned v = lo; v <= hi; ++v) if (v != anchor) outer_[outer_count_++] = v;
                uint64_t seed = detail::mix(config_.seed ^ UINT64_C(0x6f757465725f6869));
                for (unsigned i = outer_count_; i > 2; --i) {
                    seed = detail::mix(seed);
                    const unsigned j = 1U + unsigned(seed % (i - 1U));
                    std::swap(outer_[i - 1U], outer_[j]);
                }
            }
        }
        if (!append_group(-1, 1U + 15U * dimensions_, detail::from_u64(1))) return false;
        if (!config_.fixed_wave) {
            for (unsigned h = 0; h + 1U < dimensions_; ++h) {
                const Word centers = detail::multiply_small(detail::power16(h), 15U);
                if (!append_group(int(h), 15U * (dimensions_ - h - 1U), centers)) return false;
            }
        }
        ready_ = true; return true;
    }

    const Word& total() const noexcept { return total_; }

    bool get(const Word& ordinal, Result& result) const noexcept {
        result = Result{};
        if (!ready_ || detail::compare(ordinal, total_) >= 0) return false;
        unsigned lo = 0, hi = group_count_;
        while (lo < hi) {
            const unsigned mid = lo + (hi - lo) / 2U;
            if (detail::compare(ordinal, groups_[mid].end) < 0) hi = mid;
            else lo = mid + 1U;
        }
        const Group& group = groups_[lo];
        const Word within = detail::subtract(ordinal, group.begin);
        unsigned state = 0, outer_index = 0;
        Word center_rank = detail::divide_small(within, group.states, state);
        center_rank = detail::divide_small(center_rank, outer_count_, outer_index);
        result.center = config_.anchor;
        if (change_outer_) detail::set_nibble(result.center, config_.top_index, outer_[outer_index]);

        if (group.highest >= 0) {
            const unsigned h = unsigned(group.highest);
            unsigned digit = 0;
            Word lower = detail::divide_small(center_rank, 15U, digit);
            lower = detail::affine_power16(lower, group.multiplier, group.offset, h);
            std::array<unsigned, 64> b{};
            for (unsigned i = 0; i < h; ++i) b[i] = detail::nibble(lower, 63U - i);
            b[h] = 1U + (digit + group.digit_shift) % 15U;
            if (config_.kind == Kind::Wave) {
                // Inverse difference coordinates: x_i=sum(y_i..y_top) mod16.
                unsigned sum = 0;
                for (int i = int(h); i >= 0; --i) {
                    sum = (sum + b[unsigned(i)]) & 15U;
                    const unsigned pos = last_ - unsigned(i);
                    detail::set_nibble(result.center, pos, detail::nibble(result.center, pos) + sum);
                }
            } else {
                for (unsigned i = 0; i <= h; ++i) {
                    const unsigned pos = last_ - i;
                    detail::set_nibble(result.center, pos, detail::nibble(result.center, pos) + b[i]);
                }
            }
            result.first_skipped = h + 1U;
        }

        result.phase_base = result.center; result.start = result.center;
        if (group.highest < 0 && state == 0) { intersect(result); return true; }
        const unsigned local = group.highest < 0 ? state - 1U : state;
        const unsigned phase_offset = local / 15U;
        const unsigned step = local % 15U + 1U;
        // Canonical coordinates are low-to-high. Original ZONE visits them
        // high-to-low, and reports phase1 at actual position top_index+1.
        const unsigned j = config_.kind == Kind::Wave
            ? unsigned(group.highest + 1) + phase_offset : dimensions_ - 1U - phase_offset;
        result.phase = config_.kind == Kind::Wave ? j + 1U : dimensions_ - j;
        result.step = step;
        if (config_.kind == Kind::Wave) {
            for (unsigned i = 0; i < j; ++i) {
                const unsigned pos = last_ - i;
                detail::set_nibble(result.phase_base, pos,
                    detail::nibble(result.center, pos) + 15U * (j - i));
            }
            result.start = result.phase_base;
            for (unsigned i = 0; i <= j; ++i) {
                const unsigned pos = last_ - i;
                detail::set_nibble(result.start, pos, detail::nibble(result.start, pos) + step);
            }
        } else {
            const unsigned pos = last_ - j;
            detail::set_nibble(result.start, pos, detail::nibble(result.start, pos) + step);
        }
        intersect(result); return true;
    }
};

// Proof of unique ownership (all arithmetic here is digitwise modulo16):
// ZONE uses the higher digits x directly. WAVE uses y_i=x_i-x_(i+1),
// y_top=x_top; its original synchronized phase j becomes (-1,..,-1,t,0,..0).
// Start at anchor, then use centers b whose highest nonzero coordinate is h.
// Keep exactly phases j>h. For any nonzero output, let j be its highest
// nonzero coordinate. ZONE has the unique owner obtained by zeroing digit j;
// WAVE has the unique owner with b_i=y_i+1 below j and zeros from j upward.
// Its highest nonzero coordinate is less than j, so previously owned phases
// are omitted and the original surviving nibble movements remain unchanged.
// Counts: 1+15m + sum(h=0..m-2)[15*16^h *15*(m-h-1)] = 16^m.
// Center ordering is bijective in each h-group. Zone outer-digit groups are
// disjoint and interleaved inside each group, not changed by a zone phase.
// Fixed wave emits one complete original pattern, then ends instead of cycling.

} // namespace shaped
