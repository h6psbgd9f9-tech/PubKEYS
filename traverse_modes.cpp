// ============================================================================
//  traverse_modes.cpp — четыре способа обойти диапазон.
//
//  Разница между ними не в скорости: ключ стоит одинаково в любом режиме.
//  Разница в ПОРЯДКЕ и в том, что можно утверждать о покрытии.
//
//    Случайный      — участки берутся наугад. Повторы возможны, пропуски
//                     тоже. Утверждать «диапазон проверен» нельзя.
//    Последовательный — блоки подряд снизу вверх. Полное покрытие,
//                     чекпоинт, честный процент.
//    Зональный — одна старшая HEX-позиция за раз вокруг базовой точки.
//    Волновой  — растущая группа соседних позиций, синхронно по модулю 16.
//                     Каждый узор полный. Базы разделены точным ниббловым
//                     проверочным отображением whole_shape_traversal.h;
//                     список посещённых ключей не нужен. Это выборочный обход.
//    KXE            — те же блоки, что и в последовательном, но в
//                     переставленном порядке. Полное покрытие БЕЗ повторов,
//                     при этом обход выглядит случайным. Перестановка
//                     обратима и задаётся счётчиком, поэтому чекпоинт
//                     работает так же, как в последовательном.
// ============================================================================
#include "traverse.h"
#include "targets.h"
#include "interval_digits.h"
#include "packed_traversal.h"
#include "zone_walk.h"
#include "whole_shape_traversal.h"
#include "balanced_shape_traversal.h"

#include <cstring>
#include <iostream>
#include <mutex>
#include <array>
#include <map>
#include <vector>
#include <new>

// ---------------------------- старший разряд --------------------------------
//
// Ключ — это 64 шестнадцатеричных разряда (ниббла). Верхняя граница
// диапазона задаёт позицию старшего значащего: при [2^0, 2^256) это разряд 0,
// при [2^39, 2^40) — уже 54-й. Всё, что левее, всегда нули.
//
// Зафиксировав этот разряд, пространство поиска делится на равные части —
// удобно, когда счёт идёт на нескольких машинах.

int g_top_ni   = 0;    // позиция старшего значащего ниббла, 0 = самый левый
int g_top_vmin = 1;    // допустимые значения этого разряда
int g_top_vmax = 15;

// Zone metadata describes the BitPazl replacement walk independently of
// wave's g_skip_low. Bounds are closed here, half-open in ZoneTraversal.
int g_zone_first = 64;
int g_zone_count = 0;
static int g_zone_top = -1, g_zone_min = 0, g_zone_max = 15;
static interval_digits::Key g_zone_lo{}, g_zone_hi{};
static int g_zone_requested_cached = -1, g_zone_actual_skip = 0;
static bool g_zone_plan_ready = false;

static inline int zone_digit(const u256 k, int nib) {
    return (int)((k[nib/16] >> (4*(nib%16))) & 15);
}

static void zone_walk_init(pkh_zone::ZoneTraversal& walk, const uint8_t seed[32]) {
    pkh_zone::be256 lo, end, random_seed;
    u256_to_be32(lo.b, g_range_start);
    u256_to_be32(end.b, g_range_end);
    pkh_zone::be256_add_u64(&end, 1);
    std::memcpy(random_seed.b, seed, 32);
    walk.init(g_cfg.zone_nibbles, lo, end, random_seed);
}

void compute_zone_plan() {
    u256_to_be32(g_zone_lo.data(), g_range_start);
    u256_to_be32(g_zone_hi.data(), g_range_end);
    g_zone_top = 63;
    while (g_zone_top >= 0 && zone_digit(g_range_start, g_zone_top)
                               == zone_digit(g_range_end, g_zone_top)) --g_zone_top;
    g_zone_min = g_zone_top >= 0 ? zone_digit(g_range_start, g_zone_top) : 0;
    g_zone_max = g_zone_top >= 0 ? zone_digit(g_range_end, g_zone_top) : 0;
    const uint8_t zero_seed[32]{};
    pkh_zone::ZoneTraversal walk;
    zone_walk_init(walk, zero_seed);
    g_zone_actual_skip = walk.skip;
    g_zone_count = 0;
    g_zone_first = 64;
    for (int nib = walk.skip; nib <= g_zone_top; ++nib) {
        const auto digits = interval_digits::allowed_digits(g_zone_lo, g_zone_hi, 63 - nib);
        if (digits && (digits & (digits - 1))) {
            ++g_zone_count;
            g_zone_first = 63 - nib;
        }
    }
    g_zone_requested_cached = g_cfg.zone_nibbles;
    g_zone_plan_ready = true;
}

static void zone_plan_need() {
    interval_digits::Key lo{}, hi{};
    u256_to_be32(lo.data(), g_range_start); u256_to_be32(hi.data(), g_range_end);
    if (!g_zone_plan_ready || g_zone_requested_cached != g_cfg.zone_nibbles
        || lo != g_zone_lo || hi != g_zone_hi) compute_zone_plan();
}

int zone_top_position() { zone_plan_need(); return g_zone_top >= 0 ? 63 - g_zone_top : -1; }
int zone_top_min() { zone_plan_need(); return g_zone_min; }
int zone_top_max() { zone_plan_need(); return g_zone_max; }
int zone_block_nibbles() {
    // Finite production traversal clips a single oversized block; shrinking
    // belongs only to the retained legacy random-walk adapter.
    if (g_cfg.mode == TRAV_ZONE) return (std::max)(3,(std::min)(15,g_cfg.zone_nibbles));
    zone_plan_need(); return g_zone_actual_skip;
}
uint64_t zone_block_keys() { return UINT64_C(1) << (4 * zone_block_nibbles()); }

bool unique_blocks_mode() { return g_cfg.mode == TRAV_WAVE || g_cfg.mode == TRAV_ZONE; }
bool traversal_full_coverage() {
    return g_cfg.mode == TRAV_SEQ || g_cfg.mode == TRAV_KXE || unique_blocks_mode();
}
static const balanced_shaped::Plan &restored_shape_plan();
int traversal_block_bits() {
    if (g_cfg.mode == TRAV_WAVE) return 4 * g_skip_low;
    if (g_cfg.mode == TRAV_ZONE) return 4 * zone_block_nibbles();
    return g_cfg.chunk_bits;
}

// Умолчание -- прежние 4096 ключей (три ниббла). Меняет его только
// set_block_nibbles(), и только при запуске.
int      g_skip_low  = 3;
uint64_t ZONE_BLOCK  = 4096;

static void wave_tables_build();       // ниже: таблицы зависят от g_skip_low
static inline void wave_tables_need(); // и «взвести» call_once, см. ниже

void set_block_nibbles(int nib) {
    if (nib < 2)  nib = 2;             // isolated packed benchmark permits 256-key segments
    if (nib > 15) nib = 15;            // 16 нибблов -- это уже всё 64-битное число
    g_skip_low = nib;
    ZONE_BLOCK = 1ULL << (4 * nib);
    // Сперва need(), потом build(): need() «взводит» call_once, иначе первый
    // же рабочий поток, дойдя до wave_tables_need(), пересобрал бы таблицы
    // заново -- то есть сделал бы memset общих таблиц уже под многопоточным
    // чтением. Здесь мы ещё в один поток, и обе перестройки безопасны.
    wave_tables_need();
    wave_tables_build();               // число фаз изменилось -- значит и таблицы
    compute_top_nibble();              // число зон считается из того же числа
}

// Обнуляет младшие g_skip_low нибблов: ключ становится началом блока.
// Здесь стояло `out[31] = 0; out[30] &= 0xF0;` -- три ниббла, вписанные руками
// в двух разных местах. Теперь место одно и нибблов любое число.
static void align_to_block(uint8_t out[32]) {
    int nib = g_skip_low;
    int i = 31;
    while (nib >= 2 && i >= 0) { out[i] = 0; i--; nib -= 2; }
    if (nib == 1 && i >= 0) out[i] &= 0xF0;
}

void compute_top_nibble() {
    int hi = g_cfg.range_end_bit;
    // старший значащий бит диапазона — hi-1, его ниббл считаем слева
    g_top_ni = (255 - (hi - 1)) / 4;
    if (g_top_ni < 0)  g_top_ni = 0;
    if (g_top_ni > 63) g_top_ni = 63;

    // какие значения этот разряд вообще может принимать
    int lb = 252 - 4 * g_top_ni;          // младший бит разряда
    int hi_bits = hi - lb;                 // сколько бит разряда задействовано
    if (hi_bits >= 4) g_top_vmax = 15; else g_top_vmax = (1 << hi_bits) - 1;
    int lo = g_cfg.range_start_bit;
    if (lo >= lb) {
        int lo_bits = lo - lb;
        g_top_vmin = (lo_bits >= 4) ? 15 : (1 << lo_bits);
    } else g_top_vmin = 0;
    if (g_top_vmin > g_top_vmax) g_top_vmin = g_top_vmax;

    compute_zone_plan();
}

