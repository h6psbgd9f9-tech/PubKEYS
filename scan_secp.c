/* _POSIX_C_SOURCE должен быть определён ДО первого системного заголовка --
   иначе <time.h> в строгом -std=c99 не объявляет clock_gettime/
   CLOCK_MONOTONIC вовсе. Нужен только для временного профилирования ниже. */
#define _POSIX_C_SOURCE 199309L

/* ============================================================================
 * scan_secp.c — горячий цикл на арифметике libsecp256k1 (представление 5x52).
 *
 * Алгоритм тот же, что и в чистой версии:
 *
 *   1. Пакет симметричен. От базы P = c*G берутся сразу P+iG и P-iG.
 *      Точке -iG отвечает (x_i, -y_i), значит разность абсцисс x_i - x_P у
 *      обеих одна и та же — одно обращение на две точки вместо двух.
 *
 *   2. Ордината не вычисляется. Сравнивается только X: несжатый ключ
 *      04||X||Y и сжатый 02/03||X определяются координатой X однозначно,
 *      с точностью до знака Y. Знак выясняется один раз, уже после
 *      попадания, и он же отличает ключ k от зеркального n-k.
 *
 *   3. Приватный ключ внутри пакета не считается: индекс и знак дают его
 *      мгновенно, когда попадание уже случилось.
 *
 *   4. k*G — по comb-таблице 4-битных окон: 63 смешанных сложения и ни
 *      одного удвоения. Нужно только при заходе в новый участок.
 *
 * Итого на ключ: одно умножение, одно возведение в квадрат и полтора
 * умножения пакетного обращения.
 * ========================================================================== */
#include "util.h"
#include "int128_impl.h"
#include "field_impl.h"
#include "group_impl.h"

#include "port_compat.h"   /* __builtin_prefetch под MSVC */
#include "scan_secp.h"
#include "bloom.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdio.h>

/* ПРОФИЛИРОВАНИЕ ПО ФАЗАМ.
   Включается переменной окружения SCAN_PROFILE=1 -- ноль накладных
   расходов, если не включено (одна проверка на старте потока).
   Меряет по фазам за батч (не за ключ -- чтобы сам clock_gettime не
   исказил картину): построение dx[], сама инверсия, завершение точки +
   сравнение с целями.

   Раньше здесь стояло «ВРЕМЕННОЕ, уберётся после того, как выясним узкое
   место». Место выяснено (см. подбор SCANPF в Makefile), а код остался --
   и остаётся сознательно: без него любой следующий вопрос «куда уходит
   время» придётся начинать с написания этой же обвязки заново. */
static SECP256K1_INLINE double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}
static __thread double g_prof_dx = 0, g_prof_inv = 0, g_prof_complete = 0;
static __thread uint64_t g_prof_batches = 0;
static __thread int g_prof_on = -1;   /* -1 = ещё не проверяли переменную окружения */

/* ----------------------------- таблицы ----------------------------------- */

/* COMB[i][v] = v * 16^i * G, v = 1..15 — сумма по шестнадцатеричным разрядам */
static secp256k1_ge COMB[64][16];

/* GT[i] = (i+1)*G и заранее отрицённые абсциссы: -x_i нужен на каждый ключ */
static secp256k1_ge GT[SECP_HALF];
static secp256k1_fe GT_NEGX[SECP_HALF];
static secp256k1_ge GSTEP;                  /* SPAN*G — сдвиг базы пакета */

static int g_ready = 0;

/* Порядок группы n и граница n - SPAN.
   Пакет вырожден, когда P = +-(i+1)G, то есть разность абсцисс обращается в
   ноль. Это бывает у ОБОИХ краёв: при ck <= HALF и при ck >= n - HALF
   (там P = -(малое)G). Нижний край проверялся и раньше, верхний -- нет, а он
   достижим: при range_end_bit = 256 конец диапазона обрезан ровно по n-1.
   Ноль в пакетном обращении отравляет не одну пару, а ВЕСЬ пакет из SPAN
   ключей -- fe_inv_var(0) возвращает ноль, и все абсциссы выходят мусором,
   молча, без всякого признака. */
static const uint8_t ORDER_BE[32] = {
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFE,
    0xBA,0xAE,0xDC,0xE6,0xAF,0x48,0xA0,0x3B,
    0xBF,0xD2,0x5E,0x8C,0xD0,0x36,0x41,0x41};
