#include <filesystem>
#include <chrono>
// ============================================================================
//  traverse.cpp — нарезка диапазона, журнал участков, чекпоинт.
// ============================================================================
#include "traverse.h"
#include "search.h"
#include "engine.h"   // g_step: конец участка считается в шагах
#include "notify.h"   // secure_create: журнал участков тоже закрываем от посторонних

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include "platform.h"   // fsync и права -- через слой систем
#include <atomic>
#include <ctime>
#include <sstream>
#include <array>
#include <algorithm>
#include <cstring>
#include <mutex>

// g_chunk_seq -- ссылка на поле SearchContext, определена в search.cpp.

// ГРАНИЦЫ УЧАСТКА
//
// Вынесено отдельно нарочно: этой же функцией пользуется тест покрытия.
// Пока расчёт был размазан по рабочему циклу, проверить его было нечем —
// а ошибка в нём не падает, а тихо пропускает часть диапазона.
// ========================================================
// --------------------------------- маска -------------------------------------
u256 g_mask_step, g_mask_first, g_mask_total;

bool mask_active() {
    return g_cfg.mode == TRAV_SEQ && !g_cfg.mask_hex.empty() && g_cfg.mask_froz > 0;
}

static void u256_shr_bits(u256 r, const u256 a, int n) {
    u256 t; u256_zero(t);
    if (n < 256) {
        const int w = n >> 6, b = n & 63;
        for (int i = 0; i + w < 4; i++) {
            uint64_t v = a[i + w] >> b;
            if (b && i + w + 1 < 4) v |= a[i + w + 1] << (64 - b);
            t[i] = v;
        }
    }
    u256_set(r, t);
}
static void u256_shl_bits(u256 r, const u256 a, int n) {
    u256 t; u256_zero(t);
    if (n < 256) {
        const int w = n >> 6, b = n & 63;
        for (int i = 3; i - w >= 0; i--) {
            uint64_t v = a[i - w] << b;
            if (b && i - w - 1 >= 0) v |= a[i - w - 1] >> (64 - b);
            t[i] = v;
        }
    }
    u256_set(r, t);
}

// 256-битное число десятичной строкой. Нужно, чтобы «сколько кандидатов»
// читалось глазами: шестнадцатеричное здесь ничего не сообщает человеку.
std::string u256_to_dec(const u256 v) {
    u256 t; u256_set(t, v);
    if (!(t[0] | t[1] | t[2] | t[3])) return "0";
    std::string out;
    while (t[0] | t[1] | t[2] | t[3]) {
        u128 rem = 0;
        for (int i = 3; i >= 0; i--) {
            u128 cur = (rem << 64) | t[i];
            t[i] = (uint64_t)(cur / 1000000000ULL);
            rem  = cur % 1000000000ULL;
        }
        char part[16];
        if (t[0] | t[1] | t[2] | t[3]) snprintf(part, sizeof(part), "%09llu", (unsigned long long)rem);
        else                            snprintf(part, sizeof(part), "%llu",   (unsigned long long)rem);
        out = std::string(part) + out;
    }
    return out;
}

std::string mask_layout(const std::string &hex, int froz) {
    if (hex.size() != 64) return hex;
    const int vary = 64 - froz;
    if (vary <= 0) return "[" + hex + "]  (перебирать нечего: заморожен весь ключ)";
    return "[" + hex.substr(0, vary) + "]" + hex.substr(vary);
}

// Подпись под разметкой. Скобка каждой части стоит РОВНО под ней.
//
// Сначала обе подписи печатались подряд с начала строки, и «константа»
// оказывалась под перебираемой частью. Прочитать это можно было только
// наоборот — что меняются младшие ниббли, — то есть подпись утверждала
// обратное тому, что делает программа.
// Длина в ЗНАКАХ, а не в байтах: подписи кириллические, и в UTF-8 буква
// занимает два байта. Пока ширина считалась байтами, рамка выходила вдвое
// короче нужного, а короткая подпись обрезалась посреди буквы.
static int utf8_len(const std::string &s) {
    int n = 0;
    for (unsigned char c : s) if ((c & 0xC0) != 0x80) n++;
    return n;
}

static std::string span_label(int width, const std::string &full,
                              const std::string &shrt) {
    for (const std::string *t : { &full, &shrt }) {
        const int len = utf8_len(*t);
        if (width >= len + 4) {
            const int pad  = (width - 2) - len;
            const int left = pad / 2, right = pad - left;
            return "└" + std::string((size_t)left, '-') + *t
                 + std::string((size_t)right, '-') + "┘";
        }
    }
    // Совсем узко -- рамка без слов: врать шириной хуже, чем промолчать.
    if (width >= 2) return "└" + std::string((size_t)(width - 2), '-') + "┘";
    return std::string((size_t)(width > 0 ? width : 0), ' ');
}

std::string mask_legend(int froz) {
    const int vary = 64 - froz;
    if (vary <= 0) return "";
    // +2 на скобки [ ] вокруг перебираемой части
    return " " + span_label(vary + 2, "перебор: " + std::to_string(vary),
                                      std::to_string(vary))
         +       span_label(froz,     "константа: " + std::to_string(froz),
                                      std::to_string(froz));
}

bool mask_compute() {
    u256_zero(g_mask_step); u256_zero(g_mask_first); u256_zero(g_mask_total);
    if (!mask_active()) return true;

    u256 K;
    if (!hex_to_u256(g_cfg.mask_hex.c_str(), K)) return false;

    const int shift = 4 * g_cfg.mask_froz;      // 16^N == 2^(4N)

    // Весь ключ заморожен -- кандидат ровно один, шаг не нужен.
    if (g_cfg.mask_froz >= 64) {
        u256_from_u64(g_mask_step, 1);
        if (u256_cmp(K, g_range_start) < 0 || u256_cmp(K, g_range_end) > 0) return false;
        u256_set(g_mask_first, K);
        u256_from_u64(g_mask_total, 1);
        return true;
    }
    { u256 one; u256_from_u64(one, 1); u256_shl_bits(g_mask_step, one, shift); }

    // j_min: ceil((lo-K)/16^N). Subtract before shifting: adding 16^N-1
    // to a high 256-bit difference can wrap and return a key below lo.
    u256 jmin; u256_zero(jmin);
    if (u256_cmp(K, g_range_start) < 0) {
        u256 d; u256_sub(d, g_range_start, K);
        u256 t; u256_sub_u64(t, d, 1);
        u256_shr_bits(jmin, t, shift);
        u256_add_u64(jmin, jmin, 1);
    }

    // j_max по верху интервала.
    if (u256_cmp(g_range_end, K) < 0) return false;     // весь интервал ниже K
    u256 jmax; { u256 d; u256_sub(d, g_range_end, K); u256_shr_bits(jmax, d, shift); }

    // j_max по решётке: ключ не должен выйти за 2^256.
    { u256 top; for (int i = 0; i < 4; i++) top[i] = ~0ULL;
      u256 d; u256_sub(d, top, K);
      u256 jl; u256_shr_bits(jl, d, shift);
      if (u256_cmp(jl, jmax) < 0) u256_set(jmax, jl); }

    if (u256_cmp(jmax, jmin) < 0) return false;         // пересечение пусто

    u256 cnt; u256_sub(cnt, jmax, jmin);
    u256_add_u64(g_mask_total, cnt, 1);

    u256 off; u256_shl_bits(off, jmin, shift);
    u256_add(g_mask_first, K, off);
    return true;
}

