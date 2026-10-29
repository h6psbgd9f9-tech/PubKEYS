// ============================================================================
//  secp256k1 field arithmetic:  p = 2^256 - 2^32 - 977
//  Four 64-bit limbs, least significant first.
//
//  Every function is marked usable from both host and device: the host builds
//  the generator multiple tables with the same code the device scans with.
//  Any divergence between the two would be invisible -- the scan would simply
//  stop finding anything -- so sharing one implementation is deliberate.
// ============================================================================
#ifndef GPU_FE_CUH
#define GPU_FE_CUH
#include <stdint.h>
#include <string.h>

#ifdef __CUDACC__
  #define HD __host__ __device__ __forceinline__
#else
  #define HD static inline
#endif

// AUDIT FIX (A1). The old code selected the 64x64->high-64 multiply on
// __CUDACC__, i.e. on "is this file being compiled by nvcc", not on "is this
// the device pass". That put __umul64hi -- a __device__ intrinsic -- into the
// host pass as well. It is not part of the host toolchain, and the host runs
// the same field arithmetic (build_tables, self_test, report_hit), so whether
// this built at all depended on which CUDA version's headers happened to
// forward-declare the intrinsic for the host. The correct discriminator is
// __CUDA_ARCH__, which is defined only while compiling for the device.
//
// The host branch must also work under MSVC, which has no __int128. __umulh is
// the MSVC intrinsic for exactly this operation.
#if defined(__CUDA_ARCH__)
  #define MULHI(a,b) __umul64hi((a),(b))
#elif defined(_MSC_VER)
  #include <intrin.h>
  #define MULHI(a,b) __umulh((a),(b))
#else
  #define MULHI(a,b) ((uint64_t)(((unsigned __int128)(a) * (uint64_t)(b)) >> 64))
#endif

typedef struct { uint64_t d[4]; } fe;

// 2^256 - p. Reduction of the high half multiplies by this constant.
#define FE_C 0x1000003D1ULL

HD void fe_load_p(uint64_t* o) {
    o[0] = 0xFFFFFFFEFFFFFC2FULL;
    o[1] = 0xFFFFFFFFFFFFFFFFULL;
    o[2] = 0xFFFFFFFFFFFFFFFFULL;
    o[3] = 0xFFFFFFFFFFFFFFFFULL;
}

// Add with carry: returns the sum, updates the carry.
HD uint64_t addc(uint64_t a, uint64_t b, uint64_t* carry) {
    uint64_t s  = a + b;
    uint64_t c1 = (s < a);
    uint64_t s2 = s + *carry;
    uint64_t c2 = (s2 < s);
    *carry = c1 | c2;
    return s2;
}

// Subtract with borrow.
HD uint64_t subb(uint64_t a, uint64_t b, uint64_t* borrow) {
    uint64_t d  = a - b;
    uint64_t b1 = (a < b);
    uint64_t d2 = d - *borrow;
    uint64_t b2 = (d < *borrow);
    *borrow = b1 | b2;
    return d2;
}

HD void fe_set_zero(fe* r) { r->d[0]=r->d[1]=r->d[2]=r->d[3]=0; }
HD int  fe_is_zero(const fe* a) { return (a->d[0]|a->d[1]|a->d[2]|a->d[3]) == 0; }
HD int  fe_is_odd(const fe* a)  { return (int)(a->d[0] & 1); }
HD int  fe_eq(const fe* a, const fe* b) {
    return a->d[0]==b->d[0] && a->d[1]==b->d[1] && a->d[2]==b->d[2] && a->d[3]==b->d[3];
}

// 1 when a >= p
HD int fe_ge_p(const fe* a) {
    uint64_t p[4]; fe_load_p(p);
    for (int i = 3; i >= 0; i--) {
        if (a->d[i] > p[i]) return 1;
        if (a->d[i] < p[i]) return 0;
    }
    return 1;
}

