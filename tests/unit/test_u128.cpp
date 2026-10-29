// ============================================================================
//  Сверка структурного u128 с нативным unsigned __int128.
//
//  Смысл теста. На Windows математика поля пойдёт через структуру из
//  u128_compat.h. Проверить её там, где её будут исполнять, нельзя — MSVC
//  нативного типа не имеет, сравнивать не с чем. Зато здесь, под gcc, есть
//  оба: тот же самый secp256k1.h собирается ДВАЖДЫ — один раз с нативным
//  __int128, другой раз с принудительно включённой структурой — и результаты
//  сравниваются функция в функцию.
//
//  Проверяется не «мои представления об операторах», а настоящие fe_mul,
//  fe_sqr, fe_add, fe_sub, fe_neg, fe_inv на случайных и на краевых
//  значениях. Именно fe_mul в этом проекте однажды уже теряла перенос за
//  2^128 так, что ни один прежний тест этого не увидел.
// ============================================================================
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <random>
#include <vector>

// Стандартные заголовки подключены выше НАМЕРЕННО: ниже secp256k1.h
// включается внутрь namespace, и попади <mutex> туда — получился бы
// std внутри чужого пространства имён.

namespace NAT {
#include "secp_nat.h"
}

#undef U128_COMPAT_H
#undef U128_NATIVE
#undef U128_HD
#define U128_FORCE_STRUCT 1

namespace STR {
#include "secp_str.h"
}

// ----------------------------------------------------------------- проверка -
static uint64_t fails = 0, checks = 0;

static void cmp(const char *what, const uint64_t a[4], const uint64_t b[4],
                const uint64_t x[4], const uint64_t y[4])
{
    checks++;
    if (memcmp(a, b, 32) == 0) return;
    if (++fails > 10) return;
    printf("РАСХОЖДЕНИЕ в %s\n", what);
    printf("  вход  x = %016llx %016llx %016llx %016llx\n",
           (unsigned long long)x[3], (unsigned long long)x[2],
           (unsigned long long)x[1], (unsigned long long)x[0]);
    if (y) printf("  вход  y = %016llx %016llx %016llx %016llx\n",
           (unsigned long long)y[3], (unsigned long long)y[2],
           (unsigned long long)y[1], (unsigned long long)y[0]);
    printf("  нативно = %016llx %016llx %016llx %016llx\n",
           (unsigned long long)a[3], (unsigned long long)a[2],
           (unsigned long long)a[1], (unsigned long long)a[0]);
    printf("  структ. = %016llx %016llx %016llx %016llx\n",
           (unsigned long long)b[3], (unsigned long long)b[2],
           (unsigned long long)b[1], (unsigned long long)b[0]);
}

static void run_pair(const uint64_t x[4], const uint64_t y[4])
{
    uint64_t n[4], s[4];

    NAT::fe_add(n, x, y);  STR::fe_add(s, x, y);  cmp("fe_add", n, s, x, y);
    NAT::fe_sub(n, x, y);  STR::fe_sub(s, x, y);  cmp("fe_sub", n, s, x, y);
    NAT::fe_mul(n, x, y);  STR::fe_mul(s, x, y);  cmp("fe_mul", n, s, x, y);
    NAT::fe_sqr(n, x);     STR::fe_sqr(s, x);     cmp("fe_sqr", n, s, x, 0);
    NAT::fe_neg(n, x);     STR::fe_neg(s, x);     cmp("fe_neg", n, s, x, 0);

    // Приведение 512->256 напрямую: именно там живёт вся возня с acc/acc2.
    uint64_t t[8];
    for (int i = 0; i < 4; i++) { t[i] = x[i]; t[i + 4] = y[i]; }
    NAT::fe_reduce512(t, n); STR::fe_reduce512(t, s);
    cmp("fe_reduce512", n, s, x, y);

    // Обращение — самая длинная цепочка умножений (271 штука).
    NAT::fe_inv(n, x);     STR::fe_inv(s, x);     cmp("fe_inv", n, s, x, 0);
}

