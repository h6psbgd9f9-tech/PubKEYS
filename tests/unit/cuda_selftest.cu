// ============================================================================
//  cuda_selftest.cu — карта против процессора, ключ в ключ.
//
//  ЗАПУСКАТЬ ПЕРВЫМ ИЗ ВСЕГО, ЧТО КАСАЕТСЯ ВИДЕОКАРТЫ.
//
//  Смысл в одном: у нас уже есть проверенный счёт на процессоре. Он нашёл
//  настоящие ключи и прошёл 91 проверку. Значит правильность работы карты
//  можно не обсуждать, а ИЗМЕРИТЬ -- прогнав один и тот же участок обоими
//  способами и сверив, какие ключи каждый признал совпадением.
//
//  Проверка устроена так, чтобы ловить именно тихие ошибки:
//
//    * подкладывается НАСТОЯЩИЙ ключ из проверяемого отрезка. Карта обязана
//      его найти. «Ложных срабатываний нет» здесь не доказывает ничего:
//      ошибка в арифметике даёт ноль находок, и это выглядит нормально;
//
//    * ключ ставится в РАЗНЫЕ места пакета -- в центр, на нижний край, на
//      верхний, в середину. Центр и края -- ровно те места, где у ядра были
//      дыры: центр не проверялся вовсе, верхний край заезжал на соседа;
//
//    * счётчик проверенных ключей сверяется с ожидаемым: если карта считает
//      меньше, чем говорит, часть отрезка не проверена.
// ============================================================================
#include "cuda_scan.h"
#include "gpu_startup_check.h"
#include "cuda_candidate_count.h"
#include "cuda_batch.cuh"
#include "secp256k1.h"
#include "bloom.h"
#include <vector>

#include <cuda_runtime.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int fails = 0;
static int u8len(const char *s) {
    int n = 0; for (const unsigned char *p=(const unsigned char*)s; *p; p++) if ((*p & 0xC0) != 0x80) n++;
    return n;
}
static void check(bool ok, const char *name, const std::string &detail) {
    printf("  [%s] %s", ok ? "OK " : "FAIL", name);
    for (int i = u8len(name); i < 42; i++) putchar(' ');
    printf("%s\n", detail.c_str());
    if (!ok) fails++;
}

static void u256_to_key(uint8_t k[32], const u256 v) {
    for (int i = 0; i < 4; i++)
        for (int b = 0; b < 8; b++)
            k[31 - (i * 8 + b)] = (uint8_t)(v[i] >> (8 * b));
}

// Same 64-bit atomic type used by ScanVisit, exercised around both old
// 32-bit boundaries without scanning billions of synthetic candidates.
__global__ void count_boundary_kernel(unsigned long long *count) {
    atomicAdd(count, 1ULL);
}