// Ставит выбранное значение в старший разряд ключа.
void apply_top_nibble(uint8_t k[32]) {
    if (g_cfg.det_hex < 0) return;
    int byte = g_top_ni / 2;
    if (g_top_ni % 2 == 0)
        k[byte] = (uint8_t)((k[byte] & 0x0F) | ((g_cfg.det_hex & 0xF) << 4));
    else
        k[byte] = (uint8_t)((k[byte] & 0xF0) | (g_cfg.det_hex & 0xF));
}

// ------------------------------- KXE ----------------------------------------
//
// Нужен порядок, который выглядит случайным, но при этом обходит каждый блок
// ровно один раз. Это в точности обратимое отображение множества номеров
// блоков в себя, то есть перестановка.
//
// Строится сетью Фейстеля на четырёх раундах. Она обратима по построению, а
// значит, разным счётчикам отвечают разные блоки — повторов не бывает
// вовсе, и проверять это отдельной памятью не нужно.
//
// Сеть работает на степени двойки, а блоков обычно не степень. Лишние
// значения не отбрасываются, а прогоняются через сеть повторно, пока не
// попадут в диапазон: приём называется cycle-walking и перестановку
// сохраняет.
//
// ИСПРАВЛЕНО: раньше домен перестановки был uint64_t — если число блоков
// не помещалось в 64 бита (любой диапазон шире примерно 2^(64+chunk_bits)),
// KXE объявлялся недоступным и тихо подменялся случайным режимом. Теперь
// счётчик, домен и результат — полные 256-битные числа (перенос той же
// схемы, что в bitpazl256.cpp того же проекта): весь [0, 2^256) покрывается
// без исключений. Представление — uint8_t[32], старший байт первый, тот
// же порядок, что даёт u256_to_be32; конвертация на границе с u256.

static inline int be256_cmp(const uint8_t a[32], const uint8_t b[32]) noexcept {
    return memcmp(a, b, 32);
}
static void be256_sub(const uint8_t a[32], const uint8_t b[32], uint8_t out[32]) noexcept {
    int borrow = 0;
    for (int i = 31; i >= 0; i--) {
        int d = (int)a[i] - (int)b[i] - borrow;
        borrow = 0;
        if (d < 0) { d += 256; borrow = 1; }
        out[i] = (uint8_t)d;
    }
}
static void be256_add(uint8_t a[32], const uint8_t b[32]) noexcept {   // a += b (mod 2^256)
    int carry = 0;
    for (int i = 31; i >= 0; i--) {
        int s = (int)a[i] + (int)b[i] + carry;
        a[i] = (uint8_t)(s & 0xFF);
        carry = s >> 8;
    }
}
static void be256_shr(const uint8_t in[32], int n, uint8_t out[32]) noexcept {
    uint8_t shifted[32]{};
    if (n <= 0) { memcpy(out,in,32); return; }
    if (n < 256) {
        const int bytes=n/8,bits=n%8;
        for(int i=bytes;i<32;++i){
            const int source=i-bytes;
            unsigned value=(unsigned)in[source]>>bits;
            if(bits&&source>0)value|=(unsigned)in[source-1]<<(8-bits);
            shifted[i]=(uint8_t)value;
        }
    }
    memcpy(out,shifted,32);
}
static void be256_shl(const uint8_t in[32], int n, uint8_t out[32]) noexcept {
    uint8_t shifted[32]{};
    if (n <= 0) { memcpy(out,in,32); return; }
    if (n < 256) {
        const int bytes=n/8,bits=n%8;
        for(int i=0;i<32-bytes;++i){
            const int source=i+bytes;
            unsigned value=(unsigned)in[source]<<bits;
            if(bits&&source+1<32)value|=(unsigned)in[source+1]>>(8-bits);
            shifted[i]=(uint8_t)value;
        }
    }
    memcpy(out,shifted,32);
}
static void be256_keep_low(uint8_t value[32],int bits) noexcept {
    if(bits>=256)return;
    if(bits<=0){memset(value,0,32);return;}
    const int bytes=bits/8,remainder=bits%8;
    const int first=32-bytes-(remainder?1:0);
    memset(value,0,first);
    if(remainder)value[first]&=(uint8_t)((1u<<remainder)-1);
}
static void be256_extract(const uint8_t v[32], int off, int w, uint8_t out[32]) noexcept {
    be256_shr(v,off,out);
    be256_keep_low(out,w);
}
static void be256_combine(const uint8_t hi_part[32], int off,
                          const uint8_t lo_part[32], uint8_t out[32]) noexcept {
    uint8_t hi_shifted[32];
    be256_shl(hi_part, off, hi_shifted);
    for (int i = 0; i < 32; i++) out[i] = hi_shifted[i] | lo_part[i];
}

static void kxe_mix256(const uint8_t R[32], int round, const uint8_t seed[32],
                       uint8_t out32[32]) noexcept {
    uint8_t buf[32 + 1 + 32];
    memcpy(buf, seed, 32);
    buf[32] = (uint8_t)round;
    memcpy(buf + 33, R, 32);
    sha256(buf, sizeof(buf), out32);
}

static void kxe_feistel256(const uint8_t x[32], int bits, const uint8_t seed[32],
                           uint8_t out[32]) noexcept {
    int wl = bits / 2, wr = bits - wl;
    uint8_t L[32], R[32];
    be256_extract(x, wr, wl, L);
    be256_extract(x, 0, wr, R);
    for (int round = 0; round < 4; round++) {
        uint8_t f[32];
        kxe_mix256(R, round, seed, f);
        be256_keep_low(f,wl);
        uint8_t newL[32], newR[32];
        memcpy(newL, R, 32);
        for (int i = 0; i < 32; i++) newR[i] = L[i] ^ f[i];
        memcpy(L, newL, 32);
        memcpy(R, newR, 32);
        int t = wl; wl = wr; wr = t;
    }
    be256_combine(L, wr, R, out);
}

static const uint8_t kxe_seed[32] = {
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0xA5,0xA5,0xC3,0xC3
};

static void kxe_feistel_inverse256(const uint8_t x[32], int bits, const uint8_t seed[32],
                                   uint8_t out[32]) noexcept {
    int wl = bits / 2, wr = bits - wl;
    uint8_t L[32], R[32];
    be256_extract(x, wr, wl, L); be256_extract(x, 0, wr, R);
    for (int round = 3; round >= 0; --round) {
        uint8_t f[32], oldL[32], oldR[32];
        kxe_mix256(L, round, seed, f);
        // Current R is the previous round's L-sized half.
        be256_keep_low(f,wr);
        for (int i = 0; i < 32; ++i) oldL[i] = R[i] ^ f[i];
        memcpy(oldR, L, 32); memcpy(L, oldL, 32); memcpy(R, oldR, 32);
        std::swap(wl, wr);
    }
    be256_combine(L, wr, R, out);
}

// ceil(log2(total)) = bit_length(total-1). Determine it directly from bytes;
// incrementing a 256-bit power of two one bit at a time cost tens of thousands
// of bit operations for EVERY block in a wide search range.
static int kxe_domain_bits(const uint8_t total[32]) noexcept {
    uint8_t one[32]{};one[31]=1;
    if(be256_cmp(total,one)<=0)return 0;
    uint8_t maximum[32];be256_sub(total,one,maximum);
    for(int i=0;i<32;++i)if(maximum[i]){
        int high=0;for(unsigned byte=maximum[i];byte;byte>>=1)++high;
        return (31-i)*8+high;
    }
    return 0;
}
static bool inverse_permuted_block_precedes(const uint8_t block[32], const ChunkIdx &seq,
                                            const uint8_t seed[32]) {
    uint8_t one[32]{}; one[31] = 1;
    uint8_t zero[32]{};
    if (be256_cmp(g_total_blocks256, one) <= 0) return be256_cmp(seq.b, zero) > 0;
    uint8_t x[32]; memcpy(x, block, 32);
    const int bits=kxe_domain_bits(g_total_blocks256);
    // Inverse cycle walking finds the unique ordinal of the short tail block.
    // Same finite guard as the forward permutation; no search work is counted.
    for (int attempt = 0; attempt < 100000; ++attempt) {
        uint8_t next[32]; kxe_feistel_inverse256(x, bits, seed, next); memcpy(x, next, 32);
        if (be256_cmp(x, g_total_blocks256) < 0) return be256_cmp(x, seq.b) < 0;
    }
    std::fprintf(stderr, "Фатально: обратная перестановка KXE не сошлась.\n");
    std::abort();
}

static void kxe_permute_in_range256(const uint8_t counter[32], const uint8_t total_blocks[32],
                                    const uint8_t seed[32], uint8_t out[32]) noexcept {
    uint8_t one[32] = {}; one[31] = 1;
    if (be256_cmp(total_blocks, one) <= 0) { memset(out, 0, 32); return; }
    const int bits=kxe_domain_bits(total_blocks);
    uint8_t x[32]; memcpy(x, counter, 32);
    for (int iter = 0; iter < 100000; iter++) {
        uint8_t nx[32];
        kxe_feistel256(x, bits, seed, nx);
        memcpy(x, nx, 32);
        if (be256_cmp(x, total_blocks) < 0) { memcpy(out, x, 32); return; }
    }
    std::fprintf(stderr,
        "Фатально: cycle-walking KXE не сошёлся за 100000 попыток "
        "-- вероятна ошибка в реализации перестановки.\n");
    std::abort();
}