static uint8_t NEAR_ORDER[32];        /* n - SPAN*STEP, с запасом */

/* ============================ ШАГ ПЕРЕБОРА ================================
 *
 *  Пакет накрывает ключи, идущие с постоянным шагом S: [c-HALF*S, c+HALF*S).
 *  Пока S был равен единице, это были подряд идущие числа, и шаг нигде не
 *  выписывался -- он был вшит в то, что таблица кратных строится от G.
 *
 *  HEX-маска замораживает младшие ниббли ключа, и соседние кандидаты
 *  отличаются уже на 16^N, а не на единицу. Подряд они не идут никогда.
 *  Зато они по-прежнему образуют арифметическую прогрессию -- а пакетное
 *  сложение только этого и требует. Поэтому таблица строится от Q = S*G, и
 *  весь остальной горячий цикл не меняется ни на строку.
 *
 *  S = 1 даёт Q = G, то есть прежнюю программу байт в байт. Это не
 *  случайность, а то, чем проверяется отсутствие регресса.
 * ========================================================================= */
static uint8_t STEP[32]      = {0};   /* S               */
static uint8_t STEP_HALF[32] = {0};   /* HALF*S          */
static uint8_t STEP_SPAN[32] = {0};   /* SPAN*S          */

/* out = a * m, m <= 2^32-1. Возвращает 0, если произведение не влезло в 256
   бит.
   Прежде переполнение молча обрезалось, а «вызывающий обязан» ничем в коде не
   выражалось. При mask_froz >= 62 шаг равен 2^248, SPAN*S = 2^258 обрезается в
   НОЛЬ -- и тогда NEAR_ORDER = n, то есть проверка верхнего края не
   срабатывает никогда, проверка нижнего срабатывает только при ck == 0, а
   key_add_span перестаёт двигать центр пакета. Иначе говоря, отключается
   ровно та защита от вырожденного пакета, ради которой написан комментарий
   про «ноль отравляет весь пакет из SPAN ключей молча». Сегодня это гасится
   тем, что при таком шаге кандидатов меньше пакета и всё уходит на поштучный
   проход, -- совпадением двух не связанных ограничений, а не замыслом. */
static int be256_mul_small(const uint8_t a[32], uint32_t m, uint8_t out[32]) {
    uint64_t carry = 0;
    int i;
    for (i = 31; i >= 0; i--) {
        uint64_t v = (uint64_t)a[i] * m + carry;
        out[i] = (uint8_t)(v & 0xFF);
        carry = v >> 8;
    }
    return carry == 0;
}
static void be256_add(uint8_t k[32], const uint8_t d[32]) {
    unsigned carry = 0;
    int i;
    for (i = 31; i >= 0; i--) {
        unsigned v = (unsigned)k[i] + d[i] + carry;
        k[i] = (uint8_t)v; carry = v >> 8;
    }
}
static void be256_sub(uint8_t k[32], const uint8_t d[32]) {
    unsigned bor = 0;
    int i;
    for (i = 31; i >= 0; i--) {
        unsigned v = (unsigned)k[i] - d[i] - bor;
        k[i] = (uint8_t)v; bor = (v >> 8) & 1;
    }
}

static void build_near_order(void) {
    memcpy(NEAR_ORDER, ORDER_BE, 32);
    /* Было вычитание константы SPAN. Теперь запас зависит от шага: пакет
       достаёт на SPAN*S вверх, и у самого верха порядка кривой поштучный
       разбор нужен раньше. */
    be256_sub(NEAR_ORDER, STEP_SPAN);
}

/* ------------------------------- цели ------------------------------------ */

static secp256k1_fe   T_FE[8];        /* до 8 целей — сравнение без карты   */
static uint64_t       T_LIMB[8][4];   /* они же в каноническом виде         */
static int            g_ntargets = 0;
static const uint64_t *g_bloom = NULL;
static uint64_t       g_block_mask = 0;
static secp256k1_fe   g_neg_t0;       /* -X первой цели: путь для n == 1    */

