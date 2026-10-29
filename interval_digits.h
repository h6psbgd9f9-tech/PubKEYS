#pragma once

#include <array>
#include <cstdint>

namespace interval_digits {

// Big-endian unsigned 256-bit integer. HEX positions are 0 (left) to 63 (right).
using Key = std::array<uint8_t, 32>;

inline int compare(const Key& a, const Key& b) {
    for (unsigned i = 0; i < a.size(); ++i) {
        if (a[i] < b[i]) return -1;
        if (a[i] > b[i]) return 1;
    }
    return 0;
}

// An invalid position returns 0xff, outside the set of valid HEX digits.
inline uint8_t digit(const Key& key, int position) {
    if (position < 0 || position >= 64) return 0xff;
    const unsigned byte = static_cast<unsigned>(position) / 2;
    return static_cast<uint8_t>((position & 1) ? (key[byte] & 15u) : (key[byte] >> 4));
}

inline int leading_position(const Key& hi) {
    for(int pos=0;pos<64;++pos)if(digit(hi,pos))return pos;
    return 63;
}

namespace detail {
inline void set_digit(Key& key, int position, unsigned value) {
    const unsigned byte = static_cast<unsigned>(position) / 2;
    if (position & 1) key[byte] = static_cast<uint8_t>((key[byte] & 0xf0u) | value);
    else key[byte] = static_cast<uint8_t>((key[byte] & 0x0fu) | (value << 4));
}

inline void clear_lower_digits(Key& key, int position) {
    const unsigned byte = static_cast<unsigned>(position) / 2;
    if ((position & 1) == 0) key[byte] &= 0xf0u;
    for (unsigned i = byte + 1; i < key.size(); ++i) key[i] = 0;
}

inline bool increment_prefix(Key& key, int position) {
    for (int i = position - 1; i >= 0; --i) {
        const unsigned value = digit(key, i);
        if (value != 15) {
            set_digit(key, i, value + 1);
            return true;
        }
        set_digit(key, i, 0);
    }
    return false;
}
} // namespace detail

// Restrict the inclusive interval to a zone at its highest nonzero HEX position.
// Unlike changing the traversal order, this excludes all other leading digits.
inline bool leading_zone(const Key& lo,const Key& hi,unsigned value,Key& out_lo,Key& out_hi) {
    if(value>15 || compare(lo,hi)>0)return false;
    const int pos=leading_position(hi);
    Key first{},last{};
    detail::set_digit(first,pos,value);last=first;
    for(int i=pos+1;i<64;++i)detail::set_digit(last,i,15);
    if(compare(first,lo)<0)first=lo;
    if(compare(last,hi)>0)last=hi;
    if(compare(first,last)>0)return false;
    out_lo=first;out_hi=last;return true;
}

// Returns the smallest key in inclusive [lo, hi] with the requested digit.
// Failure leaves out unchanged, including invalid bounds/position/value.
inline bool first_with_digit(const Key& lo, const Key& hi, int position,
                              unsigned value, Key& out) {
    if (position < 0 || position >= 64 || value > 15 || compare(lo, hi) > 0) return false;
    Key candidate = lo;
    const unsigned current = digit(lo, position);
    if (current != value) {
        if (current > value && !detail::increment_prefix(candidate, position)) return false;
        detail::set_digit(candidate, position, value);
        detail::clear_lower_digits(candidate, position);
    }
    if (compare(candidate, hi) > 0) return false;
    out = candidate;
    return true;
}

// Bit v is set iff digit v occurs at position in at least one key in [lo, hi].
inline uint16_t allowed_digits(const Key& lo, const Key& hi, int position) {
    if (position < 0 || position >= 64 || compare(lo, hi) > 0) return 0;
    uint16_t result = 0;
    Key candidate{};
    for (unsigned value = 0; value < 16; ++value) {
        if (first_with_digit(lo, hi, position, value, candidate))
            result = static_cast<uint16_t>(result | (uint16_t{1} << value));
    }
    return result;
}

// Prefer changing only the requested digit of base. If that key is outside
// [lo, hi], use the first valid key instead. No base-in-range precondition.
inline bool choose_with_digit(const Key& base, const Key& lo, const Key& hi,
                               int position, unsigned value, Key& out) {
    if (position < 0 || position >= 64 || value > 15 || compare(lo, hi) > 0) return false;
    Key candidate = base;
    detail::set_digit(candidate, position, value);
    if (compare(candidate, lo) >= 0 && compare(candidate, hi) <= 0) {
        out = candidate;
        return true;
    }
    return first_with_digit(lo, hi, position, value, out);
}

} // namespace interval_digits