// Сколько блоков по 2^chunk_bits ключей помещается в [g_range_start,
// g_range_end]. Полный диапазон 2^256 даёт максимум порядка 2^246 блоков
// при chunk_bits=10 -- свободно умещается в 32 байта.
static void kxe_total_blocks256(uint8_t out[32]) noexcept {
    const int bits = traversal_block_bits();
    if (mask_active()) {
        uint8_t candidates[32]; u256_to_be32(candidates, g_mask_total);
        be256_shr(candidates, bits, out);
        if (g_mask_total[0] & ((1ULL << bits) - 1)) {
            uint8_t one[32]{}; one[31] = 1; be256_add(out, one);
        }
        return;
    }
    uint8_t start[32], end[32], width[32];
    u256_to_be32(start, g_range_start);
    u256_to_be32(end, g_range_end);
    if (be256_cmp(start, end) > 0) { memset(out, 0, 32); return; }
    be256_sub(end, start, width);
    // floor((end-start)/block)+1 is the exact number of closed-range blocks.
    // Unlike rounding up (end-start+1), neither addition can wrap at 2^256.
    uint8_t one[32] = {}; one[31] = 1;
    be256_shr(width, bits, out);
    be256_add(out, one);
}

void compute_total_blocks256() {
    if (unique_blocks_mode()) {
        const auto &total = restored_shape_plan().complete_total();
        std::memcpy(g_total_blocks256, total.data(), 32);
        g_total_chunks = 0;
        bool small = true;
        for (int i = 0; i < 24; ++i) if (total[(size_t)i]) { small = false; break; }
        if (small) for (int i = 24; i < 32; ++i) g_total_chunks = (g_total_chunks << 8) | total[(size_t)i];
        return;
    }
    kxe_total_blocks256(g_total_blocks256);
}


// block_idx (256 бит) -> начало и размер участка. Тот же расчёт, что в
// chunk_bounds, но индекс блока не ограничен 64 битами.
static bool chunk_bounds_from_block256(const uint8_t block[32], u256 out_start, uint64_t &out_count) {
    const int bits = traversal_block_bits();
    uint8_t start[32], end[32], offset[32], key_start[32];
    u256_to_be32(start, g_range_start);
    u256_to_be32(end, g_range_end);
    be256_shl(block, bits, offset);
    memcpy(key_start, start, 32);
    be256_add(key_start, offset);
    if (be256_cmp(key_start, end) > 0) return false;

    // Конвертация be32 -> u256 через уже проверенную fe_from_be32 (тот же
    // формат, что у u256).
    fe_from_be32(out_start, key_start);

    // ХВОСТ СЧИТАЕТСЯ ПО ВСЕЙ ШИРИНЕ, А НЕ ПО МЛАДШИМ 32 БИТАМ.
    //
    // Было: остаток признавался «маленьким» только если старшие 28 БАЙТ
    // нулевые, то есть width < 2^32. При chunk_bits > 32 остаток спокойно
    // бывает больше 2^32 и при этом МЕНЬШЕ участка -- тогда обрезка не
    // срабатывала вовсе и последний блок выезжал за g_range_end.
    //
    // Замер на исходном коде: диапазон [2^33, 2^37), участок 2^36 --
    // последний блок заказывал 2^36 ключей при остатке 2^36 - 2^33, то есть
    // 8 589 934 592 ключа проверялось ЗА концом диапазона. В
    // последовательном режиме на той же конфигурации выхода нет: там
    // chunk_bounds сравнивает полные лимбы. Два пути нарезки расходились --
    // ровно та же болезнь, что в прошлый раз была у двух backend'ов.
    //
    // Теперь остаток сравнивается с размером участка целиком, все 32 байта.
    uint8_t width[32]; be256_sub(end, key_start, width);
    uint64_t chunk = 1ULL << bits;
    out_count = chunk;
    bool small = true;
    for (int i = 0; i < 24; i++) if (width[i]) { small = false; break; }
    if (small) {
        uint64_t w64 = 0;
        for (int i = 24; i < 32; i++) w64 = (w64 << 8) | width[i];
        // w64 == 2^64-1 значит остаток заведомо шире любого участка (<= 2^40):
        // прибавлять единицу нельзя -- переполнится в ноль и участок станет
        // пустым. Та же ловушка уже ловилась в chunk_bounds.
        if (w64 != ~0ULL && w64 + 1 < chunk) out_count = w64 + 1;
    }
    return true;
}

// A per-thread immutable snapshot avoids a global lock on each block. Settings
// are fixed while workers run; a new run/restore invalidates the snapshot by
// bounds, total, mode, block size, persisted seed or explicit wave anchor.
struct UniqueBlocksPlan {
    bool ready = false;
    int mode = 0, bits = 0;
    uint64_t seed_value = 0;
    std::string anchor_text;
    uint8_t lo[32]{}, hi[32]{}, total[32]{}, seed[32]{}, rotation[32]{};
};
static const UniqueBlocksPlan &unique_blocks_plan() {
    static thread_local UniqueBlocksPlan plan;
    uint8_t lo[32], hi[32];
    u256_to_be32(lo, g_range_start); u256_to_be32(hi, g_range_end);
    const int bits = traversal_block_bits();
    const uint64_t seed = wave_get_seed();
    const std::string anchor_text = g_cfg.mode == TRAV_WAVE ? g_cfg.wave_start : std::string();
    if (plan.ready && plan.mode == g_cfg.mode && plan.bits == bits && plan.seed_value == seed
        && plan.anchor_text == anchor_text && !memcmp(plan.lo,lo,32) && !memcmp(plan.hi,hi,32)
        && !memcmp(plan.total,g_total_blocks256,32)) return plan;
    plan = UniqueBlocksPlan{};
    plan.mode=g_cfg.mode;plan.bits=bits;plan.seed_value=seed;plan.anchor_text=anchor_text;
    memcpy(plan.lo,lo,32);memcpy(plan.hi,hi,32);memcpy(plan.total,g_total_blocks256,32);
    uint8_t input[48]{};
    static const char domain[]="PKH_UNIQUE_BLOCKS_V1";
    memcpy(input,domain,sizeof(domain)-1);
    input[24]=(uint8_t)g_cfg.mode;input[25]=(uint8_t)bits;
    for(int b=0;b<8;++b)input[32+b]=(uint8_t)(seed>>(8*b));
    sha256(input,sizeof(input),plan.seed);
    if (!anchor_text.empty()) {
        u256 anchor;
        if (hex_to_u256(anchor_text.c_str(),anchor) && u256_cmp(anchor,g_range_start)>=0
            && u256_cmp(anchor,g_range_end)<=0) {
            uint8_t key[32],difference[32],anchor_block[32],first[32],zero[32]{};
            u256_to_be32(key,anchor);be256_sub(key,lo,difference);be256_shr(difference,bits,anchor_block);
            kxe_permute_in_range256(zero,plan.total,plan.seed,first);
            if(be256_cmp(anchor_block,first)>=0)be256_sub(anchor_block,first,plan.rotation);
            else {uint8_t gap[32];be256_sub(first,anchor_block,gap);be256_sub(plan.total,gap,plan.rotation);}
        }
    }
    plan.ready=true;
    return plan;
}
static void rotate_unique_block(uint8_t block[32],const UniqueBlocksPlan &plan) {
    uint8_t threshold[32];be256_sub(plan.total,plan.rotation,threshold);
    if(be256_cmp(block,threshold)>=0){uint8_t rotated[32];be256_sub(block,threshold,rotated);memcpy(block,rotated,32);}
    else be256_add(block,plan.rotation);
}
bool permuted_tail_precedes(const ChunkIdx &seq) {
    uint8_t one[32]{};one[31]=1;
    if(!unique_blocks_mode() && g_cfg.mode!=TRAV_KXE)return false;
    const uint8_t zero[32]{};if(be256_cmp(g_total_blocks256,zero)==0)return false;
    uint8_t tail[32];be256_sub(g_total_blocks256,one,tail);
    if(g_cfg.mode==TRAV_KXE)return inverse_permuted_block_precedes(tail,seq,kxe_seed);
    const auto &plan=unique_blocks_plan();
    // Undo the output rotation before inverse cycle walking.
    if(be256_cmp(tail,plan.rotation)>=0){uint8_t original[32];be256_sub(tail,plan.rotation,original);memcpy(tail,original,32);}
    else {uint8_t gap[32],original[32];be256_sub(plan.rotation,tail,gap);be256_sub(plan.total,gap,original);memcpy(tail,original,32);}
    return inverse_permuted_block_precedes(tail,seq,plan.seed);
}
bool kxe_tail_precedes(const ChunkIdx &seq) { return permuted_tail_precedes(seq); }

