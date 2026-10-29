// ============================================================================
//  targets.cpp — объекты базы и передача целей в C-модуль.
// ============================================================================
#include "targets.h"
#include <climits>
#include <cctype>
#include <iostream>
#ifdef USE_SECP
  #include "scan_secp.h"
#endif
#ifdef PKH_HAVE_CUDA
  #include "engine.h"      // g_gpu_active
  #include "cuda_scan.h"
  #include <iostream>
#endif

// g_db -- ссылка на поле SearchContext, определена в search.cpp.

// Формат предфильтра в C-модуле тот же, что и здесь: индексом служат
// старшие биты X. Поэтому карту можно отдать как есть, без перевода.
bool push_targets_to_backend() {
    if (g_db.items.empty() || g_db.items.size() > (size_t)INT_MAX) {
        std::cerr << "  Недопустимое число целей; поиск не запущен.\n";
        set_stop();
        return false;
    }
#ifdef USE_SECP
    // With a Bloom filter the CPU backend keeps only its pointer: it never
    // reads x32. Serialize only the at most eight direct-comparison targets.
    uint8_t xs[8 * 32] = {};
    if (g_db.bloom.empty()) {
        for (size_t i = 0; i < g_db.items.size() && i < 8; i++)
            fe_to_be32(&xs[i * 32], g_db.items[i].x);
    }
    if (!scan_secp_set_targets(g_db.bloom.empty() ? xs : nullptr, (int)g_db.items.size(),
                               g_db.bloom.empty() ? nullptr : g_db.bloom.data(),
                               g_db.block_mask)) {
        std::cerr << "  Не удалось загрузить цели в CPU-бэкенд; поиск остановлен.\n";
        set_stop();
        return false;
    }
#endif

#ifdef PKH_HAVE_CUDA
    // ТЕ ЖЕ ЦЕЛИ УХОДЯТ НА КАРТУ.
    //
    // Пока цели передаются сырыми -- по 32 байта на X. Это осознанный предел
    // нынешнего интерфейса (cuda_scan_set_targets), и он держится примерно до
    // сотни миллионов целей: миллиард потребовал бы 32 ГБ видеопамяти, чего
    // нет ни на одной карте. Для такой базы на карту нужно класть предфильтр,
    // а не цели, -- это отдельная работа, и она не в этом файле.
    if (g_gpu_active) {
        // Фильтр отдаётся как есть: формат у карты и у процессора один и тот
        // же файл bloom.h, поэтому переводить нечего. Если фильтра нет (целей
        // единицы), уходят сами цели -- карта сравнит их перебором.
        const bool have_bloom = !g_db.bloom.empty();
        static std::vector<uint8_t> gxs;
        if (!have_bloom) {
            gxs.assign(g_db.items.size() * 32, 0);
            for (size_t i = 0; i < g_db.items.size(); i++)
                fe_to_be32(&gxs[i * 32], g_db.items[i].x);
        }

        char why[256] = {0};
        if (!cuda_scan_set_targets(have_bloom ? nullptr : gxs.data(),
                                   (int)g_db.items.size(),
                                   have_bloom ? g_db.bloom.data() : nullptr,
                                   g_db.block_mask,
                                   why, sizeof(why))) {
            std::cout << "  Карта: не удалось загрузить цели -- "
                      << (why[0] ? why : "причина не сообщена") << "\n"
                      << "  Счёт пойдёт на процессоре.\n";
            gpu_backend_stop();
        } else {
            if (have_bloom)
                std::cout << "  Фильтр на карте  : " << g_db.items.size()
                          << " целей, "
                          << (g_db.bloom.size() * sizeof(uint64_t) / (1024 * 1024))
                          << " МБ видеопамяти (сами цели карте не нужны)\n";
            else
                std::cout << "  Цели на карте    : " << g_db.items.size() << "\n";
        }
    }
#endif
    return true;
}

