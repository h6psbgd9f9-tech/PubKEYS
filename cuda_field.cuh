// ============================================================================
//  cuda_field.cuh — арифметика поля secp256k1 ОДНИМ ИСХОДНИКОМ для процессора
//                   и для видеокарты.
//
//  ГЛАВНОЕ РЕШЕНИЕ ЭТОГО ФАЙЛА — ЧТО ОН СОБИРАЕТСЯ И БЕЗ CUDA.
//
//  Ошибка в модульной арифметике не падает. Переполнение в переносе даёт
//  неверные абсциссы, поиск ничего не находит — и всё выглядит исправным:
//  миллиарды ключей в секунду, ноль совпадений. Это худший род ошибки из
//  возможных, и на видеокарте её нечем поймать: отладчика под рукой нет,
//  а «не находит» — обычное состояние программы, которая ищет иголку.
//
//  Поэтому каждая функция помечена __host__ __device__ и собирается ДВАЖДЫ:
//  обычным компилятором для процессора и nvcc для карты. Из одного и того же
//  текста. Значит проверить арифметику можно там, где есть чем: на хосте,
//  сверив со сторонней реализацией на случайных значениях.
//
//  Непроверенной остаётся обвязка ядра — расстановка нитей, обращение к
//  памяти, — но не сама математика. Это переносит риск с того, что нельзя
//  проверить, на то, что видно глазами.
//
//  ОДНО ИСКЛЮЧЕНИЕ, ПОЯВИВШЕЕСЯ ПОЗЖЕ: умножение (cfe_mul).
//
//  Ниже в этом файле стояло, что ассемблерных вставок здесь нарочно нет --
//  потому что их нельзя собрать для процессора, а значит нельзя и сверить.
//  Довод верный, и он снят не отменой, а обходом: ассемблерная версия стоит
//  ПОД __CUDA_ARCH__ и живёт РЯДОМ с прежней C-версией, которая никуда не
//  делась. C-версия по-прежнему собирается для процессора и по-прежнему
//  сверяется со сторонней реализацией -- она осталась эталоном.
//
//  А сверяет их между собой то, чего раньше не было под рукой: pkcudatest.exe
//  (`nmake /f Makefile.win cuda-test`) считает одни и те же ключи на карте и
//  на процессоре и сравнивает значение в значение. Разойдись ассемблер с C
//  хоть на бит -- это видно сразу. Без такой проверки вставок здесь быть не
//  должно, и это не фигура речи: ошибка в переносе даёт не падение, а полную
//  скорость при нуле совпадений.
//
//  Когда файл включается без nvcc, __host__ __device__ раскрываются в пустоту,
//  и получается обычный C++.
// ============================================================================
#pragma once
#include <stdint.h>
// 64x64 -> 128 без __int128: он есть у GCC/Clang и отсутствует у MSVC,
// а хостовый проход nvcc под Windows идёт именно через MSVC.
#include "u128_compat.h"

#ifdef __CUDACC__
  #define FD_HD __host__ __device__
  #define FD_IN __forceinline__
#else
  #define FD_HD
  #define FD_IN inline
#endif

// Представление то же, что у процессорной версии: четыре 64-битных лимба,
// младший первый. Совпадение НЕ случайно -- иначе сверять результаты
// пришлось бы через перевод, и ошибка в переводе выдавала бы себя за ошибку
// в арифметике.
typedef uint64_t cfe[4];

// CUDA arithmetic adapted from the verified BitPazl implementation.
// The host reference remains independent; switches support comparison builds.
#ifndef PK_GPU_MATH
#define PK_GPU_MATH 1
#endif
#ifndef PK_GCD_INV
#define PK_GCD_INV 1
#endif
#if defined(__CUDACC__) && (PK_GPU_MATH || PK_GCD_INV)
namespace pkh_fast_field {
// Internal linkage: this header is included by multiple CUDA translation units.
namespace {
#include "vanity_vendor/GPUMath_corrected.h"
}
}
#include "vanity_vendor/undef_macros.h"
#endif