uint64_t kxe_permute(uint64_t x, uint64_t total, uint64_t seed) {
    // Оставлена только для теста покрытия (selftest.cpp сравнивает KXE с
    // детерминированным обходом на маленьком домене, где u64 достаточно).
    // Реальный горячий путь -- next_chunk() ниже -- использует 256-битную
    // версию через kxe_permute_in_range256.
    static const auto kxe_mix = [](uint64_t v) -> uint64_t {
        v ^= v >> 33; v *= 0xFF51AFD7ED558CCDULL;
        v ^= v >> 33; v *= 0xC4CEB9FE1A85EC53ULL;
        v ^= v >> 33;
        return v;
    };
    if (total <= 1) return 0;
    int bits = 1;
    while (bits < 63 && (1ULL << bits) < total) bits++;
    const int half = (bits + 1) / 2;
    const uint64_t mask = (half >= 64) ? ~0ULL : ((1ULL << half) - 1);

    for (int guard = 0; guard < 64; guard++) {
        uint64_t l = x & mask, r = (x >> half) & mask;
        for (int round = 0; round < 4; round++) {
            uint64_t f = kxe_mix(r ^ seed ^ (0x9E3779B97F4A7C15ULL * (round + 1))) & mask;
            uint64_t nl = r, nr = l ^ f;
            l = nl; r = nr;
        }
        uint64_t y = (r << half) | l;
        if (y < total) return y;
        x = y;
    }
    return x % total;
}

// ------------------- приведение смещения в диапазон -------------------------
//
// РАВНОМЕРНОСТЬ ПО ВСЕЙ ШИРИНЕ, А НЕ ПО СТАРШЕМУ ЛИМБУ.
//
// Прежняя версия брала старший ненулевой лимб ширины, приводила по нему
// остаток, обнуляла лимбы выше -- и НЕ ТРОГАЛА младшие. Для ширины вида
// 2^k - 1, где k кратно 64, это случайно совпадало с правильным ответом,
// на чём и держалось: диапазоны в опытах были ровно такие. Стоит взять
// произвольный отрезок -- скажем, [2^128 + c, 3*2^128 + c] -- и смещение
// спокойно выходит за ширину, после чего clamp_chunk_to_range режет
// участок или выбрасывает целиком.
//
// Последствия ровно те, ради борьбы с которыми случайный режим и нужен:
// одни области получают больше попыток, другие меньше, часть работы уходит
// в обрезанные куски. Со стороны это неотличимо от невезения.
//
// Правильный приём -- выборка с отбрасыванием: берём столько случайных бит,
// сколько их в ширине, и повторяем, пока значение не окажется в пределах.
// Каждая попытка удачна с вероятностью больше половины (ширина занимает не
// меньше половины своего разрядного отрезка), поэтому в среднем уходит
// меньше двух попыток независимо от диапазона. Распределение при этом
// строго равномерное -- в отличие от любой схемы с остатком, которая всегда
// перекошена в пользу младших значений.
static int u256_bitlen(const u256 a) {
    for (int i = 3; i >= 0; i--) {
        if (!a[i]) continue;
        int b = 63;
        while (b > 0 && !((a[i] >> b) & 1ULL)) b--;
        return i * 64 + b + 1;
    }
    return 0;
}

static void u256_mask_bits(u256 a, int bits) {
    for (int i = 0; i < 4; i++) {
        int lo = i * 64;
        if (bits <= lo) { a[i] = 0; continue; }
        int n = bits - lo;
        if (n < 64) a[i] &= (1ULL << n) - 1;
    }
}

// Равномерное значение в [0, span] включительно.
template <typename Gen>
static void random_into_span(u256 off, const u256 span, Gen &&next64) {
    const int bits = u256_bitlen(span);
    if (bits == 0) { u256_zero(off); return; }       // диапазон из одного ключа
    // Отбрасывание. Предел стоит не ради выхода из бесконечного цикла --
    // при вероятности удачи выше 1/2 сто попыток подряд неудачными не
    // бывают, -- а чтобы ошибка в u256_bitlen не превратилась в зависание.
    for (int guard = 0; guard < 100; guard++) {
        for (int i = 0; i < 4; i++) off[i] = next64();
        u256_mask_bits(off, bits);
        if (u256_cmp(off, span) <= 0) return;
    }
    u256_zero(off);
}

// -------------------- reproducible reference zone traversal -----------------
// Production uses ZoneSearch's committed state. This ordinal adapter exists
// for independent traversal tests and ordinary next_chunk callers; it replays
// the exact same walk instead of retaining the removed +1-all-digits mode.
static uint8_t g_zone_seed[32]{};
static bool g_zone_seed_ready = false;
static uint64_t g_zone_seed_revision = 0;
static uint64_t g_wave_seed = 0;
static bool g_wave_seed_ready = false;

void wave_set_seed(uint64_t s) { g_wave_seed = s; g_wave_seed_ready = true; }
bool wave_seed_initialized() { return g_wave_seed_ready; }
uint64_t wave_get_seed() { return g_wave_seed; }
void wave_seed_bytes(uint8_t out[32]) {
    for (int i = 0; i < 32; ++i) out[i] = (uint8_t)(g_wave_seed >> ((i % 8) * 8));
}

// The only production plan for the restored two modes. A thread-local immutable
// copy keeps the four preparation helpers independent and makes ordinal lookup
// reproducible after temporary HEX-digit rotation. No setting is written here.
static const balanced_shaped::Plan &restored_shape_plan() {
    struct Cache {
        balanced_shaped::Plan plan;
        whole_shaped::Word lo{}, hi{};
        std::string anchor;
        uint64_t seed = 0;
        int mode = -1, low = -1, top = -1;
        bool ready = false;
    };
    static thread_local Cache cache;
    whole_shaped::Word lo{}, hi{};
    u256_to_be32(lo.data(), g_range_start); u256_to_be32(hi.data(), g_range_end);
    const int low = g_cfg.mode == TRAV_ZONE ? zone_block_nibbles() : g_skip_low;
    const uint64_t seed = wave_get_seed();
    const std::string anchor_text = g_cfg.mode == TRAV_WAVE ? g_cfg.wave_start : std::string();
    if (cache.ready && cache.lo == lo && cache.hi == hi && cache.mode == g_cfg.mode
        && cache.low == low && cache.top == g_top_ni && cache.seed == seed && cache.anchor == anchor_text)
        return cache.plan;
    whole_shaped::Config cfg;
    cfg.lower = lo; cfg.upper = hi; cfg.low_nibbles = (unsigned)low;
    cfg.top_index = (unsigned)g_top_ni;
    cfg.kind = g_cfg.mode == TRAV_ZONE ? whole_shaped::Kind::Zone : whole_shaped::Kind::Wave;
    cfg.fixed_wave = g_cfg.mode == TRAV_WAVE && !anchor_text.empty(); cfg.seed = seed;
    u256 anchor;
    if (cfg.fixed_wave) {
        // compute_range also runs before the menu validates a remembered start.
        // Its count remains defined; the UI rejects an invalid start before work.
        if (!hex_to_u256(anchor_text.c_str(), anchor)
            || u256_cmp(anchor, g_range_start) < 0 || u256_cmp(anchor, g_range_end) > 0)
            u256_set(anchor, g_range_start);
    } else {
        u256 width, offset;
        u256_sub(width, g_range_end, g_range_start);
        uint64_t state = seed ^ (g_cfg.mode == TRAV_ZONE ? UINT64_C(0x5a4f4e455f5631) : UINT64_C(0x574156455f5631));
        random_into_span(offset, width, [&]() {
            state += UINT64_C(0x9e3779b97f4a7c15);
            uint64_t z = state;
            z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
            z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
            return z ^ (z >> 31);
        });
        u256_add(anchor, g_range_start, offset);
    }
    u256_to_be32(cfg.anchor.data(), anchor);
    if (!cache.plan.init(cfg)) throw std::runtime_error("Не удалось построить исходный узор обхода без повторов");
    cache.lo = lo; cache.hi = hi; cache.mode = g_cfg.mode; cache.low = low;
    cache.top = g_top_ni; cache.seed = seed; cache.anchor = anchor_text; cache.ready = true;
    return cache.plan;
}

bool restored_shape_block(const ChunkIdx &idx, u256 out_key, uint64_t &out_count,
                          int &zone_out, WaveInfo *wave, ZoneInfo *zone) {
    whole_shaped::Word ordinal{}; std::memcpy(ordinal.data(), idx.b, 32);
    whole_shaped::Result result;
    const auto& plan = restored_shape_plan();
    unsigned bit_length = 0, top_index = (unsigned)g_top_ni;
    if (!plan.get_complete(ordinal, result, &bit_length, &top_index)) { out_count = 0; return false; }
    const int shape_top = top_index < 64 ? (int)top_index : g_top_ni;
    fe_from_be32(out_key, result.start.data()); out_count = result.count;
    zone_out = (int)result.phase;
    if (wave) {
        *wave = WaveInfo{};
        wave->phase = (int)result.phase; wave->step = (int)result.step;
        wave->steps = result.phase ? 15 : 0; wave->width = (int)result.phase;
        wave->pass = 1; wave->high = false; wave->top = 63 - shape_top;
        fe_from_be32(wave->start, result.center.data());
        fe_from_be32(wave->base, result.phase_base.data());
    }
    if (zone) {
        *zone = ZoneInfo{}; zone->valid = result.count != 0;
        zone->skip = zone_block_nibbles(); zone->block_keys = zone_block_keys(); zone->count = result.count;
        zone->pos = result.phase ? shape_top + (int)result.phase : -1;
        zone->zone = (int)result.phase;
        zone->value = zone->pos < 0 ? -1 : (zone->pos % 2
            ? result.start[(size_t)zone->pos / 2] & 15 : result.start[(size_t)zone->pos / 2] >> 4);
        fe_from_be32(zone->base, result.center.data()); fe_from_be32(zone->raw, result.start.data());
    }
    return true;
}

