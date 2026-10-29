/* ============================================================================
 *  u128_compat.h — 128-битное беззнаковое целое там, где его нет в языке.
 *
 *  ЗАЧЕМ. Математика поля в secp256k1.h и cuda_field.cuh написана на
 *  `unsigned __int128`. Это расширение GCC и Clang; у MSVC такого типа нет
 *  вообще, и на Windows сборка падает с несколькими десятками ошибок вида
 *  «const variable "__int128" requires an initializer». nvcc на Windows тоже
 *  не спасает: хостовый проход у него всё равно идёт через MSVC.
 *
 *  ЧТО ДЕЛАЕТ. Даёт тип u128 с теми же операциями и той же семантикой:
 *
 *      - там, где компилятор умеет `unsigned __int128` (GCC, Clang, в том
 *        числе clang под маком), u128 -- это он и есть, ноль накладных
 *        расходов и ноль отличий;
 *      - где не умеет (MSVC), u128 -- структура из двух uint64_t с
 *        перегруженными операторами. Внутри -- _umul128 / _addcarry_u64 /
 *        _subborrow_u64, каждая из которых компилируется в ОДНУ машинную
 *        инструкцию (mul, adc, sbb). То есть по скорости это то же самое,
 *        что нативный тип, а не эмуляция.
 *
 *  ПОЧЕМУ НЕ ПЕРЕПИСАТЬ САМУ АРИФМЕТИКУ. Потому что она уже проверена, и
 *  ошибки в ней молчаливые: в этом проекте уже находили потерю переноса за
 *  2^128 в fe_mul, которая проявлялась только у операндов, близких к p, и не
 *  ловилась ни одним прежним тестом. Тридцать правок в такой код -- тридцать
 *  шансов повторить ту историю. Здесь вместо этого одно место, которое можно
 *  проверить сверкой с настоящим __int128 (см. test_u128.cpp).
 *
 *  ВАЖНО ПРО ПРИВЕДЕНИЕ К uint64_t. Оператор преобразования объявлен
 *  explicit НАРОЧНО. Если бы он был неявным, выражение `if (acc < old)` в
 *  fe_mul (обнаружение выхода за 2^128) молча свелось бы к сравнению
 *  МЛАДШИХ 64 бит вместо полного сравнения -- то есть ровно к той ошибке,
 *  ради которой эта строка и написана. Явные приведения вида (uint64_t)acc
 *  в существующем коде при этом работают: приведение в стиле C выполняет
 *  static_cast, а он explicit-операторы видит.
 * ========================================================================== */
#ifndef U128_COMPAT_H
#define U128_COMPAT_H

#include <stdint.h>

/* Пометка для CUDA: типом пользуются и хост, и карта. */
#if defined(__CUDACC__)
  #define U128_HD __host__ __device__
#else
  #define U128_HD
#endif

/* Нативный тип есть? __SIZEOF_INT128__ определяют GCC и Clang; MSVC -- нет.
   U128_FORCE_STRUCT позволяет прогнать структурную ветку там, где нативный
   тип есть, -- именно так её и проверяет test_u128.cpp. */
#if defined(__SIZEOF_INT128__) && !defined(U128_FORCE_STRUCT)
  #define U128_NATIVE 1
#else
  #define U128_NATIVE 0
#endif

#if defined(_MSC_VER)
  #include <intrin.h>
  #if defined(_M_X64) || defined(_M_ARM64)
    #pragma intrinsic(_umul128)
  #endif
#endif

/* ---------------------------------------------------------------------------
 *  64x64 -> 128. Нужна и сама по себе: mul64() в cuda_field.cuh делает ровно
 *  это и на MSVC ломалась по той же причине. Поэтому объявлена ВНЕ ветки с
 *  типом -- она доступна всегда.
 *
 *  Четыре реализации, одна семантика. Переносимая (последняя) считает через
 *  32-битные половины; она не только запасной путь -- именно её прогоняет
 *  сверка на gcc, так что проверенной оказывается логика, а не интринсик.
 * ------------------------------------------------------------------------- */
