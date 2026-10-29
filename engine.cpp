#include "minute_speed.h"
// ============================================================================
//  engine.cpp — горячий цикл: пакет точек и проверка по базе.
// ============================================================================
#include "engine.h"
#include "targets.h"
#include "notify.h"
#include "platform.h"
#include "traverse.h"   // sample_offset: общее смещение образца для экрана и журнала
#include "packed_traversal.h"
#include "search_range.h"
#include "cuda_scalar_guard.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <iostream>
#include <vector>
#include <atomic>
#include <chrono>
#include <thread>
#include "precise_sleep.h"
#ifdef USE_SECP
  #include "scan_secp.h"
#endif
#ifdef PKH_HAVE_CUDA
  #include "gpu_info.h"   // опрос карты и расчёт раскладки запуска
  #include "gpu_startup_check.h"
#endif

// ---------------------------- таблицы пакета --------------------------------
// Пакет симметричен: от базы P = k*G берутся сразу P+iG и P-iG.
// Разность абсцисс у них общая (x_iG - x_P), потому что -iG = (x_iG, -y_iG),
// то есть на две точки приходится одно обращение вместо двух.
// РАЗМЕР ПАКЕТА У ОБОИХ BACKEND'ОВ ОДИН.
//
// Было SPAN = 2*HALF + 1 = 1025: центр, 512 вверх и 512 вниз. А scan_secp.c
// специально пропускает верхний край (`if (i + 1 < SECP_HALF)`) и даёт ровно
// 2*HALF = 1024. То есть `make own` и `make secp` сканировали РАЗНЫЕ множества
// одним и тем же вызовом scan_chunk():
//
//   * 1025 не делит участок 2^22, поэтому в своей математике каждый участок
//     заканчивался неполным пакетом, который дочитывал начало следующего --
//     систематический нахлёст на каждой границе;
//   * g_count рос на 1025 за пакет, то есть быстрее, чем реально пройдено
//     ключей по заданию, и ошибка копилась линейно;
//   * «make own -- то же самое, только медленнее» переставало быть правдой.
//
// Теперь пакет покрывает [c-HALF, c+HALF-1] в обеих реализациях: центр, 511
// вверх и 512 вниз. При центре c = k0 + HALF это ровно [k0, k0+1023].
extern const int HALF = 512;
extern const int SPAN = 2 * HALF;       // ключей за пакет, ровно как в scan_secp.c
Point GT[HALF];                   // GT[i] = (i+1)*Q
Point GSTEP;                      // = SPAN*Q — сдвиг базы

// ============================================================================
//  ШАГ ПЕРЕБОРА
//
//  Пакет накрывает 2*HALF ключей, идущих с постоянным шагом S. Пока S был
//  единицей, шаг нигде не выписывался: он был вшит в то, что таблица строится
//  от G. HEX-маска морозит младшие ниббли, соседние кандидаты отличаются на
//  16^N, и подряд они не идут. Прогрессией быть не перестают -- а пакетному
//  сложению больше ничего и не нужно.
//
//  S = 1 даёт Q = G и прежние таблицы байт в байт. Это и есть проверка на
//  отсутствие регресса: все прежние тесты идут именно по этому пути.
// ============================================================================
u256 g_step;        // S
u256 g_step_half;   // HALF*S -- половина ширины пакета В КЛЮЧАХ
u256 g_step_span;   // SPAN*S -- весь пакет в ключах
static bool g_batch_step_safe = true;

// Возвращает false, если произведение не влезло в 256 бит.
//
// Прежде перенос молча отбрасывался. При mask_froz >= 62 шаг равен 2^248, и
// SPAN*S = 2^258 обрезается в НОЛЬ: центр пакета совпадает с началом участка,
// а k += g_step_span не двигает базу вовсе -- один и тот же пакет считался бы
// nfull раз. Теперь g_batch_step_safe явно направляет такой участок в
// scan_slow_range для обоих backend: одного предупреждения недостаточно.
static bool mul_u256_small(u256 out, const u256 a, uint64_t m) {
    u128 carry = 0;
    for (int i = 0; i < 4; i++) {
        u128 v = (u128)a[i] * m + carry;
        out[i] = (uint64_t)v;
        carry = v >> 64;
    }
    return carry == 0;
}

void set_scan_step(const u256 s) {
    u256_set(g_step, s);
    const bool fits = mul_u256_small(g_step_half, g_step, (uint64_t)HALF)
                    & mul_u256_small(g_step_span, g_step, (uint64_t)SPAN);
    g_batch_step_safe = fits;
    if (!fits) {
        // Говорим вслух, а не молчим: пакетный путь при таком шаге неприменим,
        // и человек должен знать, почему скорость упала на три порядка.
        std::cout << "  Шаг перебора слишком велик для пакетного пути "
                     "(SPAN*шаг не помещается в 256 бит).\n"
                     "  Участки будут проходиться поштучно.\n";
    }
#ifdef USE_SECP
    uint8_t sb[32];
    u256_to_be32(sb, g_step);
    if (!scan_secp_set_step(sb)) {
        g_batch_step_safe = false;
        std::cout << "  libsecp256k1: шаг перебора отвергнут "
                     "(нулевой либо не помещается в 256 бит).\n";
    }
#endif
#ifdef PKH_HAVE_CUDA
    gpu_backend_push_step();
#endif
    build_table();
}

void build_table() {
    ec_build_comb();
    // Первый вызов приходит до set_scan_step -- шаг тогда единичный.
    { bool zero = true;
      for (int i = 0; i < 4; i++) if (g_step[i]) { zero = false; break; }
      if (zero) { u256_from_u64(g_step, 1);
                  mul_u256_small(g_step_half, g_step, (uint64_t)HALF);
                  mul_u256_small(g_step_span, g_step, (uint64_t)SPAN); } }

    Point q = ec_mul_G(g_step);           // Q = S*G; при S=1 это ровно G
    GT[0] = q;
    for (int i = 1; i < HALF; i++) GT[i] = ec_add(GT[i - 1], q);
    // SPAN*Q = удвоенный HALF*Q. Раньше считалось как ec_mul_G(SPAN); со
    // свободным шагом скаляр SPAN*S мог бы не влезть в 256 бит, а удвоение
    // точки от разрядности скаляра не зависит.
    GSTEP = ec_add(GT[HALF - 1], GT[HALF - 1]);
}

// Глубина конвейера предвыборки: на сколько ключей вперёд запрашивается
// строка кэша. Меньше — не успевает прийти, больше — кольцо вытесняет из L1
// то, что нужно арифметике. 12 подобрано замером.
#ifndef PF_DEPTH
#define PF_DEPTH 12
#endif
#ifndef USE_SECP
// При сборке с libsecp256k1 горячий цикл идёт через scan_secp.c, а эта
// ветка не компилируется в дело -- иначе компилятор справедливо ругается на
// неиспользуемую константу.
static const int PF = PF_DEPTH;
#endif

// Границы и число блоков теперь поля SearchContext (search.h).
uint64_t g_total_chunks = 0;             // 0 = не помещается в 64 бита

// ГОРЯЧИЙ ЦИКЛ
//
// Сравнивается только абсцисса X. Ордината Y в цикле не считается вовсе:
// она нужна лишь для того, чтобы отличить ключ k от зеркального n-k, а это
// выясняется один раз, уже после попадания. Экономия — одно умножение на
// ключ из примерно четырёх.
//
// Приватный ключ внутри пакета тоже не вычисляется: индекс и знак дают его
// мгновенно, когда попадание уже случилось.
// ========================================================

// Годен ли ключ: ноль и всё, что не меньше порядка группы n, приватным
// ключом быть не может. Ключу k >= n отвечает та же точка, что и k-n, но WIF
// для него недействителен -- сообщать о такой находке нельзя.
static inline bool key_is_valid(const u256 k) {
    if (!k[0] && !k[1] && !k[2] && !k[3]) return false;
    return u256_cmp(k, EC_N) < 0;
}

// КЛЮЧ ВНУТРИ ЗАКАЗАННОГО ДИАПАЗОНА?
//
// Вторая, независимая проверка перед сообщением о находке. Раньше последним
// рубежом был только key_is_valid(), который знает про порядок группы n и
// ничего не знает про [g_range_start, g_range_end]: стоило горячему циклу
// выйти за край участка -- и находка за пределами заданного диапазона
// сообщалась бы как своя.
//
// Сам выход за край теперь закрыт в scan_chunk_pub (пакеты не выезжают за
// заказанное число ключей), поэтому здесь эта проверка не должна срабатывать
// никогда. Она и стоит именно поэтому: если вверху по течению что-то
// сломается снова, ошибка упрётся в неё, а не попадёт в Found.txt.
static inline bool key_in_search_range(const u256 k) {
    return u256_cmp(k, g_range_start) >= 0 && u256_cmp(k, g_range_end) <= 0;
}