void zone_set_seed(uint64_t s) {
    for (int i = 0; i < 32; ++i) g_zone_seed[i] = (uint8_t)(s >> ((i % 8) * 8));
    g_zone_seed_ready = true;
    ++g_zone_seed_revision;
}

void zone_block_key(const ChunkIdx &idx, u256 out_key, ZoneInfo &info) {
    info = ZoneInfo{};
    u256_zero(out_key);
    // BitPazl checkpoints a 64-bit sequence and stops on exhaustion; silently
    // truncating a wider ordinal would repeat a different state.
    for (int i = 0; i < 24; ++i) if (idx.b[i]) return;
    uint64_t ordinal = 0;
    for (int i = 24; i < 32; ++i) ordinal = (ordinal << 8) | idx.b[i];
    if (ordinal == UINT64_MAX) return;
    if (!g_zone_seed_ready) zone_set_seed(0x5DEECE66DULL);
    struct Cache {
        pkh_zone::ZoneTraversal walk;
        interval_digits::Key lo{}, hi{};
        uint64_t seed_revision = 0;
        int nibbles = -1;
        bool initialized = false;
    };
    static thread_local Cache cache;
    interval_digits::Key lo{}, hi{};
    u256_to_be32(lo.data(), g_range_start); u256_to_be32(hi.data(), g_range_end);
    if (!cache.initialized || cache.seed_revision != g_zone_seed_revision
        || cache.nibbles != g_cfg.zone_nibbles || cache.lo != lo || cache.hi != hi
        || (cache.walk.sequence && ordinal < cache.walk.sequence - 1)
        || (cache.walk.exhausted && ordinal < cache.walk.sequence)) {
        zone_walk_init(cache.walk, g_zone_seed);
        cache.lo = lo; cache.hi = hi;
        cache.nibbles = g_cfg.zone_nibbles;
        cache.seed_revision = g_zone_seed_revision;
        cache.initialized = true;
    }
    if (cache.walk.exhausted) return;
    while (!cache.walk.started || cache.walk.sequence - 1 < ordinal)
        if (!cache.walk.advance()) return;
    pkh_zone::be256 first, end;
    pkh_zone::ZoneInfo current;
    const uint64_t count = cache.walk.bounds(first, end, current);
    if (!count) return;
    fe_from_be32(out_key, first.b);
    info.valid = true; info.skip = cache.walk.skip;
    info.block_keys = cache.walk.block_keys(); info.count = count;
    info.ordinal = current.ordinal; info.pos = info.zone = current.position;
    info.value = current.value;
    fe_from_be32(info.base, current.base.b); fe_from_be32(info.raw, current.raw.b);
}

// ============================================================================
//  ВОЛНОВОЙ РЕЖИМ
//
//  Состояний на старт: 1 + 64*15 = 961.
//    состояние 0        -- сам ключ старта;
//    состояние s >= 1   -- фаза p = (s-1)/15 + 1, шаг t = (s-1)%15 + 1.
//
//  Ключ считается прямо по номеру, без прокрутки: разряд i (0 -- младший)
//  активен начиная с фазы i+1, за каждую завершённую фазу набрал +15, в
//  текущей набирает +t. Всё по модулю 16, переноса между разрядами НЕТ.
// ============================================================================
// ВОЛНА НАЧИНАЕТСЯ ВЫШЕ БЛОКА, А НЕ С САМОГО МЛАДШЕГО РАЗРЯДА.
//
// Блок -- это 4096 ключей, то есть ровно три младших разряда: он их и так
// перебирает целиком. Если пустить волну с разряда 0, её первые три фазы
// дадут ключи, уже лежащие внутри блока старта -- 45 состояний из 961
// впустую, и в журнале это выглядит работой.
//
// Волна начинается выше её собственного блока. Зонная цепочка хранит
// независимый размер блока в zone_nibbles и не использует таблицы волны.
// ============================================================================
//  ЧЕЛНОК И ЗЕРКАЛО: ЧЕТЫРЕ ХОДА НА ОДНУ ТОЧКУ СТАРТА
//
//  Было: волна росла от младшего разряда к старшему, доходила до всех 61 --
//  и старт на этом кончался, дальше бралась новая точка. Один проход в одну
//  сторону на каждую точку.
//
//  Стало четыре хода подряд, без смены точки:
//
//    ход 1  окно РАСТЁТ от младшего конца    ширина 1..61   фазы   1..61
//    ход 2  окно ЖМЁТСЯ к старшему концу      ширина 60..1   фазы  62..121
//    ход 3  окно РАСТЁТ от старшего конца     ширина 2..61   фазы 122..181
//    ход 4  окно ЖМЁТСЯ к младшему концу      ширина 60..1   фазы 182..241
//
//  Это один замкнутый круг: окно выходит узким у младшего конца, разрастается
//  на весь ключ, сползает к старшему, снова разрастается и возвращается
//  обратно. В->А и А->В, как челнок.
//
//  ГЛАВНОЕ ЗДЕСЬ -- К КАКОМУ КРАЮ ПРИЖАТО ОКНО НА ОБРАТНОМ ХОДУ.
//
//  Сперва было сделано иначе: сужаясь, окно оставалось прижатым к МЛАДШЕМУ
//  концу, то есть скобки снимались со старшего разряда. Фронт при этом не
//  возвращался, а пятился хвостом вперёд, и после полной ширины 61 всё
//  начиналось заново с той же стороны. Волна дошла до А -- значит отступать
//  она должна, оставаясь у А, а освобождаться должны МЛАДШИЕ разряды.
//  Разница видна в журнале сразу: у правильного хода 62 отпущенный разряд
//  стоит перед тремя нулями, а не в самом начале строки.
//
//  Обратный ход (ход 3) начинается с ширины 2, а не 1: ширина 1 у старшего
//  конца только что была концом хода 2, и повтор подряд не дал бы ничего
//  нового. Та же причина, по которой ход 2 начинается с 60, а не с 61.
//
//  Состояний на старт: 1 + 241*15 = 3616 против прежних 916, и все РАЗНЫЕ --
//  проверено прокруткой всех состояний, повторов ноль.
// ============================================================================
// ЧИСЛА ЗДЕСЬ ЗАВИСЯТ ОТ РАЗМЕРА БЛОКА, поэтому это функции, а не константы.
// При блоке в три ниббла выходят прежние 61 разряд, 241 фаза и 3616 состояний
// на старт; при блоке в восемь -- 56, 221 и 3316. Всё остальное устройство
// волны -- челнок, зеркало, ширины ходов -- не меняется ни на йоту.
static int g_wave_top=63, g_wave_min=0, g_wave_max=15;
static uint64_t g_wave_phase_end[WAVE_PHASES_MAX+2] = {};
static int g_wave_phase_steps[WAVE_PHASES_MAX+2] = {};
static u256 g_wave_range_start={}, g_wave_range_end={};
static int g_wave_skip_cached=-1;
static inline int wave_pos()    { return std::max(0,g_wave_top-g_skip_low+1); }
static inline int wave_ph1()    { return wave_pos(); }          // окно растёт от младшего
static inline int wave_ph234()  { return std::max(0,wave_pos() - 1); }
static inline int wave_phases() { return wave_pos() ? wave_ph1() + 3 * wave_ph234() : 0; }
static inline uint64_t wave_per_start() {
    return 1 + g_wave_phase_end[wave_phases()];
}

// Ширина фронта и с какого конца он отсчитывается -- по номеру фазы.
static void wave_phase_shape(int q, int &w, bool &high) {
    const int A = wave_ph1();            // конец хода 1  = 61 при блоке в 3 ниббла
    const int B = A + wave_ph234();      // конец хода 2  = 121
    const int C = B + wave_ph234();      // конец хода 3  = 181
    if      (q <= A) { w = q;                 high = false; }   // 1..61  у младшего
    else if (q <= B) { w = B + 1 - q;         high = true;  }   // 60..1  у старшего
    else if (q <= C) { w = q - B + 1;         high = true;  }   // 2..61  у старшего
    else             { w = C + wave_ph234() + 1 - q; high = false; } // 60..1 у младшего
}

// НАКОПЛЕНИЯ СЧИТАЮТСЯ ЧЕСТНОЙ ПРОКРУТКОЙ ОДИН РАЗ, А НЕ ФОРМУЛОЙ КАЖДЫЙ РАЗ.
//
// Пока ход был один, закрытая формула умещалась в строку: разряд j работал во
// всех фазах старше j, значит накопил 15*(p-1-j). С четырьмя ходами она
// распадается на четыре случая с краями, и проверять её пришлось бы глазами
// -- а это ровно тот способ, которым в этот код уже дважды попадала тихая
// ошибка.
//
// Поэтому таблица строится прокруткой всех фаз при запуске: 44 КБ памяти и
// доли миллисекунды, зато сомневаться не в чем. В горячем цикле остаётся
// выборка из таблицы -- десятки действий на целый блок.
//
// Отведена таблица по МАКСИМУМУ (блок в три ниббла), а заполняется столько,
// сколько фаз и разрядов даёт нынешний размер блока. Так set_block_nibbles()
// может пересобрать её на месте, ничего не перевыделяя.
static uint16_t WACC[WAVE_PHASES_MAX + 2][WAVE_POS_MAX];  // накоплено ДО фазы q
static uint8_t  WACT[WAVE_PHASES_MAX + 2][WAVE_POS_MAX];  // работает ли разряд в фазе q
static std::mutex g_wave_tables_mutex;
static bool     g_wave_tables_ready = false;

