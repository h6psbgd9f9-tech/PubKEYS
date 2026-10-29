// ============================================================================
//  cuda_field_test.cpp — сверка поля для видеокарты с работающей реализацией.
//
//  ЗАЧЕМ ЭТО РАБОТАЕТ БЕЗ ВИДЕОКАРТЫ.
//
//  cuda_field.cuh написан так, что собирается и обычным компилятором: пометки
//  __host__ __device__ раскрываются в пустоту, __umul64hi заменяется на
//  __int128. Значит на процессоре выполняется ТОТ ЖЕ ТЕКСТ, который потом
//  пойдёт на карту, и его можно сверить с secp256k1.h — реализацией, которая
//  в этом проекте проверена 87 тестами и нашла настоящие ключи.
//
//  Это не полная замена проверке на карте: nvcc может расставить команды
//  иначе, а у видеокарты своя модель памяти. Но арифметика — то место, где
//  ошибка тише всего и дороже всего, — здесь закрыта.
//
//  Сверяется не «работает вообще», а совпадение до бита на случайных
//  значениях и на краях: ноль, единица, p-1, 2^256-1.
// ============================================================================
#include "cuda_field.cuh"
#include "cuda_batch.cuh"
#include "secp256k1.h"
#include "platform.h"   // plat_console_utf8: кодировка консоли
#include <vector>

#include <cstdio>
#include <cstring>
#include <iostream>
#include <random>
#include <string>

static int fails = 0;
static int u8len(const std::string &s) {
    int n = 0; for (unsigned char c : s) if ((c & 0xC0) != 0x80) n++; return n;
}
static void check(bool ok, const std::string &name, const std::string &detail) {
    std::cout << "  [" << (ok ? "OK " : "FAIL") << "] " << name;
    for (int i = u8len(name); i < 40; i++) std::cout << ' ';
    std::cout << detail << "\n";
    if (!ok) fails++;
}
static std::string hex4(const uint64_t v[4]) {
    char b[80];
    snprintf(b, sizeof b, "%016llx%016llx%016llx%016llx",
             (unsigned long long)v[3], (unsigned long long)v[2],
             (unsigned long long)v[1], (unsigned long long)v[0]);
    return b;
}
static bool same(const uint64_t a[4], const uint64_t b[4]) {
    return memcmp(a, b, 32) == 0;
}

