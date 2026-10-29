#pragma once
// Exact BitPazl block-replacement walk; the adapters below only provide its
// big-endian 256-bit arithmetic. No curve or application globals are used.
#include <cstdint>
#include <cstring>
#include "interval_digits.h"
namespace pkh_zone {
struct be256 { uint8_t b[32]{}; };
inline int be256_cmp(const be256* a,const be256* b) { return std::memcmp(a->b,b->b,32); }
inline void be256_from_u64(uint64_t v,be256* out) {
    *out=be256{}; for(int i=31;i>=24;--i) { out->b[i]=static_cast<uint8_t>(v); v>>=8; }
}
inline void be256_add(be256* a,const be256* b) {
    unsigned carry=0; for(int i=31;i>=0;--i) {
        const unsigned s=unsigned(a->b[i])+unsigned(b->b[i])+carry;
        a->b[i]=static_cast<uint8_t>(s); carry=s>>8;
    }
}
inline void be256_add_u64(be256* a,uint64_t v) {
    // Same carry walk as BitPazl: zero offsets do no work; stop as soon as
    // the remaining addend/carry is zero. Preserve modulo-2^256 overflow.
    for(int i=31;v && i>=0;--i) {
        const unsigned sum=unsigned(a->b[i])+unsigned(v&255);
        a->b[i]=static_cast<uint8_t>(sum);
        v=(v>>8)+(sum>>8);
    }
}
inline void be256_sub(const be256* a,const be256* b,be256* out) {
    unsigned borrow=0; for(int i=31;i>=0;--i) {
        const unsigned av=a->b[i], bv=unsigned(b->b[i])+borrow;
        out->b[i]=static_cast<uint8_t>(av-bv); borrow=av<bv;
    }
}
inline void be256_mask(be256* a,int bits) {
    const int full=bits/8, rem=bits%8;
    const int first=32-full-(rem?1:0);
    for(int i=0;i<first;++i) a->b[i]=0;
    if(rem) a->b[first]&=static_cast<uint8_t>((1u<<rem)-1u);
}
// One continuous random walk. Sample the initial key uniformly in [lo, hi),
// then round down to its canonical block base. Every further block: choose a
// mutable HEX position uniformly, then uniformly choose another digit whose
// block intersects [lo, hi). bounds() clips scanned keys to that interval.
// No fixed-length generations
// and no implicit reseeding at a disconnected state.
//
// КАЖДЫЙ КЛЮЧ ЦЕПОЧКИ -- НАЧАЛО БЛОКА, А НЕ ОДИНОЧНЫЙ КЛЮЧ.
//
// Почему. Одиночный ключ стоит карте ПОЛНОГО умножения на скаляр
// (scalar_mul_nib, ~64 сложения точек). Ключ внутри блока стоит одного
// сложения с инверсией, общей на весь пакет. Разница ровно та, что видна на
// счётчике: 24 Mkeys/s против 2927 в случайном режиме на той же карте.
//
// Поэтому блуждание выбирает позицию только СРЕДИ РАЗРЯДОВ ВЫШЕ БЛОКА, а
// младшие skip разрядов карта проходит целиком на каждом шаге. Правило
// замены при этом не меняется ни на йоту: случайная позиция, случайная
// допустимая цифра, границы интервала проверяются по полному ключу.
//
// Цена названа честно: младшие skip позиций блуждание больше не выбирает.
// Они не теряются -- наоборот, из "иногда заменим одну из них" становятся
// "все 16 значений каждой, каждый шаг".
struct ZoneInfo {
    uint64_t ordinal = 0;
    int position = -1;          // Changed digit from the left, 0 .. 63.
    int value = -1;
    be256 base{}, raw{};        // Previous key (initial: self), returned key.
};

struct ZoneTraversal {
    be256 lo{}, hi{}, seed{}, current{};
    // First canonical block base. It may precede lo; bounds() clips the keys
    // actually scanned. Keeping this base eligible preserves the lower tail.
    be256 block_lo{};
    uint64_t rng[4]{};
    uint64_t sequence = 0;      // Number of keys already returned by next().
    bool started = false, exhausted = false;
    // Derived solely from bounds, rebuilt by init before checkpoint restoration.
    bool rectangle = false;
    int rectangle_top = -1, rectangle_min = 0, rectangle_max = 0;