// X(k) == X(n-k), so the cheap filter cannot restrict mirror parity.
// Keep the generated source scalar in range; an enabled n-k match may be outside.
static void confirm_in_search_range(const u256 key, const fe px, const fe py, int parity) {
    const int mask = g_db.confirm_mask(px, parity);
    if ((mask & 1) && search_match_allowed(key, 1)) report_found(key, px, py, 1);
    if ((mask & 2) && search_match_allowed(key, 2)) report_found(key, px, py, 2);
}

// ЧЕСТНЫЙ ПОШТУЧНЫЙ ПРОСМОТР.
//
// Нужен в двух местах, и оба -- края:
//   * хвост участка, который не делится на пакет нацело (последний участок
//     диапазона). Раньше он дочитывался целым пакетом, то есть заезжал за
//     g_range_end на сотни тысяч ключей;
//   * вырожденный пакет, где центр ближе к нулю или к n, чем размер пакета:
//     разность абсцисс обращается в ноль, пакетная формула неприменима.
//     В scan_secp.c это разобрано, в своей математике -- не было: нулевая
//     разность подменялась единицей, абсциссы выходили мусором, и ключи
//     такого пакета молча НЕ проверялись.
//
// Стоит одно скалярное умножение на ключ, но вызывается не более одного раза
// на участок (и на пару пакетов у краёв кривой) -- на скорость не влияет.
void scan_slow_range(const u256 from, uint64_t n, bool count) {
    if (n == 0) return;
    u256 k; u256_set(k, from);
    uint64_t done = 0;
    for (; done < n && !g_stop.load(std::memory_order_relaxed); done++) {
        if ((done & 255) == 0) { pause_check(); if (g_stop.load(std::memory_order_relaxed)) break; }
        if (key_is_valid(k) && key_in_search_range(k)) {
            Point p = ec_mul_G(k);
            if (!p.inf) {
                confirm_in_search_range(k, p.x, p.y, (int)(p.y[0] & 1));
            }
        }
        // Шаг, а не единица: при HEX-маске соседний кандидат отстоит на 16^N.
        u256 t; u256_add(t, k, g_step); u256_set(k, t);
    }
    if (count) g_count.fetch_add(done, std::memory_order_relaxed);
}

// Холодный путь: сюда попадаем в среднем реже одного раза за миллиарды ключей.
void slow_confirm(const u256 center, int idx, bool plus) {
    u256 key, off;
    mul_u256_small(off, g_step, (uint64_t)idx);   // idx ШАГОВ, не idx единиц
    if (plus) u256_add(key, center, off);
    else      u256_sub(key, center, off);
    if (!key_is_valid(key)) return;
    if (!key_in_search_range(key)) return;      // вторая, независимая проверка

    Point p = ec_mul_G(key);          // пересчёт начисто — заодно проверка
    if (p.inf) return;
    confirm_in_search_range(key, p.x, p.y, (int)(p.y[0] & 1));
}

// Образец для блока ПРОВЕРКА: ключ ИЗНУТРИ участка, а не его начало.
//
// Было `u256_set(g_sample_key, k0)` -- начало участка. А начала выровнены:
// в зональном режиме по 4096, значит образец всегда оканчивался на 000, в
// прочих режимах -- на столько нулей, сколько велит размер участка. Со
// стороны это выглядит так, будто программа проверяет одни круглые ключи, и
// именно этот вопрос задавался трижды подряд, каждый раз про новое место.
//
// Блок ПРОВЕРКА существует ровно затем, чтобы человек взял ключ, прогнал его
// посторонним инструментом и убедился, чтопубличный ключ сходится. Для этого годится
// ЛЮБОЙ ключ участка -- и лучше не круглый, потому что круглый ничего не
// доказывает про остальные 4095.
//
// Берём смещение внутри участка, размешанное из самого начала участка: ключ
// настоящий, участком проверенный, а хвост у него каждый раз разный.
static inline void set_sample(const u256 k0, uint64_t count) {
    // Смещение считает sample_offset() из traverse.cpp -- ТА ЖЕ функция, что
    // и у журнала участков. Пока формула была здесь своя, экран и журнал
    // показывали разные ключи одного блока, и сверить их было нельзя.
    // СМЕЩЕНИЕ СЧИТАЕТСЯ В КАНДИДАТАХ, А НЕ В КЛЮЧАХ.
    //
    // sample_offset даёт номер кандидата внутри участка. Пока шаг был
    // единичным, номер и смещение в ключах совпадали, и разницы не было
    // видно. При HEX-маске прибавление номера напрямую сдвигает МЛАДШИЕ
    // ниббли -- те самые, которые маска объявила неизменными. Ключ в блоке
    // ПРОВЕРКА переставал принадлежать решётке: перебор шёл верно, а
    // показанный образец ему противоречил.
    //
    // Это ровно тот же род ошибки, что был со строкой Baza в волновом
    // режиме: считается одно, показывается другое, и убедительнее выглядит
    // показанное.
    u256 s, soff;
    mul_u256_small(soff, g_step, sample_offset(k0, count));
    u256_add(s, k0, soff);
    std::lock_guard<std::mutex> lk(g_mtx_sample);
    u256_set(g_sample_key, s);
    g_sample_valid = true;
}

#ifdef USE_SECP
// --- путь через libsecp256k1 -------------------------------------------------
void secp_hit(const uint8_t ck[32], int idx, int plus, void *) {
    u256 center, key, off;
    fe_from_be32(center, ck);
    // Смещение внутри пакета -- idx ШАГОВ, а не idx единиц.
    //
    // Ошибка здесь не падает и не выдаёт чужой ключ: пересчитанный ключ
    // просто не подтвердится по базе, и попадание будет молча потеряно.
    // Поэтому тест обязан проверять, что подложенный ключ НАХОДИТСЯ, --
    // отсутствие ложных срабатываний тут ничего не доказывает.
    mul_u256_small(off, g_step, (uint64_t)idx);
    if (plus) u256_add(key, center, off);
    else      u256_sub(key, center, off);
    if (!key_is_valid(key)) return;
    if (!key_in_search_range(key)) return;      // вторая, независимая проверка

    uint8_t kb[32], c33[33], y32[32];
    u256_to_be32(kb, key);
    if (!scan_secp_pubkey(kb, c33, y32)) return;

    fe px, py;
    fe_from_be32(px, c33 + 1);
    fe_from_be32(py, y32);
    confirm_in_search_range(key, px, py, c33[0] == 0x03 ? 1 : 0);
}