int main() {
    char why[256] = {0};

    std::string startup_why;
    if (!pkh_gpu_check::run(0, 32, startup_why)) {
        check(false, "GPU startup check", startup_why);
        return 1;
    }
    check(true, "GPU startup check", "direct/Bloom, steps, continuation, packed wave/zone");

    // Раскладку берём маленькую: тест должен идти секунды, а не минуты, и
    // проверять правильность, а не скорость.
    const int BLOCKS = 4, THREADS = 32;
    if (!cuda_scan_init(0, BLOCKS, THREADS, why, sizeof why)) {
        printf("\n  Карта недоступна: %s\n", why);
        printf("  Это не провал теста -- проверять нечего.\n");
        return 2;
    }
    { int b, t; size_t v; cuda_scan_layout(&b, &t, &v);
      printf("\n=== карта против процессора ===\n\n");
      printf("  раскладка: %d блоков x %d нитей, видеопамяти %zu КБ\n\n", b, t, v / 1024); }

    ec_build_comb();

    const uint64_t nthr  = (uint64_t)BLOCKS * THREADS;
    const uint64_t span  = CUDA_SPAN;
    const uint64_t total = nthr * span;          // ровно один заход на нить

    // Начало отрезка -- не круглое: круглые числа скрывают ошибки переноса.
    u256 first; hex_to_u256("C0FFEE0000000000000000000000000000000000000000000000000000001234", first);
    uint8_t fk[32]; u256_to_key(fk, first);

    uint8_t step[32] = {0}; step[31] = 1;
    if (!cuda_scan_set_step(step, why, sizeof why)) { printf("  шаг: %s\n", why); return 1; }

    // Куда подкладывать ключ. Центр и края -- места прошлых дыр.
    struct Spot { const char *name; uint64_t off; };
    const Spot spots[] = {
        { "первый ключ отрезка",        0 },
        { "центр первого пакета",       CUDA_HALF },
        { "верхний край первого пакета", (uint64_t)CUDA_SPAN - 1 },
        { "середина отрезка",           total / 2 },
        { "последний ключ отрезка",     total - 1 },
    };

    for (const Spot &sp : spots) {
        u256 k, off;
        u256_from_u64(off, sp.off);
        u256_add(k, first, off);

        Point P = ec_mul_G(k);
        uint8_t tx[32];
        fe_to_be32(tx, P.x);

        if (!cuda_scan_set_targets(tx, 1, nullptr, 0, why, sizeof why)) {
            check(false, sp.name, std::string("цели: ") + why);
            continue;
        }

        std::vector<uint8_t> out(64 * 32);
        int found = 0;
        const uint64_t done = cuda_scan_run(fk, total, out.data(), 64, &found);

        // Ищем среди кандидатов ровно наш ключ.
        uint8_t want[32]; u256_to_key(want, k);
        bool hit = false;
        for (int i = 0; i < found && i < 64; i++)
            if (memcmp(out.data() + (size_t)i * 32, want, 32) == 0) { hit = true; break; }

        char d[192];
        snprintf(d, sizeof d, "%s, кандидатов %d, проверено %llu из %llu",
                 hit ? "найден" : "НЕ НАЙДЕН", found,
                 (unsigned long long)done, (unsigned long long)total);
        check(hit && done >= total, sp.name, d);
    }

    // Ключ ЗА пределами отрезка находиться не должен: иначе карта считает не
    // то, что ей задали, и покрытие сместилось.
    {
        u256 k, off;
        u256_from_u64(off, total + 12345);
        u256_add(k, first, off);
        Point P = ec_mul_G(k);
        uint8_t tx[32]; fe_to_be32(tx, P.x);
        cuda_scan_set_targets(tx, 1, nullptr, 0, why, sizeof why);

        std::vector<uint8_t> out(64 * 32);
        int found = 0;
        cuda_scan_run(fk, total, out.data(), 64, &found);

        uint8_t want[32]; u256_to_key(want, k);
        bool hit = false;
        for (int i = 0; i < found && i < 64; i++)
            if (memcmp(out.data() + (size_t)i * 32, want, 32) == 0) { hit = true; break; }
        check(!hit, "ключ вне отрезка НЕ найден",
              hit ? "найден чужой ключ — покрытие смещено" : "как и должно");
    }

    // ВНИМАНИЕ: cuda_scan_shutdown() СТОЯЛ ЗДЕСЬ.
    //
    // После него g_ready = false, и всё, что ниже, обращалось к выключенной
    // карте: cuda_scan_set_targets немедленно возвращал 0, cuda_scan_run --
    // тоже, found оставался нулём. То есть четыре проверки фильтра падали не
    // из-за фильтра, а из-за выключения, а последняя, «чужой ключ НЕ найден»,
    // проходила ровно потому, что НИЧЕГО НЕ СЧИТАЛОСЬ, -- ложный OK.
    //
    // Цена такой ошибки здесь особая: фильтр -- единственный путь, которым
    // работает engine.cpp на настоящей базе. Решив, что провалы вызваны
    // отсутствием карты, боевой путь можно было оставить непроверенным при
    // внешне пройденном тесте. Выключение перенесено в самый конец.

    // ========================================================================
    //  ПУТЬ ЧЕРЕЗ ФИЛЬТР -- ОТДЕЛЬНАЯ ВЕТКА ЯДРА, И ОНА НЕ ПРОВЕРЯЛАСЬ.
    //
    //  Всё выше идёт по пути перебора: одна цель, сравнение по старшему
    //  лимбу. На настоящей базе ядро работает ИНАЧЕ -- через блочный фильтр,
    //  и это другой код. Пока он не проверен, доказано лишь то, что карта
    //  быстро считает, а не то, что она НАЙДЁТ ключ из базы. Это худший род
    //  пробела: скорость прекрасна, находок нет никогда, и отличить это от
    //  обычного "ещё не нашли" невозможно.
    //
    //  Проверяем так же, как и перебор: подкладываем настоящий ключ в разные
    //  места отрезка и требуем, чтобы карта его нашла -- но уже через фильтр.
    // ========================================================================
    {
        const uint64_t blocks = bloom_blocks_for(64);
        const uint64_t bmask  = blocks - 1;
        std::vector<uint64_t> bl((size_t)blocks * BLOOM_BLOCK_WORDS, 0);

        // Кладём в фильтр 1000 посторонних целей -- чтобы он был не пустым и
        // отвечал "нет" не по случайности, а по существу.
        for (uint64_t i = 1; i <= 1000; i++) {
            u256 kk; u256_from_u64(kk, 1000000 + i);
            Point PP = ec_mul_G(kk);
            bloom_add(bl.data(), bmask, PP.x);
        }

        const struct { const char *name; uint64_t off; } fspots[] = {
            { "фильтр: первый ключ отрезка", 0 },
            { "фильтр: центр первого пакета", (uint64_t)CUDA_HALF },
            { "фильтр: середина отрезка",    total / 2 },
            { "фильтр: последний ключ",      total - 1 },
        };

        for (const auto &sp : fspots) {
            u256 k, off;
            u256_from_u64(off, sp.off);
            u256_add(k, first, off);
            Point P = ec_mul_G(k);

            // Наш ключ добавляется в фильтр поверх посторонних.
            std::vector<uint64_t> b2 = bl;
            bloom_add(b2.data(), bmask, P.x);

            if (!cuda_scan_set_targets(nullptr, 1001, b2.data(), bmask,
                                       why, sizeof why)) {
                check(false, sp.name, std::string("фильтр: ") + why);
                continue;
            }

            std::vector<uint8_t> out(64 * 32);
            int found = 0;
            const uint64_t done = cuda_scan_run(fk, total, out.data(), 64, &found);

            uint8_t want[32]; u256_to_key(want, k);
            bool hit = false;
            for (int i = 0; i < found && i < 64; i++)
                if (memcmp(out.data() + (size_t)i * 32, want, 32) == 0) { hit = true; break; }

            char d[192];
            snprintf(d, sizeof d, "%s, кандидатов %d, проверено %llu из %llu",
                     hit ? "найден" : "НЕ НАЙДЕН", found,
                     (unsigned long long)done, (unsigned long long)total);
            check(hit && done >= total, sp.name, d);
        }

        // И обратное: ключа, которого в фильтре нет, находиться не должно.
        // Ложные срабатывания фильтр даёт, но редко -- один на десятки тысяч,
        // поэтому на одном ключе ждём чистого "нет".
        {
            u256 k, off;
            u256_from_u64(off, total / 3);
            u256_add(k, first, off);
            uint8_t want[32]; u256_to_key(want, k);

            cuda_scan_set_targets(nullptr, 1000, bl.data(), bmask, why, sizeof why);
            std::vector<uint8_t> out(64 * 32);
            int found = 0;
            cuda_scan_run(fk, total, out.data(), 64, &found);

            bool hit = false;
            for (int i = 0; i < found && i < 64; i++)
                if (memcmp(out.data() + (size_t)i * 32, want, 32) == 0) { hit = true; break; }
            check(!hit, "фильтр: чужой ключ НЕ найден",
                  hit ? "НАЙДЕН, чего быть не должно" : "как и должно");
        }
    }

    // The shared next-center inverse must work after multiple rounds, for
    // non-unit scalar steps, and across the low 64-bit carry boundary.
    for (uint64_t step_value : {1ULL, 17ULL}) {
        uint8_t round_step[32] = {}; round_step[31] = (uint8_t)step_value;
        bool round_ok = cuda_scan_set_step(round_step, why, sizeof why) != 0;
        u256 carry_first; memcpy(carry_first, first, 32);
        carry_first[0] = ~UINT64_C(0) - total;
        uint8_t carry_key[32]; u256_to_key(carry_key, carry_first);
        for (uint64_t position : {total, total + CUDA_HALF, 2*total-1, 2*total, 2*total+CUDA_HALF, 3*total-1}) {
            u256 delta, k; u256_from_u64(delta, position*step_value); u256_add(k, carry_first, delta);
            Point expected = ec_mul_G(k); uint8_t tx[32], want[32];
            fe_to_be32(tx, expected.x); u256_to_key(want, k);
            round_ok = round_ok && cuda_scan_set_targets(tx, 1, nullptr, 0, why, sizeof why) != 0;
            std::vector<uint8_t> output(64*32); int found=0;
            const uint64_t done = cuda_scan_run(carry_key, 3*total, output.data(), 64, &found);
            bool hit=false;
            for(int i=0;i<found && i<64;++i) if(!memcmp(output.data()+(size_t)i*32,want,32)) hit=true;
            round_ok = round_ok && done==3*total && hit;
        }
        check(round_ok, "next center: rounds 2/3 and carry", std::string("independent kG; scalar step=")+std::to_string(step_value));
    }
    step[31] = 1;
    if (!cuda_scan_set_step(step, why, sizeof why)) { check(false,"restore unit scalar step",why); }

    // Saturated Bloom makes EVERY scanned scalar a candidate. Force a real
    // >65536 overflow, discard that partial output, then replay smaller
    // sections and require every scalar exactly once in the accepted output.
    {
        uint64_t all_bits[BLOOM_BLOCK_WORDS];
        for (int i = 0; i < BLOOM_BLOCK_WORDS; ++i) all_bits[i] = ~UINT64_C(0);
        bool ok = cuda_scan_set_targets(nullptr, 9, all_bits, 0, why, sizeof why) != 0;
        const uint64_t count = (uint64_t)CUDA_MAX_OUT * 2 + 137;
        std::vector<uint8_t> seen((size_t)count, 0), out((size_t)CUDA_MAX_OUT * 32);
        uint64_t accepted = 0;
        unsigned replayed = 0;
        while (ok && accepted < count) {
            u256 current, offset;
            u256_from_u64(offset, accepted); u256_add(current, first, offset);
            uint8_t current_be[32]; u256_to_key(current_be, current);
            uint64_t want = count - accepted, done = 0;
            int found = 0;
            for (;;) {
                done = cuda_scan_run(current_be, want, out.data(), CUDA_MAX_OUT, &found);
                if (!done || found < CUDA_MAX_OUT || want <= 1) break;
                want /= 2;
                ++replayed;
            }
            if (!done || done > want || found < 0 || found >= CUDA_MAX_OUT || (uint64_t)found != done) { ok = false; break; }
            for (int i = 0; i < found; ++i) {
                u256 candidate, delta;
                fe_from_be32(candidate, out.data() + (size_t)i * 32);
                u256_sub(delta, candidate, first);
                if (delta[1] || delta[2] || delta[3] || delta[0] < accepted ||
                    delta[0] >= accepted + done || delta[0] >= count || seen[(size_t)delta[0]]) {
                    ok = false; break;
                }
                seen[(size_t)delta[0]] = 1;
            }
            accepted += done;
        }
        uint64_t unique = 0;
        for (uint8_t v : seen) unique += v;
        char detail[200];
        snprintf(detail, sizeof detail, "unique=%llu/%llu, replay=%u, no missing/duplicate accepted keys",
                 (unsigned long long)unique, (unsigned long long)count, replayed);
        check(ok && unique == count && replayed > 0, "65536+ candidate overflow and replay", detail);
    }
    {
        unsigned long long *device_count = nullptr;
        bool ok = cudaMalloc(&device_count, sizeof(*device_count)) == cudaSuccess;
        const unsigned long long starts[] = {0x7fffffc0ULL, 0xffffffc0ULL, 0x100000000ULL};
        for (unsigned long long start : starts) {
            unsigned long long got = 0;
            if (!ok) break;
            ok = cudaMemcpy(device_count, &start, sizeof(start), cudaMemcpyHostToDevice) == cudaSuccess;
            if (!ok) break;
            count_boundary_kernel<<<1, 128>>>(device_count);
            ok = cudaGetLastError() == cudaSuccess && cudaDeviceSynchronize() == cudaSuccess;
            if (ok) ok = cudaMemcpy(&got, device_count, sizeof(got), cudaMemcpyDeviceToHost) == cudaSuccess;
            ok = ok && got == start + 128 &&
                 cuda_candidate_count_clamped(got, CUDA_MAX_OUT, CUDA_MAX_OUT) == CUDA_MAX_OUT;
        }
        if (device_count) cudaFree(device_count);
        check(ok, "64-bit candidate counter boundaries", "cross signed32 and unsigned32 limits, overflow remains visible");
    }

    cuda_scan_shutdown();

    printf("\n");
    if (fails) {
        printf("Провалено: %d\n"
               "Счёту на карте верить нельзя, пока это не сойдётся: ошибка здесь\n"
               "не роняет программу, а даёт ноль совпадений при полной скорости.\n", fails);
    } else {
        printf("Карта считает то же, что и процессор.\n");
    }
    return fails ? 1 : 0;
}