    // Блок: младшие skip HEX-разрядов перебирает сама карта.
    int skip = 0;
    // Number of keys committed within this block; checkpointed exactly.
    uint64_t offset = 0;
    // Что сделал последний шаг -- для журнала; bounds() не двигает цепочку и
    // потому должен уметь повторить эти сведения.
    int last_position = -1, last_value = -1;
    be256 last_base{};

    uint64_t block_keys() const { return 1ULL << (4 * skip); }

    // These utilities are also used by the independent wave traversal.
    static interval_digits::Key bytes(const be256& key) {
        interval_digits::Key out{}; std::memcpy(out.data(), key.b, 32); return out;
    }
    static void align(be256& key, int nibbles) {
        for (int j = 0; j < nibbles && j < 64; ++j) set_digit(key, j, 0);
    }
    static int digit_at(const be256& key, int nib) {
        return (key.b[31 - nib / 2] >> ((nib & 1) * 4)) & 15;
    }
    static void set_digit(be256& key, int nib, int value) {
        const int byte = 31 - nib / 2, shift = (nib & 1) * 4;
        key.b[byte] = static_cast<uint8_t>((key.b[byte] & ~(15 << shift)) |
                                         ((value & 15) << shift));
    }
    static void decrement(be256& key) {
        for (int i = 31; i >= 0; --i) { if (key.b[i]-- != 0) break; }
    }

    void init(int block_nibbles, const be256& begin, const be256& end,
              const be256& random_seed) {
        skip = block_nibbles < 0 ? 0 : (block_nibbles > 15 ? 15 : block_nibbles);
        // Блок шире интервала -- шагать было бы нечем: цепочка умерла бы на
        // первом же шаге, а весь интервал остался бы одним подрезанным блоком.
        // Ужимаем блок до интервала; вырожденным остаётся только случай, когда
        // интервал сам меньше одного HEX-разряда.
        {
            be256 width; be256_sub(&end, &begin, &width);
            while (skip > 0) {
                be256 span; be256_from_u64(1ULL << (4 * skip), &span);
                if (be256_cmp(&width, &span) > 0) break;
                --skip;
            }
        }
        offset = 0; last_position = -1; last_value = -1; last_base = be256{};
        lo = begin; hi = end; seed = random_seed; current = be256{};
        block_lo = lo; align(block_lo, skip);
        sequence = 0; started = false; exhausted = be256_cmp(&lo, &hi) >= 0;
        rectangle = !exhausted;
        rectangle_top = -1; rectangle_min = rectangle_max = 0;
        if (rectangle) {
            be256 last = hi; decrement(last);
            int top = 63;
            while (top >= 0 && digit_at(block_lo, top) == digit_at(last, top)) --top;
            rectangle_top = top;
            if (top >= 0) {
                rectangle_min = digit_at(block_lo, top); rectangle_max = digit_at(last, top);
                for (int nib = 0; nib < top; ++nib)
                    if (digit_at(block_lo, nib) != 0 || digit_at(last, nib) != 15) rectangle = false;
            }
        }
        for (int w = 0; w < 4; ++w) {
            rng[w] = 0;
            for (int b = 0; b < 8; ++b) rng[w] = (rng[w] << 8) | random_seed.b[8 * w + b];
        }
        if (!(rng[0] | rng[1] | rng[2] | rng[3])) rng[0] = UINT64_C(0x9e3779b97f4a7c15);
        // Mix sparse test seeds before generating either the first key or steps.
        for (int i = 0; i < 16; ++i) (void)random_u64();
    }
    static uint64_t rotate_left(uint64_t x, int n) { return (x << n) | (x >> (64 - n)); }
    uint64_t random_u64() {
        // xoshiro256**; all four words are checkpointed exactly.
        const uint64_t result = rotate_left(rng[1] * 5, 7) * 9;
        const uint64_t t = rng[1] << 17;
        rng[2] ^= rng[0]; rng[3] ^= rng[1]; rng[1] ^= rng[2]; rng[0] ^= rng[3];
        rng[2] ^= t; rng[3] = rotate_left(rng[3], 45);
        return result;
    }
    uint64_t random_below(uint64_t bound) {
        // Rejection eliminates modulo bias. Internal callers use 1 .. 64.
        const uint64_t threshold = (uint64_t(0) - bound) % bound;
        uint64_t r;
        do { r = random_u64(); } while (r < threshold);
        return r % bound;
    }
    static int popcount(uint64_t bits) {
        int n = 0; for (; bits; bits &= bits - 1) ++n; return n;
    }
    static int selected_bit(uint64_t bits, uint64_t rank) {
        while (rank--) bits &= bits - 1;
        int bit = 0; while (!(bits & 1)) { bits >>= 1; ++bit; } return bit;
    }

