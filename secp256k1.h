// ============================================================================
//  secp256k1.h  —  арифметика кривой, заточенная под перебор
//
//  Поле:   p = 2^256 - 2^32 - 977
//  Кривая: y^2 = x^3 + 7
//
//  Элемент поля — uint64_t[4], лимбы по возрастанию (little-endian).
//  Все операции возвращают канонический результат (< p), поэтому X можно
//  сравнивать прямо в лимбах, без перевода в байты.
//
//  ВХОДЫ fe_add / fe_sub / fe_neg ОБЯЗАНЫ БЫТЬ КАНОНИЧЕСКИМИ (< p).
//  fe_mul и fe_sqr гоняют результат через fe_reduce512 и потому терпят любой
//  вход, а сложение и вычитание -- нет: там свёртка переноса рассчитана на
//  операнды меньше p. Разница важна, потому что fe_from_be32 (ниже) кладёт
//  32 байта в лимбы БЕЗ приведения: её выход годится как 256-битный скаляр,
//  но перед подачей в сложение/вычитание нуждается в fe_reduce_once либо в
//  проверке X < p (см. fe_from_be32_checked в kangaroo.cpp).
//
//  Что здесь сделано ради скорости:
//    * отдельное возведение в квадрат: 10 умножений вместо 16;
//    * приведение по модулю без цикла — одно условное вычитание;
//    * пакетное обращение (приём Монтгомери);
//    * k*G по comb-таблице 4-битных окон в проективных координатах Якоби:
//      63 сложения вместо 256 удвоений с обращением на каждом шаге.
// ============================================================================
#pragma once
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <mutex>      // std::call_once для таблицы COMB (см. ниже)

typedef uint64_t fe[4];
typedef uint64_t u256[4];

// u128 БЕРЁТСЯ ИЗ ЕДИНОГО ИСТОЧНИКА, А НЕ ОБЪЯВЛЯЕТСЯ ЗДЕСЬ ЗАНОВО.
//
// Здесь стояло `typedef unsigned __int128 u128;` -- нативный тип GCC/Clang.
// На MSVC его нет, а u128_compat.h подставляет структуру с теми же
// операторами. Пока secp256k1.h объявлял СВОЙ u128, эти два объявления
// сталкивались: файл, включивший и то и другое (а через common.h это
// каждый файл программы), получал "conflicting declaration u128" на MSVC.
// Именно из-за этого не собирались cuda_field_test и kangaroo.
//
// У u128_compat.h есть свой include-guard, поэтому повторное включение
// безвредно. Правильность структурного u128 в ЭТОМ файле проверяет
// test_u128.cpp: он собирает secp256k1.h дважды -- с нативным типом и с
// принудительной структурой -- и сверяет fe_mul/fe_inv значение в значение.
#include "u128_compat.h"

// p = 2^256 - 0x1000003D1
static const uint64_t FE_P[4] = {
    0xFFFFFFFEFFFFFC2FULL, 0xFFFFFFFFFFFFFFFFULL,
    0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL};

// порядок группы n
static const uint64_t EC_N[4] = {
    0xBFD25E8CD0364141ULL, 0xBAAEDCE6AF48A03BULL,
    0xFFFFFFFFFFFFFFFEULL, 0xFFFFFFFFFFFFFFFFULL};

static const uint64_t FE_C = 0x1000003D1ULL;   // 2^256 mod p

// --------------------------- базовые действия -------------------------------