// СКОЛЬКО КЛЮЧЕЙ В ОДНОМ УЧАСТКЕ -- ЗАВИСИТ ОТ РЕЖИМА.
//
// У детерминированного, KXE и случайного участок равен 2^chunk_bits.
// Конечные волновой и зонный обходы используют свой выбранный размер
// блока 16^N, независимо от chunk_bits; последний блок подрезается.
//
// Отдельной функцией нарочно. Показ чекпоинта брал размер прямо из
// g_cfg.chunk_bits и для волны врал в 1024 раза: «2 683 241 626 599 424
// ключей» вместо 2.6 триллиона. Число выглядело осмысленным, и проверить его
// было нечем -- пока пользователь не заметил сам.
uint64_t chunk_keys() {
    if (unique_blocks_mode()) return UINT64_C(1) << traversal_block_bits();
    if (g_cfg.mode == TRAV_HYBRID) return 1ULL << (4 * g_cfg.hybrid_nibbles);
    return 1ULL << g_cfg.chunk_bits;
}

bool chunk_bounds(const ChunkIdx &idx, u256 out_start, uint64_t &out_count) {
    const int block_bits = traversal_block_bits();
    const uint64_t CHUNK = UINT64_C(1) << block_bits;

    // МАСКА: УЧАСТОК НАРЕЗАЕТСЯ ПО НОМЕРУ КАНДИДАТА, А НЕ ПО КЛЮЧУ.
    //
    // Кандидаты идут через 16^N, поэтому «участок из 2^22 ключей» -- это
    // 2^22 кандидатов, а не отрезок длиной 2^22. Считать в ключах здесь
    // означало бы нарезать пустоту между узлами решётки.
    if (mask_active()) {
        u256 i256; idx.to_u256(i256);
        if (i256[3] >> (64 - block_bits)) return false;
        u256 j0; u256_shl_bits(j0, i256, block_bits);   // j0 = idx*CHUNK
        if (u256_cmp(j0, g_mask_total) >= 0) return false;    // участков больше нет

        u256 left; u256_sub(left, g_mask_total, j0);
        out_count = CHUNK;
        if (left[3] == 0 && left[2] == 0 && left[1] == 0 && left[0] < CHUNK)
            out_count = left[0];

        const int shift = 4 * g_cfg.mask_froz;
        u256 off; u256_shl_bits(off, j0, shift >= 256 ? 255 : shift);
        if (g_cfg.mask_froz >= 64) u256_set(off, j0);
        u256_add(out_start, g_mask_first, off);
        return out_count != 0;
    }

    // start + idx * CHUNK, всё в 256 битах.
    //
    // Было: idx как uint64_t, сдвиг через unsigned __int128. Работало, пока
    // номер помещался в 64 бита -- то есть пока диапазон был у́же 2^(64+chunk).
    // Теперь номер полный, и умножение делается сдвигом байтов: idx << chunk_bits.
    u256 idx256; idx.to_u256(idx256);
    uint8_t ib[32], off[32];
    u256_to_be32(ib, idx256);

    // Сдвиг влево на chunk_bits с проверкой выхода за 2^256: если что-то
    // ненулевое уезжает за старший бит, такого участка не существует.
    {
        const int sh = block_bits;
        const int byte_sh = sh / 8, bit_sh = sh % 8;
        memset(off, 0, 32);
        for (int i = 31; i >= 0; i--) {
            int src = i + byte_sh;
            uint32_t v = 0;
            if (src <= 31) v |= (uint32_t)ib[src] << bit_sh;
            if (bit_sh && src + 1 <= 31) v |= (uint32_t)ib[src + 1] >> (8 - bit_sh);
            off[i] = (uint8_t)(v & 0xFF);
        }
        // Ушло ли что-то за верх: проверяем байты, которые сдвиг выбросил.
        for (int i = 0; i < byte_sh; i++) if (ib[i]) return false;
        if (bit_sh && byte_sh <= 31 && (ib[byte_sh] >> (8 - bit_sh))) return false;
    }

    u256 add; fe_from_be32(add, off);
    u128 c = 0;
    for (int i = 0; i < 4; i++) {
        c += (u128)g_range_start[i] + add[i];
        out_start[i] = (uint64_t)c;
        c >>= 64;
    }
    if (c) return false;                                  // ушли за 2^256
    if (u256_cmp(out_start, g_range_end) > 0) return false;

    u256 left;
    u256_sub(left, g_range_end, out_start);
    out_count = CHUNK;
    // При left[0] == 2^64-1 выражение left[0] + 1 переполнялось в ноль,
    // участок получал нулевую длину и молча пропускался.
    if (left[3] == 0 && left[2] == 0 && left[1] == 0 &&
        left[0] != ~0ULL && left[0] + 1 < CHUNK)
        out_count = left[0] + 1;                          // хвост диапазона
    return true;
}


// Смещение образца внутри участка. Перемешиваются ВСЕ четыре лимба: зоны
// меняют старшие разряды, а младший лимб внутри цикла не шелохнётся --
// размешав только его, получили бы одно и то же смещение на все блоки цикла.
uint64_t sample_offset(const u256 k0, uint64_t count) {
    if (!count) return 0;
    uint64_t z = 0x9E3779B97F4A7C15ULL;
    for (int i = 0; i < 4; i++) {
        z += k0[i] + 0x9E3779B97F4A7C15ULL;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        z ^= z >> 31;
    }
    return z % count;
}

// ========================================================
// ЖУРНАЛ УЧАСТКОВ
// Первые записи выгружаются в файл, чтобы глазами убедиться, что обход
// покрывает диапазон и потоки не топчутся друг по другу.
// ========================================================
std::mutex g_mtx_dbg;
std::atomic<uint64_t> g_dbg_written{0};

