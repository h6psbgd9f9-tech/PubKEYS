// Независимая проверка волнового хода "+1 по всем разрядам".
//
// Ограниченный разряд и его пределы выводятся здесь ЗАНОВО, из границ
// интервала, а не берутся у проверяемого кода: сверять модуль с самим собой
// бессмысленно. Проверяются правило шага, предел разряда, границы участка,
// нумерация шагов, хвост пустых состояний и быстрый путь для видеокарты.
//
// Сборка (Windows, после обычной nmake /f Makefile.win):
//   cl /nologo /EHsc /std:c++17 /O2 /utf-8 /I. /c wave_audit.cpp
//   link /nologo /out:wave_audit.exe wave_audit.obj util.obj config.obj ^
//        state.obj search.obj targets.obj engine.obj platform.obj ^
//        gpu_info.obj traverse.obj traverse_modes.obj notify.obj ^
//        found_card.obj key_qr.obj qrcodegen.obj telegram_found.obj ^
//        ui.obj selftest.obj kangaroo.obj scan_secp.obj
//
// Сборка (Linux/macOS, после make own):
//   g++ -std=c++17 -O2 -I. -c wave_audit.cpp -o wave_audit.o
//   g++ -o wave_audit wave_audit.o $(ls build/own-*/*.o | grep -v /main.o) -pthread
#include "common.h"
#include "traverse.h"
#include "packed_traversal.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int dg(const uint8_t b[32], int pos) {           // pos СЛЕВА
    return (pos % 2 == 0) ? (b[pos / 2] >> 4) : (b[pos / 2] & 0xF);
}
static std::string hx(const uint8_t b[32]) { return to_hex(b, 32); }

struct Iv { int a, b; const char *name; };