// Keep the legacy ordinal space: only mark a later copy of the same block.
// A signature is the exact modular translation of every changing nibble.
// For a valid start, equal signatures imply equal 256-bit block keys.
static std::vector<uint32_t> g_wave_first_ordinal;

static void wave_build_duplicates() {
    g_wave_first_ordinal.clear();
    try {
        const uint64_t states = wave_per_start();
        // At most 241 phases, each with lcm(16, radix<=16)-1 <=239 steps.
        if (states > 1ULL + WAVE_PHASES_MAX * 239ULL) return;
        std::vector<uint32_t> first((size_t)states);
        using Signature = std::array<uint8_t, WAVE_POS_MAX>;
        std::map<Signature, uint32_t> seen;
        Signature signature{};
        seen.emplace(signature, 0);
        first[0] = 0;
        for (int q = 1; q <= wave_phases(); ++q) {
            for (int t = 1; t <= g_wave_phase_steps[q]; ++t) {
                for (int j = 0; j < wave_pos(); ++j) {
                    const int radix = j + g_skip_low == g_wave_top
                        ? g_wave_max - g_wave_min + 1 : 16;
                    signature[j] = (uint8_t)((WACC[q][j] + (WACT[q][j] ? t : 0)) % radix);
                }
                const uint32_t ordinal = (uint32_t)(g_wave_phase_end[q-1] + t);
                first[ordinal] = seen.emplace(signature, ordinal).first->second;
            }
        }
        g_wave_first_ordinal.swap(first); // publish only the complete table
    } catch (const std::bad_alloc &) {
        // Optional optimization: memory pressure retains the legacy walk.
        g_wave_first_ordinal.clear();
    }
}

static void wave_tables_build() {
    auto digit=[](const u256 k,int nib) { return (int)((k[nib/16] >> (4*(nib%16)))&15); };
    g_wave_top=63;
    while (g_wave_top>=0 && digit(g_range_start,g_wave_top)==digit(g_range_end,g_wave_top)) --g_wave_top;
    g_wave_min=g_wave_top>=0 ? digit(g_range_start,g_wave_top) : 0;
    g_wave_max=g_wave_top>=0 ? digit(g_range_end,g_wave_top) : 0;
    const int NP = wave_pos(), NQ = wave_phases();
    memset(WACC, 0, sizeof(WACC));
    memset(WACT, 0, sizeof(WACT));
    memset(g_wave_phase_end,0,sizeof(g_wave_phase_end));
    memset(g_wave_phase_steps,0,sizeof(g_wave_phase_steps));
    uint16_t acc[WAVE_POS_MAX] = {0};
    for (int q = 1; q <= NQ; q++) {
        int w; bool high;
        wave_phase_shape(q, w, high);
        int period=1;
        for (int j = 0; j < NP; j++) {
            WACC[q][j] = acc[j];
            WACT[q][j] = (uint8_t)(high ? (j >= NP - w) : (j < w));
            if (WACT[q][j]) {
                const int radix=j+g_skip_low==g_wave_top ? g_wave_max-g_wave_min+1 : 16;
                int a=period,b=radix;
                while(b) { int r=a%b; a=b; b=r; }
                period=period/a*radix;
            }
        }
        g_wave_phase_steps[q]=period-1;
        g_wave_phase_end[q]=g_wave_phase_end[q-1]+g_wave_phase_steps[q];
        for (int j = 0; j < NP; j++)
            if (WACT[q][j]) acc[j] = (uint16_t)(acc[j] + g_wave_phase_steps[q]);
    }
    wave_build_duplicates();
    u256_set(g_wave_range_start,g_range_start); u256_set(g_wave_range_end,g_range_end);
    g_wave_skip_cached=g_skip_low;
    g_wave_tables_ready = true;
}

// Если размер блока никто не ставил, таблицы всё равно должны быть готовы к
// первому обращению: волновой режим можно выбрать и без всякой карты.
static inline void wave_tables_need() {
    // call_once, а не голый bool: wave_block_key зовут все рабочие потоки, а
    // wave_tables_build делает memset двух общих таблиц по 44 КБ. Пока один
    // поток их обнуляет, другой читает WACC и получает нули -- ключ блока
    // совпадает с ключом старта, и потоки перебирают одно и то же место,
    // тогда как журнал показывает разные фазы.
    //
    // Сегодня путь недостижим: меню строит таблицы раньше, однопоточно. Но
    // защита держалась на порядке вызовов в другом файле, а не на самой
    // ленивой инициализации -- ровно так же когда-то строилась таблица COMB.
    std::lock_guard<std::mutex> lock(g_wave_tables_mutex);
    if (!g_wave_tables_ready || g_wave_skip_cached!=g_skip_low
        || u256_cmp(g_wave_range_start,g_range_start)!=0 || u256_cmp(g_wave_range_end,g_range_end)!=0)
        wave_tables_build();
}

// Откуда волна начинает. Пусто в настройках -- случайная точка диапазона,
// иначе заданная строка. Разница по существу описана у Config::wave_start.
static void wave_start_key(const uint8_t quot[32], uint8_t out[32]) {
    if (!g_cfg.wave_start.empty()) {
        u256 k;
        if (hex_to_u256(g_cfg.wave_start.c_str(), k)) {
            // Заданная точка -- одна на все старты: узор проверяется именно
            // от неё, и менять её от старта к старту значило бы вернуться к
            // случайному блужданию.
            u256_to_be32(out, k);
            align_to_block(out);
            return;
        }
        // Строка негодная -- падать не будем, уйдём в случайный старт; о
        // непонятой настройке скажет меню.
    }
    // СЛУЧАЙНЫЙ СТАРТ ВЫВОДИТСЯ ИЗ ПОЛНОГО НОМЕРА, А НЕ ИЗ ЕГО ХВОСТА.
    //
    // Сначала номер старта усекался до младших 64 бит: частное от деления
    // 256-битного номера участка бралось только последними восемью байтами.
    // Через 916*2^64 участков оно обнулилось бы, и старты пошли бы по второму
    // кругу. Это 2^85.8 ключей -- недостижимо, но «недостижимо» и «верно» не
    // одно и то же, а стоит правильный вариант ровно столько же.
    //
    // Берём хеш от зерна и ВСЕХ 32 байт частного.
    uint8_t buf[64];
    wave_seed_bytes(buf);                    // 32 байта зерна
    memcpy(buf + 32, quot, 32);
    sha256(buf, sizeof(buf), out);

    // Приводим в диапазон и выравниваем по блоку -- как у зонального.
    u256 r, span, off;
    fe_from_be32(r, out);
    u256_sub(span, g_range_end, g_range_start);
    {
        uint32_t attempt = 0;
        uint8_t pool[32]; int left = 0;
        auto next64 = [&]() -> uint64_t {
            if (left == 0) {
                uint8_t b2[68];
                memcpy(b2, buf, 64);
                for (int i = 0; i < 4; i++) b2[64 + i] = (uint8_t)(attempt >> (8 * i));
                sha256(b2, sizeof(b2), pool);
                ++attempt; left = 4;
            }
            int i = --left;
            uint64_t v = 0;
            for (int b = 0; b < 8; b++) v = (v << 8) | pool[i * 8 + b];
            return v;
        };
        random_into_span(off, span, next64);
    }
    u128 c = 0;
    u256 kk;
    for (int i = 0; i < 4; i++) {
        c += (u128)g_range_start[i] + off[i];
        kk[i] = (uint64_t)c; c >>= 64;
    }
    u256_to_be32(out, kk);
    align_to_block(out);                     // выравниваем по началу блока
    // Keep the partial block at L. clamp_chunk_to_range trims its invalid
    // prefix; rounding up would remove valid low keys from possible starts.
}

uint64_t wave_states_per_start() { wave_tables_need(); return wave_per_start(); }