// p = 2^256 - 2^32 - 977
//
// На карте константа кладётся в постоянную память -- она кешируется отдельно
// и раздаётся всем нитям пачки за одно обращение. Без CUDA такого понятия
// нет, поэтому объявление тоже под условием: __constant__ обычный компилятор
// не знает.
#ifdef __CUDACC__
__device__ __constant__ static const uint64_t CFE_P_DEV[4] = {
    0xFFFFFFFEFFFFFC2FULL, 0xFFFFFFFFFFFFFFFFULL,
    0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL
};
#endif
static const uint64_t CFE_P_HOST[4] = {
    0xFFFFFFFEFFFFFC2FULL, 0xFFFFFFFFFFFFFFFFULL,
    0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL
};

FD_HD FD_IN const uint64_t *cfe_p() {
#ifdef __CUDA_ARCH__
    return CFE_P_DEV;      // на карте -- из постоянной памяти
#else
    return CFE_P_HOST;
#endif
}

// --------------------------------------------------------------------------
//  Сложение и вычитание 64-битных с переносом.
//
//  На карте есть команды addc/subc, и nvcc умеет их поставить сам, если
//  написать обычным кодом с проверкой переполнения. Здесь ассемблера нет и он
//  не нужен: эти две функции вызываются по нескольку раз на ключ, тогда как
//  умножение -- шестнадцать раз внутри себя и по нескольку раз на ключ сверх
//  того. Вставки поставлены только там, где они окупаются, и только вместе с
//  проверкой на карте (см. шапку файла).
// --------------------------------------------------------------------------
FD_HD FD_IN uint64_t addc64(uint64_t a, uint64_t b, uint64_t *carry) {
    const uint64_t s = a + b;
    const uint64_t c1 = (s < a) ? 1u : 0u;
    const uint64_t t = s + *carry;
    const uint64_t c2 = (t < s) ? 1u : 0u;
    *carry = c1 + c2;
    return t;
}

FD_HD FD_IN uint64_t subb64(uint64_t a, uint64_t b, uint64_t *borrow) {
    const uint64_t d = a - b;
    const uint64_t b1 = (a < b) ? 1u : 0u;
    const uint64_t e = d - *borrow;
    const uint64_t b2 = (d < *borrow) ? 1u : 0u;
    *borrow = b1 + b2;
    return e;
}

// 64x64 -> 128. На карте это __umul64hi; на процессоре -- __int128 там, где
// он есть, и _umul128 у MSVC, где его нет. Выбор спрятан в u128_mul64:
// раньше здесь стоял голый __int128, и под Windows файл не собирался вовсе.
FD_HD FD_IN void mul64(uint64_t a, uint64_t b, uint64_t *lo, uint64_t *hi) {
    u128_mul64(a, b, lo, hi);
}

// ------------------------------- сравнение ---------------------------------
FD_HD FD_IN int cfe_cmp(const cfe a, const cfe b) {
    for (int i = 3; i >= 0; i--) {
        if (a[i] > b[i]) return 1;
        if (a[i] < b[i]) return -1;
    }
    return 0;
}
FD_HD FD_IN int cfe_is_zero(const cfe a) {
    return (a[0] | a[1] | a[2] | a[3]) == 0;
}
FD_HD FD_IN void cfe_set(cfe r, const cfe a) {
    r[0]=a[0]; r[1]=a[1]; r[2]=a[2]; r[3]=a[3];
}
FD_HD FD_IN void cfe_zero(cfe r) { r[0]=r[1]=r[2]=r[3]=0; }
FD_HD FD_IN void cfe_one(cfe r)  { r[0]=1; r[1]=r[2]=r[3]=0; }

// --------------------------------- сложение --------------------------------
FD_HD FD_IN void cfe_add(cfe r, const cfe a, const cfe b) {
    const uint64_t *P = cfe_p();
    uint64_t c = 0;
    uint64_t t[4];
    for (int i = 0; i < 4; i++) t[i] = addc64(a[i], b[i], &c);

    // Приводим: если вышли за p (или был перенос за 2^256) -- вычитаем p.
    // Сравнение делается ВСЕГДА, без ветвления по значению: на карте нити
    // идут пачками, и расхождение путей стоит дороже лишнего вычитания.
    uint64_t bw = 0, u[4];
    for (int i = 0; i < 4; i++) u[i] = subb64(t[i], P[i], &bw);
    const uint64_t take = (c != 0 || bw == 0) ? 1u : 0u;   // u = t - p подошло
    for (int i = 0; i < 4; i++) r[i] = take ? u[i] : t[i];
}