static void scan_chunk_pub_impl(const u256 k0, uint64_t count) {
    if (count == 0) return;
    // Состояние scan_secp живёт по потоку и освобождается вместе с потоком.
    //
    // Прежде это был голый thread_local void*, и scan_secp_destroy не
    // вызывался нигде во всём дереве: при HALF=2048 и 64 потоках это 10 МБ,
    // невидимых в учёте, а в самопроверках, где потоки создаются десятками
    // раз подряд, утечка накапливается за один процесс.
    struct ScanHandle {
        void *p = nullptr;
        ~ScanHandle() { if (p) scan_secp_destroy(p); }
    };
    static thread_local ScanHandle hh;
    if (!hh.p) { hh.p = scan_secp_create(); if (!hh.p) { set_stop(); return; } }
    void *h = hh.p;

    u256 c; u256_add(c, k0, g_step_half);     // центр первого пакета: k0 + HALF*S
    uint8_t ck[32];
    u256_to_be32(ck, c);

    set_sample(k0, count);

    // ПАКЕТЫ НЕ ВЫЕЗЖАЮТ ЗА КОНЕЦ УЧАСТКА.
    //
    // Было ceil(count / SECP_SPAN): последний пакет проверялся ЦЕЛИКОМ, даже
    // если участок кончался в его середине. Для обычного участка 2^22 это
    // незаметно -- 2^22 делится на 1024 нацело. А вот последний участок
    // диапазона обрезан по g_range_end и на пакет не делится: при
    // [2^0, 2^256) хвост -- около 3.56 млн ключей при пакете 4.19 млн, то
    // есть примерно 638 тысяч ключей проверялось ЗА g_range_end.
    //
    // Теперь целых пакетов ровно count / SECP_SPAN, а остаток дочитывается
    // поштучно -- не более 1023 ключей на весь участок.
    uint64_t nb  = count / SECP_SPAN;
    uint64_t rem = count % SECP_SPAN;
    if (nb) {
        const uint64_t want = nb * (uint64_t)SECP_SPAN;
        uint64_t n = scan_secp_run(h, ck, nb, secp_hit, nullptr, &g_stop_flag);
        g_count.fetch_add(n, std::memory_order_relaxed);
        // BACKEND МОГ ВЕРНУТЬ МЕНЬШЕ ЗАКАЗАННОГО.
        //
        // scan_secp_run выходит с нулём, если база пакета оказалась
        // бесконечностью (центр кратен n -- достижимо на последнем участке
        // диапазона [., 2^256), где конец обрезан по n-1). Тогда участок
        // молча пропускался целиком, и g_count не досчитывал. Ключи там
        // негодны (k >= n), поэтому поиск ничего не терял, но выход из
        // середины участка без признака -- не то, на что можно опираться.
        // В своей математике этот край разбирается честно; здесь добираем
        // тем же поштучным проходом.
        //
        // Остановку по Ctrl+C от этого случая отличаем по флагу: там
        // недосчитанное добирать не нужно, там всё уже кончилось.
        // ОТСТУП СЧИТАЕТСЯ В ШАГАХ, А НЕ В КЛЮЧАХ.
        //
        // n -- число ПРОВЕРЕННЫХ КАНДИДАТОВ, а не расстояние в ключах: при
        // маске соседние кандидаты отстоят на 16^N. Прибавление n напрямую
        // уводило добивку в пустоту между узлами решётки.
        if (n < want && !g_stop.load(std::memory_order_relaxed)) {
            u256 miss, adv;
            mul_u256_small(adv, g_step, n);
            u256_add(miss, k0, adv);
            scan_slow_range(miss, want - n);
        }
    }
    if (rem) {
        // ТО ЖЕ САМОЕ ДЛЯ ХВОСТА -- И ЭТО БЫЛА НАСТОЯЩАЯ ПОТЕРЯ КЛЮЧА.
        //
        // Хвост участка (count % SPAN кандидатов) отсчитывался в ключах.
        // При маске он попадал мимо решётки, и ключ, оказавшийся в хвосте,
        // не проверялся вовсе -- молча, без признака в выводе. Срабатывало
        // на ПОСЛЕДНЕМ участке каждого прогона с маской: там count обрезан
        // по числу кандидатов и на 1024 не делится.
        //
        // В ветке собственной математики рядом стоял правильный вариант, и
        // это худшее в этой ошибке: два backend'а одним и тем же вызовом
        // сканировали разное. Тесты её не видели, потому что все прогоны
        // маски шли с count 4096 и 512 -- остатка не возникало ни разу.
        u256 tail, adv;
        mul_u256_small(adv, g_step, nb * (uint64_t)SECP_SPAN);
        u256_add(tail, k0, adv);
        scan_slow_range(tail, rem);
    }
}
#else
// --- путь на собственной математике -----------------------------------------
// Сканирует примерно count ключей вокруг k0 и выше.
static void scan_chunk_pub_impl(const u256 k0, uint64_t count) {
    if (count == 0) return;

    // Столько же целых пакетов, сколько у libsecp-версии; хвост -- поштучно.
    const uint64_t nfull = count / (uint64_t)SPAN;
    const uint64_t rem   = count % (uint64_t)SPAN;

    u256 k; u256_set(k, k0);
    u256_add(k, k, g_step_half);       // центр первого пакета: k0 + HALF*S
    Point P = ec_mul_G(k);

    set_sample(k0, count);   // образец для блока ПРОВЕРКА

    static thread_local fe dx[HALF], inv[HALF], scratch[HALF];
    // кольцо отложенных проверок: X, номер в пакете и знак
    static thread_local fe pf_x[PF];
    static thread_local int pf_idx[PF];
    static thread_local bool pf_plus[PF];
    uint64_t done = 0;

    // Порог вырождения: центр ближе к нулю либо к n, чем размер пакета.
    // Тогда среди разностей абсцисс есть нулевая (P = ±(i+1)G), пакетная
    // формула неприменима, и такой пакет разбирается честно поштучно.
    // Пакет достаёт на SPAN*S, а не на SPAN: при маске это огромное число, и
    // мерить край константой SPAN означало бы гнать вырожденные пакеты по
    // быстрому пути -- то есть считать мусор и не замечать этого.
    u256 near_zero;  u256_add_u64(near_zero, g_step_span, 1);
    u256 near_order; u256_sub(near_order, EC_N, near_zero);

    while (done < nfull * (uint64_t)SPAN && !g_stop.load(std::memory_order_relaxed)) {
        // БЕСКОНЕЧНОСТЬ В БАЗЕ -- ЭТО ТОТ ЖЕ ВЫРОЖДЕННЫЙ СЛУЧАЙ, А НЕ ВЫХОД.
        //
        // Было `if (P.inf) return;` -- и стояло это ПЕРЕД проверкой на
        // вырождение. База обращается в бесконечность при k = n, а такой
        // центр достижим на последнем участке диапазона [., 2^256): конец
        // обрезан по n-1, и центр пакета заезжает за n на полпакета. Программа
        // при этом молча бросала остаток участка и не досчитывала g_count.
        // Ключи там всё равно негодны (k >= n), поэтому поиск ничего не терял,
        // но выход из середины участка без всякого признака -- не то поведение,
        // на которое можно опираться. Разбираем честно, как и прочие края.
        if (__builtin_expect(P.inf
                          || u256_cmp(k, near_zero) <= 0
                          || u256_cmp(k, near_order) >= 0, 0)) {
            // ВЫРОЖДЕННЫЙ ПАКЕТ.
            //
            // Раньше нулевая разность просто подменялась единицей
            // (`if (fe_is_zero(dx[i])) fe_one(dx[i])`): формула считалась по
            // подставленному значению, абсциссы выходили мусором, и ключи
            // ЭТОГО пакета не проверялись вовсе -- молча, без признака в
            // выводе. В scan_secp.c этот случай разобран давно, в своей
            // математике оставался. Теперь и здесь честный поштучный проход.
            u256 from; u256_sub(from, k, g_step_half);
            scan_slow_range(from, (uint64_t)SPAN);
            u256 t; u256_add(t, k, g_step_span); u256_set(k, t);
            // База пересчитывается начисто, а не сдвигается сложением: после
            // бесконечности складывать нечего, а одно скалярное умножение на
            // весь вырожденный пакет ничего не стоит -- сюда попадают считаные
            // пакеты на весь диапазон.
            P = ec_mul_G(k);
            done += SPAN;
            continue;
        }

        bool degenerate = false;
        for (int i = 0; i < HALF; i++) {
            fe_sub(dx[i], GT[i].x, P.x);
            if (fe_is_zero(dx[i])) degenerate = true;
        }
        if (__builtin_expect(degenerate, 0)) {
            u256 from; u256_sub(from, k, g_step_half);
            scan_slow_range(from, (uint64_t)SPAN);
            u256_add(k, k, g_step_span);
            P = ec_mul_G(k);
            done += SPAN;
            continue;
        }

        // КОНВЕЙЕР ПРЕДВЫБОРКИ
        //
        // На большой базе карта фильтра не помещается в кэш, а её адрес
        // зависит от только что посчитанной абсциссы — процессор угадать его
        // не может, и каждый ключ платит промахом в память. Но абсциссы в
        // пакете независимы: можно посчитать X, попросить строку кэша и
        // проверить её через PF шагов, когда она уже пришла. Ожидание при
        // этом перекрывается арифметикой следующих ключей.
        int c = 0;
        auto push = [&](const fe xv, int idx, bool plus) {
            const int s = c % PF;
            if (c >= PF && __builtin_expect(g_db.maybe(pf_x[s]), 0))
                slow_confirm(k, pf_idx[s], pf_plus[s]);
            fe_set(pf_x[s], xv);
            pf_idx[s] = idx;
            pf_plus[s] = plus;
            const uint64_t *blk = g_db.block_ptr(xv);
            if (blk) __builtin_prefetch(blk, 0, 3);
            c++;
        };

        push(P.x, 0, true);                     // центр пакета

        fe_inv_batch(inv, dx, HALF, scratch);

        for (int i = 0; i < HALF; i++) {
            fe s, t, xp, xm;
            const fe *gx = &GT[i].x, *gy = &GT[i].y;

            // P + (i+1)G
            fe_sub(t, *gy, P.y);
            fe_mul(s, t, inv[i]);
            fe_sqr(xp, s);
            fe_sub(xp, xp, P.x);
            fe_sub(xp, xp, *gx);

            // P - (i+1)G : наклон отличается знаком, а квадрат его не помнит
            fe_add(t, *gy, P.y);
            fe_mul(s, t, inv[i]);
            fe_sqr(xm, s);
            fe_sub(xm, xm, P.x);
            fe_sub(xm, xm, *gx);

            // Верхний край пропускается -- иначе пакет накрыл бы 1025 ключей
            // и залез на первый ключ следующего пакета.
            if (i + 1 < HALF) push(xp, i + 1, true);
            push(xm, i + 1, false);
        }

        // Хвост конвейера. Пакет закрывается целиком, чтобы не тащить через
        // границу базовый ключ k: он у следующего пакета уже другой.
        {
            const int have = (c < PF) ? c : PF;
            for (int t = c - have; t < c; t++) {
                const int s = t % PF;
                if (__builtin_expect(g_db.maybe(pf_x[s]), 0))
                    slow_confirm(k, pf_idx[s], pf_plus[s]);
            }
        }

        u256_add(k, k, g_step_span);
        P = ec_add(P, GSTEP);

        done += SPAN;
        g_count.fetch_add(SPAN, std::memory_order_relaxed);
    }

    // Хвост участка: то, что не набралось на целый пакет.
    if (rem && !g_stop.load(std::memory_order_relaxed)) {
        u256 tail, adv;
        mul_u256_small(adv, g_step, nfull * (uint64_t)SPAN);
        u256_add(tail, k0, adv);
        scan_slow_range(tail, rem);
    }
}
#endif  // USE_SECP

