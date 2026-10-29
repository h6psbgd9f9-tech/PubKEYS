// ============================================================================
//  cuda_host.cpp — то, что для видеокарты готовит процессор.
//
//  Умножения точки на скаляр на карте нет и не будет: оно требует таблицы на
//  несколько мегабайт и заметно сложнее всего остального, а нужно раз на
//  пакет из 64 ключей. Дешевле и надёжнее посчитать его здесь -- проверенным
//  ec_mul_G, который в этом проекте нашёл настоящие ключи.
//
//  ВАЖНО: файл собирается обычным компилятором, без nvcc. Значит всё, что
//  здесь написано, проверяется на процессоре -- и проверяется в cuda_host_test.
//  На карту уходят только готовые числа.
// ============================================================================
#include "cuda_batch.cuh"
#include "cuda_scalar_guard.h"
#include <vector>
#include <thread>
#include <atomic>
#include <limits>
#include <new>
#include "secp256k1.h"

#include <cstring>
#include <cstdio>
#include <cstdlib>

// u256 -- это uint64_t[4], то есть массив, а массив нельзя положить в vector
// и нельзя вернуть из функции. Коробка нужна только для этого и ни для чего
// больше: .v внутри -- тот же самый u256, и все функции работают с ним
// напрямую, без единого преобразования.
struct u256_t { u256 v; };

// Перевод между представлениями. Оба -- четыре 64-битных лимба, младший
// первый, поэтому перевод это memcpy; функция существует, чтобы это
// утверждение стояло в одном месте и его можно было проверить.
static inline void pt_from_ec(cpt &d, const Point &s) {
    memcpy(d.x, s.x, 32);
    memcpy(d.y, s.y, 32);
    d.inf = s.inf ? 1 : 0;
}

// ключ из 32 байт (старший первый) в u256
static void key_to_u256(u256 out, const uint8_t k[32]) {
    for (int i = 0; i < 4; i++) {
        uint64_t v = 0;
        for (int b = 0; b < 8; b++) v = (v << 8) | k[31 - (i * 8 + 7 - b)];
        out[i] = v;
    }
}
static void u256_to_key(uint8_t k[32], const u256 v) {
    for (int i = 0; i < 4; i++)
        for (int b = 0; b < 8; b++)
            k[31 - (i * 8 + b)] = (uint8_t)(v[i] >> (8 * b));
}

// умножение 256-битного на маленькое
static void mul_small(u256 out, const u256 a, uint64_t m) {
    u128 carry = 0;
    for (int i = 0; i < 4; i++) {
        u128 t = (u128)a[i] * m + carry;
        out[i] = (uint64_t)t;
        carry = t >> 64;
    }
}