// --------------------------------------------------- операторы напрямую ----
//  fe_* не делят и не берут остаток, поэтому через них operator/ и operator%
//  не проверяются вовсе. А в traverse.cpp и traverse_modes.cpp деление есть
//  (перевод в десятичную запись, номер цикла). Поэтому операторы сверяются и
//  напрямую, на случайных 128-битных значениях.
static uint64_t op_fails = 0, op_checks = 0;

static void op_cmp(const char *what, unsigned __int128 nat, const STR::u128 &str,
                   unsigned __int128 x, unsigned __int128 y)
{
    op_checks++;
    const uint64_t nlo = (uint64_t)nat, nhi = (uint64_t)(nat >> 64);
    if (nlo == str.lo && nhi == str.hi) return;
    if (++op_fails > 10) return;
    printf("РАСХОЖДЕНИЕ в операторе %s\n", what);
    printf("  x = %016llx%016llx\n", (unsigned long long)(uint64_t)(x >> 64), (unsigned long long)(uint64_t)x);
    printf("  y = %016llx%016llx\n", (unsigned long long)(uint64_t)(y >> 64), (unsigned long long)(uint64_t)y);
    printf("  нативно = %016llx%016llx\n", (unsigned long long)nhi, (unsigned long long)nlo);
    printf("  структ. = %016llx%016llx\n", (unsigned long long)str.hi, (unsigned long long)str.lo);
}

static STR::u128 mk(unsigned __int128 v) {
    STR::u128 r; r.lo = (uint64_t)v; r.hi = (uint64_t)(v >> 64); return r;
}

static void test_operators(uint64_t rounds)
{
    std::mt19937_64 rng(777);
    const uint64_t FF = ~0ULL;

    // краевые + случайные
    std::vector<unsigned __int128> vals = {
        0, 1, 2, (unsigned __int128)FF, (unsigned __int128)FF + 1,
        ((unsigned __int128)1 << 64), ((unsigned __int128)1 << 127),
        ~(unsigned __int128)0, ((unsigned __int128)FF << 64) | FF,
        1000000000ULL, ((unsigned __int128)999999999ULL << 64) | 12345,
    };
    for (uint64_t i = 0; i < rounds; i++) {
        unsigned __int128 v = ((unsigned __int128)rng() << 64) | rng();
        vals.push_back(v);
        if ((i & 3) == 0) vals.push_back(v >> (rng() % 128));   // и мелкие тоже
    }

    for (size_t i = 0; i < vals.size(); i++) {
        const unsigned __int128 x = vals[i];
        const unsigned __int128 y = vals[(i * 7 + 3) % vals.size()];
        const STR::u128 sx = mk(x), sy = mk(y);

        op_cmp("+",  x + y,  sx + sy,  x, y);
        op_cmp("-",  x - y,  sx - sy,  x, y);
        op_cmp("*",  x * y,  sx * sy,  x, y);
        op_cmp("|",  x | y,  sx | sy,  x, y);
        op_cmp("&",  x & y,  sx & sy,  x, y);
        op_cmp("^",  x ^ y,  sx ^ sy,  x, y);
        op_cmp("~",  ~x,     ~sx,      x, y);
        if (y) {
            op_cmp("/", x / y, sx / sy, x, y);
            op_cmp("%", x % y, sx % sy, x, y);
        }
        // деление на 64-битные величины -- те самые, что в коде
        const uint64_t divs[5] = {1ULL, 10ULL, 1000000000ULL, 0xFFFFFFFFULL, FF};
        for (int di = 0; di < 5; di++) { const uint64_t d = divs[di]; {
            op_cmp("/u64", x / d, sx / STR::u128(d), x, d);
            op_cmp("%u64", x % d, sx % STR::u128(d), x, d);
        }
        }
        for (int s = 0; s < 128; s++) {
            op_cmp("<<", x << s, sx << s, x, (unsigned __int128)s);
            op_cmp(">>", x >> s, sx >> s, x, (unsigned __int128)s);
        }
        op_checks++;
        if ((x < y) != (sx < sy)) { printf("РАСХОЖДЕНИЕ в операторе <\n"); op_fails++; }
        op_checks++;
        if (((bool)x) != ((bool)sx)) { printf("РАСХОЖДЕНИЕ в приведении к bool\n"); op_fails++; }
        op_checks++;
        if ((uint64_t)x != (uint64_t)sx) { printf("РАСХОЖДЕНИЕ в приведении к uint64_t\n"); op_fails++; }
    }
    printf("Операторы напрямую: %llu проверок, расхождений %llu\n",
           (unsigned long long)op_checks, (unsigned long long)op_fails);
    fails += op_fails;
}