void debug_log_chunk(int thread, const ChunkIdx &idx, const u256 start, uint64_t count) {
    // Clipped-out shaped states complete their ordinal but contain no keys.
    // In particular, never underflow count-1 in the human-readable log.
    if (!count) return;
    if (g_cfg.mode == TRAV_WAVE) {
        u256 key; uint64_t actual = 0; int phase = -1; WaveInfo info;
        if (restored_shape_block(idx, key, actual, phase, &info) && actual)
            debug_log_wave(thread, idx, key, info, actual);
        return;
    }
    // Ровно DBG_LIMIT записей, и ни одной больше.
    //
    // Счётчик один на запуск, а файл раньше открывался на дозапись — значит,
    // за пять запусков в нём оказывалось пятьдесят тысяч записей вместо
    // десяти. Поэтому при первой записи файл ПЕРЕЗАПИСЫВАЕТСЯ: журнал всегда
    // относится к текущему запуску и всегда укладывается в предел.
    if (g_dbg_written.load(std::memory_order_relaxed) > DBG_LIMIT) return;

    // Последний ключ участка -- start + (count-1)*шаг. При маске участок
    // содержит count КАНДИДАТОВ, разнесённых на 16^N, а не count подряд
    // идущих ключей: без шага «до ключа» указывало бы внутрь пустоты между
    // узлами решётки.
    u256 last;
    { u256 off; u128 c = 0;
      const uint64_t m = count - 1;
      for (int i = 0; i < 4; i++) {
          u128 v = (u128)g_step[i] * m + c;
          off[i] = (uint64_t)v; c = v >> 64; }
      u256_add(last, start, off); }

    uint8_t sb[32], eb[32];
    u256_to_be32(sb, start);
    u256_to_be32(eb, last);

    // Запись повторяет блок ПРОВЕРКА с экрана: приватный ключ, обе
    // координаты и публичный ключ. Одно скалярное умножение на несколько
    // миллионов ключей — на скорость не влияет, зато запись становится
    // проверяемой: ключ и отвечающий ему публичный ключ можно сверить
    // любым сторонним инструментом.
    Point p = ec_mul_G(start);
    std::string sx = "(бесконечность)", sy = sx, spub = sx, spub_u = sx;
    if (!p.inf) {
        uint8_t xb[32], yb[32], c33[33], u65[65];
        fe_to_be32(xb, p.x);
        fe_to_be32(yb, p.y);
        c33[0] = (uint8_t)(0x02 | (p.y[0] & 1));
        memcpy(c33 + 1, xb, 32);
        // Несжатый: X и Y уже посчитаны для сжатого варианта выше -- сборка
        // не требует дополнительного скалярного умножения.
        u65[0] = 0x04;
        memcpy(u65 + 1, xb, 32);
        memcpy(u65 + 33, yb, 32);
        sx = to_hex(xb, 32); sy = to_hex(yb, 32);
        spub = to_hex(c33, 33); spub_u = to_hex(u65, 65);
    }

    std::lock_guard<std::mutex> lk(g_mtx_dbg);

    // Reserve the row under the same lock as its write. Otherwise the limit
    // notice could overtake an earlier row still formatting its public point.
    const uint64_t seen = g_dbg_written.fetch_add(1);
    if (seen > DBG_LIMIT) return;
    if (seen == DBG_LIMIT) {
        std::ofstream f("debug_chunks.txt", std::ios::app);
        if (f) {
            secure_create("debug_chunks.txt");
            f << "===== предел " << DBG_LIMIT
              << " записей достигнут, журнал остановлен =====\n";
        }
        return;
    }

    // Первая запись запуска затирает старое содержимое, дальше — дозапись.
    static bool header_done = false;
    // ЖУРНАЛ ЗАКРЫВАЕТСЯ ОТ ПОСТОРОННИХ, КАК И Found.txt.
    //
    // Внутри лежат приватные ключи -- пусть и просто начала участков, а не
    // находки. Само по себе это не секрет: ключи выводятся из диапазона и
    // предсказуемы. Но файл с заголовком PRIVATE KEY, доступный на чтение
    // всем пользователям машины, -- плохая привычка, а стоит её исправление
    // одной строки.
    std::ofstream f("debug_chunks.txt",
                    header_done ? std::ios::app : std::ios::trunc);
    if (!f) return;
    // Also secure a file recreated after deletion during this run, before
    // writing any private material into it.
    secure_create("debug_chunks.txt");

    if (!header_done) {
        header_done = true;
        f << "===== запуск " << now_string()
          // mode_title(), а не своя табличка: список режимов уже есть в
          // config.cpp, и вторая его копия здесь однажды отстала -- волновой
          // режим печатался как «KXE», потому что про эту строку забыли.
          << " | режим " << mode_title(g_cfg.mode)
          << " | диапазон 2^" << g_cfg.range_start_bit
          << "..2^" << g_cfg.range_end_bit << " =====\n";
    }

    f << "участок " << idx.hex() << "  поток " << thread << "  ключей " << count << "\n"
      << "  PRIVATE KEY   : " << to_hex(sb, 32) << "\n"
      << "  X             : " << sx   << "\n"
      << "  Y             : " << sy   << "\n"
      << "  PUBLIC KEY(c) : " << spub   << "\n"
      << "  PUBLIC KEY(u) : " << spub_u << "\n"
      << "  до ключа      : " << to_hex(eb, 32) << "\n";
    if (g_cfg.mode == TRAV_ZONE) {
        u256 actual_key; uint64_t actual_count = 0; int phase = -1; ZoneInfo info;
        if (restored_shape_block(idx, actual_key, actual_count, phase, nullptr, &info)) {
            uint8_t base[32]; u256_to_be32(base, info.base);
            f << "  База окрестности: " << to_hex(base, 32) << "\n"
              << "  Позиция ниббла : " << (info.pos < 0 ? 0 : info.pos + 1)
              << " (слева с 1; 0 означает базовый блок)\n";
        }
    }
}

// ========================================================
// ЧЕКПОИНТ
//
// У КАЖДОЙ КОНФИГУРАЦИИ СВОЙ ФАЙЛ.
//
// Раньше файл был один на всё: сменили диапазон -- и позиция по прежнему
// пропадала безвозвратно, вернулись назад -- начинали с нуля. Для KXE это
// особенно обидно: обход перестановочный, и без сохранённой позиции он не
// просто теряет время, а заново выдаёт те же блоки в том же порядке.
//
// Теперь имя файла собирается из режима, границ диапазона и размера участка:
//
//     checkpoint_kxe_252_256_22.dat
//     checkpoint_seq_255_256_22.dat
//
// Конфигурации больше не мешают друг другу: можно гонять 252..256 в KXE,
// переключиться на 255..256, потом вернуться -- каждая продолжится со своего
// места.
// ========================================================
static const char *mode_tag(int m) {
    switch (m) {
        case TRAV_SEQ: return "seq";
        case TRAV_KXE: return "kxe";
        case TRAV_ZONE: return "zone";
        case TRAV_HYBRID: return "hybrid";
        case TRAV_WAVE: return "wave";
        default:       return "rnd";
    }
}

// Короткая подпись строки: нужна, чтобы wave_start попал в имя файла, не
// растянув его на 64 знака.
static std::string short_tag(const std::string &v) {
    uint8_t h[32];
    sha256((const uint8_t *)v.data(), v.size(), h);
    return to_hex(h, 4);
}