    // masks[nib] contains every permitted replacement EXCEPT the current digit.
    // Bounds are evaluated against the complete block base, including the
    // lower clipped block, not independent global per-position alphabets.
    // Two linear passes provide all 64 masks.
    uint64_t replacement_masks(uint16_t masks[64]) const {
        for (int i = 0; i < 64; ++i) masks[i] = 0;
        if (be256_cmp(&current, &block_lo) < 0 || be256_cmp(&current, &hi) >= 0) return 0;
        be256 last = hi; decrement(last);
        int low_suffix[65]{}, high_suffix[65]{};
        for (int p = 63; p >= 0; --p) {
            const int d = digit_at(current, 63 - p);
            const int l = digit_at(block_lo, 63 - p), h = digit_at(last, 63 - p);
            low_suffix[p] = d == l ? low_suffix[p + 1] : (d < l ? -1 : 1);
            high_suffix[p] = d == h ? high_suffix[p + 1] : (d < h ? -1 : 1);
        }
        bool low_prefix = true, high_prefix = true;
        uint64_t positions = 0;
        for (int p = 0; p < 64; ++p) {
            const int nib = 63 - p, d = digit_at(current, nib);
            const int l = digit_at(block_lo, nib), h = digit_at(last, nib);
            const int first = low_prefix ? l + (low_suffix[p + 1] < 0 ? 1 : 0) : 0;
            const int final = high_prefix ? h - (high_suffix[p + 1] > 0 ? 1 : 0) : 15;
            if (first <= final) {
                const uint32_t upper = (UINT32_C(1) << (final + 1)) - 1;
                const uint32_t lower = (UINT32_C(1) << first) - 1;
                masks[nib] = static_cast<uint16_t>((upper & ~lower) & ~(UINT32_C(1) << d));
                if (masks[nib]) positions |= UINT64_C(1) << nib;
            }
            low_prefix = low_prefix && d == l;
            high_prefix = high_prefix && d == h;
        }
        return positions;
    }
    uint64_t valid_positions() const {
        uint16_t masks[64]; return replacement_masks(masks);
    }

    bool nominal_neighbors_inside() const {
        // Even nonrectangular intervals often allow every nominal replacement
        // at the current key. Bound the whole set of single-digit neighbours
        // by its exact extrema, avoiding construction of 64 replacement masks.
        be256 smallest = current, largest = current;
        for (int nib = rectangle_top; nib >= skip; --nib) {
            const int minimum = nib == rectangle_top ? rectangle_min : 0;
            if (digit_at(current, nib) > minimum) {
                set_digit(smallest, nib, minimum); break;
            }
        }
        for (int nib = rectangle_top; nib >= skip; --nib) {
            const int maximum = nib == rectangle_top ? rectangle_max : 15;
            if (digit_at(current, nib) < maximum) {
                set_digit(largest, nib, maximum); break;
            }
        }
        // Including current when there is no smaller/larger neighbour is safe:
        // valid init/next/checkpoint states always have current within bounds.
        return be256_cmp(&smallest, &block_lo) >= 0 && be256_cmp(&largest, &hi) < 0;
    }