// Bounded CPU slices keep pause responsive without touching the hot C loop.
static void scan_chunk_pub(const u256 k0, uint64_t count) {
    u256 current; u256_set(current, k0);
    while (count && !g_stop.load(std::memory_order_relaxed)) {
        pause_check();
        if (g_stop.load(std::memory_order_relaxed)) return;
        const uint64_t part = (std::min)(count, UINT64_C(1048576));
        scan_chunk_pub_impl(current, part);
        count -= part;
        if (!count) break;
        u256 offset, next; mul_u256_small(offset, g_step, part);
        u256_add(next, current, offset); u256_set(current, next);
    }
}


// ============================================================================
//  СЧЁТ НА ВИДЕОКАРТЕ
//
//  Третья реализация того же самого участка. Первые две (libsecp256k1 и
//  собственная математика) считают на процессоре и выбираются на сборке; эта
//  добавляется рядом и включается на запуске, если карта нашлась.
//
//  ЧТО ДЕЛАЕТ КАРТА, А ЧТО ХОСТ. Карта считает точки и отсеивает по
//  предфильтру, возвращая КАНДИДАТОВ -- это её собственные слова в
//  cuda_scan.h: "попадание здесь -- это совпадение по предфильтру, а не
//  находка". Настоящую проверку делает хост: пересчитывает k*G начисто и
//  сверяет с базой. Так устроен и процессорный путь, и это не лишний шаг:
//  на карте нет ни базы целиком, ни разбора чётности.
//
//  ПОЧЕМУ ОДИН ПОТОК. cuda_scan_* держит состояние в глобальных переменных
//  модуля -- на это указывает сам его интерфейс: ни cuda_scan_run, ни
//  cuda_scan_set_targets не принимают описателя, в отличие от процессорного
//  scan_secp_create/scan_secp_run. Значит вызывать их можно только из одного
//  потока. Это не потеря: одна карта и так занята целиком одним вызовом, а
//  двадцать четыре потока процессора при работающей карте только мешали бы
//  ей, отбирая такты на подтверждение кандидатов.
// ============================================================================
#ifdef PKH_HAVE_CUDA
#include "cuda_scan.h"

// Сколько кандидатов карта может отдать за один заход. Настоящих находок
// столько не бывает никогда; запас нужен под ложные срабатывания предфильтра,
// которых на большой базе несколько штук в секунду.
// СКОЛЬКО КАНДИДАТОВ КАРТА МОЖЕТ ВЕРНУТЬ ЗА ОДИН ЗАХОД.
//
// Число было 1024, и переполнение проходило МОЛЧА: ядро считает найденных
// через atomicAdd, а всё, что не влезло, просто не записывает. Кандидат --
// это возможная находка; потерять его значит потерять ключ и не узнать об
// этом никогда. Самопроверка показала это прямо: тест на покрытие 4096
// ключей сообщал "найдено 1024 из 4096" -- ровно размер буфера.
//
// Теперь две вещи. Буфер больше -- переполнение стало редким. И оно больше
// не молчит: scan_chunk_gpu видит found == предела, берёт участок вдвое
// меньше и проходит его заново, пока все кандидаты не поместятся.
// Число одно на обе стороны и живёт в cuda_scan.h: порознь оно уже расходилось
// (4096 на карте против 8192 здесь), и проверка переполнения не срабатывала.
#define GPU_MAX_OUT CUDA_MAX_OUT

// Подтверждение одного кандидата. Повторяет secp_hit, но проще: карта
// возвращает готовый ключ, а не пару "центр пакета + смещение".
static void gpu_confirm(const uint8_t kb[32]) {
    u256 key;
    fe_from_be32(key, kb);
    if (!key_is_valid(key)) return;
    if (!key_in_search_range(key)) return;   // вторая, независимая проверка

    uint8_t c33[33], y32[32];
#ifdef USE_SECP
    if (!scan_secp_pubkey(kb, c33, y32)) return;
    fe px, py;
    fe_from_be32(px, c33 + 1);
    fe_from_be32(py, y32);
    const int parity = (c33[0] == 0x03) ? 1 : 0;
#else
    (void)c33; (void)y32;
    Point P = ec_mul_G(key);
    if (P.inf) return;
    fe px, py;
    fe_set(px, P.x);
    fe_set(py, P.y);
    const int parity = (int)(P.y[0] & 1);
#endif

    confirm_in_search_range(key, px, py, parity);
}

// Сколько ключей отдавать карте за ОДИН вызов.
//
// Подбор между двумя бедами. Мелкий кусок -- подготовка начальных точек (100
// мс на вызов) съедает всё; крупный -- ядро выполняется одним запуском, и на
// карте с подключённым монитором сторожевой таймер Windows сбрасывает запуск
// длиннее двух секунд, а на экране до самого конца вызова стоит ноль, потому
// что счётчик растёт только после возврата.
//
// ЗАМЕР НА RTX 5090 Laptop: 67 млн ключей за 183 мс, из них около 100 мс --
// подготовка. То есть карта считала 83 мс и выдала ~800 млн/с, а показала 365:
// больше половины времени она ждала процессор.
//
// 2^29 = 537 млн ключей за заход: те же 100 мс приходятся на в восемь раз
// больший объём, и доля подготовки падает с 55% примерно до 13%.
//
// 2^30 -- ТЕКУЩЕЕ ЗНАЧЕНИЕ, и оно связано с размером участка в меню.
// Меню с картой предлагает участок 2^30; если резать его здесь на 2^29,
// подготовка сделается ДВАЖДЫ на участок, и весь выигрыш от крупного участка
// пропадёт на ровном месте. Замер счёта: 2^30 ключей при 1.9 млрд/с -- около
// 0.57 с, то есть втрое меньше двух секунд, за которыми сторожевой таймер
// драйвера сбрасывает ядро.
//
// НА КАРТЕ С МОНИТОРОМ ПОРОГ ДРУГОЙ. Там ядро делит время с рабочим столом,
// сторожевой таймер тот же, а запас нужен больше -- поэтому режим бережный,
// и кусок остаётся 2^28.
#define GPU_SLICE_BITS        30
#define GPU_SLICE_BITS_GENTLE 28

// КУСОК ПОДСТРАИВАЕТСЯ ПОД ИЗМЕРЕННОЕ ВРЕМЯ ЯДРА, А НЕ ВПИСАН НАВСЕГДА.
//
// 2^30 выбрано по замеру на RTX 5090 (0.57 с). На карте вчетверо медленнее
// тот же кусок шёл бы 2.3 с -- дольше двух секунд сторожевого таймера
// драйвера Windows, и ядро сбрасывалось бы на ПЕРВОМ же заходе. Раз время
// ядра всё равно измеряется (cuda_scan_last_cost), им и пользуемся: длиннее
// секунды -- кусок вдвое меньше, короче трети секунды -- вдвое больше, в
// пределах [2^24, 2^32]. Начинаем с 2^28, чтобы первый заход был безопасен
// на любой карте, и за два-три захода приходим к нужному.
#define GPU_SLICE_BITS_START  28
#define GPU_SLICE_BITS_MIN    24
#define GPU_SLICE_BITS_MAX    32
#define GPU_KERN_MS_LOW       330.0
#define GPU_KERN_MS_HIGH     1000.0
static int g_gpu_slice_bits = GPU_SLICE_BITS_START;

// СКОРОСТЬ САМОЙ КАРТЫ ЗА ИНТЕРВАЛ -- ОТДЕЛЬНО ОТ СКОРОСТИ НА ЭКРАНЕ.
//
// Число на экране делит ключи на всё время: с подготовкой, с паузами
// мощности, с подтверждением кандидатов. Оно честное, но не отвечает на
// вопрос "карта стала медленнее или что-то вокруг неё". Здесь копится чистое
// время ядра и число ключей; строка статистики забирает и обнуляет.
static std::atomic<uint64_t> g_kern_keys{0};
static std::atomic<uint64_t> g_kern_us{0};

void gpu_interval_rate(uint64_t *keys, double *seconds) {
    *keys    = g_kern_keys.exchange(0);
    *seconds = (double)g_kern_us.exchange(0) / 1e6;
}

size_t packed_traversal_capacity(){
    if(!g_gpu_active||g_gpu_blocks<=0||g_gpu_threads<=0||ZONE_BLOCK>65536||CUDA_SPAN>ZONE_BLOCK||ZONE_BLOCK%CUDA_SPAN)return 0;
    return (size_t)g_gpu_blocks*(size_t)g_gpu_threads/(ZONE_BLOCK/CUDA_SPAN);
}

// One GPU worker owns the packed backend. Its zero-delta configuration is
// reusable only for the exact effective bounds and segment length; starting
// or stopping CUDA, pushing a step or configuring a different packed plan
// invalidates this host-side acknowledgement before touching device state.
struct ZoneGpuConfigCache {
    uint64_t segment_keys = 0;
    u256 lower{}, upper{};
    bool ready = false;