HD void fe_sub_p_inplace(fe* a) {
    uint64_t p[4]; fe_load_p(p);
    uint64_t br = 0;
    for (int i = 0; i < 4; i++) a->d[i] = subb(a->d[i], p[i], &br);
}

// r = a + b (mod p).
// Both operands are below p, so the sum stays below 2^257. When it carries
// past 2^256, subtracting p is the same as adding the constant c, and that
// addition cannot carry again.
HD void fe_add(fe* r, const fe* a, const fe* b) {
    uint64_t carry = 0;
    fe t;
    for (int i = 0; i < 4; i++) t.d[i] = addc(a->d[i], b->d[i], &carry);
    if (carry) {
        uint64_t c2 = 0;
        t.d[0] = addc(t.d[0], FE_C, &c2);
        for (int i = 1; i < 4; i++) t.d[i] = addc(t.d[i], 0, &c2);
    } else if (fe_ge_p(&t)) {
        fe_sub_p_inplace(&t);
    }
    *r = t;
}

// r = a - b (mod p). Both below p, so one addition of p covers any borrow.
HD void fe_sub(fe* r, const fe* a, const fe* b) {
    uint64_t br = 0;
    fe t;
    for (int i = 0; i < 4; i++) t.d[i] = subb(a->d[i], b->d[i], &br);
    if (br) {
        uint64_t p[4]; fe_load_p(p);
        uint64_t c = 0;
        for (int i = 0; i < 4; i++) t.d[i] = addc(t.d[i], p[i], &c);
    }
    *r = t;
}

HD void fe_neg(fe* r, const fe* a) {
    if (fe_is_zero(a)) { fe_set_zero(r); return; }
    fe z; fe_set_zero(&z);
    fe_sub(r, &z, a);
}

// 4x4 limb product into 8 limbs.
// At every step a[i]*b[j] + t[i+j] + carry stays below 2^128, so the high
// part always fits one limb.
// ЗАЧЕМ ЗДЕСЬ АССЕМБЛЕР.
//
// У видеокарты есть флаг переноса и инструкции, которые его читают и пишут:
// add.cc / addc.cc для сложения цепочкой, mad.hi.cc / madc.hi для умножения
// со сложением. Написанное на C "s = a + b; carry = (s < b);" компилятор
// восстанавливает в них не всегда: между сложениями стоят сравнения, которые
// он обязан сохранить, потому что по правилам языка их результат наблюдаем.
//
// Умножение в поле -- это шестнадцать умножений с переносами, и на каждый
// ключ таких умножений около полудюжины. Здесь и лежит разница между этой
// программой и Cyclone на самом низком уровне; макросы ниже -- его же,
// проверенные на этой самой карте.
//
// Хостовая ветка остаётся прежней. Она не запасная: это эталон, с которым
// ассемблерная сверяется на карте (audit_device.cu). Две реализации одного и
// того же полезны ровно тем, что расхождение между ними видно сразу.
#if defined(__CUDA_ARCH__)

#define BP_UMULLO(lo,a,b) asm volatile("mul.lo.u64 %0, %1, %2;"  : "=l"(lo) : "l"(a), "l"(b))
#define BP_MADDO(r,a,b,c) asm volatile("mad.hi.cc.u64 %0, %1, %2, %3;"  : "=l"(r) : "l"(a), "l"(b), "l"(c) : "memory")
#define BP_MADDC(r,a,b,c) asm volatile("madc.hi.cc.u64 %0, %1, %2, %3;" : "=l"(r) : "l"(a), "l"(b), "l"(c) : "memory")
#define BP_MADD(r,a,b,c)  asm volatile("madc.hi.u64 %0, %1, %2, %3;"    : "=l"(r) : "l"(a), "l"(b), "l"(c))
#define BP_UADDO1(c,a)    asm volatile("add.cc.u64 %0, %0, %1;"  : "+l"(c) : "l"(a) : "memory")
#define BP_UADDC1(c,a)    asm volatile("addc.cc.u64 %0, %0, %1;" : "+l"(c) : "l"(a) : "memory")
#define BP_UADD1(c,a)     asm volatile("addc.u64 %0, %0, %1;"    : "+l"(c) : "l"(a))