int scan_secp_set_targets(const uint8_t *x32, int n,
                           const uint64_t *bloom, uint64_t block_mask) {
    g_ntargets = 0;
    g_bloom = NULL;
    g_block_mask = 0;
    if (n < 0 || (n > 0 && n <= 8 && !x32)) return 0;
    if (n > 8) {
        /* Без фильтра при n > 8 предфильтр ушёл бы в цикл по T_LIMB и читал
           бы за границей массива из восьми элементов. Вызывающая сторона
           обязана передать карту -- если её нет, это ошибка сборки, а не
           повод искать вслепую. */
        if (!bloom) {
            fprintf(stderr, "scan_secp: %d целей без фильтра Блума -- "
                            "предфильтр не может работать\n", n);
            return 0;
        }
        g_ntargets = n; g_bloom = bloom; g_block_mask = block_mask; return 1;
    }
    g_bloom = NULL;
    for (int i = 0; i < n && i < 8; i++) {
        /* Возврат больше не отбрасывается: цель с X >= p (обрезанная строка
           в pubkey.txt) молча превращалась в неканоническое поле, из которого
           затем строились g_neg_t0 и T_LIMB, и точное сравнение по этой цели
           начинало давать неверный ответ -- тоже молча. */
        if (!secp256k1_fe_set_b32_limit(&T_FE[i], x32 + (size_t)i * 32)) {
            fprintf(stderr, "scan_secp: цель %d имеет X >= p -- негодный ключ\n", i);
            return 0;
        }
        secp256k1_fe t = T_FE[i];
        secp256k1_fe_normalize_var(&t);
        unsigned char b[32];
        secp256k1_fe_get_b32(b, &t);
        for (int j = 0; j < 4; j++) {
            uint64_t v = 0;
            for (int k = 0; k < 8; k++) v = (v << 8) | b[(3 - j) * 8 + k];
            T_LIMB[i][j] = v;
        }
    }
    if (n >= 1) secp256k1_fe_negate(&g_neg_t0, &T_FE[0], 1);
    g_ntargets = n;
    return 1;
}

/* ПОДГОТОВКА ПРОБЫ И САМА ПРОБА — РАЗНЕСЕНЫ НАРОЧНО.
 *
 * На большой базе карта фильтра не помещается в кэш, а её адрес зависит от
 * только что посчитанной абсциссы: предсказать его процессор не может, и
 * каждый ключ платит промахом в память. Но абсциссы в пакете независимы,
 * поэтому адрес блока считается сразу (probe_prep), строка запрашивается
 * через __builtin_prefetch, а сама проверка (probe_test) делается через
 * SCAN_PF ключей — когда строка уже пришла. Ожидание перекрывается
 * арифметикой следующих ключей. */
typedef struct { uint64_t limb[4]; const uint64_t *blk; } probe;

static SECP256K1_INLINE void probe_limbs(const secp256k1_fe *x, uint64_t limb[4]) {
    secp256k1_fe t = *x;
    secp256k1_fe_normalize_var(&t);
#if defined(SECP256K1_WIDEMUL_INT128)
    limb[0] =  t.n[0]        | (t.n[1] << 52);
    limb[1] = (t.n[1] >> 12) | (t.n[2] << 40);
    limb[2] = (t.n[2] >> 24) | (t.n[3] << 28);
    limb[3] = (t.n[3] >> 36) | (t.n[4] << 16);
#else
    {   unsigned char b[32];
        int j, k;
        secp256k1_fe_get_b32(b, &t);
        for (j = 0; j < 4; j++) {
            uint64_t v = 0;
            for (k = 0; k < 8; k++) v = (v << 8) | b[(3 - j) * 8 + k];
            limb[j] = v;
        }
    }
#endif
}

static SECP256K1_INLINE void probe_prep(const secp256k1_fe *x, probe *p) {
    probe_limbs(x, p->limb);
    p->blk = g_bloom ? g_bloom + bloom_block_of(p->limb, g_block_mask) * BLOOM_BLOCK_WORDS
                     : NULL;
#if defined(__GNUC__)
    if (p->blk) __builtin_prefetch(p->blk, 0, 3);
#endif
}

static SECP256K1_INLINE int probe_test(const probe *p) {
    if (p->blk) return bloom_maybe_at(p->blk, p->limb);
    {   int i;
        for (i = 0; i < g_ntargets; i++) if (p->limb[3] == T_LIMB[i][3]) return 1;
    }
    return 0;
}

/* Кандидат ли? Ложные срабатывания допустимы, пропуски — нет.
   Остаётся для центра пакета и для вырожденного разбора — там ключей мало и
   конвейер не нужен. */