// Shared strict SEC1 public-key parser; independent of any search method.
namespace {
static void fe_sqrt_power(fe r, const fe a) {
    fe x2, x3, x6, x9, x11, x22, x44, x88, x176, x220, x223, t;
    auto shift_mul = [](fe out, const fe src, int count, const fe factor) {
        fe_set(out, src);
        for (int i = 0; i < count; ++i) fe_sqr(out, out);
        fe_mul(out, out, factor);
    };
    fe_sqr(x2, a); fe_mul(x2, x2, a);
    fe_sqr(x3, x2); fe_mul(x3, x3, a);
    shift_mul(x6, x3, 3, x3); shift_mul(x9, x6, 3, x3);
    shift_mul(x11, x9, 2, x2); shift_mul(x22, x11, 11, x11);
    shift_mul(x44, x22, 22, x22); shift_mul(x88, x44, 44, x44);
    shift_mul(x176, x88, 88, x88); shift_mul(x220, x176, 44, x44);
    shift_mul(x223, x220, 3, x3); shift_mul(t, x223, 23, x22);
    shift_mul(t, t, 6, x2); fe_sqr(t, t); fe_sqr(r, t);
}

static bool fe_from_be32_checked(fe r, const uint8_t in[32]) {
    fe_from_be32(r, in);
    return u256_cmp(r, FE_P) < 0;
}

// y^2 == x^3 + 7 ?
static bool on_curve(const fe x, const fe y) {
    fe l, rr, seven;
    fe_sqr(l, y);                    // y^2
    fe_sqr(rr, x);
    fe_mul(rr, rr, x);               // x^3
    fe_zero(seven); seven[0] = 7;
    fe_add(rr, rr, seven);
    return fe_eq(l, rr);
}

static bool hex_bytes(const std::string &h, size_t off, uint8_t *out, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int hi = 0, lo = 0;
        char a = h[off + i * 2], b = h[off + i * 2 + 1];
        auto dig = [](char c, int &v) {
            if (c >= '0' && c <= '9') { v = c - '0'; return true; }
            if (c >= 'a' && c <= 'f') { v = c - 'a' + 10; return true; }
            return false;
        };
        if (!dig(a, hi) || !dig(b, lo)) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}


} // namespace

bool point_from_pubkey(const std::string &hex, Point &out) {
    // РАЗБОР СТРОГИЙ.
    //
    // Раньше из строки выбрасывалось всё, что не hex-цифра. Пользователь мог
    // вставить обрывок текста или ключ с переносом строки посередине, и
    // программа молча склеивала остатки в «публичный ключ», который потом
    // искала часами. Теперь: пробелы по краям и необязательный префикс 0x
    // снимаются, а всё остальное содержимое обязано быть шестнадцатеричным.
    std::string h = trim(hex);
    if (h.size() > 2 && h[0] == '0' && (h[1] == 'x' || h[1] == 'X')) h = h.substr(2);
    for (char &c : h) c = (char)tolower((unsigned char)c);
    for (char c : h)
        if (!isxdigit((unsigned char)c)) return false;

    if (h.size() == 130 && h.compare(0, 2, "04") == 0) {
        uint8_t bx[32], by[32];
        if (!hex_bytes(h, 2, bx, 32) || !hex_bytes(h, 66, by, 32)) return false;

        // ТЕ ЖЕ ПРОВЕРКИ, ЧТО И ДЛЯ СЖАТОГО КЛЮЧА.
        //
        // Для 02/03 программа считала корень и сверяла его квадрат, то есть
        // заведомо получала точку кривой. Для 04 она принимала X и Y как есть:
        // ни X < p, ни Y < p, ни y^2 == x^3 + 7 не проверялось. Ключ с опечаткой
        // проходил, поиск запускался и не мог найти ничего никогда -- при
        // этом выглядело это как «не повезло», а не как ошибка ввода.
        if (!fe_from_be32_checked(out.x, bx)) return false;   // X >= p
        if (!fe_from_be32_checked(out.y, by)) return false;   // Y >= p
        if (!on_curve(out.x, out.y)) return false;            // не на кривой
        if (fe_is_zero(out.x) && fe_is_zero(out.y)) return false;
        out.inf = false;
        return true;
    }
    if (h.size() == 66 && (h.compare(0, 2, "02") == 0 || h.compare(0, 2, "03") == 0)) {
        uint8_t b[32];
        if (!hex_bytes(h, 2, b, 32)) return false;
        if (!fe_from_be32_checked(out.x, b)) return false;     // X >= p

        fe t, y;
        fe_sqr(t, out.x);
        fe_mul(t, t, out.x);                       // x^3
        fe seven; fe_zero(seven); seven[0] = 7;
        fe_add(t, t, seven);                       // x^3 + 7

        fe_sqrt_power(y, t);

        fe chk; fe_sqr(chk, y);
        if (!fe_eq(chk, t)) return false;           // X не лежит на кривой

        int want = (h[1] == '3') ? 1 : 0;
        if ((int)(y[0] & 1) != want) fe_neg(y, y);
        fe_set(out.y, y);
        out.inf = false;
        return true;
    }
    return false;
}