// r[0..4] = a[0..3] * b, где b -- один лимб. Пять лимбов, потому что
// произведение 256 на 64 бита занимает 320.
#define BP_MUL_ROW(r, a, b) { \
    BP_UMULLO((r)[0], (a)[0], (b));           \
    BP_UMULLO((r)[1], (a)[1], (b));           \
    BP_MADDO ((r)[1], (a)[0], (b), (r)[1]);   \
    BP_UMULLO((r)[2], (a)[2], (b));           \
    BP_MADDC ((r)[2], (a)[1], (b), (r)[2]);   \
    BP_UMULLO((r)[3], (a)[3], (b));           \
    BP_MADDC ((r)[3], (a)[2], (b), (r)[3]);   \
    BP_MADD  ((r)[4], (a)[3], (b), 0ULL);     }

HD void fe_mul_raw(uint64_t* t /*8*/, const uint64_t* a, const uint64_t* b) {
    #pragma unroll
    for (int i = 0; i < 8; i++) t[i] = 0;

    // Школьное умножение по строкам -- та же схема, что и в хостовой ветке,
    // отличаются только примитивы. Перенос за t[i+4] не уходит: в позиции
    // i+4 до этой строки ноль, и сумма в ней остаётся меньше 2^64.
    #pragma unroll
    for (int i = 0; i < 4; i++) {
        uint64_t r[5];
        BP_MUL_ROW(r, b, a[i]);
        BP_UADDO1(t[i+0], r[0]);
        BP_UADDC1(t[i+1], r[1]);
        BP_UADDC1(t[i+2], r[2]);
        BP_UADDC1(t[i+3], r[3]);
        BP_UADD1 (t[i+4], r[4]);
    }
}
#else
HD void fe_mul_raw(uint64_t* t /*8*/, const uint64_t* a, const uint64_t* b) {
    for (int i = 0; i < 8; i++) t[i] = 0;
    for (int i = 0; i < 4; i++) {
        uint64_t carry = 0;
        for (int j = 0; j < 4; j++) {
            uint64_t lo = a[i] * b[j];
            uint64_t hi = MULHI(a[i], b[j]);
            uint64_t s1 = t[i+j] + lo;   uint64_t c1 = (s1 < lo);
            uint64_t s2 = s1 + carry;    uint64_t c2 = (s2 < carry);
            t[i+j] = s2;
            carry  = hi + c1 + c2;
        }
        t[i+4] = carry;
    }
}
#endif

// Fold 512 bits into 256:  value = lo + hi*2^256 == lo + hi*c  (mod p)
HD void fe_reduce512(fe* r, const uint64_t* t /*8*/) {
    uint64_t m[5] = {0,0,0,0,0};
    uint64_t carry = 0;
    for (int i = 0; i < 4; i++) {
        uint64_t lo = t[4+i] * FE_C;
        uint64_t hi = MULHI(t[4+i], FE_C);
        uint64_t s  = lo + carry;  uint64_t c1 = (s < lo);
        m[i]  = s;
        carry = hi + c1;
    }
    m[4] = carry;

    uint64_t w[4], c2 = 0;
    for (int i = 0; i < 4; i++) w[i] = addc(t[i], m[i], &c2);
    uint64_t over = m[4] + c2;

    // Second fold: over fits one limb, over*c fits two.
    if (over) {
        uint64_t lo = over * FE_C;
        uint64_t hi = MULHI(over, FE_C);
        uint64_t c3 = 0;
        w[0] = addc(w[0], lo, &c3);
        w[1] = addc(w[1], hi, &c3);
        w[2] = addc(w[2], 0,  &c3);
        w[3] = addc(w[3], 0,  &c3);
        if (c3) {   // third fold, the carry here is never more than one
            uint64_t c4 = 0;
            w[0] = addc(w[0], FE_C, &c4);
            for (int i = 1; i < 4; i++) w[i] = addc(w[i], 0, &c4);
        }
    }

    r->d[0]=w[0]; r->d[1]=w[1]; r->d[2]=w[2]; r->d[3]=w[3];
    if (fe_ge_p(r)) fe_sub_p_inplace(r);
    if (fe_ge_p(r)) fe_sub_p_inplace(r);
}