// ---------------------------------------------------------------------------
//  ПОДГОТОВКА РАЗДЕЛЕНА НА ДВЕ ЧАСТИ. ЗАЧЕМ.
//
//  Раньше это была одна функция: таблица кратных, точка-шаг и начальные точки
//  ВСЕХ нитей за один вызов. Пока она считает -- 34 мс на этой машине, -- карта
//  стоит и ждёт. Замер показал, что это восьмая часть всего времени захода.
//
//  Спрятать её можно только одним способом: считать подготовку СЛЕДУЮЩЕГО
//  куска, пока карта считает текущий. Для этого подготовку надо уметь просить
//  по частям, а не всю сразу, -- отсюда разделение.
//
//  cuda_host_tables -- то, что общее для всего захода и зависит только от шага
//  перебора и числа нитей. Считается один раз.
//  cuda_host_bases  -- начальные точки ОДНОГО отрезка нитей. Вызывается по разу
//  на кусок, с ключа, с которого этот отрезок начинается.
//
//  cuda_host_prepare остался и делает ровно то же, что и раньше: он теперь
//  просто зовёт обе. Ни одна проверка, написанная под него, не изменилась --
//  и это важно, потому что cuda_host_test сверяет его начальные точки с
//  честным k*G, и эта сверка должна продолжать проверять тот же код.
// ---------------------------------------------------------------------------
extern "C" int cuda_host_tables(int nthreads,
                                const uint8_t step32[32],
                                cpt *out_gt, int half,
                                cpt *out_stride, uint8_t *out_stridekey)
{
    if (nthreads <= 0 || half <= 0 || half > INT_MAX / 2 || !step32
        || !out_gt || !out_stride || !out_stridekey) return 0;
    const uint64_t stride_count = (uint64_t)nthreads * ((uint64_t)half * 2);
    if (!pkh_cuda_scalar::mul_fits(step32, stride_count)) return 0;
    ec_build_comb();

    u256 step; key_to_u256(step, step32);
    // Было (const uint64_t[4]){0,0,0,0} -- составной литерал из C99.
    // В C++ его нет: GCC и Clang принимают как расширение, MSVC отвергает.
    static const uint64_t ZERO4[4] = {0, 0, 0, 0};
    if (u256_cmp(step, ZERO4) == 0) return 0;

    // --- таблица кратных: (i+1)*Q, где Q = step*G ---------------------------
    //
    // Строится сложением, а не умножением: half умножений на скаляр стоили бы
    // в шестьдесят раз дороже, а результат тот же.
    // The multiples depend on step and half, while the stride below also
    // depends on nthreads. Packed KXE alternates full batches and short tails;
    // rebuilding this affine table at each size change serves no purpose.
    // Keep one bounded, thread-local entry. Failure to populate this optional
    // cache leaves the ordinary calculation and its outputs intact.
    struct MultipleTableCache {
        uint8_t step[32]{};
        std::vector<cpt> points;
        bool valid = false;
    };
    static thread_local MultipleTableCache multiples;
    constexpr int max_cached_half = 4096;
    const bool cacheable = half <= max_cached_half;
    const bool cached = cacheable && multiples.valid
        && multiples.points.size() == (size_t)half
        && std::memcmp(multiples.step, step32, 32) == 0;
    if (cached) {
        std::memcpy(out_gt, multiples.points.data(), (size_t)half * sizeof(cpt));
    } else {
        Point Q = ec_mul_G(step);
        if (Q.inf) return 0;
        Point acc = Q;
        for (int i = 0; i < half; i++) {
            pt_from_ec(out_gt[i], acc);
            acc = ec_add(acc, Q);
        }
        if (cacheable) {
            multiples.valid = false;
            try {
                multiples.points.resize((size_t)half);
                std::memcpy(multiples.points.data(), out_gt, (size_t)half * sizeof(cpt));
                std::memcpy(multiples.step, step32, 32);
                multiples.valid = true;
            } catch (const std::bad_alloc&) {
                // The already-computed table remains the successful result.
            }
        }
    }

    // --- точка-шаг: сдвиг центра между заходами ------------------------------
    //
    // Каждый заход одна нить проходит SPAN ключей, а нитей nthreads. Значит
    // между заходами центр сдвигается на nthreads * SPAN * step.
    const uint64_t span = (uint64_t)(2 * half);
    u256 stride_k;
    mul_small(stride_k, step, span * (uint64_t)nthreads);
    Point S = ec_mul_G(stride_k);
    pt_from_ec(*out_stride, S);
    u256_to_key(out_stridekey, stride_k);

    return 1;
}