int main() {
    // КОНСОЛЬ В UTF-8 -- ПЕРВОЙ СТРОКОЙ.
    //
    // Без этого весь вывод на Windows выходит кракозябрами: консоль там по
    // умолчанию в кодовой странице 866, а программа печатает UTF-8. Дефект
    // был виден ровно в этом тесте и НЕ виден в plat_selftest -- тот вызов
    // делал, а этот нет. Кракозябры не мешают счёту, но делают
    // нечитаемым единственный отчёт, ради которого тест и запускают.
    plat_console_utf8();

    std::mt19937_64 rng(20260821);
    auto rnd = [&](uint64_t v[4]) {
        for (int i = 0; i < 4; i++) v[i] = rng();
        // Загоняем в поле: значения выше p оба представления трактуют
        // по-разному, и сверять их было бы бессмысленно.
        fe t; memcpy(t, v, 32);
        fe zero; fe_set(zero, t);
        (void)zero;
        // Простейший способ -- взять остаток вычитанием, пока больше p.
        while (memcmp(v, FE_P, 32) >= 0) {
            uint64_t bw = 0;
            for (int i = 0; i < 4; i++) {
                u128 d = (u128)v[i] - FE_P[i] - bw;
                v[i] = (uint64_t)d;
                // (uint64_t) явно: у структурного u128 на MSVC перевод в
                // целое НЕ неявный (иначе fe_mul однажды молча потеряла бы
                // перенос -- ради этого он и explicit).
                bw = (uint64_t)(d >> 64) & 1;
            }
        }
    };

    std::cout << "\n=== поле для видеокарты против проверенной реализации ===\n\n";

    const int N = 20000;

    // ------------------------------------------------------------ сложение
    {
        int bad = 0; std::string first;
        for (int k = 0; k < N; k++) {
            uint64_t a[4], b[4]; rnd(a); rnd(b);
            fe  ra; cfe rb;
            fe_add(ra, a, b);
            cfe_add(rb, a, b);
            if (!same(ra, rb)) { if (!bad) first = hex4(a) + " + " + hex4(b); bad++; }
        }
        check(bad == 0, "сложение, " + std::to_string(N) + " случайных",
              bad ? std::to_string(bad) + " расхождений, первое: " + first : "совпало всё");
    }

    // ----------------------------------------------------------- вычитание
    {
        int bad = 0; std::string first;
        for (int k = 0; k < N; k++) {
            uint64_t a[4], b[4]; rnd(a); rnd(b);
            fe ra; cfe rb;
            fe_sub(ra, a, b);
            cfe_sub(rb, a, b);
            if (!same(ra, rb)) { if (!bad) first = hex4(a) + " - " + hex4(b); bad++; }
        }
        check(bad == 0, "вычитание, " + std::to_string(N) + " случайных",
              bad ? std::to_string(bad) + " расхождений, первое: " + first : "совпало всё");
    }

    // ----------------------------------------------------------- умножение
    //
    // Самое важное место: здесь живёт приведение 512 -> 256, и именно в нём
    // ошибка переноса даёт неверные абсциссы, не роняя ничего.
    {
        int bad = 0; std::string first;
        for (int k = 0; k < N; k++) {
            uint64_t a[4], b[4]; rnd(a); rnd(b);
            fe ra; cfe rb;
            fe_mul(ra, a, b);
            cfe_mul(rb, a, b);
            if (!same(ra, rb)) { if (!bad) first = hex4(a) + " * " + hex4(b); bad++; }
        }
        check(bad == 0, "умножение, " + std::to_string(N) + " случайных",
              bad ? std::to_string(bad) + " расхождений, первое: " + first : "совпало всё");
    }

    // -------------------------------------------------------------- квадрат
    {
        int bad = 0;
        for (int k = 0; k < N; k++) {
            uint64_t a[4]; rnd(a);
            fe ra; cfe rb;
            fe_sqr(ra, a);
            cfe_sqr(rb, a);
            if (!same(ra, rb)) bad++;
        }
        check(bad == 0, "квадрат, " + std::to_string(N) + " случайных",
              bad ? std::to_string(bad) + " расхождений" : "совпало всё");
    }

    // ------------------------------------------------------------ обращение
    //
    // Меньше повторов: обращение это 255 возведений в квадрат, и на двадцати
    // тысячах тест шёл бы минуты. Тысячи хватает: ошибка в цепочке даёт
    // расхождение на первом же значении, а не на редком.
    {
        int bad = 0; std::string first;
        for (int k = 0; k < 1000; k++) {
            uint64_t a[4]; rnd(a);
            if (cfe_is_zero(a)) continue;              // нуль необратим
            fe ra; cfe rb;
            fe_inv(ra, a);
            cfe_inv(rb, a);
            if (!same(ra, rb)) { if (!bad) first = hex4(a); bad++; }
        }
        check(bad == 0, "обращение, 1000 случайных",
              bad ? std::to_string(bad) + " расхождений, первое: " + first : "совпало всё");
    }

    // Обращение обязано быть настоящим обращением, а не просто совпадать с
    // соседней реализацией: если ошибка в ОБЕИХ, сверка её не увидит.
    {
        int bad = 0;
        for (int k = 0; k < 500; k++) {
            uint64_t a[4]; rnd(a);
            if (cfe_is_zero(a)) continue;
            cfe inv, prod, one;
            cfe_inv(inv, a);
            cfe_mul(prod, a, inv);
            cfe_one(one);
            if (!same(prod, one)) bad++;
        }
        check(bad == 0, "a * a^-1 == 1 (проверка по существу)",
              bad ? std::to_string(bad) + " раз не единица" : "всегда единица");
    }

    // --------------------------------------------------------------- края
    //
    // Случайные значения почти никогда не попадают на границы, а переносы
    // ломаются именно там.
    {
        struct E { const char *name; uint64_t v[4]; };
        const E edges[] = {
            { "ноль",     {0,0,0,0} },
            { "единица",  {1,0,0,0} },
            { "p-1",      {0xFFFFFFFEFFFFFC2EULL, ~0ULL, ~0ULL, ~0ULL} },
            { "2^32+976", {0x1000003D0ULL, 0,0,0} },
            { "старший",  {0, 0, 0, 0x8000000000000000ULL} },
        };
        int bad = 0; std::string first;
        for (const E &x : edges)
            for (const E &y : edges) {
                fe ra; cfe rb;
                fe_add(ra, x.v, y.v);  cfe_add(rb, x.v, y.v);
                if (!same(ra, rb)) { if (!bad) first = std::string(x.name)+" + "+y.name; bad++; }
                fe_sub(ra, x.v, y.v);  cfe_sub(rb, x.v, y.v);
                if (!same(ra, rb)) { if (!bad) first = std::string(x.name)+" - "+y.name; bad++; }
                fe_mul(ra, x.v, y.v);  cfe_mul(rb, x.v, y.v);
                if (!same(ra, rb)) { if (!bad) first = std::string(x.name)+" * "+y.name; bad++; }
            }
        check(bad == 0, "края: 0, 1, p-1, 2^32+976, 2^255",
              bad ? std::to_string(bad) + " расхождений, первое: " + first
                  : "25 пар, три действия — всё совпало");
    }

    // ------------------------------------------------------------------
    //  ПАКЕТ БЕЗ МАССИВОВ СЧИТАЕТ ТО ЖЕ, ЧТО ПАКЕТ С МАССИВАМИ.
    //
    //  Ядро больше не зовёт cpt_batch_xs: два массива абсцисс на нить стоили
    //  8 КБ локальной памяти, которые заполнялись целиком и целиком же
    //  перечитывались ради одной проверки на значение. Вместо них
    //  cpt_batch_visit отдаёт каждую абсциссу сразу и нигде её не хранит.
    //
    //  cpt_batch_xs при этом ОСТАВЛЕН как образец: по нему написаны проверки,
    //  сверяющие пакет с честным k*G. Пока обе функции дают одно и то же, все
    //  те проверки относятся и к той, что работает на карте. Сверяются не
    //  только значения, но и то, что посещено КАЖДОЕ смещение пакета: дыра
    //  здесь означала бы непроверенные ключи без всякого признака.
    {
        const int HALF = 128, SPAN = 2 * HALF;
        std::mt19937_64 r2(20260908);
        auto rnd_fe = [&](cfe a) {
            for (int i = 0; i < 4; i++) a[i] = r2();
            a[3] &= 0x7FFFFFFFFFFFFFFFULL;
        };
        std::vector<cpt> gt((size_t)HALF);
        std::vector<cfe> op(HALF), om(HALF), dxa(HALF), iva(HALF), dxb(HALF), ivb(HALF);

        long long bad = 0, checked = 0;
        for (int round = 0; round < 100; round++) {
            cpt P; rnd_fe(P.x); rnd_fe(P.y); P.inf = 0;
            for (int i = 0; i < HALF; i++) { rnd_fe(gt[i].x); rnd_fe(gt[i].y); gt[i].inf = 0; }

            const int ok1 = cpt_batch_xs(P, gt.data(), HALF, op.data(), om.data(),
                                         dxa.data(), iva.data());
            std::vector<cfe>  got((size_t)SPAN);
            std::vector<char> seen((size_t)SPAN, 0);
            struct Collect {
                std::vector<cfe> *g; std::vector<char> *s;
                void operator()(int off, const cfe &x) {
                    memcpy((*g)[(size_t)off], x, 32); (*s)[(size_t)off] = 1;
                }
            } vis{&got, &seen};
            // Шаг 1 -- сплошная раскладка: на хосте чередовать нечего.
            const int ok2 = cpt_batch_visit(P, gt.data(), HALF, dxb.data(), 1, vis);

            if (ok1 != ok2) { bad++; continue; }
            if (!ok1) continue;                      // вырожденный -- обе отказались

            checked++;
            if (!seen[HALF] || memcmp(got[HALF], P.x, 32)) bad++;
            for (int i = 0; i < HALF; i++) {
                if (i + 1 < HALF) {
                    checked++;
                    if (!seen[HALF+i+1] || memcmp(got[HALF+i+1], op[i], 32)) bad++;
                }
                checked++;
                if (!seen[HALF-i-1] || memcmp(got[HALF-i-1], om[i], 32)) bad++;
            }
            for (int o = 0; o < SPAN; o++) if (!seen[o]) bad++;
        }
        check(bad == 0, "пакет без массивов == пакет с массивами",
              bad ? std::to_string(bad) + " расхождений"
                  : std::to_string(checked) + " значений, все смещения посещены");
    }

    std::cout << "\n";
    if (fails) {
        std::cout << "Провалено: " << fails << "\n"
                  << "Арифметику нельзя выпускать на карту, пока это не сойдётся:\n"
                  << "ошибка здесь не роняет программу, а тихо портит абсциссы.\n";
    } else {
        std::cout << "Поле для видеокарты совпадает с проверенной реализацией.\n"
                  << "Это тот же текст, что пойдёт на карту: пометки __host__ __device__\n"
                  << "означают одну реализацию, собранную дважды.\n";
    }
    return fails ? 1 : 0;
}