static std::string legacy_checkpoint_path() {
    char buf[200];
    // ВОЛНА: В ИМЯ ВХОДИТ И ТОЧКА СТАРТА.
    //
    // Для волны позиция отсчитывается от wave_start -- значит это ровно то,
    // «от чего считаем», и без него имя файла не различает задачи. Сменили
    // гипотезу о форме ключа, оставив тот же диапазон и участок, -- и новый
    // запуск подхватил бы чужую позицию, объявив пройденными состояния,
    // которых при новом старте никто не считал.
    //
    // Ровно та же беда была у старшего разряда, и лечится она тем же: то, от
    // чего зависит смысл позиции, обязано быть в имени.
    // МАСКА -- ЭТО ТО, ОТ ЧЕГО СЧИТАЕТСЯ ПОЗИЦИЯ, ЗНАЧИТ ОНА В ИМЕНИ.
    //
    // Номер участка при маске -- номер кандидата на решётке, а решётка задана
    // ключом и числом замороженных нибблов. Сменили ключ, оставив тот же
    // режим и интервал, -- и без этого имени новый запуск подхватил бы чужую
    // позицию, объявив пройденными кандидаты, которых на новой решётке никто
    // не считал. Та же беда была у старшего разряда и у старта волны.
    if (mask_active()) {
        snprintf(buf, sizeof(buf), "checkpoint_mask_%d_%d_%d_%d_%s.dat",
                 g_cfg.range_start_bit, g_cfg.range_end_bit, g_cfg.chunk_bits,
                 g_cfg.mask_froz, short_tag(g_cfg.mask_hex).c_str());
        return std::string(buf);
    }
    if (g_cfg.mode == TRAV_WAVE) {
        // "s3631" -- сколько состояний приходится на одну точку старта.
        //
        // От этого числа зависит, во что превращается номер участка: он
        // делится на него, давая номер старта и состояние внутри. Челнок и
        // зеркало изменили его с 916 на 3631, и прежние файлы указывают
        // теперь совсем в другое место -- при том же режиме, диапазоне и
        // участке. Число в имени разводит старое и новое: файлы прежних
        // запусков просто не найдутся, и счёт начнётся сначала. Это верно:
        // обход стал другим, продолжать в нём нечего.
        const uint64_t low_mask = (1ULL << (4 * g_skip_low)) - 1;
        bool revised_alignment = (g_range_start[0] & low_mask) != 0;
        if (!g_cfg.wave_start.empty()) {
            u256 fixed;
            if (hex_to_u256(g_cfg.wave_start.c_str(), fixed)) {
                revised_alignment |= (fixed[0] & low_mask) != 0;
            }
        }
        // Old unaligned fixed-start checkpoints describe different blocks.
        // Preserve names when the lower bound and any fixed start are aligned.
        const std::string block_suffix = g_skip_low == 3 ? "" : "_b" + std::to_string(4 * g_skip_low);
        snprintf(buf, sizeof(buf), "checkpoint_wave_%d_%d_%d_s%llu_%s%s%s.dat",
                 g_cfg.range_start_bit, g_cfg.range_end_bit, g_cfg.chunk_bits,
                 (unsigned long long)wave_states_per_start(),
                 (g_cfg.wave_start.empty() ? "rnd"
                                           : short_tag(g_cfg.wave_start).c_str()),
                 revised_alignment ? "_a1" : "",
                 block_suffix.c_str());
        return std::string(buf);
    }
    // СТАРШИЙ РАЗРЯД ВХОДИТ В ИМЯ.
    //
    // С тех пор как det_hex сужает границы, две настройки с одинаковыми
    // 2^a..2^b, но разным разрядом -- это РАЗНЫЕ отрезки, и позиция участка
    // в одном ничего не значит для другого. Без разряда в имени они делили
    // бы один файл: продолжение с чужой позиции пропустило бы начало своего
    // отрезка и объявило пройденным то, чего никто не считал.
    if (g_cfg.det_hex >= 0)
        snprintf(buf, sizeof(buf), "checkpoint_%s_%d_%d_%d_h%x.dat",
                 mode_tag(g_cfg.mode), g_cfg.range_start_bit,
                 g_cfg.range_end_bit, g_cfg.chunk_bits, g_cfg.det_hex & 0xF);
    else
        snprintf(buf, sizeof(buf), "checkpoint_%s_%d_%d_%d.dat",
                 mode_tag(g_cfg.mode), g_cfg.range_start_bit,
                 g_cfg.range_end_bit, g_cfg.chunk_bits);
    return std::string(buf);
}

namespace {
// Streaming SHA-256; target serialization is independent of struct padding,
// source file ordering, duplicate rows, and compressed/uncompressed spelling.
class CheckpointHash {
    sha256d::Ctx state_;
    uint8_t pending_[64]{};
    size_t used_ = 0;
    uint64_t bytes_ = 0;
public:
    CheckpointHash() { state_.init(); }
    void add(const uint8_t *data, size_t size) {
        bytes_ += (uint64_t)size;
        while (size) {
            size_t take = std::min(size, sizeof pending_ - used_);
            memcpy(pending_ + used_, data, take);
            used_ += take; data += take; size -= take;
            if (used_ == sizeof pending_) { state_.block(pending_); used_ = 0; }
        }
    }
    std::string finish() {
        uint8_t tail[128]{};
        memcpy(tail, pending_, used_); tail[used_] = 0x80;
        const size_t size = used_ < 56 ? 64 : 128;
        const uint64_t bits = bytes_ * 8;
        for (int i = 0; i < 8; ++i) tail[size - 1 - i] = (uint8_t)(bits >> (8 * i));
        state_.block(tail); if (size == 128) state_.block(tail + 64);
        uint8_t out[32];
        for (int i = 0; i < 8; ++i)
            for (int j = 0; j < 4; ++j) out[i * 4 + j] = (uint8_t)(state_.h[i] >> (24 - 8 * j));
        return to_hex(out, 32);
    }
};
std::string checkpoint_target_hash;
bool checkpoint_identity_ready = false;
bool checkpoint_write_blocked = false;
struct LoadedShapeCheckpoint {
    bool valid = false;
    ChunkIdx sequence;
    u256 completed{};
    uint64_t seed = 0;
    std::string identity;
};
LoadedShapeCheckpoint loaded_shape_checkpoint;

std::string key_hex(const u256 value) {
    uint8_t bytes[32]; u256_to_be32(bytes, value); return to_hex(bytes, 32);
}
std::string canonical_hex(const std::string &value) {
    if (value.empty()) return "-";
    u256 parsed;
    return hex_to_u256(value.c_str(), parsed) ? key_hex(parsed) : value;
}
std::string checkpoint_identity() {
    // Version the mapping from ordinal to keys, not the GPU arithmetic/backend.
    std::ostringstream out;
    out << (unique_blocks_mode() ? "PubKEYS-balanced-shapes-checkpoint-1\n" : "PubKEYS-traversal-2\n") << g_cfg.mode << ' '
        << g_cfg.range_start_bit << ' ' << g_cfg.range_end_bit << ' '
        << (unique_blocks_mode() ? traversal_block_bits() :
            (g_cfg.mode == TRAV_HYBRID ? 0 : g_cfg.chunk_bits)) << ' ' << g_cfg.det_hex << ' ' << g_cfg.want_nk << '\n'
        << key_hex(g_range_start) << '\n' << key_hex(g_range_end) << '\n';
    if (mask_active()) out << g_cfg.mask_froz << '\n' << canonical_hex(g_cfg.mask_hex)
        << '\n' << key_hex(g_mask_step) << '\n' << key_hex(g_mask_first)
        << '\n' << key_hex(g_mask_total) << '\n';
    else out << "no-mask\n";
    if (unique_blocks_mode()) {
        // The seed is restored from the checksummed payload, so the file can
        // be located before its saved seed is known. All mapping parameters
        // other than that seed are bound to this new namespace/identity.
        out << "balanced-whole-pattern-rounds-v1 admitted-top-v1 exact-complement-v1 "
            << traversal_block_bits() << '\n';
        if (g_cfg.mode == TRAV_WAVE) out << canonical_hex(g_cfg.wave_start) << '\n';
    }
    if (g_cfg.mode == TRAV_HYBRID) out << "hybrid-prefix1 " << g_cfg.hybrid_nibbles << '\n';
    out << (checkpoint_identity_ready ? checkpoint_target_hash : "unprepared") << '\n';
    const std::string data = out.str(); uint8_t digest[32];
    sha256((const uint8_t *)data.data(), data.size(), digest);
    return to_hex(digest, 32);
}
bool checkpoint_limit(u256 limit) {
    if (g_cfg.mode != TRAV_SEQ && g_cfg.mode != TRAV_KXE && !unique_blocks_mode()) return false;
    if (unique_blocks_mode()) { fe_from_be32(limit, g_total_blocks256); return true; }
    const int block_bits = traversal_block_bits();
    if (block_bits < 0 || block_bits > 60 ||
        (!unique_blocks_mode() && (block_bits < 10 || block_bits > 40))
        || u256_cmp(g_range_start, g_range_end) > 0) return false;
    u256 items;
    if (!mask_active()) {
        // ceil((H-L+1)/B) == floor((H-L)/B)+1. This form also
        // represents the block count of the synthetic full 256-bit domain.
        u256_sub(items, g_range_end, g_range_start);
        u256_shr_bits(limit, items, block_bits);
        u256_add_u64(limit, limit, 1);
        return true;
    }
    u256_set(items, g_mask_total);
    u256_shr_bits(limit, items, block_bits);
    if (items[0] & ((UINT64_C(1) << block_bits) - 1)) u256_add_u64(limit, limit, 1);
    return true;
}
bool checkpoint_position_valid(const ChunkIdx &seq) {
    u256 limit, position; seq.to_u256(position);
    return checkpoint_limit(limit) && u256_cmp(position, limit) <= 0;
}
bool shape_checkpoint_count_valid(const ChunkIdx &seq, const u256 confirmed) {
    u256 span, limit, position;
    if (!checkpoint_limit(limit)) return false;
    u256_sub(span, g_range_end, g_range_start);u256_add_u64(span, span, 1);
    const bool overflow = !(span[0] || span[1] || span[2] || span[3]);
    if ((!overflow && u256_cmp(confirmed, span) > 0)
        || (seq == ChunkIdx{} && (confirmed[0] || confirmed[1] || confirmed[2] || confirmed[3]))) return false;
    seq.to_u256(position);
    return u256_cmp(position, limit) != 0 || (!overflow && u256_cmp(confirmed, span) == 0);
}
std::string checkpoint_payload(const ChunkIdx &seq, uint64_t seed, const u256 completed = nullptr) {
    // The following SHA-256 checksum detects corruption and configuration
    // mismatch; it is not authentication against a writer of this directory.
    // Checkpoint files and executable sources require a trusted workspace.
    if (unique_blocks_mode()) {
        if (!completed) throw std::logic_error("Missing confirmed shape counter");
        return std::string("PKHSCP1\nidentity ") + checkpoint_identity()
            + "\nsequence " + seq.hex() + "\ntraversal_seed " + std::to_string(seed)
            + "\nconfirmed_keys " + key_hex(completed) + "\n";
    }
    // Keep the established sequential/KXE serialization byte-for-byte.
    return std::string("PKHCP2\nidentity ") + checkpoint_identity()
        + "\nsequence " + seq.hex() + "\nwave_seed " + std::to_string(seed) + "\n";
}
void checkpoint_rejected(const std::string &path, const char *why) {
    std::cerr << "  Чекпоинт " << path << " не применяется: " << why
              << ".\n  Счёт начнётся заново; прежний файл сохранён.\n";
}
} // namespace