    void invalidate() noexcept { ready = false; segment_keys = 0; }
    bool same_target(uint64_t keys, const u256 lo, const u256 hi) const noexcept {
        return segment_keys != 0 && segment_keys == keys
            && std::memcmp(lower, lo, sizeof(lower)) == 0
            && std::memcmp(upper, hi, sizeof(upper)) == 0;
    }
    bool matches(uint64_t keys, const u256 lo, const u256 hi) const noexcept {
        return ready && same_target(keys, lo, hi);
    }
    void remember(uint64_t keys, const u256 lo, const u256 hi) noexcept {
        u256_set(lower, lo);
        u256_set(upper, hi);
        segment_keys = keys;
        ready = true;
    }
    void require_refresh(uint64_t keys, const u256 lo, const u256 hi) noexcept {
        remember(keys, lo, hi);
        ready = false;
    }
};
static ZoneGpuConfigCache g_zone_gpu_config;

// A failed/aborted packed attempt must never leave a reusable acknowledgement.
// This also covers allocation/confirmation exceptions without changing replay.
struct PackedConfigAttempt {
    bool accepted = false;
    uint64_t retry_keys = 0;
    u256 lower{}, upper{};
    void retry_target(uint64_t keys, const u256 lo, const u256 hi) noexcept {
        retry_keys = keys;
        u256_set(lower, lo);
        u256_set(upper, hi);
    }
    ~PackedConfigAttempt() {
        if (accepted) return;
        if (retry_keys) g_zone_gpu_config.require_refresh(retry_keys, lower, upper);
        else g_zone_gpu_config.invalidate();
    }
};

bool scan_packed_configure(const PackedTraversalPlan &plan){
    g_zone_gpu_config.invalidate();
    if(!g_gpu_active||g_cfg.threads!=1||!packed_traversal_capacity()||mask_active())return false;
    if(g_step[0]!=1||g_step[1]||g_step[2]||g_step[3])return false;
    // Legacy packed traversal plans are translations of a cycle base.
    // The current zone traversal uses scan_zone_batch with exact host starts.
    if(plan.mode!=TRAV_WAVE&&plan.mode!=TRAV_ZONE)return false;
    const uint64_t rows=plan.deltas.size()/64;
    return cuda_scan_packed_configure(PACKED_SCAN_WAVE,
        plan.deltas.data(),nullptr,rows,
        plan.top_pos,plan.top_min,plan.top_radix,
        plan.lo.data(),plan.hi.data(),ZONE_BLOCK)!=0;
}

bool scan_packed_traversal(const PackedTraversalPlan &plan,const PackedTraversalBatch &batch){
    PackedConfigAttempt attempt;
    if(!batch.fast||!g_gpu_active||g_stop.load(std::memory_order_relaxed))return false;
    if(batch.segments.empty()){attempt.accepted=true;return true;}
    if(batch.bases.empty()||batch.segments.size()>packed_traversal_capacity())return false;
    pause_check();
    if(g_stop.load(std::memory_order_relaxed))return false;
    static std::vector<uint8_t> out(GPU_MAX_OUT*32);
    uint8_t sample[32]{};int found=0;
    const uint64_t expected=(uint64_t)batch.segments.size()*ZONE_BLOCK;
    const auto begin=std::chrono::steady_clock::now();
    const uint64_t checked=cuda_scan_packed_batch(batch.bases[0].data(),(int)batch.bases.size(),
        batch.segments.data(),(int)batch.segments.size(),sample,out.data(),GPU_MAX_OUT,&found);
    const auto completed=std::chrono::steady_clock::now();
    if(checked!=expected||found<0||found>=GPU_MAX_OUT
       ||std::memcmp(sample,batch.sample_key.data(),32)!=0){
        static bool reported=false;
        if(!reported){
            std::lock_guard<std::mutex> output(g_mtx_cout);
            MinuteSpeed::LogScope speed_log;
            std::cerr << "  GPU-пакет волны отклонён: обработано " << checked
                      << " из " << expected << "; кандидатов " << found
                      << "; образец " << (std::memcmp(sample,batch.sample_key.data(),32)==0?"совпал":"не совпал") << ".\n";
            reported=true;
        }
        return false;
    }
    // No published counters/candidates precede this whole-attempt success point.
    g_count.fetch_add(checked,std::memory_order_relaxed);
    double kernel_ms=0;cuda_scan_last_cost(nullptr,&kernel_ms,nullptr,nullptr);
    g_kern_keys.fetch_add(checked,std::memory_order_relaxed);
    g_kern_us.fetch_add((uint64_t)(kernel_ms*1000.0),std::memory_order_relaxed);
    u256 sample_key;fe_from_be32(sample_key,sample);set_sample(sample_key,ZONE_BLOCK);
    if(batch.sample_zone>=0)g_zone_idx.store(batch.sample_zone,std::memory_order_relaxed);
    for(int i=0;i<found;++i)gpu_confirm(out.data()+(size_t)i*32);
    const int power = g_gpu_power_pct.load(std::memory_order_relaxed);
    if(power<100){
        const double busy=std::chrono::duration<double,std::milli>(completed-begin).count();
        const double rest=busy*(100.0-power)/power;
        if(rest>0.5)pk_timing::sleep_for(std::chrono::duration<double,std::milli>(rest));
    }
    attempt.accepted = true;
    return true;
}

size_t zone_scan_capacity(uint64_t segment_keys) {
    if (!g_gpu_active || g_gpu_blocks <= 0 || g_gpu_threads <= 0
        || segment_keys < CUDA_SPAN || segment_keys > 65536
        || segment_keys % CUDA_SPAN) return 0;
    return (size_t)g_gpu_blocks * (size_t)g_gpu_threads / (segment_keys / CUDA_SPAN);
}

static bool zone_gpu_config_eligible(uint64_t segment_keys,
                                     const u256 lower, const u256 upper) {
    return g_cfg.threads == 1 && zone_scan_capacity(segment_keys) && !mask_active()
        && g_step[0] == 1 && !g_step[1] && !g_step[2] && !g_step[3]
        && key_is_valid(lower) && key_is_valid(upper)
        && u256_cmp(lower, upper) <= 0;
}

bool scan_zone_configure(uint64_t segment_keys) {
    u256 lower_key, upper_key;
    u256_set(lower_key, g_range_start);
    u256_set(upper_key, g_range_end);
    if (!zone_gpu_config_eligible(segment_keys, lower_key, upper_key)) {
        g_zone_gpu_config.invalidate();
        return false;
    }
    if (g_zone_gpu_config.matches(segment_keys, lower_key, upper_key)) return true;
    g_zone_gpu_config.invalidate();
    uint8_t lower[32], upper[32], deltas[64]{};
    u256_to_be32(lower, lower_key);
    u256_to_be32(upper, upper_key);
    if (!cuda_scan_packed_configure(PACKED_SCAN_WAVE, deltas, nullptr, 1,
                                    0, 0, 16, lower, upper, segment_keys)) return false;
    g_zone_gpu_config.remember(segment_keys, lower_key, upper_key);
    return true;
}

bool scan_zone_batch(const uint8_t *starts32, int count, uint64_t segment_keys,
                     uint8_t *sample32) {
    PackedConfigAttempt attempt;
    u256 lower_key, upper_key;
    u256_set(lower_key, g_range_start);
    u256_set(upper_key, g_range_end);
    if (!starts32 || count <= 0 || g_stop.load(std::memory_order_relaxed)
        || !zone_gpu_config_eligible(segment_keys, lower_key, upper_key)
        || !g_zone_gpu_config.same_target(segment_keys, lower_key, upper_key)
        || (size_t)count > zone_scan_capacity(segment_keys)) return false;
    // Configure-once wave/zone/hybrid callers must recover after a rejected
    // attempt. Retain only their exact prior intent and require a real setup;
    // legacy configuration and lifecycle crossings have already erased it.
    attempt.retry_target(segment_keys, lower_key, upper_key);
    if (!g_zone_gpu_config.matches(segment_keys, lower_key, upper_key)
        && !scan_zone_configure(segment_keys)) return false;
    pause_check();
    if (g_stop.load(std::memory_order_relaxed)) return false;
    // The packed backend uses state zero (identity), one base per segment.
    // Keeping the original starts on the host permits exact replay on any
    // clipped segment, degenerate center, CUDA failure or output overflow.
    static std::vector<PackedSegment> segments;
    static std::vector<uint8_t> out(GPU_MAX_OUT * 32);
    segments.resize((size_t)count);
    for (int i = 0; i < count; ++i) segments[(size_t)i] = {0, (uint32_t)i};
    uint8_t sample[32]{};
    int found = 0;
    const uint64_t expected = (uint64_t)count * segment_keys;
    const auto begin = std::chrono::steady_clock::now();
    const uint64_t checked = cuda_scan_packed_batch(starts32, count, segments.data(),
        count, sample, out.data(), GPU_MAX_OUT, &found);
    const auto completed = std::chrono::steady_clock::now();
    if (checked != expected || found < 0 || found >= GPU_MAX_OUT
        || std::memcmp(sample, starts32, 32) != 0) return false;
    // Publish only after the complete attempt succeeded. Host confirmation
    // independently recalculates every candidate and checks the search bounds.
    g_count.fetch_add(checked, std::memory_order_relaxed);
    double kernel_ms = 0;
    cuda_scan_last_cost(nullptr, &kernel_ms, nullptr, nullptr);
    g_kern_keys.fetch_add(checked, std::memory_order_relaxed);
    g_kern_us.fetch_add((uint64_t)(kernel_ms * 1000.0), std::memory_order_relaxed);
    u256 sample_key;
    fe_from_be32(sample_key, sample);
    set_sample(sample_key, segment_keys);
    if (sample32) std::memcpy(sample32, sample, 32);
    for (int i = 0; i < found; ++i) gpu_confirm(out.data() + (size_t)i * 32);
    const int power = g_gpu_power_pct.load(std::memory_order_relaxed);
        if (power < 100) {
        const double busy = std::chrono::duration<double,std::milli>(completed - begin).count();
        const double rest = busy * (100.0 - power) / power;
        if (rest > 0.5) pk_timing::sleep_for(std::chrono::duration<double,std::milli>(rest));
    }
    attempt.accepted = true;
    return true;
}