// -------------------------------- вычитание --------------------------------
FD_HD FD_IN void cfe_sub(cfe r, const cfe a, const cfe b) {
    const uint64_t *P = cfe_p();
    uint64_t bw = 0, t[4];
    for (int i = 0; i < 4; i++) t[i] = subb64(a[i], b[i], &bw);
    // Ушли ниже нуля -- прибавляем p обратно.
    uint64_t c = 0, u[4];
    for (int i = 0; i < 4; i++) u[i] = addc64(t[i], P[i], &c);
    for (int i = 0; i < 4; i++) r[i] = bw ? u[i] : t[i];
}

// --------------------------------------------------------------------------
//  ПРИВЕДЕНИЕ 512 -> 256 ПО МОДУЛЮ p.
//
//  p = 2^256 - c, где c = 2^32 + 977. Значит 2^256 == c (mod p), и старшую
//  половину произведения достаточно умножить на c и сложить с младшей. Это
//  дешевле общего деления в разы и есть главная причина, по которой у
//  secp256k1 такое p.
//
//  Умножать приходится дважды: первое сложение снова может дать перенос за
//  2^256, и его тоже надо свернуть. Второй раз перенос уже мал -- не больше
//  одного слова.
// --------------------------------------------------------------------------
#if defined(__CUDA_ARCH__)

#define PK_UMULLO(lo,a,b) asm volatile("mul.lo.u64 %0, %1, %2;"  : "=l"(lo) : "l"(a), "l"(b))
#define PK_UMULHI(hi,a,b) asm volatile("mul.hi.u64 %0, %1, %2;"  : "=l"(hi) : "l"(a), "l"(b))
#define PK_MADDO(r,a,b,c) asm volatile("mad.hi.cc.u64 %0, %1, %2, %3;"  : "=l"(r) : "l"(a), "l"(b), "l"(c) : "memory")
#define PK_MADDC(r,a,b,c) asm volatile("madc.hi.cc.u64 %0, %1, %2, %3;" : "=l"(r) : "l"(a), "l"(b), "l"(c) : "memory")
#define PK_MADD(r,a,b,c)  asm volatile("madc.hi.u64 %0, %1, %2, %3;"    : "=l"(r) : "l"(a), "l"(b), "l"(c))
#define PK_UADDO1(c,a)    asm volatile("add.cc.u64 %0, %0, %1;"  : "+l"(c) : "l"(a) : "memory")
#define PK_UADDC1(c,a)    asm volatile("addc.cc.u64 %0, %0, %1;" : "+l"(c) : "l"(a) : "memory")
#define PK_UADD1(c,a)     asm volatile("addc.u64 %0, %0, %1;"    : "+l"(c) : "l"(a))

// r[0..4] = a[0..3] * b, где b -- один лимб. Пять лимбов, потому что
// произведение 256 на 64 бита занимает 320.
#define PK_MUL_ROW(r, a, b) { \
    PK_UMULLO((r)[0], (a)[0], (b));           \
    PK_UMULLO((r)[1], (a)[1], (b));           \
    PK_MADDO ((r)[1], (a)[0], (b), (r)[1]);   \
    PK_UMULLO((r)[2], (a)[2], (b));           \
    PK_MADDC ((r)[2], (a)[1], (b), (r)[2]);   \
    PK_UMULLO((r)[3], (a)[3], (b));           \
    PK_MADDC ((r)[3], (a)[2], (b), (r)[3]);   \
    PK_MADD  ((r)[4], (a)[3], (b), 0ULL);     }

#endif  // __CUDA_ARCH__