int main(int argc, char **argv)
{
    const uint64_t ROUNDS = (argc > 1) ? strtoull(argv[1], 0, 10) : 200000;

    printf("Сверка структурного u128 с нативным __int128\n");
    printf("  NAT::u128 нативный  : %s\n", "да (unsigned __int128)");
    printf("  STR::u128 структура : размер %zu байт\n", sizeof(STR::u128));
    printf("  раундов             : %llu\n\n", (unsigned long long)ROUNDS);

    // --- краевые значения: около нуля, около p, около 2^64 и 2^128 ---------
    const uint64_t P0 = 0xFFFFFFFEFFFFFC2FULL, FF = 0xFFFFFFFFFFFFFFFFULL;
    std::vector<std::vector<uint64_t>> edge = {
        {0,0,0,0},
        {1,0,0,0},
        {FF,0,0,0},
        {0,1,0,0},
        {FF,FF,FF,FF},
        {P0,FF,FF,FF},                 // p само
        {P0-1,FF,FF,FF},               // p-1
        {P0+1,FF,FF,FF},               // p+1
        {0x1000003D1ULL,0,0,0},        // 2^256 mod p
        {FF,FF,FF,0x7FFFFFFFFFFFFFFFULL},
        {0,0,0,FF},
        {FF,0,FF,0},
        {0x0123456789ABCDEFULL,0xFEDCBA9876543210ULL,FF,1},
    };
    for (size_t i = 0; i < edge.size(); i++)
        for (size_t j = 0; j < edge.size(); j++)
            run_pair(edge[i].data(), edge[j].data());
    printf("Краевые значения: %llu проверок, расхождений %llu\n",
           (unsigned long long)checks, (unsigned long long)fails);

    // --- случайные значения, включая «почти p» ------------------------------
    std::mt19937_64 rng(20260907);
    const uint64_t before = checks;
    for (uint64_t it = 0; it < ROUNDS; it++) {
        uint64_t x[4], y[4];
        for (int i = 0; i < 4; i++) { x[i] = rng(); y[i] = rng(); }
        // Каждый восьмой раз загоняем операнд к верхнему краю поля: именно
        // там пряталась прежняя ошибка переноса, у случайных её не видно.
        if ((it & 7) == 0) { x[3] = FF; x[2] = FF; x[1] = FF; x[0] = P0 - (rng() & 0xFF); }
        if ((it & 7) == 1) { y[3] = FF; y[2] = FF; y[1] = FF; y[0] = P0 - (rng() & 0xFF); }
        run_pair(x, y);
    }
    printf("Случайные значения: %llu проверок, расхождений %llu\n",
           (unsigned long long)(checks - before), (unsigned long long)fails);

    test_operators(2000);
    checks += op_checks;

    printf("\nВсего %llu проверок, расхождений %llu -- %s\n",
           (unsigned long long)checks, (unsigned long long)fails,
           fails ? "ПРОВАЛ" : "СОШЛОСЬ ПОЛНОСТЬЮ");
    return fails ? 1 : 0;
}