static SECP256K1_INLINE int prefilter(const secp256k1_fe *x) {
    if (g_ntargets == 1) {
        /* Самый частый случай: одна цель. Вычитаем и проверяем на нуль —
           проверка выходит по первому же несовпавшему лимбу. */
        secp256k1_fe d = *x;
        secp256k1_fe_add(&d, &g_neg_t0);
        return secp256k1_fe_normalizes_to_zero_var(&d);
    }
    /* Лимбы обязаны совпасть с теми, по которым фильтр строился в C++,
       иначе хеши разойдутся и попадания начнут теряться — молча. */
    uint64_t limb[4];
    probe_limbs(x, limb);

    if (g_bloom) return bloom_maybe(g_bloom, g_block_mask, limb);
    for (int i = 0; i < g_ntargets; i++) if (limb[3] == T_LIMB[i][3]) return 1;
    return 0;
}

/* ---------------------------- построение --------------------------------- */

static int build_comb(void) {
    const int N = 64 * 15;
    secp256k1_gej *tmp = (secp256k1_gej *)malloc(sizeof(secp256k1_gej) * N);
    secp256k1_ge  *out = (secp256k1_ge  *)malloc(sizeof(secp256k1_ge)  * N);
    if (!tmp || !out) { free(tmp); free(out); return 0; }

    secp256k1_ge G = secp256k1_ge_const_g;
    secp256k1_gej base;
    secp256k1_gej_set_ge(&base, &G);

    for (int i = 0; i < 64; i++) {
        tmp[i * 15 + 0] = base;                       /* 1 * 16^i * G */
        for (int v = 1; v < 15; v++)
            secp256k1_gej_add_var(&tmp[i*15+v], &tmp[i*15+v-1], &base, NULL);
        for (int d = 0; d < 4; d++)                   /* base *= 16 */
            secp256k1_gej_double_var(&base, &base, NULL);
    }
    secp256k1_ge_set_all_gej_var(out, tmp, N);
    for (int i = 0; i < 64; i++)
        for (int v = 1; v < 16; v++) {
            COMB[i][v] = out[i * 15 + (v - 1)];
            secp256k1_fe_normalize_var(&COMB[i][v].x);
            secp256k1_fe_normalize_var(&COMB[i][v].y);
        }
    free(tmp); free(out);
    return 1;
}

static void mul_g(const uint8_t k[32], secp256k1_ge *r);   /* ниже */

static int build_gt(void) {
    secp256k1_gej *tmp = (secp256k1_gej *)malloc(sizeof(secp256k1_gej) * (SECP_HALF + 1));
    secp256k1_ge  *out = (secp256k1_ge  *)malloc(sizeof(secp256k1_ge)  * (SECP_HALF + 1));
    if (!tmp || !out) { free(tmp); free(out); return 0; }

    /* Q = S*G. При S = 1 это в точности G, и вся таблица ниже совпадает с
       прежней -- проверяется тестом «шаг 1 -> те же таблицы». */
    secp256k1_ge Q;
    mul_g(STEP, &Q);
    if (secp256k1_ge_is_infinity(&Q)) { free(tmp); free(out); return 0; }
    secp256k1_fe_normalize_var(&Q.x);
    secp256k1_fe_normalize_var(&Q.y);

    secp256k1_gej_set_ge(&tmp[0], &Q);
    for (int i = 1; i <= SECP_HALF; i++)
        secp256k1_gej_add_ge_var(&tmp[i], &tmp[i-1], &Q, NULL);
    /* tmp[i] = (i+1)*Q, всего HALF+1 штук: последний пойдёт на шаг пакета */
    secp256k1_ge_set_all_gej_var(out, tmp, SECP_HALF + 1);

    for (int i = 0; i < SECP_HALF; i++) {
        GT[i] = out[i];
        secp256k1_fe_normalize_var(&GT[i].x);
        secp256k1_fe_normalize_var(&GT[i].y);
        secp256k1_fe_negate(&GT_NEGX[i], &GT[i].x, 1);
    }

    /* GSTEP = SPAN*Q = 2*(HALF*Q) = удвоенный последний элемент таблицы.
       Раньше считалось отдельно по comb-таблице со скаляром SPAN; со
       свободным шагом скаляр был бы SPAN*S и мог бы не влезть в 256 бит,
       а удвоение точки не зависит ни от того, ни от другого. */
    {
        secp256k1_gej t;
        secp256k1_gej_set_ge(&t, &GT[SECP_HALF - 1]);   /* HALF*Q */
        secp256k1_gej_double_var(&t, &t, NULL);
        secp256k1_ge_set_gej_var(&GSTEP, &t);
        secp256k1_fe_normalize_var(&GSTEP.x);
        secp256k1_fe_normalize_var(&GSTEP.y);
    }
    free(tmp); free(out);
    return 1;
}