std::string checkpoint_search_identity() { return checkpoint_identity(); }

void checkpoint_clear_loaded_shape() { loaded_shape_checkpoint = LoadedShapeCheckpoint{}; }
bool checkpoint_loaded_shape(ChunkIdx &seq, u256 completed, uint64_t *seed) {
    if (!loaded_shape_checkpoint.valid || !unique_blocks_mode()
        || loaded_shape_checkpoint.identity != checkpoint_identity()
        || loaded_shape_checkpoint.seed != wave_get_seed()) return false;
    seq = loaded_shape_checkpoint.sequence;
    u256_set(completed, loaded_shape_checkpoint.completed);
    if (seed) *seed = loaded_shape_checkpoint.seed;
    return true;
}

bool checkpoint_prepare_identity(const PubDB &db, std::string &why) {
    checkpoint_identity_ready = false; checkpoint_target_hash.clear(); why.clear();
    if (db.items.empty()) { why = "пустая база целей"; return false; }
    CheckpointHash hash;
    static const uint8_t tag[] = "PubKEYS-effective-targets-v1";
    hash.add(tag, sizeof tag - 1);
    uint64_t number = 0;
    for (const PubEntry &entry : db.items) {
        if ((number++ & 4095) == 0 && g_stop.load(std::memory_order_relaxed)) {
            why = "остановлено при проверке базы"; return false;
        }
        if (entry.par < 1 || entry.par > 3) { why = "неверная маска чётности цели"; return false; }
        uint8_t bytes[33]; u256_to_be32(bytes, entry.x); bytes[32] = entry.par;
        hash.add(bytes, sizeof bytes);
    }
    checkpoint_target_hash = hash.finish(); checkpoint_identity_ready = true;
    return true;
}

bool checkpoint_completed_keys(const ChunkIdx &seq, u256 out) {
    // Shaped ordinals include clipped-out windows. Their real checked count is
    // retained separately for in-process digit rotation, never ordinal*block.
    if (unique_blocks_mode()) {
        ChunkIdx loaded;
        if (checkpoint_loaded_shape(loaded, out) && loaded == seq) return true;
        ChunkIdx current;
        if (checkpoint_snapshot(g_search, current, out) && current == seq) return true;
        u256_zero(out); return false;
    }
    if (g_cfg.mode != TRAV_SEQ && g_cfg.mode != TRAV_KXE) return false;
    u256 limit, position; seq.to_u256(position);
    if (!checkpoint_limit(limit) || u256_cmp(position, limit) > 0) return false;
    u256 items;
    if (mask_active()) u256_set(items, g_mask_total);
    else { u256_sub(items, g_range_end, g_range_start); u256_add_u64(items, items, 1); }
    // 2^256 items cannot be returned in u256. Earlier prefixes still fit;
    // only the completed endpoint is unrepresentable in that synthetic case.
    const bool items_overflow = !(items[0] | items[1] | items[2] | items[3]);
    if (u256_cmp(position, limit) == 0) {
        if (items_overflow) return false;
        u256_set(out, items); return true;
    }
    const int block_bits = traversal_block_bits();
    u256_shl_bits(out, position, block_bits);
    const uint64_t chunk = UINT64_C(1) << block_bits;
    const uint64_t tail = items[0] & (chunk - 1);
    if (g_cfg.mode == TRAV_KXE && tail && permuted_tail_precedes(seq))
        u256_sub_u64(out, out, chunk - tail);
    return true;
}

std::string checkpoint_path() {
    if (!mode_has_checkpoint()) return {};
    // Separate namespace: neither legacy files nor a different target set can
    // be overwritten when starting a new search.
    return std::string("checkpoint_") + mode_tag(g_cfg.mode)
         + (unique_blocks_mode() ? "_balanced_v1_" : "_v2_")
         + checkpoint_identity() + ".dat";
}