#if defined(__CUDA_ARCH__)
// ПРИВЕДЕНИЕ ТОЖЕ НА АССЕМБЛЕРЕ.
//
// Умножение перевели раньше, а приведение осталось на C -- и зря: оно идёт
// после КАЖДОГО умножения и каждого квадрата, то есть столько же раз. Внутри
// него пять умножений и полтора десятка сложений с переносом, и переносы там
// те же самые: написанное на C "s = a + b; carry = (s < b)" компилятор
// восстанавливает в add.cc/addc.cc не всегда.
//
// Схема ровно та же, что в хостовой ветке, шаг в шаг: свернуть старшую
// половину через C = 2^32 + 977, свернуть остаток, при необходимости вычесть
// p. Отличаются только примитивы. Совпадение проверено на трёх миллионах
// случайных наборов и на крайних (все единицы, только старшая половина,
// только младшая).
FD_HD FD_IN void cfe_reduce512(cfe r, const uint64_t t[8]) {
    const uint64_t C = 0x1000003D1ULL;      // 2^32 + 977

    // row = t[4..7] * C -- та же строка, что и в умножении.
    uint64_t row[5];
    PK_MUL_ROW(row, (t + 4), C);

    // m[0..4] = t[0..3] + row
    uint64_t m0 = t[0], m1 = t[1], m2 = t[2], m3 = t[3], m4 = 0;
    PK_UADDO1(m0, row[0]);
    PK_UADDC1(m1, row[1]);
    PK_UADDC1(m2, row[2]);
    PK_UADDC1(m3, row[3]);
    PK_UADD1 (m4, row[4]);

    // Свернуть остаток: m4 стоит при 2^256, его произведение на C занимает
    // два слова.
    uint64_t lo, hi;
    PK_UMULLO(lo, m4, C);
    PK_UMULHI(hi, m4, C);
    uint64_t v0 = m0, v1 = m1, v2 = m2, v3 = m3, c4 = 0;
    PK_UADDO1(v0, lo);
    PK_UADDC1(v1, hi);
    PK_UADDC1(v2, 0ULL);
    PK_UADDC1(v3, 0ULL);
    PK_UADD1 (c4, 0ULL);                    // остался перенос -- не больше единицы

    if (c4) {
        uint64_t z = 0;
        PK_UADDO1(v0, C);
        PK_UADDC1(v1, 0ULL);
        PK_UADDC1(v2, 0ULL);
        PK_UADDC1(v3, 0ULL);
        PK_UADD1 (z,  0ULL);
        (void)z;
    }
    r[0] = v0; r[1] = v1; r[2] = v2; r[3] = v3;

    // Последнее приведение: результат мог остаться в [p, 2^256).
    {
        const uint64_t *PP = cfe_p();
        uint64_t bw = 0, u[4];
        for (int i = 0; i < 4; i++) u[i] = subb64(r[i], PP[i], &bw);
        if (!bw) cfe_set(r, u);
    }
}
#else
FD_HD FD_IN void cfe_reduce512(cfe r, const uint64_t t[8]) {
    const uint64_t C = 0x1000003D1ULL;      // 2^32 + 977
    uint64_t m[5] = {0,0,0,0,0};

    // m = t[0..3] + t[4..7] * C
    {
        uint64_t carry = 0;
        for (int i = 0; i < 4; i++) {
            uint64_t lo, hi;
            mul64(t[4 + i], C, &lo, &hi);
            uint64_t c1 = 0;
            const uint64_t s = addc64(t[i], lo, &c1);
            uint64_t c2 = 0;
            m[i] = addc64(s, carry, &c2);
            carry = hi + c1 + c2;
        }
        m[4] = carry;
    }

    // Свернуть остаток: m[4] тоже стоит при 2^256.
    {
        uint64_t lo, hi;
        mul64(m[4], C, &lo, &hi);
        uint64_t c = 0;
        uint64_t v[4];
        v[0] = addc64(m[0], lo, &c);
        v[1] = addc64(m[1], hi, &c);
        v[2] = addc64(m[2], 0,  &c);
        v[3] = addc64(m[3], 0,  &c);

        // Остался перенос -- ещё раз, теперь он не больше единицы.
        if (c) {
            uint64_t c2 = 0;
            v[0] = addc64(v[0], C * c, &c2);
            v[1] = addc64(v[1], 0, &c2);
            v[2] = addc64(v[2], 0, &c2);
            v[3] = addc64(v[3], 0, &c2);
        }
        cfe_set(r, v);
    }

    // Последнее приведение: результат мог остаться в [p, 2^256).
    {
        const uint64_t *P = cfe_p();
        uint64_t bw = 0, u[4];
        for (int i = 0; i < 4; i++) u[i] = subb64(r[i], P[i], &bw);
        if (!bw) cfe_set(r, u);
    }
}
#endif  // __CUDA_ARCH__