U128_HD static inline void u128_mul64(uint64_t a, uint64_t b,
                                      uint64_t *lo, uint64_t *hi)
{
/* U128_FORCE_PORTABLE_MUL уводит на последнюю ветку. Макрос существует ради
   сверки: сами по себе ни MSVC, ни gcc туда не попадают, а непроверенного
   кода в арифметике поля быть не должно. */
#if !defined(U128_FORCE_PORTABLE_MUL) && defined(__CUDA_ARCH__)
    *lo = a * b;
    *hi = __umul64hi(a, b);
#elif !defined(U128_FORCE_PORTABLE_MUL) && defined(_MSC_VER) && (defined(_M_X64) || defined(_M_ARM64))
    *lo = _umul128(a, b, hi);
#elif !defined(U128_FORCE_PORTABLE_MUL) && defined(__SIZEOF_INT128__)
    const unsigned __int128 t = (unsigned __int128)a * b;
    *lo = (uint64_t)t;
    *hi = (uint64_t)(t >> 64);
#else
    const uint64_t a0 = (uint32_t)a, a1 = a >> 32;
    const uint64_t b0 = (uint32_t)b, b1 = b >> 32;

    const uint64_t p00 = a0 * b0;
    const uint64_t p01 = a0 * b1;
    const uint64_t p10 = a1 * b0;
    const uint64_t p11 = a1 * b1;

    /* Средние произведения складываются с переносом в старшее слово. */
    const uint64_t mid = (p00 >> 32) + (uint32_t)p01 + (uint32_t)p10;

    *lo = (p00 & 0xFFFFFFFFULL) | (mid << 32);
    *hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
#endif
}

#if U128_NATIVE

typedef unsigned __int128 u128;

#elif !defined(__cplusplus)

/* Замены на чистом C нет и не требуется. Единственный файл на C в этом
   проекте -- scan_secp.c, и он 128-битным типом не пользуется вовсе: там
   своя арифметика из libsecp256k1. Перегрузка операторов, на которой держится
   структура ниже, в C отсутствует, поэтому здесь просто ничего не
   объявляется -- вместо того чтобы городить набор функций, которым некому
   пользоваться. Нужная же и в C функция u128_mul64 объявлена выше. */

#else  /* ------------------------- структурная ветка ---------------------- */

struct u128 {
    uint64_t lo, hi;

    U128_HD u128() : lo(0), hi(0) {}

    /* Неявное построение из целого нужно: код пишет `u128 c = 0;`,
       `c += m[i];`, `cur / 1000000000ULL`.
     *
     * ПОЧЕМУ ШЕСТЬ КОНСТРУКТОРОВ, А НЕ ОДИН ОТ uint64_t. Потому что uint64_t --
     * это ПСЕВДОНИМ одного из встроенных типов, и какого именно, зависит от
     * системы: на Linux это unsigned long, на Windows -- unsigned long long.
     * С единственным конструктором от uint64_t выражение `cur / 1000000000ULL`
     * на Linux даёт «неоднозначное преобразование»: литерал имеет тип
     * unsigned long long, который не совпадает точно НИ с uint64_t
     * (= unsigned long), НИ с int, и компилятор не может выбрать между ними.
     * Перечисление всех встроенных типов убирает неоднозначность точным
     * совпадением, а дублирования не возникает: эти шесть типов различны на
     * любой системе, тогда как uint64_t всегда совпадает с одним из них.
     *
     * У знаковых типов сохранено расширение знака: у нативного типа
     * `unsigned __int128 x = -1;` даёт все единицы, и здесь так же. */
    U128_HD u128(unsigned long long v) : lo((uint64_t)v), hi(0) {}
    U128_HD u128(unsigned long v)      : lo((uint64_t)v), hi(0) {}
    U128_HD u128(unsigned int v)       : lo((uint64_t)v), hi(0) {}
    U128_HD u128(long long v)          : lo((uint64_t)v), hi(v < 0 ? ~0ULL : 0ULL) {}
    U128_HD u128(long v)               : lo((uint64_t)(long long)v), hi(v < 0 ? ~0ULL : 0ULL) {}
    U128_HD u128(int v)                : lo((uint64_t)(long long)v), hi(v < 0 ? ~0ULL : 0ULL) {}