// ------------------------ водяной знак завершённости -------------------------
//
// Ниже g_done_below закрыто всё. Выше -- лежат номера тех участков, что уже
// досчитаны, но чьи младшие соседи ещё в работе; как только младший
// закрывается, знак поднимается по этой цепочке вверх.
//
// Множество маленькое по построению: в нём не может быть больше записей,
// чем работает потоков (не считая мига между двумя обновлениями). Мьютекс
// берётся один раз на участок, то есть раз в несколько секунд -- на скорость
// это не влияет никак, а атомарная схема здесь была бы сложнее без выигрыша.
// СОСТОЯНИЕ ЖИВЁТ В КОНТЕКСТЕ, А НЕ ЗДЕСЬ.
//
// Сначала при переносе я оставил файловые статические переменные на месте и
// завёл такие же поля в SearchContext -- получилось два комплекта, из
// которых работал старый, а новый молча простаивал. Ровно тот случай, когда
// «перенёс состояние» выглядит сделанным, а на деле состояние раздвоилось.
// Поэтому здесь ничего своего нет: функции берут контекст доводом.
static void chunk_done_weighted(SearchContext &ctx, const ChunkIdx &idx, uint64_t keys, bool known) {
    std::lock_guard<std::mutex> lk(ctx.mtx_done);
    if (idx < ctx.done_below) return;            // уже учтён (пересчёт после возобновления)
    const auto advance = [&ctx](uint64_t completed) {
        u256 previous; u256_set(previous, ctx.confirmed_items);
        u256_add_u64(ctx.confirmed_items, ctx.confirmed_items, completed);
        if (u256_cmp(ctx.confirmed_items, previous) < 0) ctx.confirmed_items_valid = false;
        ctx.done_below.inc();
    };
    if (idx == ctx.done_below) {
        // The ordinary single-GPU path completes in order. Do not allocate a
        // map node for each logical block just to erase it immediately.
        if (!known && unique_blocks_mode()) ctx.confirmed_items_valid = false;
        advance(keys);
    } else {
        const auto added = ctx.done_ahead.emplace(idx, keys);
        if (!added.second) {
            if (known && added.first->second != keys) ctx.confirmed_items_valid = false;
            return;
        }
        if (!known && unique_blocks_mode()) ctx.confirmed_items_valid = false;
    }
    while (!ctx.done_ahead.empty() && ctx.done_ahead.begin()->first == ctx.done_below) {
        advance(ctx.done_ahead.begin()->second);
        ctx.done_ahead.erase(ctx.done_ahead.begin());
    }
}
void chunk_done(SearchContext &ctx, const ChunkIdx &idx) { chunk_done_weighted(ctx, idx, 0, false); }
void chunk_done(SearchContext &ctx, const ChunkIdx &idx, uint64_t keys) { chunk_done_weighted(ctx, idx, keys, true); }

ChunkIdx checkpoint_seq(SearchContext &ctx) {
    std::lock_guard<std::mutex> lk(ctx.mtx_done);
    return ctx.done_below;
}

void checkpoint_reset(SearchContext &ctx, const ChunkIdx &seq) {
    std::lock_guard<std::mutex> lk(ctx.mtx_done);
    ctx.done_below = seq;
    ctx.done_ahead.clear();
    u256_zero(ctx.confirmed_items);
    ctx.confirmed_items_valid = !unique_blocks_mode() || seq == ChunkIdx{};
}
void checkpoint_reset(SearchContext &ctx, const ChunkIdx &seq, const u256 completed) {
    std::lock_guard<std::mutex> lk(ctx.mtx_done);
    ctx.done_below = seq;ctx.done_ahead.clear();
    u256_set(ctx.confirmed_items, completed);ctx.confirmed_items_valid = true;
}
bool checkpoint_snapshot(SearchContext &ctx, ChunkIdx &seq, u256 completed) {
    std::lock_guard<std::mutex> lk(ctx.mtx_done);
    seq = ctx.done_below;u256_set(completed, ctx.confirmed_items);
    return ctx.confirmed_items_valid;
}

size_t chunks_in_flight(SearchContext &ctx) {
    std::lock_guard<std::mutex> lk(ctx.mtx_done);
    return ctx.done_ahead.size();
}

// ЗАПИСЬ ЧЕКПОИНТА ДОЛЖНА БЫТЬ ЛИБО ЦЕЛОЙ, ЛИБО НИКАКОЙ.
//
// Было: открыли файл по месту и пишем поверх прежнего содержимого. Убьют
// процесс в этот миг -- на диске останется обрубок, а прежняя позиция уже
// затёрта. Разбор при загрузке такой файл отвергнет, и это правильно, но
// накопленное всё равно потеряно: отвергать уже нечего.
//
// Пишем во временный файл, доводим до диска и переименовываем. rename на
// POSIX атомарен: в каталоге либо старый файл, либо новый, промежуточного
// состояния не бывает.
//
// Здесь стояло: «тот же приём давно стоит у кенгуру -- переносим его сюда».
// Неправда, и я же её потом опроверг: у кенгуру были только временный файл и
// rename, без единого fsync. Правильный порядок событий обратный -- сначала
// сделали здесь, потом дописали кенгуру.
//
// fsync перед переименованием не декоративен: без него содержимое может
// осесть в кеше, а имя уже смениться. После внезапного отключения питания
// получится файл нужного имени и нулевой длины.
static bool write_file_atomically(const std::string &path, const std::string &data) {
    const std::string tmp = path + ".tmp";
    {
        FILE *f = fopen(tmp.c_str(), "wb");
        if (!f) return false;
        if (fwrite(data.data(), 1, data.size(), f) != data.size()) {
            fclose(f); std::remove(tmp.c_str()); return false;
        }
        if (fflush(f) != 0) { fclose(f); std::remove(tmp.c_str()); return false; }
        if (fclose(f) != 0) { std::remove(tmp.c_str()); return false; }
        // Сброс на диск ПОСЛЕ закрытия: слой открывает файл сам, потому что
        // способ довести запись до диска у Windows и POSIX разный, а
        // дескриптор здесь всё равно свой у каждой системы.
        if (!plat_fsync_file(tmp)) { std::remove(tmp.c_str()); return false; }
    }
    // plat_replace_file, а не std::rename: на Windows rename отказывается
    // заменять существующий файл, и чекпоинт обновлялся бы только один раз.
    if (!plat_replace_file(tmp, path)) {
        std::remove(tmp.c_str());
        return false;
    }
    // Само переименование тоже живёт в каталоге, и на POSIX каталог тоже
    // кешируется. На Windows такого понятия нет, и слой честно вернёт true,
    // ничего не делая -- это свойство системы, а не упущение.
    return plat_fsync_dir(".");
}

bool mode_has_checkpoint() {
    return g_cfg.mode == TRAV_SEQ || g_cfg.mode == TRAV_KXE || unique_blocks_mode();
}