int scan_secp_set_step(const uint8_t s32[32]) {
    int i, zero = 1;
    for (i = 0; i < 32; i++) if (s32[i]) { zero = 0; break; }
    if (zero) return 0;                       /* шаг 0 -- один ключ вечно */
    memcpy(STEP, s32, 32);
    /* Шаг, при котором пакет уходит за 2^256, отвергается ЗДЕСЬ. Молча
       обрезанный STEP_SPAN обнуляет защиту от вырожденного пакета -- см.
       be256_mul_small. */
    if (!be256_mul_small(STEP, (uint32_t)SECP_HALF, STEP_HALF)) return 0;
    if (!be256_mul_small(STEP, (uint32_t)SECP_SPAN, STEP_SPAN)) return 0;
    if (!g_ready) return 1;                   /* таблицы построит init */
    if (!build_gt()) return 0;                /* иначе перестраиваем сейчас */
    build_near_order();
    return 1;
}

int scan_secp_init(void) {
    if (g_ready) return 1;
    { int i, zero = 1;
      for (i = 0; i < 32; i++) if (STEP[i]) { zero = 0; break; }
      if (zero) { memset(STEP, 0, 32); STEP[31] = 1;      /* по умолчанию S = 1 */
                  be256_mul_small(STEP, (uint32_t)SECP_HALF, STEP_HALF);
                  be256_mul_small(STEP, (uint32_t)SECP_SPAN, STEP_SPAN); } }
    if (!build_comb()) return 0;              /* comb нужен build_gt: Q = S*G */
    if (!build_gt())   return 0;
    build_near_order();
    g_ready = 1;
    return 1;
}

/* k*G по comb-таблице: 63 сложения, ни одного удвоения */
static void mul_g(const uint8_t k[32], secp256k1_ge *r) {
    secp256k1_gej acc;
    secp256k1_gej_set_infinity(&acc);
    for (int i = 0; i < 64; i++) {
        int byte = 31 - (i >> 1);
        int v = (i & 1) ? (k[byte] >> 4) : (k[byte] & 15);
        if (v) secp256k1_gej_add_ge_var(&acc, &acc, &COMB[i][v], NULL);
    }
    secp256k1_ge_set_gej_var(r, &acc);
}

int scan_secp_pubkey(const uint8_t key[32], uint8_t out_c33[33], uint8_t out_y32[32]) {
    if (!g_ready && !scan_secp_init()) return 0;
    int zero = 1;
    for (int i = 0; i < 32; i++) if (key[i]) { zero = 0; break; }
    if (zero) return 0;

    secp256k1_ge p;
    mul_g(key, &p);
    if (secp256k1_ge_is_infinity(&p)) return 0;
    secp256k1_fe_normalize_var(&p.x);
    secp256k1_fe_normalize_var(&p.y);
    if (out_c33) {
        out_c33[0] = (uint8_t)(0x02 | secp256k1_fe_is_odd(&p.y));
        secp256k1_fe_get_b32(out_c33 + 1, &p.x);
    }
    if (out_y32) secp256k1_fe_get_b32(out_y32, &p.y);
    return 1;
}

/* --------------------------- рабочее состояние --------------------------- */

/* Глубина конвейера предвыборки: на сколько ключей вперёд запрашивается
   строка кэша. Меньше — не успевает прийти, больше — кольцо вытесняет из L1
   то, что нужно арифметике. Задаётся при сборке: make secp SCANPF=16 */
#ifndef SCAN_PF
#define SCAN_PF 12
#endif

typedef struct {
    secp256k1_fe dx[SECP_HALF];
    secp256k1_fe iv[SECP_HALF];
} state;

void *scan_secp_create(void) {
    if (!g_ready && !scan_secp_init()) return NULL;
    return malloc(sizeof(state));
}

void scan_secp_destroy(void *h) { free(h); }

/* прибавить SPAN*S к 32-байтовому ключу (big-endian) */
static void key_add_span(uint8_t k[32]) { be256_add(k, STEP_SPAN); }

/* ------------------------------ горячий цикл ----------------------------- */