// Само определение ниже, рядом с остальными глобальными о карте. Здесь
// объявление, потому что читается оно выше определения: держать все
// переменные о карте в одном месте важнее, чем избежать одной строки.
extern GpuLoad g_gpu_load;

static void scan_chunk_gpu(const u256 k0, uint64_t count) {
    if (count == 0) return;
    set_sample(k0, count);

    static std::vector<uint8_t> out(GPU_MAX_OUT * 32);
    // Потолок: бережному режиму (монитор на карте) 2^28, обычному -- 2^32;
    // фактический кусок внутри потолка выбирает замер, см. ниже.
    const int slice_cap = (g_gpu_load == LOAD_GENTLE) ? GPU_SLICE_BITS_GENTLE
                                                      : GPU_SLICE_BITS_MAX;
    if (g_gpu_slice_bits > slice_cap) g_gpu_slice_bits = slice_cap;

    u256 cur; u256_set(cur, k0);
    uint64_t left = count;
    const bool unit_step = g_step[0] == 1 && !g_step[1] && !g_step[2] && !g_step[3];
    uint8_t scalar_step[32];
    if (!unit_step) u256_to_be32(scalar_step, g_step);

    // Keep KXE's block permutation and completion watermark unchanged. Only
    // the execution within its current block uses the hybrid packed backend.
    // Each successful batch is an exact contiguous prefix of the remaining
    // block; a failed/overflowing/degenerate batch publishes nothing and is
    // replayed from the same cur by the established ordinary path below.
    constexpr uint64_t packed_piece = 65536;
    const size_t packed_capacity = zone_scan_capacity(packed_piece);
    if (g_cfg.mode == TRAV_KXE && unit_step && left >= packed_piece
        && packed_capacity && scan_zone_configure(packed_piece)) {
        static bool packed_announced = false;
        if (!packed_announced) {
            std::lock_guard<std::mutex> output(g_mtx_cout);
            MinuteSpeed::LogScope speed_log;
            std::cout << "  KXE: объединённые заходы и подготовка начальных точек на GPU.\n";
            packed_announced = true;
        }
        static std::vector<uint8_t> starts;
        while (left >= packed_piece && !g_stop.load(std::memory_order_relaxed)) {
            pause_check();
            if (g_stop.load(std::memory_order_relaxed)) return;
            const uint64_t max_pieces = (UINT64_C(1) << slice_cap) / packed_piece;
            const size_t segments = (size_t)(std::min)(
                (std::min)((uint64_t)packed_capacity, max_pieces), left / packed_piece);
            try { starts.resize(segments * 32); }
            catch (const std::bad_alloc&) { break; } // optional optimization
            u256 next; u256_set(next, cur);
            for (size_t i = 0; i < segments; ++i) {
                u256_to_be32(starts.data() + i * 32, next);
                u256_add_u64(next, next, packed_piece);
            }
            if (!scan_zone_batch(starts.data(), (int)segments, packed_piece)) break;
            u256_set(cur, next);
            left -= (uint64_t)segments * packed_piece;
        }
    }

    while (left && !g_stop.load(std::memory_order_relaxed)) {
        // Пробел -- пауза. Проверяется НА ГРАНИЦЕ ЗАХОДА, до его запуска:
        // тогда карта встаёт между заходами, ни один ключ не остаётся
        // недосчитанным, и продолжение начинается ровно там, где встали.
        pause_check();
        if (g_stop.load(std::memory_order_relaxed)) break;

        const uint64_t SLICE = 1ULL << g_gpu_slice_bits;
        uint64_t want = (left < SLICE) ? left : SLICE;

        uint8_t curb[32];
        u256_to_be32(curb, cur);

        int found = 0;
        uint64_t n = 0;
        const auto t0 = std::chrono::steady_clock::now();

        // ПЕРЕПОЛНЕНИЕ БУФЕРА КАНДИДАТОВ -- ПОВОД ПЕРЕСЧИТАТЬ, А НЕ ЗАБЫТЬ.
        //
        // found == предела значит "кандидатов было не меньше", а сколько на
        // самом деле -- неизвестно: лишние ядро не записало. Раньше их просто
        // теряли. Берём вдвое меньший кусок и проходим заново; кандидатов на
        // него придётся вдвое меньше, и рано или поздно все поместятся.
        //
        // A dense target set or saturated Bloom can fill even a small slice.
        // Equality with capacity is ambiguous, so keep splitting below it;
        // never accept the truncated output as a completely checked slice.
        for (;;) {
            // A large mask can make an unrequested center or the retained
            // next center wrap at 2^256. Never scan with a mislabeled point.
            // Unit-step searches retain their existing path and hot kernel.
            if (!unit_step && !pkh_cuda_scalar::launch_fits(
                    curb, scalar_step, want, g_gpu_blocks, g_gpu_threads, CUDA_HALF)) {
                static bool warned = false;
                if (!warned) {
                    std::lock_guard<std::mutex> output(g_mtx_cout);
                    MinuteSpeed::LogScope speed_log;
                    std::cout << "  Край участка с большим шагом: центры GPU не помещаются"
                                 " в 256 бит; точный проход на процессоре.\n";
                    warned = true;
                }
                scan_chunk_pub(cur, left);
                return;
            }
            n = cuda_scan_run(curb, want, out.data(), GPU_MAX_OUT, &found);
            if (n == 0 || found < GPU_MAX_OUT) break;
            if (want <= 1) {
            std::lock_guard<std::mutex> output(g_mtx_cout);
            MinuteSpeed::LogScope speed_log;
                std::cout << "\n  Кандидатов больше, чем помещается за заход ("
                          << GPU_MAX_OUT << "), и участок делить дальше некуда.\n"
                             "  Это значит, что предфильтр срабатывает почти на "
                             "каждый ключ:\n"
                             "  Счёт остановлен; этот участок не засчитан.\n";
                set_stop();
                return;
            }
            want /= 2;
        }
        const auto t1 = std::chrono::steady_clock::now();

        // КАРТА ВЕРНУЛА НОЛЬ -- ЭТО ОТКАЗ, А НЕ ПОВОД МОЛЧА СЧИТАТЬ САМИМ.
        //
        // Здесь стоял переход на scan_slow_range для всего недосчитанного. Для
        // "вернула чуть меньше" это верно, а для "вернула ноль" -- беда: на
        // участке 2^28 медленный проход идёт по одному k*G на ключ, то есть
        // около суток, и всё это время на экране ноль проверок. Программа
        // выглядела бы работающей, не проверяя ничего.
        if (n == 0) {
            {
            std::lock_guard<std::mutex> output(g_mtx_cout);
            MinuteSpeed::LogScope speed_log;
            std::cout << "\n  Карта вернула ноль проверенных ключей — счёт на ней"
                         " прекращён.\n"
                         "  Дальше считает процессор. Если это повторяется,"
                         " причина обычно в сбросе\n"
                         "  запуска сторожевым таймером драйвера: уменьшите"
                         " размер участка.\n";
            }
            gpu_backend_stop();
            scan_chunk_pub(cur, left);      // остаток -- процессорным пакетом
            return;
        }

        const uint64_t did = (n < want) ? n : want;
        // Validate all output buffers before publishing any work or candidates.

        // ВЫРОЖДЕННЫЕ ПАКЕТЫ ДОСЧИТЫВАЕТ ХОСТ -- ВЕСЬ ПАКЕТ, А НЕ ОДИН КЛЮЧ.
        //
        // Карта отдаёт центр пакета, у которого разность абсцисс обратилась в
        // ноль: считать его пакетом она не может. Раньше центр шёл в общий
        // список кандидатов и проверялся как ОДИН ключ; остальные CUDA_SPAN-1
        // ключей пакета не проверял никто. Теперь каждый такой пакет проходит
        // поштучно, от центра минус CUDA_HALF шагов и на CUDA_SPAN ключей --
        // ровно отрезок нити. Без прибавки к счётчику: карта его уже засчитала.
        {
            static uint8_t dg[256 * 32];
            const int nd = cuda_scan_take_degenerate(dg, 256);
            if (nd > 256) {
                // ПЕЧАТЬЮ ЗДЕСЬ НЕ ОБОЙТИСЬ.
                //
                // Пакеты сверх 256 карта не сохранила, и досчитать их нечем:
                // их центров у нас просто нет. Продолжив счёт, мы объявили бы
                // проверенным то, чего никто не проверял, -- по CUDA_SPAN
                // ключей на каждый непоместившийся пакет. Останавливаемся.
            std::lock_guard<std::mutex> output(g_mtx_cout);
            MinuteSpeed::LogScope speed_log;
                std::cout << "\n  Карта сообщила о " << nd
                          << " вырожденных пакетах за заход -- больше, чем помещается.\n"
                             "  Так не бывает: проверьте шаг перебора (маску).\n"
                             "  Счёт остановлен: непоместившиеся пакеты досчитать нечем,\n"
                             "  а засчитать их как проверенные значило бы соврать.\n";
                set_stop();
                return;
            }
            g_count.fetch_add(did, std::memory_order_relaxed);
            for (int i = 0; i < found; i++)
                gpu_confirm(&out[(size_t)i * 32]);
            for (int i = 0; i < nd; i++) {
                u256 c; fe_from_be32(c, &dg[(size_t)i * 32]);
                u256 half_off; mul_u256_small(half_off, g_step, (uint64_t)CUDA_HALF);
                u256 from; u256_sub(from, c, half_off);
                // A degenerate final thread may contain fewer requested keys.
                // Preserve the scalar lattice and clip replay to this launch.
                u256 last_offset, last, key;
                mul_u256_small(last_offset, g_step, did - 1);
                u256_add(last, cur, last_offset);
                u256_set(key, from);
                for (int j = 0; j < CUDA_SPAN && !g_stop.load(); ++j) {
                    if (u256_cmp(key, last) > 0) break;
                    if (u256_cmp(key, cur) >= 0) scan_slow_range(key, 1, false);
                    u256 next; u256_add(next, key, g_step);
                    if (u256_cmp(next, key) <= 0) break;
                    u256_set(key, next);
                }
            }
        }

        // ПОДСТРОЙКА КУСКА ПО ВРЕМЕНИ ЯДРА -- см. GPU_SLICE_BITS_START.
        {
            double kern = 0;
            cuda_scan_last_cost(nullptr, &kern, nullptr, nullptr);
            g_kern_keys.fetch_add(did, std::memory_order_relaxed);
            g_kern_us.fetch_add((uint64_t)(kern * 1000.0), std::memory_order_relaxed);
            if (kern > GPU_KERN_MS_HIGH && g_gpu_slice_bits > GPU_SLICE_BITS_MIN)
                g_gpu_slice_bits--;
            else if (kern > 0 && kern < GPU_KERN_MS_LOW && g_gpu_slice_bits < slice_cap
                     && want == SLICE)            // растим только по полному куску
                g_gpu_slice_bits++;
        }

        // ПЕРВЫЙ ЗАХОД НАЗЫВАЕТ СВОЮ СКОРОСТЬ ВСЛУХ.
        //
        // Иначе первые сведения о том, работает ли карта вообще, приходят
        // только со строкой скорости -- через минуту, а то и позже. Один раз
        // за запуск это не мешает, а отвечает на главный вопрос сразу.
        static bool told = false;
        if (!told) {
            told = true;
            const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
            std::lock_guard<std::mutex> output(g_mtx_cout);
            MinuteSpeed::LogScope speed_log;
            std::cout << "  Первый заход карты: " << did << " ключей за "
                      << (long long)ms << " мс = "
                      << (long long)(did / (ms > 0 ? ms : 1) / 1000.0)
                      << " млн ключей/с\n";

            // И СРАЗУ -- ИЗ ЧЕГО СЛОЖИЛИСЬ ЭТИ МИЛЛИСЕКУНДЫ.
            //
            // Одно общее число не отвечает на главный вопрос: карта считала
            // или ждала процессор? Эти два случая лечатся по-разному, а на
            // экране выглядят одинаково -- и однажды здесь уже потеряли время,
            // виня карту в том, что делала подготовка. Поэтому обе части
            // печатаются порознь, вместе со скоростью САМОЙ карты.
            double prep = 0, kern = 0; uint64_t thr = 0, rnd = 0;
            cuda_scan_last_cost(&prep, &kern, &thr, &rnd);
            if (kern > 0) {
                std::cout << "    из них: подготовка " << (long long)prep
                          << " мс, счёт на карте " << (long long)kern << " мс"
                          << "  (сама карта: "
                          << (long long)(did / kern / 1000.0) << " млн ключей/с)\n"
                          << "    раскладка захода: " << thr << " нитей x "
                          << rnd << " кругов\n";
            }
        }

        // ПАУЗА, ЕСЛИ ЧЕЛОВЕК ПОПРОСИЛ СЧИТАТЬ НЕ ВО ВСЮ СИЛУ.
        //
        // Длина паузы считается от ФАКТИЧЕСКОЙ длительности захода, а не от
        // какой-то постоянной: заходы разной длины (хвост участка короче), и
        // постоянная пауза давала бы то 90%, то 40% вместо заказанного.
        const int power = g_gpu_power_pct.load(std::memory_order_relaxed);
        if (power < 100) {
            const double busy = std::chrono::duration<double, std::milli>(t1 - t0).count();
            const double rest = busy * (100.0 - power) / power;
            if (rest > 0.5)
                pk_timing::sleep_for(std::chrono::duration<double, std::milli>(rest));
        }

        u256 adv; mul_u256_small(adv, g_step, did);
        u256 nxt; u256_add(nxt, cur, adv); u256_set(cur, nxt);
        left -= did;
    }
}