void save_checkpoint() {
    if (!mode_has_checkpoint()) return;
    if(checkpoint_write_blocked)return;
    if (!checkpoint_identity_ready) {
        checkpoint_rejected(checkpoint_path(), "не подготовлен отпечаток базы");
        return;
    }
    // Сохраняется наименьший НЕзавершённый участок, а не позиция выдачи.
    // Разница принципиальная: всё, что ниже этого номера, досчитано до
    // последнего ключа, и после восстановления пропустить его невозможно.
    // Подробнее -- в traverse.h, у объявления chunk_done.
    ChunkIdx s;
    u256 confirmed{};
    if (unique_blocks_mode()) {
        if (!checkpoint_snapshot(g_search, s, confirmed)) {
            checkpoint_rejected(checkpoint_path(), "неизвестен точный счёт подтверждённого префикса");return;
        }
    } else s = checkpoint_seq(g_search);
    // PKHSCP1 identifies the balanced full-interval shape walk and its exact
    // confirmed count. PKHCP2 sequential/KXE serialization stays unchanged.
    if (!checkpoint_position_valid(s)) {
        checkpoint_rejected(checkpoint_path(), "позиция находится за концом обхода");
        return;
    }
    if (unique_blocks_mode() && !shape_checkpoint_count_valid(s, confirmed)) {
        checkpoint_rejected(checkpoint_path(), "счёт подтверждённых ключей не соответствует позиции");return;
    }
    // Seed is serialized for every mode; zero is a valid seed, not "missing".
    const std::string payload = checkpoint_payload(s, wave_get_seed(), unique_blocks_mode() ? confirmed : nullptr);
    uint8_t digest[32]; sha256((const uint8_t *)payload.data(), payload.size(), digest);
    const std::string data = payload + "checksum " + to_hex(digest, 32) + "\n";
    if (!write_file_atomically(checkpoint_path(), data)) {
        // МОЛЧАТЬ ЗДЕСЬ НЕЛЬЗЯ.
        //
        // Возврат отбрасывался, а отказать write_file_atomically может по
        // пяти причинам, и на Windows все они вероятнее, чем на POSIX: файл
        // на долю секунды захвачен антивирусом или индексатором, MoveFileEx
        // упёрся в ERROR_SHARING_VIOLATION из-за OneDrive. Программа при этом
        // месяц показывает ровную скорость и растущий процент, а на диске
        // лежит позиция от первого успешного сохранения. При перезапуске она
        // объявляет её сохранённой и пересчитывает всё заново.
        //
        // Кричим, но не чаще раза в минуту: при устойчивом отказе иначе
        // зальёт вывод и утопит в нём всё остальное.
        static std::atomic<long long> last_cry{-1000};
        const long long now = (long long)time(nullptr);
        long long prev = last_cry.load();
        if (now - prev >= 60 && last_cry.compare_exchange_strong(prev, now)) {
            std::cerr << "  ВНИМАНИЕ: чекпоинт НЕ сохранён (" << checkpoint_path()
                      << "). Позиция не записывается; при перезапуске счёт\n"
                      << "  пойдёт с прежнего места. Проверьте права на каталог "
                         "и свободное место.\n";
        }
    }
}

// Разбор одного файла чекпоинта с проверкой, что он от нашей конфигурации.
static bool read_checkpoint_file(const std::string &path, ChunkIdx &seq) {
    std::ifstream f(plat_wide(path).c_str(), std::ios::binary);
    if (!f) return false;
    char buffer[1025]; f.read(buffer, sizeof buffer);
    const std::streamsize size = f.gcount();
    auto invalid = [&](const char *reason) { checkpoint_rejected(path, reason); return false; };
    if (size <= 0 || size > 1024 || f.bad()) return invalid("повреждённый или слишком большой файл");
    const std::string data(buffer, (size_t)size);
    std::istringstream fields(data);
    std::string magic, label1, identity, label2, ordinal, label3, seed_text, label4, checksum, extra;
    std::string count_text, checksum_label;
    if (unique_blocks_mode()) {
        if (!(fields >> magic >> label1 >> identity >> label2 >> ordinal >> label3 >> seed_text
              >> label4 >> count_text >> checksum_label >> checksum)
            || fields >> extra || magic != "PKHSCP1" || label1 != "identity" || label2 != "sequence"
            || label3 != "traversal_seed" || label4 != "confirmed_keys" || checksum_label != "checksum")
            return invalid("нет полного проверяемого формата сбалансированного обхода");
    } else if (!(fields >> magic >> label1 >> identity >> label2 >> ordinal >> label3 >> seed_text >> label4 >> checksum)
        || fields >> extra || magic != "PKHCP2" || label1 != "identity" || label2 != "sequence"
        || label3 != "wave_seed" || label4 != "checksum")
        return invalid("нет полного проверяемого формата текущего обхода");
    if (identity != checkpoint_identity()) return invalid("изменились цели или параметры обхода");
    ChunkIdx got;
    if (ordinal.size() != 64 || !got.from_hex_str(ordinal)) return invalid("неверный 256-битный номер участка");
    uint64_t seed = 0;
    if (seed_text.empty()) return invalid("отсутствует зерно обхода");
    for (char digit : seed_text) {
        if (digit < '0' || digit > '9' || seed > (UINT64_MAX - (uint64_t)(digit - '0')) / 10)
            return invalid("неверное беззнаковое зерно обхода");
        seed = seed * 10 + (uint64_t)(digit - '0');
    }
    u256 confirmed{};
    if (unique_blocks_mode()) {
        ChunkIdx parsed_count;
        if (count_text.size() != 64 || !parsed_count.from_hex_str(count_text))
            return invalid("неверное 256-битное число подтверждённых ключей");
        parsed_count.to_u256(confirmed);
    }
    const std::string payload = checkpoint_payload(got, seed, unique_blocks_mode() ? confirmed : nullptr);
    uint8_t expected[32]; sha256((const uint8_t *)payload.data(), payload.size(), expected);
    if (checksum != to_hex(expected, 32)) return invalid("контрольная сумма не совпала");
    if (!checkpoint_position_valid(got)) return invalid("номер находится за концом обхода");
    if (unique_blocks_mode()) {
        if (!shape_checkpoint_count_valid(got, confirmed))
            return invalid("счёт подтверждённых ключей не соответствует позиции");
        // The balanced domain size is seed-independent. Only after complete
        // validation may we restore the seed and rebuild the actual mapping.
        wave_set_seed(seed);compute_total_blocks256();
        loaded_shape_checkpoint.valid = true;loaded_shape_checkpoint.sequence = got;
        u256_set(loaded_shape_checkpoint.completed, confirmed);loaded_shape_checkpoint.seed = seed;
        loaded_shape_checkpoint.identity = identity;
    }
    seq = got;
    return true;
}

bool load_checkpoint(ChunkIdx &seq) {
    checkpoint_clear_loaded_shape();
    if (!mode_has_checkpoint()) return false;
    if (!checkpoint_identity_ready) {
        checkpoint_rejected(checkpoint_path(), "не подготовлен отпечаток загруженной базы");
        return false;
    }
    const std::string current = checkpoint_path();
    { std::ifstream f(plat_wide(current).c_str(), std::ios::binary);
      if (f) {
          f.close();
          if(read_checkpoint_file(current,seq))return true;
          const std::string saved=current+".rejected_"+
              std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
          std::error_code ec;
          const auto source=std::filesystem::u8path(current), target=std::filesystem::u8path(saved);
          if(std::filesystem::exists(target,ec))ec=std::make_error_code(std::errc::file_exists);
          else if(!ec)std::filesystem::rename(source,target,ec);
          if(ec) {
              checkpoint_write_blocked=true;set_stop();
              std::cerr<<"  Не удалось сохранить отклонённый чекпоинт отдельно: "<<ec.message()
                       <<". Поиск остановлен; запись поверх этого файла запрещена.\n";
          } else std::cerr<<"  Отклонённый чекпоинт сохранён: "<<saved<<"\n";
          return false;
      } }
    // Old formats lack the effective target set, full mask and algorithm version.
    // They cannot establish that the skipped candidates were checked for this job.
    for (const std::string &old : {legacy_checkpoint_path(), std::string("checkpoint.dat")}) {
        std::ifstream f(plat_wide(old).c_str(), std::ios::binary);
        if (f) checkpoint_rejected(old, "старый формат не подтверждает совпадение базы и параметров");
    }
    return false;
}
// ========================================================