// Isolated packed benchmark exports. The plan contains the same retained
// ordinals as wave_block_key for valid in-range starts, in legacy order.
// Each row is a nibble translation indexed from the left of the hex key;
// fixed leading positions and the low block positions remain zero.
extern "C" uint64_t packed_wave_translation_plan(uint8_t *deltas64,
                                                uint64_t *ordinals,
                                                uint64_t capacity) {
    wave_tables_need();
    const uint64_t states = wave_per_start();
    auto retained = [](uint64_t ordinal) {
        return ordinal >= g_wave_first_ordinal.size()
            || g_wave_first_ordinal[(size_t)ordinal] == ordinal;
    };
    uint64_t required = 0;
    for (uint64_t ordinal = 0; ordinal < states; ++ordinal)
        if (retained(ordinal)) ++required;
    // A size query or undersized request never modifies either output.
    if (!deltas64 || !ordinals || capacity < required) return required;

    uint64_t row = 0;
    std::memset(deltas64, 0, 64);
    ordinals[row++] = 0;
    for (int q = 1; q <= wave_phases(); ++q) {
        for (int t = 1; t <= g_wave_phase_steps[q]; ++t) {
            const uint64_t ordinal = g_wave_phase_end[q - 1] + t;
            if (!retained(ordinal)) continue;
            uint8_t *delta = deltas64 + (size_t)row * 64;
            std::memset(delta, 0, 64);
            for (int j = 0; j < wave_pos(); ++j) {
                const int radix = j + g_skip_low == g_wave_top
                    ? g_wave_max - g_wave_min + 1 : 16;
                delta[63 - (j + g_skip_low)] = (uint8_t)
                    ((WACC[q][j] + (WACT[q][j] ? t : 0)) % radix);
            }
            ordinals[row++] = ordinal;
        }
    }
    return required;
}

extern "C" void packed_wave_start_base(uint64_t start_no, uint8_t *out32) {
    if (!out32) return;
    uint8_t quot[32] = {};
    for (int i = 0; i < 8; ++i)
        quot[31 - i] = (uint8_t)(start_no >> (8 * i));
    wave_start_key(quot, out32);
}

void wave_block_key(const ChunkIdx &idx, u256 out_key, WaveInfo &info) {
    wave_tables_need();
    const uint64_t PER_START = wave_per_start();
    info=WaveInfo{};
    info.top=g_wave_top;
    // Номер участка -> номер старта и состояние внутри него. Делим 256-битный
    // номер на PER_START столбиком по байтам: свести к uint64_t нельзя.
    uint64_t within = 0, start_no = 0;
    uint8_t quot[32];
    {
        u128 rem = 0;
        for (int i = 0; i < 32; i++) {
            rem = (rem << 8) | idx.b[i];
            quot[i] = (uint8_t)(rem / PER_START);
            rem  = rem % PER_START;
        }
        within = (uint64_t)rem;
        // Для показа в журнале хватает младших 64 бит: это просто подпись.
        // Сам ключ старта выводится из ВСЕХ 32 байт -- см. wave_start_key.
        for (int i = 24; i < 32; i++) start_no = (start_no << 8) | quot[i];
    }

    uint8_t k[32];
    wave_start_key(quot, k);
    info.start_no = start_no;
    fe_from_be32(info.start, k);
    // Invalid fixed starts can be normalized differently by a zero/nonzero
    // addition. Never deduplicate those; retain the legacy clipping behavior.
    if (within < g_wave_first_ordinal.size()
        && u256_cmp(info.start, g_range_start) >= 0
        && u256_cmp(info.start, g_range_end) <= 0) {
        info.duplicate_of = g_wave_first_ordinal[(size_t)within];
        info.duplicate = info.duplicate_of != within;
    }

    if (within == 0) {                       // сам старт волны
        info.phase = 0; info.step = 0;
        fe_from_be32(info.base, k);
        fe_from_be32(out_key, k);
        return;
    }

    int p=1;
    while (within>g_wave_phase_end[p]) ++p;
    const int t=(int)(within-g_wave_phase_end[p-1]);
    info.phase = p;
    info.step  = t;
    info.steps=g_wave_phase_steps[p];
    { int w; bool high; wave_phase_shape(p, w, high);
      info.width = w;
      info.high  = high;
      info.pass  = (p <= wave_ph1())                             ? 1
                 : (p <= wave_ph1() + wave_ph234())              ? 2
                 : (p <= wave_ph1() + 2 * wave_ph234())          ? 3 : 4; }

    // Разряд i считаем от МЛАДШЕГО. В строке HEX он стоит на месте 63-i,
    // то есть в байте (63-i)/2, в старшей или младшей половине.
    // БАЗА ФАЗЫ -- это её нулевой шаг: та же формула при t = 0. Считается
    // отдельно, потому что журнал показывает именно её, а не старт волны.
    {
        uint8_t bb[32]; memcpy(bb, k, 32);
        for (int j = 0; j < wave_pos(); j++) {
            const int add = WACC[p][j];
            if (!add) continue;
            const int pos  = 63 - (j + WAVE_SKIP_LOW);
            const int byte = pos / 2;
            const int cur  = (pos % 2 == 0) ? (bb[byte] >> 4) : (bb[byte] & 0xF);
            const int minimum=j+g_skip_low==g_wave_top ? g_wave_min : 0;
            const int radix=j+g_skip_low==g_wave_top ? g_wave_max-g_wave_min+1 : 16;
            const int nv=minimum+(cur-minimum+16*radix+add)%radix;
            if (pos % 2 == 0) bb[byte] = (uint8_t)((bb[byte] & 0x0F) | (nv << 4));
            else              bb[byte] = (uint8_t)((bb[byte] & 0xF0) | nv);
        }
        fe_from_be32(info.base, bb);
    }

    // Разряд j отсчитывается от начала волны: j = i - WAVE_SKIP_LOW.
    for (int j = 0; j < wave_pos(); j++) {
        const int add = WACC[p][j] + (WACT[p][j] ? t : 0);
        if (!add) continue;
        const int pos  = 63 - (j + WAVE_SKIP_LOW);
        const int byte = pos / 2;
        const int cur  = (pos % 2 == 0) ? (k[byte] >> 4) : (k[byte] & 0xF);
        const int minimum=j+g_skip_low==g_wave_top ? g_wave_min : 0;
        const int radix=j+g_skip_low==g_wave_top ? g_wave_max-g_wave_min+1 : 16;
        const int nv=minimum+(cur-minimum+16*radix+add)%radix;
        if (pos % 2 == 0) k[byte] = (uint8_t)((k[byte] & 0x0F) | (nv << 4));
        else              k[byte] = (uint8_t)((k[byte] & 0xF0) | nv);
    }
    fe_from_be32(out_key, k);
}

// ------------------------- обрезка участка по диапазону ---------------------
//
// СЛУЧАЙНЫЙ И ЗОНАЛЬНЫЙ РЕЖИМЫ ТОЖЕ ОБЯЗАНЫ ДЕРЖАТЬСЯ ДИАПАЗОНА.
//
// Прошлый разбор закрыл выход за край на уровне ПАКЕТА и на уровне нарезки в
// последовательном режиме, и в тех правках сказано, что «выход за край теперь
// закрыт выше по течению». Для последовательного и KXE это правда, для
// случайного и зонального -- нет: там длина участка ставилась постоянной
// (1 << chunk_bits и ZONE_BLOCK) и с g_range_end не сверялась вовсе.
//
// Насколько это заметно -- зависит от диапазона. Замер на исходном коде:
//
//   [2^30, 2^40), участок 2^22   выхода за конец нет вовсе (участок ничтожен
//                                рядом с шириной -- потому и не замечалось)
//   [2^19, 2^20), участок 2^22   ВСЕ 20000 из 20000 участков выходят за конец;
//                                программа заявляет 83.9 млрд проверенных
//                                ключей при ширине диапазона 524 288
//   [2^10, 2^12), участок 2^10   6740 из 20000
//   зональный [2^30, 2^40)       8 участков из 20000 начинались НИЖЕ начала
//                                диапазона (база цикла обнуляет младшие 12
//                                бит уже после приведения в диапазон)
//
// Находка за краем не сообщалась -- её ловила сеть key_in_search_range. Но
// счётчик проверенных ключей, скорость и всё, что от них считается, врали в
// разы, а на узком диапазоне почти вся работа уходила мимо цели.
//
// Обрезка возвращает число ключей, которое законно проверять с этого места:
// начало при необходимости поднимается до g_range_start, длина укорачивается
// по g_range_end. Ноль означает, что участок целиком вне диапазона.
uint64_t clamp_chunk_to_range(u256 key, uint64_t count) {
    if (!count) return 0;
    if (u256_cmp(key, g_range_end) > 0) return 0;          // весь участок выше
    if (u256_cmp(key, g_range_start) < 0) {                // начало ниже дна
        u256 gap; u256_sub(gap, g_range_start, key);
        if (gap[3] || gap[2] || gap[1] || gap[0] >= count) return 0;
        count -= gap[0];
        u256_set(key, g_range_start);
    }
    u256 left; u256_sub(left, g_range_end, key);           // ключей до конца - 1
    if (!left[3] && !left[2] && !left[1] && left[0] != ~0ULL && left[0] + 1 < count)
        count = left[0] + 1;
    return count;
}

// --------------------------- единая выдача ----------------------------------
//
// Рабочий цикл не знает, какой режим выбран: он просто просит следующий
// участок. Вся разница между режимами живёт здесь.

