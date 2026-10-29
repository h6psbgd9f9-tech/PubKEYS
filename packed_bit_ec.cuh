// ============================================================================
//  secp256k1 curve points.
//
//  Scalar multiplication runs in Jacobian coordinates: addition there needs no
//  modular inversion, so one whole multiplication costs a single inversion at
//  the very end when converting back to affine.
//
//  Points inside a batch come from affine addition against a table of
//  generator multiples, and every inverse in the batch is obtained from one
//  inversion (Montgomery's trick).
// ============================================================================
#ifndef GPU_EC_CUH
#define GPU_EC_CUH
#include "packed_bit_fe.cuh"

typedef struct { fe x, y; int inf; } ge;      // affine point
typedef struct { fe x, y, z; int inf; } gej;  // Jacobian: (x/z^2, y/z^3)

HD void ge_set_g(ge* r) {
    r->x.d[0]=0x59F2815B16F81798ULL; r->x.d[1]=0x029BFCDB2DCE28D9ULL;
    r->x.d[2]=0x55A06295CE870B07ULL; r->x.d[3]=0x79BE667EF9DCBBACULL;
    r->y.d[0]=0x9C47D08FFB10D4B8ULL; r->y.d[1]=0xFD17B448A6855419ULL;
    r->y.d[2]=0x5DA4FBFC0E1108A8ULL; r->y.d[3]=0x483ADA7726A3C465ULL;
    r->inf = 0;
}

HD void gej_set_ge(gej* r, const ge* a) {
    r->x = a->x; r->y = a->y;
    fe_set_zero(&r->z); r->z.d[0] = 1;
    r->inf = a->inf;
}

HD void gej_set_infinity(gej* r) {
    fe_set_zero(&r->x); fe_set_zero(&r->y); fe_set_zero(&r->z);
    r->inf = 1;
}

// Jacobian doubling for a curve with a = 0.
HD void gej_double(gej* r, const gej* a) {
    if (a->inf || fe_is_zero(&a->y)) { gej_set_infinity(r); return; }
    fe A, B, C, D, X3, Y3, Z3, t1, t2;
    fe_sqr(&A, &a->y);                        // A = y^2
    fe_mul(&B, &a->x, &A);                    // B = x*y^2
    fe_add(&B, &B, &B); fe_add(&B, &B, &B);   // B = 4*x*y^2
    fe_sqr(&C, &A);                           // C = y^4
    fe_add(&C, &C, &C); fe_add(&C, &C, &C);
    fe_add(&C, &C, &C);                       // C = 8*y^4
    fe_sqr(&D, &a->x);                        // D = x^2
    fe_add(&t1, &D, &D); fe_add(&D, &t1, &D); // D = 3*x^2
    // Z is computed BEFORE anything is written to r. It used to be last and
    // read a->y after the new y had already been stored there: a call shaped
    // gej_double(&t, &t) then fed the wrong y into the third coordinate.
    // The failure is silent -- the result stays on the curve, just not the
    // point you asked for.
    fe_mul(&Z3, &a->y, &a->z);
    fe_add(&Z3, &Z3, &Z3);                    // Z3 = 2*y*z
    fe_sqr(&X3, &D);
    fe_sub(&X3, &X3, &B); fe_sub(&X3, &X3, &B);
    fe_sub(&t2, &B, &X3);
    fe_mul(&t2, &D, &t2);
    fe_sub(&Y3, &t2, &C);
    r->x = X3; r->y = Y3; r->z = Z3; r->inf = 0;
}

// Add an affine point to a Jacobian one. Safe when r and a are the same object.
HD void gej_add_ge(gej* r, const gej* a, const ge* b) {
    if (b->inf) { *r = *a; return; }
    if (a->inf) { gej_set_ge(r, b); return; }
    fe z2, z3, u2, s2, h, rr, h2, h3, t;
    fe_sqr(&z2, &a->z);
    fe_mul(&z3, &z2, &a->z);
    fe_mul(&u2, &b->x, &z2);
    fe_mul(&s2, &b->y, &z3);
    fe_sub(&h,  &u2, &a->x);
    fe_sub(&rr, &s2, &a->y);
    if (fe_is_zero(&h)) {
        if (fe_is_zero(&rr)) { gej_double(r, a); return; }
        gej_set_infinity(r); return;
    }
    fe_sqr(&h2, &h);
    fe_mul(&h3, &h2, &h);
    fe_mul(&t,  &a->x, &h2);
    fe X3, Y3, Z3;
    fe_sqr(&X3, &rr);
    fe_sub(&X3, &X3, &h3);
    fe_sub(&X3, &X3, &t);
    fe_sub(&X3, &X3, &t);
    fe_sub(&t, &t, &X3);
    fe_mul(&t, &rr, &t);
    fe_mul(&z2, &a->y, &h3);
    fe_sub(&Y3, &t, &z2);
    fe_mul(&Z3, &a->z, &h);
    r->x = X3; r->y = Y3; r->z = Z3; r->inf = 0;
}