// Canonical GPU arithmetic shared with the main scan; independent host reference.
HD void fe_mul(fe* r, const fe* a, const fe* b) {
#if defined(__CUDA_ARCH__) && PK_GPU_MATH
    pkh_fast_field::_ModMult(r->d, const_cast<uint64_t*>(a->d), const_cast<uint64_t*>(b->d));
#else
    uint64_t t[8];
    fe_mul_raw(t, a->d, b->d);
    fe_reduce512(r, t);
#endif
}

HD void fe_sqr(fe* r, const fe* a) {
#if defined(__CUDA_ARCH__) && PK_GPU_MATH
    pkh_fast_field::_ModSqr(r->d, const_cast<uint64_t*>(a->d));
#else
    fe_mul(r, a, a);
#endif
}

// Inversion by Fermat's little theorem: a^(p-2).
// Addition chain taken from libsecp256k1.
HD void fe_inv(fe* r, const fe* a) {
    fe x2, x3, x6, x9, x11, x22, x44, x88, x176, x220, x223, t;
    int j;
    fe_sqr(&x2, a);      fe_mul(&x2, &x2, a);
    fe_sqr(&x3, &x2);    fe_mul(&x3, &x3, a);
    x6  = x3;   for (j=0;j<3;j++)  fe_sqr(&x6,&x6);     fe_mul(&x6,&x6,&x3);
    x9  = x6;   for (j=0;j<3;j++)  fe_sqr(&x9,&x9);     fe_mul(&x9,&x9,&x3);
    x11 = x9;   for (j=0;j<2;j++)  fe_sqr(&x11,&x11);   fe_mul(&x11,&x11,&x2);
    x22 = x11;  for (j=0;j<11;j++) fe_sqr(&x22,&x22);   fe_mul(&x22,&x22,&x11);
    x44 = x22;  for (j=0;j<22;j++) fe_sqr(&x44,&x44);   fe_mul(&x44,&x44,&x22);
    x88 = x44;  for (j=0;j<44;j++) fe_sqr(&x88,&x88);   fe_mul(&x88,&x88,&x44);
    x176= x88;  for (j=0;j<88;j++) fe_sqr(&x176,&x176); fe_mul(&x176,&x176,&x88);
    x220= x176; for (j=0;j<44;j++) fe_sqr(&x220,&x220); fe_mul(&x220,&x220,&x44);
    x223= x220; for (j=0;j<3;j++)  fe_sqr(&x223,&x223); fe_mul(&x223,&x223,&x3);
    t = x223;   for (j=0;j<23;j++) fe_sqr(&t,&t);       fe_mul(&t,&t,&x22);
                for (j=0;j<5;j++)  fe_sqr(&t,&t);       fe_mul(&t,&t,a);
                for (j=0;j<3;j++)  fe_sqr(&t,&t);       fe_mul(&t,&t,&x2);
                for (j=0;j<2;j++)  fe_sqr(&t,&t);       fe_mul(r,a,&t);
}

// 32 bytes, most significant first
HD void fe_get_b32(uint8_t* o, const fe* a) {
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 8; j++)
            o[i*8+j] = (uint8_t)(a->d[3-i] >> (56 - 8*j));
}

HD void fe_set_b32(fe* r, const uint8_t* b) {
    for (int i = 0; i < 4; i++) {
        uint64_t v = 0;
        for (int j = 0; j < 8; j++) v = (v << 8) | (uint64_t)b[i*8+j];
        r->d[3-i] = v;
    }
}
#endif