// ---------------------------------------------------------------------------
//  Начальные точки нитей ОДНОГО отрезка.
//
//  nthreads здесь -- длина отрезка, а не всё число нитей захода, а first_key --
//  ключ, с которого начинается ИМЕННО ЭТОТ отрезок. Вызывающий, разбивая заход
//  на куски, сдвигает first_key на lo*SPAN*step и передаёт указатели на свою
//  часть массивов. Внутри ничего про соседние куски знать не нужно: точки нитей
//  ни от чего, кроме собственного ключа, не зависят.
// ---------------------------------------------------------------------------
extern "C" int cuda_host_bases(int nthreads,
                               const uint8_t first_key[32],
                               const uint8_t step32[32], int half,
                               cpt *out_base, uint8_t *out_basekey)
{
    if (nthreads <= 0 || half <= 0 || half > INT_MAX / 2 || !first_key || !step32
        || !out_base || !out_basekey) return 0;
    const uint64_t guarded_span = (uint64_t)half * 2;
    const uint64_t last_center = (uint64_t)(nthreads - 1) * guarded_span + (uint64_t)half;
    if (!pkh_cuda_scalar::mul_fits(step32, guarded_span)
        || !pkh_cuda_scalar::add_mul_fits(first_key, step32, last_center)) return 0;
    ec_build_comb();

    u256 step; key_to_u256(step, step32);
    static const uint64_t ZERO4B[4] = {0, 0, 0, 0};
    if (u256_cmp(step, ZERO4B) == 0) return 0;
    const uint64_t span = (uint64_t)(2 * half);

    // Нить t начинает с центра first + (t*SPAN + HALF)*step.
    //
    // Первый центр считается умножением, остальные -- сложением с точкой
    // SPAN*step*G. Иначе вышло бы nthreads умножений на скаляр, то есть
    // около 16 тысяч по 63 сложения каждое.
    u256 first; key_to_u256(first, first_key);

    u256 off0;  mul_small(off0, step, (uint64_t)half);
    u256 k0;    u256_add(k0, first, off0);
    Point P = ec_mul_G(k0);

    u256 per;   mul_small(per, step, span);      // шаг между нитями
    Point PER = ec_mul_G(per);

    // ЦЕПОЧКА СТРОИТСЯ В КООРДИНАТАХ ЯКОБИ, А ОБРАЩЕНИЕ ДЕЛАЕТСЯ ОДНО НА ВСЕХ.
    //
    // Здесь стоял прямой цикл P = ec_add(P, PER) по аффинным точкам. Он верен,
    // но ec_add на КАЖДОЕ сложение делает fe_inv -- обращение в поле, около
    // 271 умножения. При 83 968 нитях (328 блоков x 256, как советник
    // предлагает для 5090) это 23 миллиона умножений на КАЖДЫЙ вызов карты.
    //
    // Замер на Core Ultra 9: 498 мс подготовки на вызов. При участке 2^22 это
    // потолок 8.4 млн ключей/с -- меньше, чем даёт ОДИН поток процессора, и
    // карта всё это время простаивает, ожидая начальных точек. То есть
    // подготовка была не накладным расходом, а узким местом всей программы.
    //
    // Теперь так: цепочка идёт в координатах Якоби, где сложение обращения не
    // требует вовсе, все Z собираются в массив, и обращаются они ОДНИМ вызовом
    // fe_inv_batch -- тот самый приём Монтгомери, который в этом проекте уже
    // используется в горячем цикле. Вместо 83 968 обращений остаётся одно.
    //
    // Результат обязан совпасть с прежним до бита: проверяется в
    // cuda_host_test.cpp, где начальные точки нитей сверяются с честным k*G.
    std::vector<Jac> acc((size_t)nthreads);
    std::vector<fe>  zs((size_t)nthreads), zi((size_t)nthreads), scratch((size_t)nthreads);

    // ПОДГОТОВКА ИДЁТ ВО ВСЕ ЯДРА, А НЕ В ОДНО.
    //
    // Почему это оказалось важнее всего остального. Подготовка делается ОДИН
    // раз на вызов карты, независимо от того, сколько ключей за этот вызов
    // проверено, -- но она пропорциональна числу НИТЕЙ. Пока нитей было 84
    // тысячи, она стоила около 100 мс и терялась в тени счёта. Когда число
    // нитей подняли до 1.34 млн (чтобы карте было чем прятать задержки
    // памяти), подготовка выросла ровно во столько же раз -- до ~0.8 с, и
    // стала занимать больше времени, чем сам счёт.
    //
    // Замер на этой машине показывал это прямо: участок 2^28 проходил за
    // 0.98 с при том, что карта считала около 0.18 с. Скорость на экране --
    // 272 млн/с -- была скоростью ПОДГОТОВКИ, а не карты; сама карта в эти
    // 0.18 с шла примерно 1.9 млрд/с. То есть больше нитей карте помогло, а
    // видно этого не было: всё съедал один процессорный поток.
    //
    // Цепочка P_i = P_{i-1} + PER выглядит строго последовательной, и в лоб
    // она не делится. Но её можно РАЗРЕЗАТЬ: началом куска s служит точка
    // (k0 + lo_s*per)*G, посчитанная честным ec_mul_G. Одно умножение на
    // скаляр стоит около 970 умножений в поле -- дороже одного звена цепи, но
    // таких умножений ровно столько, сколько кусков (десятки), а звеньев --
    // миллионы. Внутри куска идёт та же цепочка Якоби и СВОЁ пакетное
    // обращение: обращений становится не одно, а по одному на кусок, и это
    // единственная плата за распараллеливание.
    //
    // Результат обязан совпасть с однопоточным до бита -- это то же самое
    // равенство, что проверяет cuda_host_test.cpp, сверяя начальные точки с
    // честным k*G. Куски ничего не разделяют: каждый пишет только в свой
    // отрезок acc/zs/zi/scratch и out_base/out_basekey.
    unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 1;
    // PKH_PREP_THREADS -- только чтобы можно было сравнить: тот же прогон в
    // один поток и в двадцать четыре обязан дать те же числа. Пусто или
    // мусор -- берём число ядер.
    if (const char *e = getenv("PKH_PREP_THREADS")) {
        const int v = atoi(e);
        if (v > 0) hw = (unsigned)v;
    }
    int nseg = (int)hw;
    if (nseg > 32) nseg = 32;                       // дальше выигрыш съедает память
    if (nseg > nthreads / 4096) nseg = nthreads / 4096;  // мелочь дробить незачем
    if (nseg < 1) nseg = 1;

    auto do_segment = [&](int lo, int hi, const Point &Pstart, const u256 kstart) {
        {
            Jac cur;
            fe_set(cur.X, Pstart.x);
            fe_set(cur.Y, Pstart.y);
            fe_one(cur.Z);
            cur.inf = Pstart.inf;
            for (int t = lo; t < hi; t++) {
                acc[(size_t)t] = cur;
                if (t + 1 < hi) jac_add_affine(cur, cur, PER);
            }
        }

        // Точка в бесконечности Z не имеет; чтобы не портить произведение в
        // пакетном обращении, подставляем ей единицу и разбираем отдельно ниже.
        for (int t = lo; t < hi; t++) {
            if (acc[(size_t)t].inf || fe_is_zero(acc[(size_t)t].Z)) fe_one(zs[(size_t)t]);
            else fe_set(zs[(size_t)t], acc[(size_t)t].Z);
        }
        fe_inv_batch(zi.data() + lo, zs.data() + lo, hi - lo, scratch.data() + lo);

        u256 k; u256_set(k, kstart);
        for (int t = lo; t < hi; t++) {
            const Jac &j = acc[(size_t)t];
            if (j.inf || fe_is_zero(j.Z)) {
                out_base[t].inf = 1;
                fe_zero(out_base[t].x);
                fe_zero(out_base[t].y);
            } else {
                fe zi2, zi3, x, y;
                fe_sqr(zi2, zi[(size_t)t]);
                fe_mul(zi3, zi2, zi[(size_t)t]);
                fe_mul(x, j.X, zi2);
                fe_mul(y, j.Y, zi3);
                memcpy(out_base[t].x, x, 32);
                memcpy(out_base[t].y, y, 32);
                out_base[t].inf = 0;
            }
            u256_to_key(out_basekey + (size_t)t * 32, k);
            if (t + 1 < hi) { u256 nk; u256_add(nk, k, per); u256_set(k, nk); }
        }
    };

    if (nseg <= 1) {
        do_segment(0, nthreads, P, k0);
    } else {
        // Начальные точки кусков считаются ЗАРАНЕЕ И ЗДЕСЬ, в одном потоке.
        // Не ради безопасности (ec_mul_G после ec_build_comb только читает
        // таблицу), а чтобы одинаковый ответ не зависел от того, сколько
        // ядер у машины: числа получаются те же самые при любом nseg.
        const int chunk = (nthreads + nseg - 1) / nseg;
        std::vector<int>   los; los.reserve((size_t)nseg);
        std::vector<u256_t> ks;
        std::vector<Point> ps;
        for (int lo = 0; lo < nthreads; lo += chunk) {
            u256 off; mul_small(off, per, (uint64_t)lo);
            u256 kk;  u256_add(kk, k0, off);
            u256_t box; u256_set(box.v, kk);
            los.push_back(lo);
            ks.push_back(box);
            ps.push_back(lo == 0 ? P : ec_mul_G(kk));
        }

        std::vector<std::thread> pool;
        pool.reserve(los.size());
        for (size_t s = 0; s < los.size(); s++) {
            const int lo = los[s];
            const int hi = (lo + chunk < nthreads) ? lo + chunk : nthreads;
            pool.emplace_back([&, s, lo, hi] { do_segment(lo, hi, ps[s], ks[s].v); });
        }
        for (auto &th : pool) th.join();
    }
    return 1;
}