bool next_chunk(const ChunkIdx &idx, u256 out_key, uint64_t &out_count,
                int &zone_out, std::mt19937_64 &rng) {
    zone_out = -1;
    switch (g_cfg.mode) {
    case TRAV_SEQ: {
        // Конец обхода: номер дошёл до общего числа блоков. Сравнение идёт по
        // полным 256 битам, а не по g_total_chunks (тот обнулялся, когда
        // блоков было больше 2^64, и обход тогда не заканчивался вовсе).
        if (memcmp(idx.b, g_total_blocks256, 32) >= 0) return false;
        return chunk_bounds(idx, out_key, out_count);
    }

    case TRAV_KXE: {
        // Счётчик и домен перестановки теперь одной ширины -- 256 бит.
        // Раньше номер расширялся нулями из uint64_t, и дальше 2^64 блоков
        // обход просто не заходил.
        uint8_t counter[32];
        memcpy(counter, idx.b, 32);
        uint8_t zero32[32] = {};
        if (be256_cmp(g_total_blocks256, zero32) == 0) return false;   // 0 блоков
        if (be256_cmp(counter, g_total_blocks256) >= 0) return false;
        uint8_t block[32];
        kxe_permute_in_range256(counter, g_total_blocks256, kxe_seed, block);
        return chunk_bounds_from_block256(block, out_key, out_count);
    }

    case TRAV_WAVE:
    case TRAV_ZONE: {
        if(std::memcmp(idx.b,g_total_blocks256,32)>=0){out_count=0;return false;}
        return restored_shape_block(idx, out_key, out_count, zone_out);
    }

    case TRAV_HYBRID:
        // HybridSearch owns the committed prefix, suffix and RNG state.
        // It cannot be reconstructed from the generic chunk ordinal.
        out_count = 0;
        return false;

    case TRAV_RANDOM:
    default: {
        u256 span, off;
        u256_sub(span, g_range_end, g_range_start);
        random_into_span(off, span, [&]() { return rng(); });
        u128 c = 0;
        for (int i = 0; i < 4; i++) {
            c += (u128)g_range_start[i] + off[i];
            out_key[i] = (uint64_t)c; c >>= 64;
        }
        // Правка старшего разряда здесь больше не нужна: если разряд
        // задан, он уже вшит в границы диапазона (см. compute_range).
        // Вставлять его повторно поверх готового ключа значило бы
        // сместить распределение -- ровно то, от чего избавились выше.
        out_count = clamp_chunk_to_range(out_key, 1ULL << g_cfg.chunk_bits);
        return true;
    }
    }
}

// ---------------- isolated packed traversal metadata -----------------------
bool packed_traversal_prepare(PackedTraversalPlan &plan) {
    // The legacy translation table can overlap across bases. Production
    // finite modes use exact disjoint starts, batched by WaveGpuCursor.
    if(unique_blocks_mode())return false;
    plan = PackedTraversalPlan{};
    if (g_cfg.mode != TRAV_WAVE
        || (g_skip_low != 3 && g_skip_low != 4) || ZONE_BLOCK != (UINT64_C(1) << (4*g_skip_low)) || mask_active()
        || u256_cmp(g_range_start, g_range_end) > 0) return false;
    u256 span;
    u256_sub(span, g_range_end, g_range_start);
    if (!span[3] && !span[2] && !span[1] && span[0] < ZONE_BLOCK - 1) return false;
    if (g_cfg.mode == TRAV_WAVE && !g_cfg.wave_start.empty()) {
        u256 fixed;
        if (!hex_to_u256(g_cfg.wave_start.c_str(), fixed)
            || u256_cmp(fixed, g_range_start) < 0
            || u256_cmp(fixed, g_range_end) > 0) return false;
    }
    try {
        PackedTraversalPlan prepared;
        prepared.mode = g_cfg.mode;
        prepared.skip_low = g_skip_low;
        u256_to_be32(prepared.lo.data(), g_range_start);
        u256_to_be32(prepared.hi.data(), g_range_end);
        if (prepared.mode == TRAV_WAVE) {
            wave_tables_need();
            prepared.top_pos = g_wave_top >= 0 ? 63 - g_wave_top : -1;
            prepared.top_min = g_wave_min;
            prepared.top_radix = g_wave_max - g_wave_min + 1;
            prepared.legacy_states = wave_per_start();
            const uint64_t count = packed_wave_translation_plan(nullptr, nullptr, 0);
            std::vector<uint64_t> ordinals((size_t)count);
            prepared.deltas.resize((size_t)count * 64);
            packed_wave_translation_plan(prepared.deltas.data(), ordinals.data(), count);
            prepared.row_by_ordinal.assign((size_t)prepared.legacy_states, UINT32_MAX);
            prepared.display_phase.assign((size_t)prepared.legacy_states, 0);
            for (size_t row = 0; row < ordinals.size(); ++row)
                prepared.row_by_ordinal[(size_t)ordinals[row]] = (uint32_t)row;
            for (int q = 1; q <= wave_phases(); ++q)
                for (uint64_t ordinal = g_wave_phase_end[q - 1] + 1;
                     ordinal <= g_wave_phase_end[q]; ++ordinal)
                    prepared.display_phase[(size_t)ordinal] = q;
        }
        plan = std::move(prepared);
        return true;
    } catch (const std::bad_alloc &) {
        return false;
    }
}

bool packed_traversal_claim(const PackedTraversalPlan &plan, ChunkCounter &counter,
                            size_t wanted, PackedTraversalBatch &batch) {
    batch.pending_ids.clear();
    batch.segments.clear();
    batch.bases.clear();
    batch.edge_ids.clear();
    batch.sample_id = ChunkIdx{};
    batch.sample_key = PackedKey{};
    batch.sample_zone = -1;
    batch.fast = true;
    if (plan.mode != TRAV_WAVE
        || !plan.legacy_states || (plan.skip_low != 3 && plan.skip_low != 4)
        || plan.row_by_ordinal.size() != plan.legacy_states
        || wanted > (UINT32_MAX - 1024ULL) / 4) return false;
    if (!wanted) return true;
    const size_t claim_limit = wanted * 4 + 1024;
    try {
        // Reserve before claiming so allocation failure cannot lose an ID.
        batch.pending_ids.reserve(claim_limit);
        batch.segments.reserve(wanted);
        batch.bases.reserve(claim_limit);
        batch.edge_ids.reserve(claim_limit);
    } catch (const std::bad_alloc &) {
        return false;
    }

    PackedKey quotient{}, base_quotient{};
    ChunkIdx previous, expected;
    uint64_t within = 0;
    uint32_t base_index = 0;
    bool have_previous = false, have_base = false;
    for (size_t claimed = 0; claimed < claim_limit && batch.segments.size() < wanted; ++claimed) {
        ChunkIdx idx;
        counter.next(idx);
        batch.pending_ids.push_back(idx);
        if (have_previous && idx == expected && previous < idx) {
            if (++within == plan.legacy_states) {
                within = 0;
                for (int b = 31; b >= 0; --b) if (++quotient[(size_t)b]) break;
            }
        } else {
            // Also handles interleaved claimers and the 256-bit counter wrap.
            u128 remainder = 0;
            for (int b = 0; b < 32; ++b) {
                remainder = (remainder << 8) | idx.b[b];
                quotient[(size_t)b] = (uint8_t)(remainder / plan.legacy_states);
                remainder %= plan.legacy_states;
            }
            within = (uint64_t)remainder;
        }
        previous = expected = idx;
        expected.inc();
        have_previous = true;

        if (!have_base || quotient != base_quotient) {
            PackedKey base{};
            wave_start_key(quotient.data(), base.data());
            // The original wave code only deduplicates valid aligned bases.
            // Replay before consulting row_by_ordinal if that condition fails.
            if (std::memcmp(base.data(), plan.lo.data(), 32) < 0
                || std::memcmp(base.data(), plan.hi.data(), 32) > 0) {
                batch.fast = false;
                break;
            }
            base_index = (uint32_t)batch.bases.size();
            batch.bases.push_back(base);
            base_quotient = quotient;
            have_base = true;
        }
        const uint32_t row = plan.row_by_ordinal[(size_t)within];
        if (row == UINT32_MAX) continue;
        // Validate every translated block, not just the first sample. A broad
        // interval can include top-digit states outside its lower/upper bound.
        PackedKey translated=batch.bases[base_index];
        for(int p=0;p<64;++p){
            const unsigned d=plan.deltas[(size_t)row*64+p];
            if(!d)continue;
            const unsigned shift=(p&1)?0:4;
            const unsigned old=(translated[p/2]>>shift)&15;
            const unsigned value=p==plan.top_pos
                ? plan.top_min+(old-plan.top_min+d)%plan.top_radix : (old+d)&15;
            translated[p/2]=(uint8_t)((translated[p/2]&~(15u<<shift))|(value<<shift));
        }
        PackedKey last=translated;uint64_t carry=ZONE_BLOCK-1;
        for(int p=31;p>=0;--p){const uint64_t v=last[p]+carry;last[p]=(uint8_t)v;carry=v>>8;}
        if(carry||translated<plan.lo||plan.hi<last){
            batch.edge_ids.push_back(idx);
            continue;
        }
        batch.segments.push_back(PackedSegment{row, base_index});
        if (batch.segments.size() == 1) {
            u256 sample;
            WaveInfo info;
            wave_block_key(idx, sample, info);
            batch.sample_zone = info.phase;
            const uint64_t count = clamp_chunk_to_range(sample, ZONE_BLOCK);
            batch.sample_id = idx;
            u256_to_be32(batch.sample_key.data(), sample);
            if (count != ZONE_BLOCK) {
                batch.fast = false;
                break;
            }
        }
    }
    return true;
}