// Включена ли карта в этом запуске. Ставится из main после удачной
// инициализации; до неё счёт идёт по процессорному пути (например, в
// самотесте, где карта не нужна и может отсутствовать).
bool     g_gpu_active  = false;
GpuInfo  g_gpu_info;
GpuPlan  g_gpu_plan;
GpuLoad  g_gpu_load    = LOAD_NORMAL;
int      g_gpu_blocks  = 0;
int      g_gpu_threads = 0;
int      g_gpu_count   = 0;

// ДОЛЯ ВРЕМЕНИ ПОД СЧЁТОМ, % -- ЕДИНСТВЕННЫЙ РЫЧАГ, КОТОРЫЙ У НАС ЕСТЬ.
//
// Честно о том, чего программа НЕ делает. Настоящий предел мощности ставится
// через nvidia-smi -pl, и для этого нужны права администратора, а на
// ноутбучных картах он часто заперт заводской прошивкой вовсе. Поэтому
// советник и печатал "70% от штатного" как СОВЕТ человеку, ничего при этом не
// делая. Строка выглядела как действие, а действием не была.
//
// Что можно без всяких прав: считать не всё время. После каждого захода карта
// стоит столько, чтобы доля счёта вышла заданной. 70% -- на каждые 7 единиц
// счёта 3 единицы покоя. Карта греется меньше, вентиляторы тише, а скорость
// падает ровно на ту же долю -- это не бесплатно и об этом сказано прямо.
//
// 100 -- пауз нет вовсе.
std::atomic<int> g_gpu_power_pct{100};
size_t   g_gpu_vram    = 0;

// Сколько видеопамяти нужно на один одновременно считаемый ключ.
//
// Число считается, а не выдумано, и здесь оно было ЗАНИЖЕНО ВДВОЕ.
//
// На нить приходится не два массива, а четыре по CUDA_HALF элементов поля по
// 32 байта: рабочие dx и iv (они в общей памяти карты) плюс op и om -- две
// таблицы абсцисс, которые ядро держит как локальные массивы, а локальная
// память на карте это та же глобальная. Итого 4*CUDA_HALF*32 байт на нить при
// CUDA_SPAN = 2*CUDA_HALF ключей за заход, то есть 64 байта на ключ.
//
// Величина не зависит от CUDA_HALF: увеличив пакет, мы не изменим расход на
// ключ, но изменим расход на нить -- а от него зависит, сколько нитей вообще
// поместится. Поэтому советник должен знать правду: с 32 он разрешал вдвое
// больше нитей, чем есть памяти под них.
//
// СТАЛО 32, потому что массивов на нить теперь два, а не четыре: op и om
// убраны -- абсцисса проверяется там же, где посчитана. Остались рабочие dx
// и iv, по CUDA_HALF элементов поля на CUDA_SPAN = 2*CUDA_HALF ключей, то
// есть ровно 32 байта на ключ.
#define GPU_BYTES_PER_KEY 32