static inline void fe_set(fe r, const fe a) { memcpy(r, a, 32); }
static inline void fe_zero(fe r) { r[0] = r[1] = r[2] = r[3] = 0; }
static inline void fe_one(fe r) { r[0] = 1; r[1] = r[2] = r[3] = 0; }
static inline bool fe_is_zero(const fe a) { return (a[0] | a[1] | a[2] | a[3]) == 0; }
static inline bool fe_eq(const fe a, const fe b) {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

// Если r >= p, вычесть p. Раньше здесь стоял цикл while с отдельной проверкой
// «больше либо равно»: два прохода по лимбам вместо одного. Теперь разность
// считается сразу, а её заём и говорит, нужна ли она была.
static inline void fe_reduce_once(fe r) {
    uint64_t t[4];
    u128 br = 0;
    for (int i = 0; i < 4; i++) {
        u128 v = (u128)r[i] - FE_P[i] - br;
        t[i] = (uint64_t)v;
        br = (v >> 64) & 1;
    }
    if (!br) memcpy(r, t, 32);      // заёма нет => r было >= p
}

static inline void fe_add(fe r, const fe a, const fe b) {
    u128 c = 0;
    for (int i = 0; i < 4; i++) { c += (u128)a[i] + b[i]; r[i] = (uint64_t)c; c >>= 64; }
    if ((uint64_t)c) {              // перенос за 2^256 => прибавляем C
        c = (u128)r[0] + FE_C;
        r[0] = (uint64_t)c; c >>= 64;
        for (int i = 1; i < 4 && c; i++) { c += r[i]; r[i] = (uint64_t)c; c >>= 64; }
    }
    fe_reduce_once(r);
}

static inline void fe_sub(fe r, const fe a, const fe b) {
    u128 br = 0;
    for (int i = 0; i < 4; i++) {
        u128 t = (u128)a[i] - b[i] - br;
        r[i] = (uint64_t)t;
        br = (t >> 64) & 1;
    }
    if (br) {                       // ушли ниже нуля => прибавляем p
        u128 c = 0;
        for (int i = 0; i < 4; i++) { c += (u128)r[i] + FE_P[i]; r[i] = (uint64_t)c; c >>= 64; }
    }
    // Одно условное вычитание, как в fe_add. При каноническом входе оно не
    // срабатывает никогда и стоит один незанятый переход; при входе >= p
    // (fe_from_be32) оно втягивает результат обратно в поле вместо того,
    // чтобы отдать неканоничное значение, которое дальше сравнивается по
    // лимбам и потому даёт неверное "не равно".
    fe_reduce_once(r);
}

static inline void fe_neg(fe r, const fe a) {
    if (fe_is_zero(a)) { fe_zero(r); return; }
    u128 br = 0;
    for (int i = 0; i < 4; i++) {
        u128 t = (u128)FE_P[i] - a[i] - br;
        r[i] = (uint64_t)t;
        br = (t >> 64) & 1;
    }
}

// Приведение 512-битного произведения. 2^256 == C (mod p), поэтому старшая
// половина просто умножается на C и складывается с младшей; двух проходов
// достаточно, потому что C помещается в 33 бита.
static inline void fe_reduce512(const uint64_t t[8], fe r) {
    const u128 C = (u128)FE_C;
    uint64_t m4;
    u128 acc;

    acc  = (u128)t[0] + (u128)t[4] * C;  uint64_t m0 = (uint64_t)acc; acc >>= 64;
    acc += (u128)t[1] + (u128)t[5] * C;  uint64_t m1 = (uint64_t)acc; acc >>= 64;
    acc += (u128)t[2] + (u128)t[6] * C;  uint64_t m2 = (uint64_t)acc; acc >>= 64;
    acc += (u128)t[3] + (u128)t[7] * C;  uint64_t m3 = (uint64_t)acc; acc >>= 64;
    m4 = (uint64_t)acc;                                     // < 2^34

    acc  = (u128)m0 + (u128)m4 * C;  r[0] = (uint64_t)acc; acc >>= 64;
    acc += m1;                       r[1] = (uint64_t)acc; acc >>= 64;
    acc += m2;                       r[2] = (uint64_t)acc; acc >>= 64;
    acc += m3;                       r[3] = (uint64_t)acc; acc >>= 64;

    if ((uint64_t)acc) {
        acc = (u128)r[0] + C;
        r[0] = (uint64_t)acc; acc >>= 64;
        for (int i = 1; i < 4 && acc; i++) { acc += r[i]; r[i] = (uint64_t)acc; acc >>= 64; }
    }
    fe_reduce_once(r);
}

static inline void fe_mul(fe r, const fe a, const fe b) {
    // НАКОПИТЕЛЬ ИЗ ТРЁХ СЛОВ, А НЕ ИЗ ДВУХ.
    //
    // Прежняя версия копила столбец в одной 128-битной переменной:
    //     c = k2 + (u128)a[0]*b[3];
    // Произведение доходит до 2^128 - 2^65 + 1, накопленный перенос -- до
    // 2^65. Их сумма выходит за 2^128, и старший бит терялся МОЛЧА.
    //
    // Оттого ошибка была видна только у операндов с единицами в старших
    // разрядах: у самого верха поля неверны все произведения, при двух
    // старших лимбах из единиц -- половина, у случайных значений -- ни одного.
    // Именно поэтому её не поймал ни один тест: все они брали случайные числа.
    //
    // Нашлась при сверке с реализацией для видеокарты, где то же умножение
    // написано заново; расхождение подтверждено независимым счётом.
    //
    // Теперь переполнение за 2^128 не отбрасывается, а копится в acc2 и
    // переносится в следующий столбец.
    uint64_t t[8];
    u128     acc  = 0;
    uint64_t acc2 = 0;

    for (int col = 0; col < 7; col++) {
        for (int i = 0; i < 4; i++) {
            const int j = col - i;
            if (j < 0 || j > 3) continue;
            const u128 prod = (u128)a[i] * b[j];
            const u128 old  = acc;
            acc += prod;
            if (acc < old) acc2++;              // ушли за 2^128
        }
        t[col] = (uint64_t)acc;
        acc  = (acc >> 64) | ((u128)acc2 << 64);
        acc2 = 0;
    }
    t[7] = (uint64_t)acc;

    fe_reduce512(t, r);
}

// Возведение в квадрат: перекрёстные произведения a_i*a_j (i<j) считаются один
// раз, удваиваются, затем добавляются квадраты по диагонали. 6 умножений
// вместо 12 в перекрёстной части — итого 10 против 16.
static inline void fe_sqr(fe r, const fe a) {
    uint64_t m[8] = {0,0,0,0,0,0,0,0};
    u128 c;

    c  = (u128)a[0]*a[1];              m[1] = (uint64_t)c; c >>= 64;
    c += (u128)a[0]*a[2];              m[2] = (uint64_t)c; c >>= 64;
    c += (u128)a[0]*a[3];              m[3] = (uint64_t)c; c >>= 64;
    m[4] = (uint64_t)c;

    c  = (u128)m[3] + (u128)a[1]*a[2]; m[3] = (uint64_t)c; c >>= 64;
    c += (u128)m[4] + (u128)a[1]*a[3]; m[4] = (uint64_t)c; c >>= 64;
    m[5] = (uint64_t)c;

    c  = (u128)m[5] + (u128)a[2]*a[3]; m[5] = (uint64_t)c; c >>= 64;
    m[6] = (uint64_t)c;

    uint64_t carry = 0;                                  // удвоение
    for (int i = 0; i < 8; i++) {
        uint64_t nv = (m[i] << 1) | carry;
        carry = m[i] >> 63;
        m[i] = nv;
    }

    u128 c2 = 0;                                         // диагональ
    for (int i = 0; i < 4; i++) {
        c2 += (u128)m[2*i] + (u128)a[i]*a[i];
        m[2*i] = (uint64_t)c2; c2 >>= 64;
        c2 += m[2*i+1];
        m[2*i+1] = (uint64_t)c2; c2 >>= 64;
    }

    fe_reduce512(m, r);
}

static inline void fe_sqr_n(fe r, const fe a, int n) {
    fe_sqr(r, a);
    for (int i = 1; i < n; i++) fe_sqr(r, r);
}

// Обращение по малой теореме Ферма, a^(p-2). Цепочка сложений — как в
// libsecp256k1: 255 возведений в квадрат и 15 умножений.
static inline void fe_inv(fe r, const fe a) {
    fe x2, x3, x6, x9, x11, x22, x44, x88, x176, x220, x223, t;

    fe_sqr(x2, a);           fe_mul(x2, x2, a);
    fe_sqr(x3, x2);          fe_mul(x3, x3, a);
    fe_sqr_n(x6, x3, 3);     fe_mul(x6, x6, x3);
    fe_sqr_n(x9, x6, 3);     fe_mul(x9, x9, x3);
    fe_sqr_n(x11, x9, 2);    fe_mul(x11, x11, x2);
    fe_sqr_n(x22, x11, 11);  fe_mul(x22, x22, x11);
    fe_sqr_n(x44, x22, 22);  fe_mul(x44, x44, x22);
    fe_sqr_n(x88, x44, 44);  fe_mul(x88, x88, x44);
    fe_sqr_n(x176, x88, 88); fe_mul(x176, x176, x88);
    fe_sqr_n(x220, x176, 44);fe_mul(x220, x220, x44);
    fe_sqr_n(x223, x220, 3); fe_mul(x223, x223, x3);

    fe_sqr_n(t, x223, 23);   fe_mul(t, t, x22);
    fe_sqr_n(t, t, 5);       fe_mul(t, t, a);
    fe_sqr_n(t, t, 3);       fe_mul(t, t, x2);
    fe_sqr_n(t, t, 2);       fe_mul(r, t, a);
}

// Пакетное обращение (приём Монтгомери): одно обращение на весь массив
// плюс три умножения на элемент.
//
// Требования к вызывающему:
//   * scratch -- массив длиной не меньше n;
//   * НИ ОДИН a[i] не равен нулю. Ноль отравляет не одну пару, а ВЕСЬ пакет,
//     и молча: fe_inv(0) возвращает 0, после чего каждое inv[i] -- мусор.
//     Вызывающие подменяют нули заранее (engine.cpp, scan_secp.c).
// Совмещение inv и a допустимо: работает и fe_inv_batch(x, x, n, s).
static inline void fe_inv_batch(fe *inv, const fe *a, int n, fe *scratch) {
    if (n <= 0) return;
    fe_set(scratch[0], a[0]);
    for (int i = 1; i < n; i++) fe_mul(scratch[i], scratch[i-1], a[i]);
    fe acc, t;
    fe_inv(acc, scratch[n-1]);
    for (int i = n - 1; i > 0; i--) {
        fe_mul(t, acc, scratch[i-1]);
        // ПОРЯДОК ЭТИХ ДВУХ СТРОК -- НЕ СТИЛЬ.
        //
        // a[i] обязан быть прочитан ДО записи в inv[i]. Иначе при совмещённых
        // массивах (fe_inv_batch(x, x, ...) -- естественный способ обратить
        // массив на месте) аккумулятор домножается на a[i]^-1 вместо a[i], и
        // весь остаток цепочки выходит мусором. Молча: неверная абсцисса
        // просто не совпадёт с целью, то есть попадание будет потеряно.
        fe_mul(acc, acc, a[i]);
        fe_set(inv[i], t);
    }
    fe_set(inv[0], acc);
}

// ------------------------------ точки ---------------------------------------

struct Point {
    fe x, y;
    bool inf;
    Point() : inf(true) { fe_zero(x); fe_zero(y); }
};

static const uint64_t GX[4] = {
    0x59F2815B16F81798ULL, 0x029BFCDB2DCE28D9ULL,
    0x55A06295CE870B07ULL, 0x79BE667EF9DCBBACULL};
static const uint64_t GY[4] = {
    0x9C47D08FFB10D4B8ULL, 0xFD17B448A6855419ULL,
    0x5DA4FBFC0E1108A8ULL, 0x483ADA7726A3C465ULL};

static inline Point ec_G() {
    Point g; fe_set(g.x, GX); fe_set(g.y, GY); g.inf = false; return g;
}

// Аффинное удвоение и сложение — с полным обращением. Вызываются редко:
// при построении таблиц и при сдвиге базы пакета.
static inline Point ec_dbl(const Point &p) {
    Point r;
    if (p.inf || fe_is_zero(p.y)) return r;
    fe num, den, s, t1, t2;
    fe_sqr(num, p.x);
    fe_add(t1, num, num);
    fe_add(num, t1, num);          // 3x^2
    fe_add(den, p.y, p.y);         // 2y
    fe_inv(t1, den);
    fe_mul(s, num, t1);
    fe_sqr(t1, s);
    fe_add(t2, p.x, p.x);
    fe_sub(r.x, t1, t2);
    fe_sub(t1, p.x, r.x);
    fe_mul(t2, s, t1);
    fe_sub(r.y, t2, p.y);
    r.inf = false;
    return r;
}

static inline Point ec_add(const Point &p, const Point &q) {
    if (p.inf) return q;
    if (q.inf) return p;
    if (fe_eq(p.x, q.x)) {
        if (fe_eq(p.y, q.y)) return ec_dbl(p);
        Point r; return r;
    }
    Point r;
    fe dx, dy, inv, s, t1, t2;
    fe_sub(dx, q.x, p.x);
    fe_sub(dy, q.y, p.y);
    fe_inv(inv, dx);
    fe_mul(s, dy, inv);
    fe_sqr(t1, s);
    fe_sub(t1, t1, p.x);
    fe_sub(r.x, t1, q.x);
    fe_sub(t2, p.x, r.x);
    fe_mul(t1, s, t2);
    fe_sub(r.y, t1, p.y);
    r.inf = false;
    return r;
}

// ------------------------- 256-битные скаляры -------------------------------

static inline void u256_zero(u256 a) { a[0] = a[1] = a[2] = a[3] = 0; }
static inline void u256_set(u256 r, const u256 a) { memcpy(r, a, 32); }
static inline void u256_from_u64(u256 r, uint64_t v) { r[0] = v; r[1] = r[2] = r[3] = 0; }

static inline void u256_add_u64(u256 r, const u256 a, uint64_t v) {
    u128 c = (u128)a[0] + v;
    r[0] = (uint64_t)c; c >>= 64;
    for (int i = 1; i < 4; i++) { c += a[i]; r[i] = (uint64_t)c; c >>= 64; }
}

static inline void u256_sub_u64(u256 r, const u256 a, uint64_t v) {
    u128 br = (u128)v;
    for (int i = 0; i < 4; i++) {
        u128 t = (u128)a[i] - (uint64_t)br - (uint64_t)(br >> 64);
        r[i] = (uint64_t)t;
        br = (t >> 64) & 1;
    }
}

static inline int u256_cmp(const u256 a, const u256 b) {
    for (int i = 3; i >= 0; i--) {
        if (a[i] > b[i]) return 1;
        if (a[i] < b[i]) return -1;
    }
    return 0;
}

// Сложение двух 256-битных. Раньше хватало u256_add_u64: всё, что
// прибавлялось, помещалось в 64 бита. Шаг HEX-маски (16^N) не помещается.
// Перенос за 2^256 отбрасывается -- как и в u256_sub с заёмом.
static inline void u256_add(u256 r, const u256 a, const u256 b) {
    u128 c = 0;
    for (int i = 0; i < 4; i++) {
        c += (u128)a[i] + b[i];
        r[i] = (uint64_t)c;
        c >>= 64;
    }
}

static inline void u256_sub(u256 r, const u256 a, const u256 b) {
    u128 br = 0;
    for (int i = 0; i < 4; i++) {
        u128 t = (u128)a[i] - b[i] - br;
        r[i] = (uint64_t)t;
        br = (t >> 64) & 1;
    }
}

static inline int u256_bit(const u256 a, int i) { return (int)((a[i >> 6] >> (i & 63)) & 1); }

// ---------------- координаты Якоби для быстрого k*G -------------------------
// x = X/Z^2, y = Y/Z^3. Обращений нет вовсе — только одно в самом конце.

struct Jac { fe X, Y, Z; bool inf; Jac() : inf(true) { fe_zero(X); fe_zero(Y); fe_zero(Z); } };

static inline void jac_dbl(Jac &r, const Jac &p) {
    if (p.inf || fe_is_zero(p.Y)) { r.inf = true; return; }
    fe A, B, C, D, E, F, t1, t2;
    fe_sqr(A, p.X);
    fe_sqr(B, p.Y);
    fe_sqr(C, B);
    fe_add(t1, p.X, B);
    fe_sqr(t1, t1);
    fe_sub(t1, t1, A);
    fe_sub(t1, t1, C);
    fe_add(D, t1, t1);                 // D = 2((X+B)^2 - A - C)
    fe_add(E, A, A); fe_add(E, E, A);  // E = 3A
    fe_sqr(F, E);
    fe_add(t1, D, D);
    fe_sub(t2, F, t1);                 // X3 = F - 2D
    fe_sub(t1, D, t2);
    fe_mul(t1, E, t1);
    fe_add(C, C, C); fe_add(C, C, C); fe_add(C, C, C);   // 8C
    fe_sub(r.Y, t1, C);
    fe_set(r.X, t2);
    fe_mul(t1, p.Y, p.Z);
    fe_add(r.Z, t1, t1);
    r.inf = false;
}

// Смешанное сложение: точка Якоби + аффинная точка.
static inline void jac_add_affine(Jac &r, const Jac &p, const Point &q) {
    if (q.inf) { r = p; return; }
    if (p.inf) { fe_set(r.X, q.x); fe_set(r.Y, q.y); fe_one(r.Z); r.inf = false; return; }
    fe Z1Z1, U2, S2, H, R, HH, HHH, t1, t2, V;
    fe_sqr(Z1Z1, p.Z);
    fe_mul(U2, q.x, Z1Z1);
    fe_mul(t1, p.Z, Z1Z1);
    fe_mul(S2, q.y, t1);
    fe_sub(H, U2, p.X);
    fe_sub(R, S2, p.Y);
    if (fe_is_zero(H)) {
        if (fe_is_zero(R)) { Jac pp = p; jac_dbl(r, pp); return; }
        r.inf = true; return;
    }
    fe_sqr(HH, H);
    fe_mul(HHH, H, HH);
    fe_mul(V, p.X, HH);
    fe_sqr(t1, R);
    fe_sub(t1, t1, HHH);
    fe_add(t2, V, V);
    fe_sub(r.X, t1, t2);
    fe_sub(t1, V, r.X);
    fe_mul(t1, R, t1);
    fe_mul(t2, p.Y, HHH);
    fe_sub(r.Y, t1, t2);
    fe_mul(r.Z, p.Z, H);
    r.inf = false;
}

static inline Point jac_to_affine(const Jac &p) {
    Point r;
    if (p.inf || fe_is_zero(p.Z)) return r;
    fe zi, zi2, zi3;
    fe_inv(zi, p.Z);
    fe_sqr(zi2, zi);
    fe_mul(zi3, zi2, zi);
    fe_mul(r.x, p.X, zi2);
    fe_mul(r.y, p.Y, zi3);
    r.inf = false;
    return r;
}

// ------------------------- comb-таблица для G -------------------------------
// COMB[i][v] = v * 16^i * G,  i = 0..63,  v = 1..15.
// Тогда k*G — это сумма 64 слагаемых по шестнадцатеричным разрядам ключа,
// то есть максимум 63 сложения и ни одного удвоения.
//
// ТАБЛИЦА ОДНА НА ПРОГРАММУ, И СТРОИТСЯ ОНА РОВНО ОДИН РАЗ.
//
// Было `static Point COMB[64][16]` в ЗАГОЛОВКЕ -- то есть отдельная копия в
// каждом .cpp, который его включает. Двенадцать копий по 72 КБ (проверено по
// таблице символов собранного бинарника), у каждой свой флаг готовности, и
// каждая строится заново при первом обращении из своего файла.
//
// Память -- полбеды. Беда в том, что инициализация была ленивой и без всякой
// синхронизации, а ec_mul_G вызывается из рабочих потоков. Копия traverse.cpp
// не строится нигде заранее: build_table() строит копию engine.cpp. Поэтому
// при включённом журнале участков (debug=1) и нескольких потоках первые
// вызовы debug_log_chunk из разных потоков строят одну и ту же таблицу
// одновременно -- гонка данных, подтверждённая ThreadSanitizer. Последствие
// не абстрактное: в журнал -- файл, который заводят ровно ради того, чтобы
// сверить ключ и публичный ключ посторонним инструментом, -- мог попасть
// публичный ключ, посчитанный по недостроенной таблице.
//
// Теперь inline-переменная (одна на программу, C++17) и std::call_once.
inline Point COMB[64][16];

inline void ec_build_comb() {
    static std::once_flag once;
    std::call_once(once, [] {
        Point base = ec_G();
        for (int i = 0; i < 64; i++) {
            COMB[i][1] = base;
            for (int v = 2; v < 16; v++) COMB[i][v] = ec_add(COMB[i][v-1], base);
            for (int d = 0; d < 4; d++) base = ec_dbl(base);   // base *= 16
        }
    });
}

static inline Point ec_mul_G(const u256 k) {
    ec_build_comb();
    Jac acc;                       // бесконечность
    for (int i = 0; i < 64; i++) {
        int v = (int)((k[i >> 4] >> ((i & 15) * 4)) & 15);
        if (v) jac_add_affine(acc, acc, COMB[i][v]);
    }
    return jac_to_affine(acc);
}

// ----------------------------- сериализация ---------------------------------

static inline void fe_to_be32(uint8_t out[32], const fe a) {
    for (int i = 0; i < 4; i++) {
        uint64_t v = a[3 - i];
        for (int j = 0; j < 8; j++) out[i * 8 + j] = (uint8_t)(v >> (56 - 8 * j));
    }
}

static inline void u256_to_be32(uint8_t out[32], const u256 a) { fe_to_be32(out, a); }

static inline void fe_from_be32(fe r, const uint8_t in[32]) {
    for (int i = 0; i < 4; i++) {
        uint64_t v = 0;
        for (int j = 0; j < 8; j++) v = (v << 8) | in[(3 - i) * 8 + j];
        r[i] = v;
    }
}

static inline bool hex_to_u256(const char *s, u256 r) {
    u256_zero(r);
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    size_t len = strlen(s);
    if (len == 0 || len > 64) return false;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        int v;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else return false;
        uint64_t carry = (uint64_t)v;
        for (int j = 0; j < 4; j++) {
            uint64_t hi = r[j] >> 60;
            r[j] = (r[j] << 4) | carry;
            carry = hi;
        }
    }
    return true;
}