// -------------------------------- умножение --------------------------------
//
//  ЗДЕСЬ ЛЕЖИТ ОСНОВНАЯ РАБОТА ПРОГРАММЫ. На каждый ключ приходится около
//  четырёх с половиной умножений поля, а внутри каждого -- шестнадцать
//  умножений 64x64 со сложением переносов. Всё остальное на их фоне мелочь.
//
//  У карты есть флаг переноса и команды, которые его читают и пишут:
//  add.cc/addc.cc для сложения цепочкой, mad.hi.cc/madc.hi для умножения со
//  сложением. Написанное на C "s = a + b; carry = (s < b)" компилятор
//  восстанавливает в них не всегда: между сложениями стоят сравнения, которые
//  он обязан сохранить, потому что по правилам языка их результат наблюдаем.
//
//  Обе ветки считают одно и то же одним и тем же школьным способом по
//  строкам; отличаются только примитивы. Хостовая -- эталон, и она же
//  собирается всюду, где нет nvcc.
#if defined(__CUDA_ARCH__)
FD_HD FD_IN void cfe_mul(cfe r, const cfe a, const cfe b) {
#if PK_GPU_MATH
    pkh_fast_field::_ModMult(r, const_cast<uint64_t*>(a), const_cast<uint64_t*>(b));
#else
    uint64_t t[8];
    #pragma unroll
    for (int i = 0; i < 8; i++) t[i] = 0;

    // Перенос за t[i+4] не уходит: в позиции i+4 до этой строки ноль, старший
    // лимб строки не больше 2^64-2, и сумма с одним переносом остаётся в
    // пределах слова.
    #pragma unroll
    for (int i = 0; i < 4; i++) {
        uint64_t row[5];
        PK_MUL_ROW(row, b, a[i]);
        PK_UADDO1(t[i+0], row[0]);
        PK_UADDC1(t[i+1], row[1]);
        PK_UADDC1(t[i+2], row[2]);
        PK_UADDC1(t[i+3], row[3]);
        PK_UADD1 (t[i+4], row[4]);
    }
    cfe_reduce512(r, t);

#endif
}
#else
FD_HD FD_IN void cfe_mul(cfe r, const cfe a, const cfe b) {
    uint64_t t[8] = {0,0,0,0,0,0,0,0};
    for (int i = 0; i < 4; i++) {
        uint64_t carry = 0;
        for (int j = 0; j < 4; j++) {
            uint64_t lo, hi;
            mul64(a[i], b[j], &lo, &hi);
            uint64_t c1 = 0;
            const uint64_t s = addc64(t[i + j], lo, &c1);
            uint64_t c2 = 0;
            t[i + j] = addc64(s, carry, &c2);
            carry = hi + c1 + c2;
        }
        t[i + 4] += carry;
    }
    cfe_reduce512(r, t);
}
#endif