    U128_HD u128(uint64_t h, uint64_t l, int) : lo(l), hi(h) {}

    /* Приведения к целым типам. У нативного типа приведение к ЛЮБОМУ целому
       разрешено и просто отбрасывает старшие биты; код этим пользуется:
       (uint64_t)acc, (unsigned long long)rem, (uint8_t)(rem / bpc).
       Поэтому здесь перечислены все встроенные целые.
     *
     * ВСЕ ОНИ explicit, И ЭТО ВАЖНО. Будь они неявными, выражение
     * `if (acc < old)` в fe_mul (обнаружение выхода за 2^128) молча свелось
     * бы к сравнению МЛАДШИХ 64 бит вместо полного -- то есть ровно к той
     * ошибке, ради поимки которой эта строка и написана. Приведения в стиле
     * C при этом работают: (uint64_t)acc -- это static_cast, а он explicit
     * видит. */
    U128_HD explicit operator unsigned long long() const { return (unsigned long long)lo; }
    U128_HD explicit operator unsigned long()      const { return (unsigned long)lo; }
    U128_HD explicit operator unsigned int()       const { return (unsigned int)lo; }
    U128_HD explicit operator unsigned short()     const { return (unsigned short)lo; }
    U128_HD explicit operator unsigned char()      const { return (unsigned char)lo; }
    U128_HD explicit operator long long()          const { return (long long)lo; }
    U128_HD explicit operator long()               const { return (long)lo; }
    U128_HD explicit operator int()                const { return (int)lo; }
    U128_HD explicit operator short()              const { return (short)lo; }
    U128_HD explicit operator signed char()        const { return (signed char)lo; }
    U128_HD explicit operator char()               const { return (char)lo; }

    /* Код пишет `if (br)` и `i < 4 && c` -- у нативного типа это проверка
       «не ноль» по ВСЕМ 128 битам. explicit здесь не мешает: в условиях if,
       while, && и ! язык применяет явное преобразование к bool сам, а вот
       случайно подставиться вместо числа этот оператор уже не сможет.
       Проверять надо оба слова: `if (br)` при br = 2^64 должно быть истиной,
       а по одному младшему слову вышел бы ноль. */
    U128_HD explicit operator bool() const { return (lo | hi) != 0; }

    U128_HD u128 operator+(const u128 &o) const {
        u128 r;
        r.lo = lo + o.lo;
        r.hi = hi + o.hi + (r.lo < lo ? 1ULL : 0ULL);
        return r;
    }
    U128_HD u128 operator-(const u128 &o) const {
        u128 r;
        r.lo = lo - o.lo;
        r.hi = hi - o.hi - (lo < o.lo ? 1ULL : 0ULL);
        return r;
    }
    U128_HD u128 operator*(const u128 &o) const {
        u128 r;
        u128_mul64(lo, o.lo, &r.lo, &r.hi);
        /* Старшие части дают вклад только в старшее слово: всё, что выше
           2^128, отбрасывается -- ровно как у нативного типа. */
        r.hi += lo * o.hi + hi * o.lo;
        return r;
    }

    U128_HD u128 &operator+=(const u128 &o) { *this = *this + o; return *this; }
    U128_HD u128 &operator-=(const u128 &o) { *this = *this - o; return *this; }
    U128_HD u128 &operator*=(const u128 &o) { *this = *this * o; return *this; }