int main() {
    const Iv ivs[] = {
        {70, 71,  "2^70..2^71"},  {66, 67,  "2^66..2^67"},
        {120,124, "2^120..2^124"},{160,161, "2^160..2^161"},
        {251,256, "2^251..2^256"},{0,  256, "2^0..2^256"},
        {19, 20,  "2^19..2^20"},
    };
    int total_fail = 0;

    // Показ узора: три генерации подряд на интервале пазла №71.
    {
        g_cfg = Config{}; g_cfg.det_hex = -1; g_cfg.chunk_bits = 12;
        g_cfg.mode = TRAV_WAVE; g_cfg.wave_start.clear();
        g_cfg.range_start_bit = 70; g_cfg.range_end_bit = 71;
        set_block_nibbles(3); compute_range(); wave_set_seed(0x71ULL);
        std::printf("узор на интервале 2^70..2^71 (блок 3 разряда):\n");
        uint64_t shown = 0, gens = 0, prev_gen = ~0ULL;
        for (uint64_t s = 0; s < 64 && gens < 3; ++s) {
            ChunkIdx idx; idx.from_u64(s);
            u256 k; WaveInfo wi; wave_block_key(idx, k, wi);
            if (wi.start_no != prev_gen) {
                if (prev_gen != ~0ULL) std::printf("\n");
                std::printf("  генерация %llu:\n", (unsigned long long)wi.start_no);
                prev_gen = wi.start_no; ++gens;
            }
            if (wi.beyond) continue;
            uint8_t b32[32]; u256_to_be32(b32, k);
            std::printf("    %s\n", to_hex(b32, 32).c_str());
            ++shown;
        }
        std::printf("\n");
    }
    for (int skip : {3, 4, 8}) {
      for (const Iv &iv : ivs) {
        g_cfg = Config{};
        g_cfg.det_hex = -1; g_cfg.chunk_bits = 12; g_cfg.mode = TRAV_WAVE;
        g_cfg.wave_start.clear();
        g_cfg.range_start_bit = iv.a; g_cfg.range_end_bit = iv.b;
        set_block_nibbles(skip); compute_range(); wave_set_seed(0x51D0FA57ULL);

        uint8_t lob[32], hib[32];
        u256_to_be32(lob, g_range_start); u256_to_be32(hib, g_range_end);

        // Ограниченный разряд выводится здесь заново, из границ интервала.
        int TOP = 0; while (TOP < 64 && dg(lob, TOP) == dg(hib, TOP)) ++TOP;
        const int TMIN = TOP < 64 ? dg(lob, TOP) : 0;
        const int TMAX = TOP < 64 ? dg(hib, TOP) : 0;
        const int LAST_CHANGING = 63 - skip;
        const bool has_steps = (TOP < 64) && (TOP <= LAST_CHANGING);

        long bad_step = 0, bad_limit = 0, bad_block = 0, bad_ord = 0,
             bad_tail = 0, bad_agree = 0, empty_states = 0;
        std::vector<long> gen_len;

        const uint64_t PER = wave_states_per_start();
        const uint64_t STATES = 4000;
        uint8_t prev[32]; bool have_prev = false;
        uint64_t prev_gen = 0; int prev_step = -1; long cur_len = 0;
        bool gen_tail = false;                    // пустые уже начались

        for (uint64_t s = 0; s < STATES; ++s) {
            ChunkIdx idx; idx.from_u64(s);
            u256 key; WaveInfo wi;
            wave_block_key(idx, key, wi);
            uint8_t kb[32]; u256_to_be32(kb, key);

            // подпись согласована с независимым расчётом
            if (wi.top != (TOP < 64 ? 63 - TOP : -1) ||
                wi.top_min != TMIN || wi.top_max != TMAX) ++bad_agree;

            // нумерация: внутри генерации шаг растёт на 1, с новой -- в 0
            const uint64_t gen = wi.start_no;
            if (have_prev) {
                if (gen == prev_gen) { if (wi.step != prev_step + 1) ++bad_ord; }
                else                 { if (wi.step != 0) ++bad_ord;
                                       gen_len.push_back(cur_len); cur_len = 0;
                                       gen_tail = false; }
            } else if (wi.step != 0) ++bad_ord;

            // границы участка -- через ту же выдачу, что и рабочий цикл
            u256 ck; uint64_t cnt = 0; int zone = -1; std::mt19937_64 rng(1);
            next_chunk(idx, ck, cnt, zone, rng);
            uint8_t cb[32] = {};
            if (cnt) {
                uint8_t eb[32];
                u256 end; u256_add_u64(end, ck, cnt - 1);
                u256_to_be32(cb, ck);
                u256_to_be32(eb, end);
                if (memcmp(cb, lob, 32) < 0 || memcmp(eb, hib, 32) > 0 ||
                    memcmp(cb, eb, 32) > 0) ++bad_block;
                ++cur_len;
            } else {
                ++empty_states;
                gen_tail = true;                  // дальше в этой генерации пусто
            }
            if (cnt && gen_tail) ++bad_tail;      // непустое ПОСЛЕ пустого

            // предел ограниченного разряда: только на непустых состояниях
            // Проверяется ПЕРВЫЙ ЗАКОННЫЙ ключ участка, а не сырое начало
            // блока: у интервала, целиком лежащего внутри одного блока,
            // начало блока и обязано быть ниже нижней границы.
            if (cnt && TOP < 64) {
                const int v = dg(cb, TOP);
                if (v < TMIN || v > TMAX) ++bad_limit;
            }

            // правило шага: каждая цифра над блоком ровно +1 mod 16
            if (have_prev && gen == prev_gen) {
                for (int pos = (TOP < 64 ? TOP : 64); pos <= LAST_CHANGING; ++pos)
                    if (dg(kb, pos) != ((dg(prev, pos) + 1) & 15)) ++bad_step;
                // разряды НИЖЕ блока и ВЫШЕ ограниченного не трогаются
                for (int pos = 0; pos < (TOP < 64 ? TOP : 64); ++pos)
                    if (dg(kb, pos) != dg(prev, pos)) ++bad_step;
                for (int pos = LAST_CHANGING + 1; pos < 64; ++pos)
                    if (dg(kb, pos) != dg(prev, pos)) ++bad_step;
            }
            memcpy(prev, kb, 32); have_prev = true;
            prev_gen = gen; prev_step = wi.step;
        }

        const bool ok = !bad_step && !bad_limit && !bad_block && !bad_ord &&
                        !bad_tail && !bad_agree &&
                        (has_steps ? PER == (uint64_t)(TMAX - TMIN + 1) : PER == 1);
        if (!ok) ++total_fail;
        std::printf("%-14s skip=%d | разряд %2d слева (%x..%x) | состояний %2llu | "
                    "шаг %ld | предел %ld | блок %ld | нумерация %ld | хвост %ld | "
                    "подпись %ld | пустых %ld  %s\n",
                    iv.name, skip, TOP < 64 ? TOP : -1, TMIN, TMAX,
                    (unsigned long long)PER, bad_step, bad_limit, bad_block,
                    bad_ord, bad_tail, bad_agree, empty_states, ok ? "ok" : "ОШИБКА");
      }
    }

    // Таблица: что получается из интервала само собой.
    std::printf("\nИнтервал                   последний ключ                   разряд  пределы  шагов\n");
    const Iv tbl[] = {{70,71,"2^70..2^71"},{66,67,"2^66..2^67"},{120,124,"2^120..2^124"},
                      {160,161,"2^160..2^161"},{251,256,"2^251..2^256"},{0,256,"2^0..2^256"}};
    for (const Iv &iv : tbl) {
        g_cfg = Config{}; g_cfg.det_hex = -1; g_cfg.chunk_bits = 12;
        g_cfg.mode = TRAV_WAVE; g_cfg.wave_start.clear();
        g_cfg.range_start_bit = iv.a; g_cfg.range_end_bit = iv.b;
        set_block_nibbles(3); compute_range();
        uint8_t hib[32]; u256_to_be32(hib, g_range_end);
        std::string h = hx(hib);
        std::printf("%-14s %s  %2d-й справа  %x..%x   до %llu\n", iv.name,
                    (h.substr(0, 8) + "..." + h.substr(56)).c_str(),
                    64 - wave_top_position(), wave_top_min(), wave_top_max(),
                    (unsigned long long)wave_states_per_start());
    }
    // ------------------------------------------------------------------
    //  БЫСТРЫЙ ПУТЬ ДЛЯ КАРТЫ СЧИТАЕТ ТО ЖЕ САМОЕ
    //
    //  План для карты -- это таблица сдвигов по разрядам, и карта применяет
    //  их своей арифметикой: ограниченный разряд по модулю, остальные по
    //  модулю 16. Здесь ядро повторено на хосте строка в строку и сверено с
    //  обычным путём wave_block_key. Заодно проверяется, что пустые
    //  состояния до карты не доходят: им на карте соответствовал бы ключ,
    //  которого волновой ход не даёт.
    // ------------------------------------------------------------------
    std::printf("\nбыстрый путь (план для карты), блок 4 разряда:\n");
    for (const Iv &iv : ivs) {
        g_cfg = Config{}; g_cfg.det_hex = -1; g_cfg.chunk_bits = 12;
        g_cfg.mode = TRAV_WAVE; g_cfg.wave_start.clear();
        g_cfg.range_start_bit = iv.a; g_cfg.range_end_bit = iv.b;
        set_block_nibbles(4); compute_range(); wave_set_seed(0x51D0FA57ULL);

        PackedTraversalPlan plan;
        if (!packed_traversal_prepare(plan)) {
            std::printf("%-14s план не строится (так и задумано для этого интервала)\n", iv.name);
            continue;
        }
        ChunkCounter counter;
        PackedTraversalBatch batch;
        long mismatch = 0, out_of_range = 0, skipped_wrong = 0, segs = 0;
        for (int round = 0; round < 8; ++round) {
            if (!packed_traversal_claim(plan, counter, 64, batch)) { ++mismatch; break; }
            if (!batch.fast) continue;
            // Сегмент обязан быть ровно у тех номеров, где обычный путь даёт
            // непустой участок, и ровно в том же порядке.
            std::vector<ChunkIdx> expect;
            for (const ChunkIdx &id : batch.pending_ids) {
                u256 kk; WaveInfo wi; wave_block_key(id, kk, wi);
                if (!wi.beyond) expect.push_back(id);
            }
            if (expect.size() != batch.segments.size()) { ++skipped_wrong; continue; }
            for (size_t i = 0; i < batch.segments.size(); ++i) {
                const PackedSegment sg = batch.segments[i];
                u256 want_key; WaveInfo wi; wave_block_key(expect[i], want_key, wi);
                uint8_t k[32]; memcpy(k, batch.bases[sg.base].data(), 32);
                const uint8_t *d64 = plan.deltas.data() + (size_t)sg.state * 64;
                bool valid = true;
                for (int pp = 0; pp < 64; ++pp) {               // ядро карты, строка в строку
                    const unsigned d = d64[pp]; if (!d) continue;
                    const unsigned old = (pp & 1) ? (k[pp/2] & 15) : (unsigned)(k[pp/2] >> 4);
                    unsigned value;
                    if (pp == plan.top_pos) {
                        if (old < (unsigned)plan.top_min
                            || old >= (unsigned)(plan.top_min + plan.top_radix)) { valid = false; break; }
                        value = (unsigned)plan.top_min
                              + (old - (unsigned)plan.top_min + d) % (unsigned)plan.top_radix;
                    } else value = (old + d) & 15;
                    if (pp & 1) k[pp/2] = (uint8_t)((k[pp/2] & 0xf0) | value);
                    else        k[pp/2] = (uint8_t)((k[pp/2] & 15) | (value << 4));
                }
                uint8_t wb[32]; u256_to_be32(wb, want_key);
                if (!valid || memcmp(k, wb, 32) != 0) ++mismatch;
                if (memcmp(k, plan.lo.data(), 32) < 0
                    || memcmp(k, plan.hi.data(), 32) > 0) ++out_of_range;
                ++segs;
            }
        }
        const bool ok = !mismatch && !out_of_range && !skipped_wrong && segs > 0;
        if (!ok) ++total_fail;
        std::printf("%-14s сегментов %4ld | расхождений %ld | вне интервала %ld | "
                    "лишних %ld  %s\n", iv.name, segs, mismatch, out_of_range,
                    skipped_wrong, ok ? "ok" : "ОШИБКА");
    }

    std::printf("\n%s\n", total_fail ? "ЕСТЬ ОШИБКИ" : "ВСЕ ПРОВЕРКИ ПРОЙДЕНЫ");
    return total_fail ? 1 : 0;
}