// ============================================================================
//  ЖУРНАЛ ЗОНАЛЬНОГО РЕЖИМА
//
//  По одному ключу нельзя понять, верно ли работает обход. Нужно видеть,
//  ЧТО именно изменилось: от какой базы, какой разряд, на какое значение.
//  Поэтому в записи стоят номер пакета, цикл, зона и смещение, а сами ключи
//  печатаются с выделенным нибблом — тем самым, который зона и меняет.
//  BASE и CURR обязаны отличаться ровно одним знаком, в скобках.
// ============================================================================
static std::string hex_mark_nibble(const u256 k, int pos) {
    uint8_t b[32];
    u256_to_be32(b, k);
    std::string h = to_hex(b, 32);
    if (pos < 0 || pos > 63) return h;
    return h.substr(0, pos) + "[" + h.substr(pos, 1) + "]" + h.substr(pos + 1);
}

// ============================================================================
//  ЖУРНАЛ ВОЛНОВОГО РЕЖИМА
//
//  Формат повторяет то, как узор рисуется от руки: Baza -- ключ старта
//  волны, Curr -- текущий, и в обоих квадратными скобками выделены разряды,
//  которые в этой фазе крутятся. Сколько скобок -- столько разрядов в волне,
//  это видно с одного взгляда.
//
//      Baza 884222...6148fb1[3]
//      Curr 884222...6148fb[2][4]      фаза 2: два разряда
//      Curr 884222...6148f[c][1][c]    фаза 3: три разряда
// ============================================================================
// Выделяет ровно те разряды, которые крутятся в этой фазе.
//
// Волна начинается НЕ с последнего разряда, а через три от него: младшие три
// перебирает сам блок из 4096 ключей. Первая версия помечала последние width
// знаков -- и скобки оказывались на разрядах `b14`, которые как раз и не
// меняются. Со стороны выглядело так, будто волна крутит не то.
// Сколько младших разрядов перебирает сам блок -- их волна не трогает.
// Раньше здесь стояла копия в static const int. Теперь размер блока ставится
// при запуске (set_block_nibbles), а статическая копия сняла бы с него слепок
// ДО этого -- журнал рисовал бы скобки по трём нибблам, когда блок занимает
// восемь. Берём значение в момент записи строки.

// Скобки ставятся на РАБОТАЮЩИЕ разряды -- с того конца, с которого идёт
// фронт. Пока ход был один, конец подразумевался младший и параметра не
// требовалось. С зеркалом подразумевать больше нечего: не передай сюда
// направление -- и журнал показывал бы скобки не там, где программа считает.
static std::string hex_mark_wave(const u256 k, int width, bool high, int skip_low, int top) {
    uint8_t b[32];
    u256_to_be32(b, k);
    std::string h = to_hex(b, 32);
    if (width <= 0 || skip_low < 0 || skip_low > 64 || top < 0 || top > 63) return h;
    if (width > 64 - skip_low) width = 64 - skip_low;
    if (width <= 0) return h;
    // Разряды нумеруются от младшего; в строке младший стоит справа.
    // Фронт от младшего конца -- крайние правые (не считая пропущенных),
    // от старшего -- крайние левые.
    const int hi = high ? 63-top+width : (64 - skip_low);
    const int lo = hi-width;
    if (lo < 0 || hi < lo || hi > 64) return h;
    std::string out = h.substr(0, lo);
    for (int i = lo; i < hi; i++) out += "[" + h.substr(i, 1) + "]";
    out += h.substr(hi);
    return out;
}

void debug_log_wave(int /*thread*/, const ChunkIdx &idx, const u256 key,
                    const WaveInfo &info, uint64_t count) {
    if (g_dbg_written.load(std::memory_order_relaxed) > DBG_LIMIT) return;
    std::lock_guard<std::mutex> lk(g_mtx_dbg);
    const uint64_t seen = g_dbg_written.fetch_add(1);
    if (seen > DBG_LIMIT) return;
    if (seen == DBG_LIMIT) {
        std::ofstream f("debug_chunks.txt", std::ios::app);
        if (f) {
            secure_create("debug_chunks.txt");
            f << "===== предел " << DBG_LIMIT
              << " записей достигнут, журнал остановлен =====\n";
        }
        return;
    }

    static bool header_done = false;
    std::ofstream f("debug_chunks.txt",
                    header_done ? std::ios::app : std::ios::trunc);
    if (!f) return;
    secure_create("debug_chunks.txt");
    if (!header_done) {
        header_done = true;
        f << "===== запуск " << now_string() << " | режим волновой"
          << " | диапазон 2^" << g_cfg.range_start_bit
          << "..2^" << g_cfg.range_end_bit << " =====\n"
          << "Исходная растущая волна: один разряд, затем два вместе, затем три.\n"
          << "  Каждый узор полный; следующее семейство баз исключает пересечения.\n"
          << "  Только изменяемые позиции выбранного диапазона; младшие " << g_skip_low << " проверяет блок.\n"
          << "В скобках -- разряды, работающие в этой фазе.\n"
          << "Переворот f -> 0 идёт БЕЗ переноса в соседний разряд.\n\n";
    }

    f << "участок " << idx.hex();
    if (!unique_blocks_mode()) f << "  старт " << info.start_no;
    if (info.phase == 0) f << "  (сам ключ старта)";
    else f << "  фаза " << info.phase << " (разрядов " << info.width << ")"
           << "  шаг " << info.step << " из " << info.steps;
    f << "  ключей " << count << "\n";

    // Baza -- база ИМЕННО ЭТОЙ фазы, то есть её нулевой шаг. Показывать здесь
    // старт всей волны было бы враньём: с фазы 2 они расходятся.
    // Ширина, а не номер фазы: с челноком они разошлись. Фаза 62 -- это уже
    // 60 разрядов, а не 62; печатать номер вместо ширины значило бы ставить
    // скобки не по тому числу.
    if (unique_blocks_mode()) {
        uint8_t origin[32]; u256_to_be32(origin, info.start);
        f << "Начало полного узора " << to_hex(origin, 32) << "\n";
    }
    f << "Baza " << hex_mark_wave(info.base, info.width, info.high, g_skip_low, info.top) << "\n";
    if (info.phase > 0)
        f << "Curr " << hex_mark_wave(key, info.width, info.high, g_skip_low, info.top) << "\n";

    if (count) {
        u256 last; u256_add_u64(last, key, count - 1);
        f << "ПОСЛ.: " << hex_mark_wave(last, info.width, info.high, g_skip_low, info.top)
          << "   (блок перебирает всё между Curr и этой строкой)\n";
    }

    Point p = ec_mul_G(key);
    if (!p.inf) {
        uint8_t xb[32], c33[33];
        fe_to_be32(xb, p.x);
        c33[0] = (uint8_t)(0x02 | (p.y[0] & 1));
        memcpy(c33 + 1, xb, 32);
        f << "  PUBLIC KEY : " << to_hex(c33, 33) << "   (для Curr)\n";
    }
    f << "\n";
}