// ---------------------------------------------------------------------------
// Prepare independently located segments. Each segment contains contiguous
// thread spans (at most 4096 threads); output index s*threads_per_segment+t has center key
// first_keys[s] + (t*2*half + half)*step. A failed call must be discarded.
// ---------------------------------------------------------------------------
extern "C" int cuda_host_packed_bases(int segments, int threads_per_segment,
                                      const uint8_t *first_keys,
                                      const uint8_t step32[32], int half,
                                      cpt *out_base, uint8_t *out_basekey)
{
    if (segments <= 0 || threads_per_segment <= 0 || threads_per_segment > 4096 || half <= 0 ||
        half > (std::numeric_limits<int>::max)() / 2 ||
        !first_keys || !step32 || !out_base || !out_basekey)
        return 0;

    const size_t segment_count = (size_t)segments;
    const size_t segment_threads = (size_t)threads_per_segment;
    const size_t max_size = (std::numeric_limits<size_t>::max)();
    if (segment_count > max_size / segment_threads) return 0;
    const size_t total_threads = segment_count * segment_threads;
    if (segment_count > max_size / 32 || total_threads > max_size / 32 ||
        total_threads > max_size / sizeof(cpt))
        return 0;

    u256 step; key_to_u256(step, step32);
    static const uint64_t ZERO4P[4] = {0, 0, 0, 0};
    if (u256_cmp(step, ZERO4P) == 0) return 0;

    std::atomic<size_t> next_segment{0};
    std::atomic<bool> failed{false};
    std::vector<std::thread> pool;
    auto worker = [&] {
        try {
            while (!failed.load(std::memory_order_relaxed)) {
                const size_t s = next_segment.fetch_add(1, std::memory_order_relaxed);
                if (s >= segment_count) break;
                const size_t first_thread = s * segment_threads;
                if (!cuda_host_bases(threads_per_segment, first_keys + s * 32,
                                     step32, half, out_base + first_thread,
                                     out_basekey + first_thread * 32)) {
                    failed.store(true, std::memory_order_relaxed);
                    break;
                }
            }
        } catch (...) {
            failed.store(true, std::memory_order_relaxed);
        }
    };

    try {
        // All workers only read the completed shared multiplication table.
        ec_build_comb();
        unsigned workers = std::thread::hardware_concurrency();
        if (workers == 0) workers = 1;
        if (workers > 16) workers = 16;
        if ((size_t)workers > segment_count) workers = (unsigned)segment_count;
        // The validated segment limit keeps cuda_host_bases single-threaded.
        pool.reserve(workers - 1);
        for (unsigned i = 1; i < workers; ++i) pool.emplace_back(worker);
        worker();
    } catch (...) {
        // Join every worker already launched even if a later launch fails.
        failed.store(true, std::memory_order_relaxed);
    }
    for (auto &thread : pool) thread.join();
    return failed.load(std::memory_order_relaxed) ? 0 : 1;
}