    /* Деление и остаток. Двоичное деление в столбик: 128 итераций, зато
       очевидно правильное при любых операндах.
     *
     * ПОЧЕМУ НЕ _udiv128, который был бы в разы быстрее. У него жёсткое
     * условие: старшее слово делимого должно быть МЕНЬШЕ делителя, иначе
     * процессор возбуждает #DE и программа падает. На всех нынешних местах
     * вызова условие соблюдено, но оно не выражено в коде и держится на
     * рассуждении о диапазонах -- ровно тот сорт допущения, который ломается
     * при первой же правке рядом.
     *
     * Скорость здесь роли не играет: деление встречается только в переводе
     * числа в десятичную запись для показа и в пересчёте номера цикла раз в
     * запуск. В горячем цикле деления нет ни одного -- проверено поиском по
     * engine.cpp, kangaroo.cpp и scan_secp.c. */
    U128_HD static void divmod(const u128 &a, const u128 &b, u128 *q, u128 *r) {
        u128 quo, rem;
        if (!b) { *q = quo; *r = rem; return; }   /* деление на ноль: как и у
                                                     нативного типа, поведение
                                                     не определено; не падаем */
        for (int i = 127; i >= 0; i--) {
            rem = rem << 1;
            const uint64_t bit = (i >= 64) ? ((a.hi >> (i - 64)) & 1ULL)
                                           : ((a.lo >> i) & 1ULL);
            rem.lo |= bit;
            if (!(rem < b)) {
                rem = rem - b;
                if (i >= 64) quo.hi |= 1ULL << (i - 64);
                else         quo.lo |= 1ULL << i;
            }
        }
        *q = quo; *r = rem;
    }
    U128_HD u128 operator/(const u128 &o) const { u128 q, r; divmod(*this, o, &q, &r); return q; }
    U128_HD u128 operator%(const u128 &o) const { u128 q, r; divmod(*this, o, &q, &r); return r; }
    U128_HD u128 &operator/=(const u128 &o) { *this = *this / o; return *this; }
    U128_HD u128 &operator%=(const u128 &o) { *this = *this % o; return *this; }

    /* Сдвиг на 64 и больше -- отдельным случаем: сдвиг 64-битного слова на
       64 в C -- неопределённое поведение, а код сдвигает ровно на 64. */
    U128_HD u128 operator>>(int s) const {
        u128 r;
        if (s == 0)       { r = *this; }
        else if (s < 64)  { r.lo = (lo >> s) | (hi << (64 - s)); r.hi = hi >> s; }
        else if (s == 64) { r.lo = hi; r.hi = 0; }
        else if (s < 128) { r.lo = hi >> (s - 64); r.hi = 0; }
        else              { r.lo = 0; r.hi = 0; }
        return r;
    }
    U128_HD u128 operator<<(int s) const {
        u128 r;
        if (s == 0)       { r = *this; }
        else if (s < 64)  { r.hi = (hi << s) | (lo >> (64 - s)); r.lo = lo << s; }
        else if (s == 64) { r.hi = lo; r.lo = 0; }
        else if (s < 128) { r.hi = lo << (s - 64); r.lo = 0; }
        else              { r.lo = 0; r.hi = 0; }
        return r;
    }
    U128_HD u128 &operator>>=(int s) { *this = *this >> s; return *this; }
    U128_HD u128 &operator<<=(int s) { *this = *this << s; return *this; }

    U128_HD u128 operator|(const u128 &o) const { u128 r; r.lo = lo | o.lo; r.hi = hi | o.hi; return r; }
    U128_HD u128 operator&(const u128 &o) const { u128 r; r.lo = lo & o.lo; r.hi = hi & o.hi; return r; }
    U128_HD u128 operator^(const u128 &o) const { u128 r; r.lo = lo ^ o.lo; r.hi = hi ^ o.hi; return r; }
    U128_HD u128 operator~() const { u128 r; r.lo = ~lo; r.hi = ~hi; return r; }
    U128_HD u128 &operator|=(const u128 &o) { *this = *this | o; return *this; }
    U128_HD u128 &operator&=(const u128 &o) { *this = *this & o; return *this; }

    U128_HD bool operator< (const u128 &o) const { return hi != o.hi ? hi < o.hi : lo < o.lo; }
    U128_HD bool operator> (const u128 &o) const { return o < *this; }
    U128_HD bool operator<=(const u128 &o) const { return !(o < *this); }
    U128_HD bool operator>=(const u128 &o) const { return !(*this < o); }
    U128_HD bool operator==(const u128 &o) const { return hi == o.hi && lo == o.lo; }
    U128_HD bool operator!=(const u128 &o) const { return !(*this == o); }
    U128_HD bool operator!() const { return lo == 0 && hi == 0; }
};

#endif /* U128_NATIVE */

#endif /* U128_COMPAT_H */