    // Ровно один шаг цепочки: первый ключ либо одна замена разряда.
    // Ключ всегда выровнен по началу блока -- младшие skip разрядов нулевые,
    // их перебирает карта.
    bool step() {
        if (exhausted) return false;
        if (sequence == UINT64_MAX) { exhausted = true; return false; }
        last_position = -1; last_value = -1;
        if (!started) {
            be256 maximum; be256_sub(&hi, &lo, &maximum); decrement(maximum);
            int bits = 0;
            for (int i = 0; i < 32; ++i) if (maximum.b[i]) {
                bits = (31 - i) * 8;
                for (uint8_t v = maximum.b[i]; v; v >>= 1) ++bits;
                break;
            }
            be256 start_offset{};
            if (bits) do {
                for (int w = 0; w < 4; ++w) {
                    const uint64_t value = random_u64();
                    for (int b = 0; b < 8; ++b)
                        start_offset.b[8 * w + b] = static_cast<uint8_t>(value >> (56 - 8 * b));
                }
                be256_mask(&start_offset, bits);
            } while (be256_cmp(&start_offset, &maximum) > 0);
            current = lo; be256_add(&current, &start_offset);
            // Выравнивание по началу блока. Вниз, а не вверх: так первый
            // неполный блок у нижней границы не теряет своих ключей.
            align(current, skip);
            // The first block base may be below lo. Replacement masks use
            // block_lo, while bounds() clips this base to lo before scanning.
            // Clamping the base itself to lo would change the grid residue;
            // with any other initial block the lower partial block would then
            // remain unreachable for the entire random walk.
            last_base = current; started = true;
        } else {
            int nib, value;
            if (rectangle || nominal_neighbors_inside()) {
                if (rectangle_top < skip) { exhausted = true; return false; }
                // An exact product of fixed digit alphabets has constant domains.
                // Ascending position/value ranks and the same two bounded RNG
                // draws give byte-for-byte the same walk as replacement_masks.
                // current is valid after init/next and validated checkpoint load.
                nib = skip + static_cast<int>(random_below(rectangle_top + 1 - skip));
                const int first = nib == rectangle_top ? rectangle_min : 0;
                const int final = nib == rectangle_top ? rectangle_max : 15;
                value = first + static_cast<int>(random_below(final - first));
                if (value >= digit_at(current, nib)) ++value;
            } else {
                uint16_t masks[64]; uint64_t eligible = replacement_masks(masks);
                // Разряды внутри блока цепочка не выбирает: их и так проходит
                // карта целиком на каждом шаге.
                if (skip > 0) eligible &= ~((skip >= 64) ? ~UINT64_C(0)
                                                        : ((UINT64_C(1) << skip) - 1));
                if (!eligible) { exhausted = true; return false; }
                // Positions with more permitted digits receive no extra weight.
                nib = selected_bit(eligible, random_below(popcount(eligible)));
                value = selected_bit(masks[nib], random_below(popcount(masks[nib])));
            }
            last_base = current; set_digit(current, nib, value);
            last_position = 63 - nib; last_value = value;
        }
        ++sequence;
        return true;
    }

    void fill(ZoneInfo& info) const {
        info = ZoneInfo{};
        info.ordinal = sequence ? sequence - 1 : 0;
        info.position = last_position; info.value = last_value;
        info.base = last_base; info.raw = current;
    }

    // Прежняя подпись: шаг и ключ разом. Оставлена для самопроверки запуска.
    bool next(be256& key, ZoneInfo& info) {
        if (!step()) return false;
        fill(info); key = current;
        return true;
    }

    // Границы блока ТЕКУЩЕГО ключа. Цепочку не двигает. Ноль -- блок целиком
    // за интервалом; хвост блока у края не пропадает, начало и конец
    // подрезаются по [lo, hi).
    uint64_t bounds(be256& begin, be256& end, ZoneInfo& info) {
        if (!started && !step()) return 0;
        fill(info);
        begin = current; end = current; be256_add_u64(&end, block_keys());
        if (be256_cmp(&end, &current) < 0 || be256_cmp(&end, &hi) > 0) end = hi;
        if (be256_cmp(&begin, &lo) < 0) begin = lo;
        if (be256_cmp(&begin, &end) >= 0) return 0;
        be256 length; be256_sub(&end, &begin, &length);
        uint64_t count = 0;
        for (int i = 24; i < 32; ++i) count = (count << 8) | length.b[i];
        return count;
    }

    // Блок досчитан -- следующая замена разряда.
    bool advance() { offset = 0; return step(); }
};

} // namespace pkh_zone