// Byte-window table for device-side scalar multiplication. Entry n*256+v
// is (v*256^n)*G. Halves the additions per independently located segment;
// especially useful for 4096-key blocks. The table is uploaded only once.
extern "C" int cuda_host_packed_bytes(cpt *out)
{
    if (!out) return 0;
    try {
        ec_build_comb();
        for (int n = 0; n < 32; ++n) {
            u256 scalar = {0, 0, 0, 0};
            scalar[n / 8] = (uint64_t)1 << (8 * (n % 8));
            const Point base = ec_mul_G(scalar);
            cpt &zero = out[(size_t)n * 256];
            memset(zero.x, 0, sizeof(zero.x));
            memset(zero.y, 0, sizeof(zero.y));
            zero.inf = 1;
            Point multiple = base;
            for (int v = 1; v < 256; ++v) {
                pt_from_ec(out[(size_t)n * 256 + (size_t)v], multiple);
                if (v < 255) multiple = ec_add(multiple, base);
            }
        }
    } catch (...) {
        return 0;
    }
    return 1;
}

// Unit-step offsets from a segment's first thread center: j*(2*half)*G.
extern "C" int cuda_host_packed_offsets(int n, int half, cpt *out)
{
    if (n <= 0 || half <= 0 || !out ||
        (size_t)n > (std::numeric_limits<size_t>::max)() / sizeof(cpt))
        return 0;
    try {
        ec_build_comb();
        u256 scalar = {(uint64_t)half * 2, 0, 0, 0};
        const Point step = ec_mul_G(scalar);
        memset(out[0].x, 0, sizeof(out[0].x));
        memset(out[0].y, 0, sizeof(out[0].y));
        out[0].inf = 1;
        Point multiple = step;
        for (int j = 1; j < n; ++j) {
            pt_from_ec(out[j], multiple);
            if (j + 1 < n) multiple = ec_add(multiple, step);
        }
    } catch (...) {
        return 0;
    }
    return 1;
}

// ---------------------------------------------------------------------------
//  Прежний вход: всё сразу. Оставлен как есть по составу и по результату --
//  им пользуются проверки и путь без разбиения на куски.
// ---------------------------------------------------------------------------
extern "C" int cuda_host_prepare(int nthreads,
                                 const uint8_t first_key[32],
                                 const uint8_t step32[32],
                                 cpt *out_gt, int half,
                                 cpt *out_base, uint8_t *out_basekey,
                                 cpt *out_stride, uint8_t *out_stridekey)
{
    if (!cuda_host_tables(nthreads, step32, out_gt, half, out_stride, out_stridekey))
        return 0;
    return cuda_host_bases(nthreads, first_key, step32, half, out_base, out_basekey);
}