// --------------------------------------------------------------------------
//  ВОЗВЕДЕНИЕ В КВАДРАТ.
//
//  У a*a половина произведений повторяется: a_i*a_j встречается дважды при
//  i != j. Значит вместо шестнадцати умножений хватает десяти -- четыре
//  квадрата лимбов и шесть перекрёстных произведений, каждое взятое дважды.
//
//  ЦЕНА, КОТОРУЮ ЗА ЭТО ПЛАТЯТ, И ПОЧЕМУ ЗДЕСЬ ЭТО НЕ ОЧЕВИДНАЯ ПОБЕДА.
//
//  Школьное умножение выше идёт строками и пользуется mad.hi.cc -- одной
//  командой карты, которая умножает, складывает и переносит. Квадрат так не
//  ложится: удвоенные перекрёстные произведения приходится складывать в
//  трёхсловный накопитель отдельными сложениями. Умножений меньше, сложений
//  больше, и что перевесит -- вопрос замера, а не подсчёта.
//
//  Поэтому реализация переключаемая: -DPK_FAST_SQR=0 возвращает прежнее
//  поведение (квадрат как умножение на себя), и оба варианта можно сравнить
//  на одной и той же карте. Считают они одно и то же -- совпадение с a*a
//  проверено на трёх миллионах случайных значений и на крайних.
//
//  ЗАМЕР ОТВЕТИЛ, И ОТВЕТ ОКАЗАЛСЯ ОБРАТНЫМ ОЖИДАЕМОМУ:
//      квадрат как a*a      5 762 414 455 ключей/с
//      отдельный квадрат    6 175 417 653            +7.2 %
//
//  Рассуждение выше было неверным в одном месте: 64-битное умножение считалось
//  за одну операцию. У карты нет 64-битного умножителя -- каждое такое
//  умножение собирается из нескольких 32-битных и стоит в разы дороже
//  сложения. При таком соотношении шестнадцать умножений против десяти решают,
//  а лишние сложения почти ничего не стоят. Переключатель оставлен: на другой
//  карте соотношение может быть иным.
//
//  Расписание -- стандартное для четырёх лимбов: накопитель (c0,c1,c2),
//  на каждом шаге в него добавляются произведения, дающие вклад в этот разряд,
//  затем младшее слово снимается в результат, а накопитель сдвигается.
// --------------------------------------------------------------------------
#ifndef PK_FAST_SQR
#define PK_FAST_SQR 1
#endif

#if PK_FAST_SQR
#if defined(__CUDA_ARCH__)

// c += a*b, трёхсловный накопитель, перенос командами карты.
#define PK_SQ_MAD(a,b) do {                          \
    uint64_t _lo, _hi;                               \
    PK_UMULLO(_lo, (a), (b));                        \
    PK_UMULHI(_hi, (a), (b));                        \
    PK_UADDO1(c0, _lo);                              \
    PK_UADDC1(c1, _hi);                              \
    PK_UADD1 (c2, 0ULL);                             \
} while (0)

// Перекрёстное произведение входит дважды. Складываем его два раза, а не
// удваиваем: удвоенное произведение занимает 129 бит и потребовало бы своей
// возни с переносом.
#define PK_SQ_MAD2(a,b) do { PK_SQ_MAD((a),(b)); PK_SQ_MAD((a),(b)); } while (0)

#define PK_SQ_EXT(n) do { t[(n)] = c0; c0 = c1; c1 = c2; c2 = 0; } while (0)