bool gpu_backend_start(std::string &why) {
    g_zone_gpu_config.invalidate();
    why.clear();

    std::vector<GpuInfo> cards;
    if (!gpu_detect(cards) || cards.empty()) {
        why = "видеокарта NVIDIA не найдена (или драйвер не отвечает)";
        return false;
    }

    // Карта берётся первая. Выбор между несколькими -- отдельный разговор:
    // cuda_scan_* держит одно глобальное состояние, поэтому вторая карта
    // требует не настройки, а второго экземпляра модуля.
    g_gpu_count = (int)cards.size();
    GpuInfo g = cards[0];

    // Ask the actual compiled kernel after selecting this device. Its register
    // allocation can differ between native cubins and a future driver's PTX JIT.
    CudaScanLaunchInfo kernel;
    char reason[256] = {0};
    if (!cuda_scan_query_launch(g.index, 256, &kernel, reason, sizeof(reason))) {
        why = reason[0] ? reason : "CUDA kernel/device query failed";
        return false;
    }
    g.sm_count = kernel.sm_count;
    g.mem_free = kernel.memory_free;
    g.mem_total = kernel.memory_total;
    const GpuLoad load = g.is_display ? LOAD_GENTLE : LOAD_NORMAL;
    // Targets are loaded after the menu. The scratch quota leaves most VRAM
    // free for that upload; cuda_scan_set_targets checks the remaining bytes.
    GpuPlan plan = gpu_scan_plan(kernel);
    if (plan.blocks <= 0 || plan.threads_per_block <= 0) {
        why = plan.warnings.empty() ? "CUDA launch planner failed" : plan.warnings.front();
        return false;
    }

    // ОПРОС МОЛЧИТ. Печать -- отдельно, из gpu_backend_print().
    //
    // Опрос стоит до меню (иначе совет о числе потоков устаревает раньше, чем
    // его дочитают), а до меню ещё не выведен даже заголовок программы. Пока
    // печать была здесь, подробности карты вываливались над заголовком, а в
    // разделе "ОБОРУДОВАНИЕ" следом шли два противоречащих совета: "24 потока"
    // от процессора и "1 поток" от карты.
    g_gpu_info = g;
    g_gpu_plan = plan;
    g_gpu_load = load;

    // Совет по умолчанию: ноутбучной карте 70, настольной 100. Настольная
    // рассчитана на длительную полную нагрузку -- сбавлять ей нечего и незачем.
    // Осознанный выбор человека (config.ini, меню) всегда важнее совета.
    g_gpu_power_pct = g_cfg.gpu_power_pct > 0 ? g_cfg.gpu_power_pct
                                              : (g.is_mobile ? 70 : 100);
    if (g_gpu_power_pct < 50)  g_gpu_power_pct = 50;
    if (g_gpu_power_pct > 100) g_gpu_power_pct = 100;

    if (!pkh_gpu_check::run(g.index, plan.threads_per_block, why))
        return false;

    if (!cuda_scan_init(g.index, plan.blocks, plan.threads_per_block,
                        reason, sizeof(reason))) {
        why = reason[0] ? reason : "cuda_scan_init не удалась";
        return false;
    }

    cuda_scan_layout(&g_gpu_blocks, &g_gpu_threads, &g_gpu_vram);

    g_gpu_active = true;
    return true;
}

// Сведения о карте одной опрятной справкой -- вызывается из раздела
// "ОБОРУДОВАНИЕ", то есть уже после заголовка программы и рядом с процессором.
void gpu_backend_print() {
    if (!g_gpu_active) return;
    const GpuInfo &g = g_gpu_info;
    const GpuPlan &p = g_gpu_plan;

    std::cout << "    Видеокарта: " << g.name;
    if (g.is_mobile) std::cout << " (ноутбучная)";
    std::cout << "\n                " << g.cuda_cores() << " ядер CUDA ("
              << g.sm_count << " x " << g.cores_per_sm << "), "
              << (g.mem_total >> 30) << " ГБ";
    if (g.temp_now > 0) std::cout << ", " << g.temp_now << " C";
    std::cout << "\n";

    std::cout << "    Счёт      : на видеокарте, " << g_gpu_blocks << " блоков x "
              << g_gpu_threads << " нитей (всего "
              << ((long long)g_gpu_blocks * g_gpu_threads) << "), "
              << p.keys_in_flight << " ключей в полёте\n"
              << "                под счёт " << (p.vram_needed >> 20)
              << " МБ видеопамяти, пакет " << (2 * CUDA_HALF) << " ключей на нить\n"
              << "                процессор подтверждает кандидатов, 1 поток\n";
    std::cout << "                ядро: " << p.resident_blocks_per_sm
              << " резидентных блоков/SM, очередь до " << p.queued_resident_waves
              << " резидентных волн; свободно при опросе " << (g.mem_free >> 20) << " МБ\n";

    // Замечания советника печатаем только те, что меняют поведение, и без
    // повтора: полный список у него длинный и в шапке ни к чему.
    if (g_gpu_count > 1)
        std::cout << "    Найдено   : " << g_gpu_count
                  << " карт NVIDIA, считает первая\n";

    // ЗДЕСЬ ПЕЧАТАЕТСЯ СОВЕТ, А НЕ ДЕЙСТВУЮЩЕЕ ЗНАЧЕНИЕ, И ЭТО ВАЖНО.
    //
    // Шапка выводится ДО меню, а config.ini читается В меню (см. пояснение к
    // load_config_ini в ui.cpp: карта поднимается раньше файла). Значит в этот
    // момент g_cfg.gpu_power_pct ещё нулевой, и g_gpu_power_pct держит только
    // совет -- 70 для ноутбучной карты, 100 для настольной.
    //
    // Раньше эта строка печатала совет так, будто он и есть настройка. На
    // экране получалось прямое противоречие: в шапке "Мощность: 70%", а через
    // десяток строк в меню "ENTER = 100" -- потому что в файле лежало 100 с
    // прошлого запуска. Какое из двух чисел настоящее, понять было неоткуда.
    //
    // Настоящее значение печатается ниже, в блоке "Конфигурация", уже после
    // меню -- и в обеих ветках, и когда вопросы задавались, и когда взяли
    // прежние настройки целиком.
    std::cout << "    Мощность  : выбирается в меню; совет для "
              << (g_gpu_info.is_mobile ? "ноутбучной" : "настольной")
              << " карты — " << (g_gpu_info.is_mobile ? 70 : 100) << "%\n";
    for (const auto &w : p.warnings)
        std::cout << "    Внимание  : " << w << "\n";
}

// ШАГ ПЕРЕБОРА УХОДИТ НА КАРТУ ОТДЕЛЬНО ОТ ЗАПУСКА.
//
// Причина в порядке: опрос карты стоит ДО меню -- иначе человек видит совет
// "24 потока", а после меню узнаёт, что потоков будет 1. А шаг становится
// известен только ПОСЛЕ меню: HEX-маска задаётся там. Поэтому запуск карты и
// передача шага разведены, и вызывается это отсюда же, откуда шаг уходит в
// процессорный бэкенд, -- из set_scan_step, рядом со scan_secp_set_step.
// Путать шаг нельзя: карта считала бы не те ключи, и молча.
void gpu_now(int *sm_mhz, int *temp_c, int *power_w) {
    *sm_mhz = *temp_c = *power_w = 0;
    if (!g_gpu_active) return;
    // Три числа через запятую; единиц нет (nounits), заголовка нет (noheader).
    const std::string out = plat_run_capture(
        "nvidia-smi -i " + std::to_string(g_gpu_info.index) +
        " --query-gpu=clocks.sm,temperature.gpu,power.draw --format=csv,noheader,nounits "
        + plat_devnull_2());
    double a = 0, b = 0, c = 0;
    if (sscanf(out.c_str(), "%lf , %lf , %lf", &a, &b, &c) == 3 ||
        sscanf(out.c_str(), "%lf, %lf, %lf", &a, &b, &c) == 3) {
        *sm_mhz = (int)a; *temp_c = (int)b; *power_w = (int)(c + 0.5);
    }
}

void gpu_backend_push_step() {
    g_zone_gpu_config.invalidate();
    if (!g_gpu_active) return;
    uint8_t s32[32];
    u256_to_be32(s32, g_step);
    char reason[256] = {0};
    if (!cuda_scan_set_step(s32, reason, sizeof(reason))) {
        std::cout << "  Карта: не удалось задать шаг перебора -- "
                  << (reason[0] ? reason : "причина не сообщена") << "\n"
                  << "  Счёт пойдёт на процессоре.\n";
        gpu_backend_stop();
    }
}

void gpu_backend_stop() {
    g_zone_gpu_config.invalidate();
    if (!g_gpu_active) return;
    g_gpu_active = false;
    cuda_scan_shutdown();
}
#endif  // PKH_HAVE_CUDA


// ========================================================

void scan_chunk(const u256 k0, uint64_t count) {
    if (!count) return;
    pause_check();
    if (g_stop.load(std::memory_order_relaxed)) return;
    if (!g_batch_step_safe) {
        set_sample(k0, count);
        scan_slow_range(k0, count);
        return;
    }
#ifdef PKH_HAVE_CUDA
    if (g_gpu_active) { scan_chunk_gpu(k0, count); return; }
#endif
    scan_chunk_pub(k0, count);
}