uint64_t scan_secp_run(void *h, const uint8_t center_key[32], uint64_t nbatches,
                       scan_secp_cb cb, void *user, const volatile int *stop) {
    state *s = (state *)h;
    if (!s || !g_ready || g_ntargets <= 0) return 0;

    uint8_t ck[32];
    memcpy(ck, center_key, 32);

    secp256k1_ge P;
    mul_g(ck, &P);
    if (secp256k1_ge_is_infinity(&P)) return 0;
    secp256k1_fe_normalize_var(&P.x);
    secp256k1_fe_normalize_var(&P.y);

    uint64_t done = 0;
    if (g_prof_on < 0) g_prof_on = (getenv("SCAN_PROFILE") != NULL);

    for (uint64_t b = 0; b < nbatches; b++) {
        if (stop && *stop) break;

        secp256k1_fe negx0, negy0;
        secp256k1_fe_negate(&negx0, &P.x, 1);
        secp256k1_fe_negate(&negy0, &P.y, 1);

        /* центр пакета */
        if (prefilter(&P.x)) cb(ck, 0, 1, user);

        double t0 = g_prof_on ? now_sec() : 0;

        for (int i = 0; i < SECP_HALF; i++) {
            s->dx[i] = GT[i].x;
            secp256k1_fe_add(&s->dx[i], &negx0);
        }

        double t1 = g_prof_on ? now_sec() : 0;

        /* Нулевая разность значит P = ±(i+1)G, то есть сам ключ меньше пакета.
           Проверять это на каждой из 512 разностей — половина лишней проверки
           на ключ; вместо этого смотрим на сам центр: вырождение возможно
           только когда он не больше размера пакета. */
        int degenerate = 0;
        {
            /* Пакет тянется вниз на HALF*S и вверх на HALF*S. Проверка
               сравнивала центр с константой SPAN -- это верно только при
               шаге 1. При маске шаг равен 16^N, и «маленький центр» надо
               мерить в тех же единицах, иначе вырожденный пакет уйдёт по
               быстрому пути и выдаст мусор вместо ключа. Сравнение
               256-битное: STEP_SPAN сам по себе может быть огромным. */
            if (memcmp(ck, STEP_SPAN, 32) <= 0) degenerate = 1;
            /* верхний край: ck >= n - SPAN*S */
            if (!degenerate && memcmp(ck, NEAR_ORDER, 32) >= 0) degenerate = 1;
        }
        if (degenerate) {
            /* редкий случай: разбираем пакет поштучно, честным k*G */
            uint8_t k[32];
            for (int i = 1; i <= SECP_HALF; i++) {
                for (int sgn = 0; sgn < 2; sgn++) {
                    if (sgn == 0 && i >= SECP_HALF) continue;   /* верхний край */
                    /* Смещение внутри пакета -- i ШАГОВ, а не i единиц. */
                    memcpy(k, ck, 32);
                    { uint8_t off[32];
                      be256_mul_small(STEP, (uint32_t)i, off);
                      if (sgn == 0) be256_add(k, off); else be256_sub(k, off); }
                    secp256k1_ge q;
                    int nz = 0;
                    for (int j = 0; j < 32; j++) if (k[j]) { nz = 1; break; }
                    if (!nz) continue;
                    mul_g(k, &q);
                    if (secp256k1_ge_is_infinity(&q)) continue;
                    secp256k1_fe_normalize_var(&q.x);
                    if (prefilter(&q.x)) cb(ck, i, sgn == 0, user);
                }
            }
            done += SECP_SPAN;
            key_add_span(ck);
            {   /* сдвиг базы */
                secp256k1_gej t;
                secp256k1_gej_set_ge(&t, &P);
                secp256k1_gej_add_ge_var(&t, &t, &GSTEP, NULL);
                secp256k1_ge_set_gej_var(&P, &t);
                secp256k1_fe_normalize_var(&P.x);
                secp256k1_fe_normalize_var(&P.y);
            }
            continue;
        }

        /* пакетное обращение Монтгомери: одно обращение на 512 разностей */
        s->iv[0] = s->dx[0];
        for (int i = 1; i < SECP_HALF; i++)
            secp256k1_fe_mul(&s->iv[i], &s->iv[i-1], &s->dx[i]);

        secp256k1_fe acc;
        secp256k1_fe_inv_var(&acc, &s->iv[SECP_HALF - 1]);
        for (int i = SECP_HALF - 1; i >= 1; i--) {
            secp256k1_fe t;
            secp256k1_fe_mul(&t, &acc, &s->iv[i-1]);
            secp256k1_fe_mul(&acc, &acc, &s->dx[i]);
            s->iv[i] = t;
        }
        s->iv[0] = acc;

        double t2 = g_prof_on ? now_sec() : 0;

        /* Одна цель — точное сравнение без карты и без промахов в память:
           конвейер там не нужен и только мешал бы. */
        int pipelined = (g_ntargets > 1);
        probe ring[SCAN_PF];
        int ring_idx[SCAN_PF], ring_plus[SCAN_PF];
        int nq = 0;

        for (int i = 0; i < SECP_HALF; i++) {
            secp256k1_fe dy, lam, xr;

            /* P + (i+1)G. Верхний край пропускается: пакет покрывает
               [c-HALF, c+HALF-1], то есть ровно 2*HALF ключей без нахлёста
               с соседним пакетом. */
            if (i + 1 < SECP_HALF) {
                dy = GT[i].y;
                secp256k1_fe_add(&dy, &negy0);
                secp256k1_fe_mul(&lam, &dy, &s->iv[i]);
                secp256k1_fe_sqr(&xr, &lam);
                secp256k1_fe_add(&xr, &GT_NEGX[i]);
                secp256k1_fe_add(&xr, &negx0);
                if (!pipelined) {
                    if (prefilter(&xr)) cb(ck, i + 1, 1, user);
                } else {
                    int q = nq % SCAN_PF;
                    if (nq >= SCAN_PF && probe_test(&ring[q]))
                        cb(ck, ring_idx[q], ring_plus[q], user);
                    probe_prep(&xr, &ring[q]);
                    ring_idx[q] = i + 1; ring_plus[q] = 1;
                    nq++;
                }
            }

            /* P - (i+1)G: наклон отличается знаком, а квадрат знака не помнит */
            dy = GT[i].y;
            secp256k1_fe_add(&dy, &P.y);
            secp256k1_fe_mul(&lam, &dy, &s->iv[i]);
            secp256k1_fe_sqr(&xr, &lam);
            secp256k1_fe_add(&xr, &GT_NEGX[i]);
            secp256k1_fe_add(&xr, &negx0);
            if (!pipelined) {
                if (prefilter(&xr)) cb(ck, i + 1, 0, user);
            } else {
                int q = nq % SCAN_PF;
                if (nq >= SCAN_PF && probe_test(&ring[q]))
                    cb(ck, ring_idx[q], ring_plus[q], user);
                probe_prep(&xr, &ring[q]);
                ring_idx[q] = i + 1; ring_plus[q] = 0;
                nq++;
            }
        }

        /* Хвост конвейера. Закрывается внутри пакета: дальше ck сдвигается,
           и номер в пакете стал бы отсчитываться от другого центра. */
        if (pipelined) {
            int have = (nq < SCAN_PF) ? nq : SCAN_PF;
            for (int t = nq - have; t < nq; t++) {
                int q = t % SCAN_PF;
                if (probe_test(&ring[q])) cb(ck, ring_idx[q], ring_plus[q], user);
            }
        }

        if (g_prof_on) {
            double t3 = now_sec();
            g_prof_dx       += t1 - t0;
            g_prof_inv      += t2 - t1;
            g_prof_complete += t3 - t2;
            g_prof_batches++;
            if (g_prof_batches % 20000 == 0) {
                double total = g_prof_dx + g_prof_inv + g_prof_complete;
                if (total <= 0.0) total = 1e-9;   /* иначе nan/inf в выводе */
                fprintf(stderr,
                    "[PROFILE поток] батчей=%llu | dx: %.1f%% | инверсия: %.1f%% | "
                    "завершение+сравнение: %.1f%% | (dx=%.3fs инверсия=%.3fs "
                    "заверш=%.3fs всего=%.3fs)\n",
                    (unsigned long long)g_prof_batches,
                    100.0*g_prof_dx/total, 100.0*g_prof_inv/total, 100.0*g_prof_complete/total,
                    g_prof_dx, g_prof_inv, g_prof_complete, total);
            }
        }

        done += SECP_SPAN;
        key_add_span(ck);

        secp256k1_gej t;
        secp256k1_gej_set_ge(&t, &P);
        secp256k1_gej_add_ge_var(&t, &t, &GSTEP, NULL);
        secp256k1_ge_set_gej_var(&P, &t);
        secp256k1_fe_normalize_var(&P.x);
        secp256k1_fe_normalize_var(&P.y);
    }
    return done;
}