// Back to affine coordinates: one inversion.
HD void ge_set_gej(ge* r, const gej* a) {
    if (a->inf) { r->inf = 1; fe_set_zero(&r->x); fe_set_zero(&r->y); return; }
    fe zi, zi2, zi3;
    fe_inv(&zi, &a->z);
    fe_sqr(&zi2, &zi);
    fe_mul(&zi3, &zi2, &zi);
    fe_mul(&r->x, &a->x, &zi2);
    fe_mul(&r->y, &a->y, &zi3);
    r->inf = 0;
}

// Affine addition with the inverse of the abscissa difference supplied.
// This is the hot path: the batch hands out all its inverses at once.
// Вычитание точки при уже готовой обратной величине.
//
// У -B та же абсцисса, что у B, поэтому разность абсцисс -- а значит и её
// обратная величина -- у сложения и вычитания ОДНА И ТА ЖЕ. Именно на этом
// стоит вся экономия: цепочка Монтгомери строится по H разностям, а ключей
// покрывает 2H. Отличие от сложения ровно в одном знаке.
HD void ge_sub_with_inv(ge* r, const ge* a, const ge* b, const fe* inv_dx) {
    fe dy, lam, x3, y3, t;
    fe_add(&dy, &b->y, &a->y);
    fe_neg(&dy, &dy);                 // dy = -(y_b + y_a) = (-y_b) - y_a
    fe_mul(&lam, &dy, inv_dx);
    fe_sqr(&x3, &lam);
    fe_sub(&x3, &x3, &a->x);
    fe_sub(&x3, &x3, &b->x);          // абсцисса та же, что у B
    fe_sub(&t, &a->x, &x3);
    fe_mul(&t, &lam, &t);
    fe_sub(&y3, &t, &a->y);
    r->x = x3; r->y = y3; r->inf = 0;
}

HD void ge_add_with_inv(ge* r, const ge* a, const ge* b, const fe* inv_dx) {
    fe dy, lam, x3, y3, t;
    fe_sub(&dy, &b->y, &a->y);
    fe_mul(&lam, &dy, inv_dx);
    fe_sqr(&x3, &lam);
    fe_sub(&x3, &x3, &a->x);
    fe_sub(&x3, &x3, &b->x);
    fe_sub(&t, &a->x, &x3);
    fe_mul(&t, &lam, &t);
    fe_sub(&y3, &t, &a->y);
    r->x = x3; r->y = y3; r->inf = 0;
}

// ---------------------------------------------------------------------------
//  Serialization. Four flavours, because one scanned key yields up to four
//  addresses: its own compressed and uncompressed form, plus the same two for
//  the mirror key n-k whose point is -P = (x, p-y).
// ---------------------------------------------------------------------------
HD void ge_serialize_c(uint8_t* o, const ge* p) {
    o[0] = (uint8_t)(0x02 | fe_is_odd(&p->y));
    fe_get_b32(o + 1, &p->x);
}

HD void ge_serialize_u(uint8_t* o, const ge* p) {
    o[0] = 0x04;
    fe_get_b32(o + 1,  &p->x);
    fe_get_b32(o + 33, &p->y);
}

// Mirror, compressed: same abscissa, opposite parity. Negating y flips the
// low bit, so the whole operation is one XOR on the prefix byte.
HD void ge_serialize_c_neg(uint8_t* o, const ge* p) {
    o[0] = (uint8_t)(0x02 | (fe_is_odd(&p->y) ^ 1));
    fe_get_b32(o + 1, &p->x);
}

// Mirror, uncompressed: the ordinate is written out in full, so p-y has to be
// computed. This is where a wrong field constant would produce a perfectly
// valid looking address for the wrong key.
HD void ge_serialize_u_neg(uint8_t* o, const ge* p) {
    fe ny; fe_neg(&ny, &p->y);
    o[0] = 0x04;
    fe_get_b32(o + 1,  &p->x);
    fe_get_b32(o + 33, &ny);
}
#endif