FD_HD FD_IN void cfe_sqr(cfe r, const cfe a) {
#if PK_GPU_MATH
    pkh_fast_field::_ModSqr(r, a);
#else
    uint64_t t[8];
    uint64_t c0 = 0, c1 = 0, c2 = 0;

    PK_SQ_MAD (a[0], a[0]);                          PK_SQ_EXT(0);
    PK_SQ_MAD2(a[0], a[1]);                          PK_SQ_EXT(1);
    PK_SQ_MAD2(a[0], a[2]); PK_SQ_MAD (a[1], a[1]);  PK_SQ_EXT(2);
    PK_SQ_MAD2(a[0], a[3]); PK_SQ_MAD2(a[1], a[2]);  PK_SQ_EXT(3);
    PK_SQ_MAD2(a[1], a[3]); PK_SQ_MAD (a[2], a[2]);  PK_SQ_EXT(4);
    PK_SQ_MAD2(a[2], a[3]);                          PK_SQ_EXT(5);
    PK_SQ_MAD (a[3], a[3]);                          PK_SQ_EXT(6);
    t[7] = c0;                                       // c1 здесь уже ноль

    cfe_reduce512(r, t);

#endif
}
#else
// Хостовая ветка -- то же расписание обычным кодом. Она и есть эталон: с ней
// сверяется всё остальное, и она собирается там, где нет nvcc.
FD_HD FD_IN void cfe_sqr(cfe r, const cfe a) {
    uint64_t t[8];
    uint64_t c0 = 0, c1 = 0, c2 = 0;

    #define HS_MAD(x,y) do {                                        \
        uint64_t _lo, _hi; mul64((x), (y), &_lo, &_hi);             \
        uint64_t _k = 0;                                            \
        c0 = addc64(c0, _lo, &_k);                                  \
        uint64_t _k2 = 0;                                           \
        c1 = addc64(c1, _hi, &_k2);                                 \
        uint64_t _k3 = 0;                                           \
        c1 = addc64(c1, _k,  &_k3);                                 \
        c2 += _k2 + _k3;                                            \
    } while (0)
    #define HS_MAD2(x,y) do { HS_MAD((x),(y)); HS_MAD((x),(y)); } while (0)
    #define HS_EXT(n)    do { t[(n)] = c0; c0 = c1; c1 = c2; c2 = 0; } while (0)

    HS_MAD (a[0], a[0]);                       HS_EXT(0);
    HS_MAD2(a[0], a[1]);                       HS_EXT(1);
    HS_MAD2(a[0], a[2]); HS_MAD (a[1], a[1]);  HS_EXT(2);
    HS_MAD2(a[0], a[3]); HS_MAD2(a[1], a[2]);  HS_EXT(3);
    HS_MAD2(a[1], a[3]); HS_MAD (a[2], a[2]);  HS_EXT(4);
    HS_MAD2(a[2], a[3]);                       HS_EXT(5);
    HS_MAD (a[3], a[3]);                       HS_EXT(6);
    t[7] = c0;

    #undef HS_MAD
    #undef HS_MAD2
    #undef HS_EXT
    cfe_reduce512(r, t);
}
#endif  // __CUDA_ARCH__
#else
// Переключатель выключен: квадрат считается общим умножением. Так было до
// появления отдельной реализации, и так же остаётся способом сравнить.
FD_HD FD_IN void cfe_sqr(cfe r, const cfe a) { cfe_mul(r, a, a); }
#endif  // PK_FAST_SQR

// ------------------------------- обращение ---------------------------------
//
// a^(p-2) mod p по малой теореме Ферма. Цепочка возведений в квадрат взята
// такой же, как в процессорной версии, -- чтобы сверять было с чем.
FD_HD FD_IN void cfe_sqr_n(cfe r, const cfe a, int n) {
    cfe_sqr(r, a);
    for (int i = 1; i < n; i++) cfe_sqr(r, r);
}

FD_HD FD_IN void cfe_inv(cfe r, const cfe a) {
#if defined(__CUDA_ARCH__) && PK_GCD_INV
    if (cfe_is_zero(a)) { cfe_zero(r); return; }
    uint64_t value[5] = {a[0], a[1], a[2], a[3], 0};
    pkh_fast_field::_ModInv(value);
    cfe_set(r, value);
#else
    cfe x2, x3, x6, x9, x11, x22, x44, x88, x176, x220, x223, t;
    cfe_sqr(x2, a);      cfe_mul(x2, x2, a);
    cfe_sqr(x3, x2);     cfe_mul(x3, x3, a);
    cfe_sqr_n(x6, x3, 3);   cfe_mul(x6, x6, x3);
    cfe_sqr_n(x9, x6, 3);   cfe_mul(x9, x9, x3);
    cfe_sqr_n(x11, x9, 2);  cfe_mul(x11, x11, x2);
    cfe_sqr_n(x22, x11, 11);cfe_mul(x22, x22, x11);
    cfe_sqr_n(x44, x22, 22);cfe_mul(x44, x44, x22);
    cfe_sqr_n(x88, x44, 44);cfe_mul(x88, x88, x44);
    cfe_sqr_n(x176, x88, 88); cfe_mul(x176, x176, x88);
    cfe_sqr_n(x220, x176, 44);cfe_mul(x220, x220, x44);
    cfe_sqr_n(x223, x220, 3); cfe_mul(x223, x223, x3);
    cfe_sqr_n(t, x223, 23);   cfe_mul(t, t, x22);
    cfe_sqr_n(t, t, 5);       cfe_mul(t, t, a);
    cfe_sqr_n(t, t, 3);       cfe_mul(t, t, x2);
    cfe_sqr_n(t, t, 2);       cfe_mul(r, t, a);

#endif
}
