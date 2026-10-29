// ============================================================================
//  cuda_scan.cu — горячий цикл на видеокарте.
//
//  ЧТО ПРОВЕРЕНО, А ЧТО НЕТ.
//
//  Вся арифметика -- поле (cuda_field.cuh) и пакет (cuda_batch.cuh) --
//  помечена __host__ __device__ и проверена НА ПРОЦЕССОРЕ: поле сверено с
//  реализацией, нашедшей настоящие ключи, пакет -- с честным k*G на 19 200
//  ключах. Это тот же текст, что исполняется на карте.
//
//  Не проверено ровно то, что осталось в этом файле: расстановка нитей,
//  выделение памяти, перенос данных туда и обратно. Ни nvcc, ни видеокарты
//  там, где это писалось, не было.
//
//  Поэтому первым запускается cuda_selftest: он гоняет один участок на
//  процессоре и на карте и сверяет ключи. Пока он не сошёлся, счёту на карте
//  верить нельзя -- ошибка здесь не роняет программу, а даёт ноль совпадений
//  при внушительной скорости.
//
//  --------------------------------------------------------------------------
//  КАК РАЗДЕЛЕНА РАБОТА
//
//  Умножения точки на скаляр (k*G) на карте НЕТ. Оно требует таблицы на
//  несколько мегабайт и заметно сложнее всего остального, а нужно всего раз
//  на пакет из 64 ключей. Поэтому:
//
//    хост   -- считает начальную точку каждой нити и точку-шаг,
//              обе своим проверенным ec_mul_G;
//    карта  -- берёт пакет вокруг своей точки, сдвигает её на точку-шаг и
//              повторяет. Ни одного умножения на скаляр.
//
//  Хост делает столько сложений точек, сколько нитей, и один раз на запуск.
// ============================================================================
#include "cuda_scan.h"
#include "cuda_candidate_count.h"
#include "cuda_scalar_guard.h"
#include <chrono>
#include "cuda_batch.cuh"
#include "bloom.h"   // тот же блочный фильтр, что у процессора

#include <cuda_runtime.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>

// Profiling only: NOT a search build, deliberately reports no candidates.
#ifndef PKH_PROFILE_NOFILTER
#define PKH_PROFILE_NOFILTER 0
#endif
#if PKH_PROFILE_NOFILTER
__device__ unsigned long long g_profile_coordinate_sink;
#endif

// --------------------------- состояние на карте -----------------------------
// Tables depend on step and thread count, not on the next scalar start.
static bool g_tables_ok = false;
static size_t g_tables_nthr = 0;
static uint8_t g_tables_step[32], g_tables_skey[32];
static cpt g_tables_stride;
static cpt      *d_gt      = nullptr;   // (i+1)*Q, CUDA_HALF штук
static cfe      *d_work    = nullptr;   // цепочка произведений всех нитей, с шагом
static cpt      *d_base    = nullptr;   // текущая точка каждой нити
static uint8_t  *d_basekey = nullptr;   // и её ключ, по 32 байта
static uint64_t *d_targ    = nullptr;   // старшие лимбы целей (только при малом n)
static uint64_t *d_bloom   = nullptr;   // блочный фильтр целиком
static uint64_t  g_bmask   = 0;
static uint8_t  *d_out     = nullptr;   // ключи-кандидаты
static unsigned long long *d_found = nullptr;
// ВЫРОЖДЕННЫЕ ПАКЕТЫ -- ОТДЕЛЬНО ОТ КАНДИДАТОВ.
//
// Раньше центр вырожденного пакета клался в общий буфер кандидатов, и хост
// проверял его как обычного кандидата: ОДИН ключ из SPAN. Остальные SPAN-1
// ключей пакета не проверял никто -- дыра в покрытии, о которой не знал ни
// счётчик, ни экран. Теперь такие центры идут в свой буфер, а хост
// пересчитывает ВЕСЬ пакет вокруг каждого поштучно (scan_slow_range).
static uint8_t  *d_degen   = nullptr;
static int      *d_ndegen  = nullptr;
#define CUDA_MAX_DEGEN 256

// --------------------------------------------------------------------------
//  ПРОДОЛЖЕНИЕ БЕЗ ПОДГОТОВКИ.
//
//  Ядро в конце сохраняет на карту точку и ключ каждой нити:
//      base[tid] = P;  basekey[tid] = K;
//  Это ровно то состояние, с которого нить должна начать СЛЕДУЮЩИЙ заход,
//  если он идёт сразу за этим. А хост всё равно считал начальные точки заново
//  -- 29 мс на заход при 63 мс счёта, то есть почти треть времени карта
//  простаивала, дожидаясь того, что у неё уже было.
//
//  Условия, при которых состояние годится, проверяются все и по отдельности:
//    * начало заказанного участка равно тому ключу, на котором прошлый заход
//      закончился (сверка все 32 байта, а не «похоже»);
//    * то же число нитей -- иначе точка-шаг другая;
//    * тот же шаг перебора;
//    * прошлый заход прошёл РОВНО кругов*нитей*SPAN ключей, без обрезки по
//      краю участка. Иначе часть нитей вышла из цикла досрочно, и их точки
//      остались не там, где нужно.
//  Не сошлось хоть что-то -- считаем подготовку как раньше. Дешевле лишний
//  раз посчитать, чем один раз пропустить ключи молча.
//
//  ПРОВЕРКА. Переменная окружения PKH_CHAIN_CHECK=1 заставляет хост посчитать
//  подготовку ДАЖЕ когда состояние годится, и сверить её с тем, что лежит на
//  карте, побайтно. Это снимает единственный настоящий риск этой правки:
//  ошибка здесь не роняет программу, а тихо сдвигает часть ключей.
// --------------------------------------------------------------------------
// --------------------------------------------------------------------------
//  ЗАКРЕПЛЁННЫЕ БУФЕРЫ ХОСТА, ВЫДЕЛЕННЫЕ ОДИН РАЗ.
//
//  Прежде эти три массива выделялись и освобождались НА КАЖДЫЙ заход -- вместе
//  до 33 МБ обычной памяти по десять раз в секунду. Это и само по себе не
//  бесплатно, но главное другое: обычную память нельзя передавать на карту
//  асинхронно. Драйвер сперва копирует её во временный закреплённый буфер, и
//  cudaMemcpyAsync по ней ведёт себя как синхронный -- то есть совмещения,
//  ради которого всё затевается, не получилось бы вовсе.
//
//  cudaHostAlloc даёт закреплённую память: страницы не уезжают, карта читает
//  их напрямую, передача идёт параллельно счёту. Выделяем под МАКСИМАЛЬНОЕ
//  число нитей один раз при инициализации.
// --------------------------------------------------------------------------
// Подготовка приехала из cuda_host.cpp двумя частями -- см. пояснение там.
extern "C" int cuda_host_tables(int nthreads, const uint8_t step32[32],
                                cpt *out_gt, int half,
                                cpt *out_stride, uint8_t *out_stridekey);
extern "C" int cuda_host_bases(int nthreads, const uint8_t first_key[32],
                               const uint8_t step32[32], int half,
                               cpt *out_base, uint8_t *out_basekey);

// out = key + step*count. Столбиком по 32 байтам, старший байт первый -- в
// этом виде ключи ходят через всю программу, и переводить их в числа ради
// одного сложения было бы дороже самого сложения.
static void key_add_mul(uint8_t out[32], const uint8_t key[32],
                        const uint8_t step[32], uint64_t count) {
    uint64_t carry = 0;
    memcpy(out, key, 32);
    for (int j = 31; j >= 0; j--) {
        const uint64_t add = (uint64_t)step[j] * count + carry;
        const uint64_t sm  = (uint64_t)out[j] + (add & 0xFF);
        out[j] = (uint8_t)sm;
        carry  = (add >> 8) + (sm >> 8);
    }
}

static uint8_t  *d_skey_p   = nullptr;   // ключ точки-шага, на карте
static uint8_t  *d_step_p   = nullptr;   // шаг перебора, на карте
static cpt      *h_gt_pin   = nullptr;   // таблица кратных
static cpt      *h_base_pin = nullptr;   // начальные точки нитей
static uint8_t  *h_bkey_pin = nullptr;   // и их ключи

static bool     g_chain_ok = false;      // на карте лежит годное состояние
static uint8_t  g_chain_next[32];        // ключ, с которого оно годится
static uint8_t  g_chain_step[32];        // при каком шаге перебора
static size_t   g_chain_nthr = 0;        // при каком числе нитей
static cpt      g_chain_stride;          // точка сдвига базы, она же аргумент ядра

static int     g_blocks = 0, g_threads = 0, g_ntargets = 0, g_maxout = 0;
static size_t  g_vram = 0;
static size_t  g_target_vram = 0;
static bool    g_ready = false;
static uint8_t g_step[32];

// Готовит таблицу кратных, начальные точки нитей и точку-шаг. Живёт в
// cuda_host.cpp и пользуется проверенной процессорной арифметикой.
extern "C" int cuda_host_prepare(int nthreads,
                                 const uint8_t first_key[32],
                                 const uint8_t step32[32],
                                 cpt *out_gt, int half,
                                 cpt *out_base, uint8_t *out_basekey,
                                 cpt *out_stride, uint8_t *out_stridekey);

// Отказ на середине выделения ОСВОБОЖДАЕТ уже занятое.
//
// Прежде не освобождал: d_work при 128-полупакете и 375 тыс. нитей -- это
// 3 ГБ, и если он прошёл, а следующее выделение нет, три гигабайта
// видеопамяти оставались занятыми до конца процесса. Человек видел «не
// хватило памяти», уменьшал настройку и пробовал снова из того же меню --
// второй заход выделял поверх первого, и памяти становилось ещё меньше.
// Диагностика при этом уводила в сторону: «мало памяти» на карте, где её
// вдоволь. cudaFree(nullptr) законен, так что shutdown здесь безопасен.
#define CU_TRY(call) do {                                                  \
    cudaError_t _e = (call);                                               \
    if (_e != cudaSuccess) {                                               \
        snprintf(why, why_len, "%s: %s", #call, cudaGetErrorString(_e));    \
        cuda_scan_shutdown();                                              \
        return 0;                                                          \
    }                                                                      \
} while (0)

// ============================================================================
//  ЯДРО
//
//  Одна нить -- один пакет из CUDA_SPAN ключей за проход.
//
//  Не одна нить на ключ: пакетное обращение по своей природе последовательно,
//  разрезать его между нитями можно только через общую память и согласование.
//  Это быстрее, но заметно сложнее -- а сложность в коде, который нельзя
//  проверить, стоит дороже скорости.
// ============================================================================
// ЧТО ДЕЛАЕТ ОДНА НИТЬ С ОДНОЙ АБСЦИССОЙ.
//
// Вынесено в отдельный тип нарочно. Пакет считает абсциссы и тут же отдаёт их
// сюда -- ни одна не сохраняется. Раньше все 2*CUDA_HALF значений складывались
// в два массива в локальной памяти нити, и ядро писало их целиком, а потом
// целиком перечитывало ради одной проверки на каждое.
struct ScanVisit {
    const uint64_t *bloom; uint64_t bmask;
    const uint64_t *targ;  int ntarg;
    const uint8_t  *K;                 // ключ центра пакета
    const uint8_t  *step32;
    uint8_t *out; int max_out; unsigned long long *found;
    int      half;
    uint64_t avail;                    // сколько ключей пакета ещё в заказе
#if PKH_PROFILE_NOFILTER
    unsigned long long coordinate_checksum = 0;
#endif

    // __host__ ТОЖЕ, И ЭТО НЕ УКРАШЕНИЕ.
    //
    // cpt_batch_visit объявлен FD_HD, то есть __host__ __device__, и nvcc
    // инстанцирует его В ОБОИХ ПРОХОДАХ. В хостовом проходе тело шаблона
    // зовёт этот operator(), и device-only функция оттуда недостижима:
    //     error: calling a __device__ function from a __host__ __device__
    //            function is not allowed
    // То есть без этой правки файл не собирается вовсе -- это первое, во что
    // упрётся сборка, когда появится nvcc. Исполняться на хосте телу не
    // требуется; требуется компилироваться, поэтому atomicAdd ниже под
    // __CUDA_ARCH__.
    __host__ __device__ __forceinline__ void operator()(int off, const cfe &x) {
        // ЗА КРАЙ ЗАКАЗАННОГО НЕ ВЫХОДИМ.
        //
        // Участок редко делится на пакеты нацело, и хвостовой пакет заезжал
        // за конец: карта проверяла ключи, которых не просили, и сообщала о
        // них как о своих. Проверка "ничего лишнего за участком" ловила это
        // прямо -- 431 лишний ключ на участке в 3000.
        if ((uint64_t)off >= avail) return;

#if PKH_PROFILE_NOFILTER
        // Consume ALL X limbs so the compiler cannot remove the EC work when
        // membership testing is disabled. One sink atomic per thread below.
        coordinate_checksum ^= x[0] ^ x[1] ^ x[2] ^ x[3];
        return;
#endif

        bool hit;
        if (bloom) {
            hit = bloom_maybe(bloom, bmask, x) != 0;
        } else {
            // Сравнение по СТАРШЕМУ лимбу: ложное срабатывание возможно, одно
            // на 2^64, а подтверждает хост.
            hit = false;
            for (int t = 0; t < ntarg; t++) if (x[3] == targ[t]) { hit = true; break; }
        }
        if (!hit) return;

#ifdef __CUDA_ARCH__
        const unsigned long long slot = atomicAdd(found, 1ULL);
#else
        const unsigned long long slot = (*found)++; // host instantiation
#endif
        // A saturated filter can produce 2^32 candidates in one launch.
        // Keep the full count until the host clamps it to its replay signal.
        if (slot >= (unsigned long long)max_out) return;

        uint8_t *o = out + (size_t)slot * 32;
        for (int j = 0; j < 32; j++) o[j] = K[j];

        const int d = off - half;          // смещение от центра, со знаком
        if (d == 0) return;                // сам центр -- ключ уже верный
        const uint64_t mul = (uint64_t)(d > 0 ? d : -d);
        if (d > 0) {
            uint64_t carry = 0;
            for (int j = 31; j >= 0; j--) {
                const uint64_t add = (uint64_t)step32[j] * mul + carry;
                const uint64_t sm  = (uint64_t)o[j] + (add & 0xFF);
                o[j]  = (uint8_t)sm;
                carry = (add >> 8) + (sm >> 8);
            }
        } else {
            uint64_t borrow = 0;
            for (int j = 31; j >= 0; j--) {
                const uint64_t sub = (uint64_t)step32[j] * mul + borrow;
                const int64_t  sm  = (int64_t)o[j] - (int64_t)(sub & 0xFF);
                o[j]   = (uint8_t)sm;
                borrow = (sub >> 8) + (sm < 0 ? 1 : 0);
            }
        }
    }
};

__global__ void kern_scan(const cpt *gt, cfe *work,
                          cpt *base, uint8_t *basekey,
                          const cpt stride, const uint8_t *stridekey,
                          int tid_off,
                          uint64_t rounds, uint64_t nthr_total, uint64_t nkeys,
                          const uint64_t *targ, int ntarg,
                          const uint64_t *bloom, uint64_t bmask,
                          const uint8_t *step32,
                          uint8_t *out, int max_out, unsigned long long *found,
                          uint8_t *dout, int max_degen, int *ndegen)
{
    // СМЕЩЕНИЕ НОМЕРА НИТИ.
    //
    // Заход может запускаться не одним ядром, а несколькими -- по куску нитей
    // в каждом, чтобы подготовку следующего куска считать, пока карта считает
    // текущий. Номер нити при этом обязан остаться СКВОЗНЫМ по всему заходу:
    // от него зависит и место нити в рабочем массиве, и то, какие ключи она
    // берёт. Смещение передаётся снаружи; при запуске одним ядром оно нулевое
    // и всё считается как раньше.
    const int tid = tid_off + blockIdx.x * blockDim.x + threadIdx.x;
    if ((uint64_t)tid >= nthr_total) return; // packed probe may use a partial grid

    // РАСКЛАДКА РАБОЧЕГО МАССИВА -- С ШАГОМ, А НЕ КУСКАМИ.
    //
    // Запись i этой нити лежит по адресу work[i*stride + tid], поэтому нити
    // одного варпа обращаются к соседним адресам и чтение укладывается в
    // строки памяти целиком. Прежняя раскладка давала каждой нити сплошной
    // кусок, соседи расходились на 8 КБ, и из каждой 128-байтной строки
    // использовались 32 байта.
    //
    // Шаг -- ПОЛНОЕ число нитей захода, а не размер этой сетки. Раньше здесь
    // стояло gridDim.x*blockDim.x, и пока заход был одним ядром, это было то
    // же самое число. С разбиением на куски это перестало быть верным: у
    // каждого куска своя сетка, а рабочий массив общий на весь заход, и куски
    // затирали бы друг другу цепочку произведений. Берём nthr_total -- оно и
    // так передаётся, и означает ровно то, что нужно.
    //
    // Имя pref_stride, а не stride: параметр ядра stride -- это ТОЧКА сдвига
    // базы, совсем другая величина. Два разных смысла под одним именем в
    // одной функции -- заготовка для ошибки, которую потом ищут глазами.
    const size_t pref_stride = (size_t)nthr_total;
    cfe *pref = work + tid;

    cpt      P = base[tid];
    uint8_t  K[32];
    for (int i = 0; i < 32; i++) K[i] = basekey[(size_t)tid * 32 + i];

    ScanVisit vis;
    vis.bloom = bloom; vis.bmask = bmask;
    vis.targ  = targ;  vis.ntarg = ntarg;
    vis.K = K; vis.step32 = step32;
    vis.out = out; vis.max_out = max_out; vis.found = found;
    vis.half = CUDA_HALF;

    for (uint64_t r = 0; r < rounds; r++) {
        // Где начинается пакет этой нити в этом круге -- в ключах от начала
        // заказанного участка. Отсюда и видно, сколько его ключей ещё в деле.
        const uint64_t bstart = (r * nthr_total + (uint64_t)tid) * (uint64_t)CUDA_SPAN;
        if (bstart >= nkeys) break;                 // весь остаток -- за краем
        const uint64_t rest = nkeys - bstart;
        vis.avail = (rest < (uint64_t)CUDA_SPAN) ? rest : (uint64_t)CUDA_SPAN;

        cpt np{}; bool advanced = false;
        if (!cpt_batch_visit_step(P, gt, CUDA_HALF, pref, pref_stride, vis, stride, np, advanced)) {
            // ВЫРОЖДЕННЫЙ ПАКЕТ -- НЕ ПРОПУСКАЕМ МОЛЧА.
            //
            // Среди разностей абсцисс попал нуль: центр совпал с кратным Q.
            // Разобрать поштучно карта не может -- у неё нет k*G. Сообщаем
            // хосту ключ центра, и он разберёт сам. Пропустить означало бы
            // дыру в покрытии, о которой никто не узнает.
            const int slot = atomicAdd(ndegen, 1);
            if (slot >= 0 && slot < max_degen) {   // int переполняется -- см. ScanVisit
                uint8_t *o = dout + (size_t)slot * 32;
                for (int j = 0; j < 32; j++) o[j] = K[j];
            }
        }

        // Сдвиг базы: одна точка вместо умножения на скаляр.
        if (!advanced) cpt_add(np, P, stride);
        P = np;
        {
            uint64_t carry = 0;
            for (int j = 31; j >= 0; j--) {
                const uint64_t s = (uint64_t)K[j] + stridekey[j] + carry;
                K[j] = (uint8_t)s;
                carry = s >> 8;
            }
        }
    }

    base[tid] = P;
    for (int i = 0; i < 32; i++) basekey[(size_t)tid * 32 + i] = K[i];
#if PKH_PROFILE_NOFILTER
    atomicXor(&g_profile_coordinate_sink, vis.coordinate_checksum);
#endif
}

// ============================================================================
//  Хостовая часть
// ============================================================================
extern "C" int cuda_scan_query_launch(int device, int preferred_threads,
                                      CudaScanLaunchInfo *info, char *why, size_t why_len) {
    if (!info || !why || !why_len) return 0;
    memset(info, 0, sizeof(*info));
    why[0] = 0;
    auto checked = [&](cudaError_t error, const char *operation) {
        if (error == cudaSuccess) return true;
        snprintf(why, why_len, "%s: %s", operation, cudaGetErrorString(error));
        return false;
    };
    cudaDeviceProp device_info;
    cudaFuncAttributes kernel_info;
    if (!checked(cudaSetDevice(device), "cudaSetDevice")
        || !checked(cudaGetDeviceProperties(&device_info, device), "cudaGetDeviceProperties")
        || !checked(cudaFuncGetAttributes(&kernel_info, kern_scan), "cudaFuncGetAttributes(kern_scan)")) return 0;
    int threads = preferred_threads > 0 ? preferred_threads : 256;
    if (threads > device_info.maxThreadsPerBlock) threads = device_info.maxThreadsPerBlock;
    if (threads > kernel_info.maxThreadsPerBlock) threads = kernel_info.maxThreadsPerBlock;
    if (threads >= device_info.warpSize && device_info.warpSize > 0)
        threads -= threads % device_info.warpSize;
    int resident_blocks = 0;
    for (; threads > 0; threads /= 2) {
        if (!checked(cudaOccupancyMaxActiveBlocksPerMultiprocessor(
                         &resident_blocks, kern_scan, threads, 0), "cudaOccupancy(kern_scan)")) return 0;
        if (resident_blocks > 0) break;
    }
    if (threads <= 0 || resident_blocks <= 0) {
        snprintf(why, why_len, "kern_scan: no legal resident block on this device"); return 0;
    }
    if (!checked(cudaMemGetInfo(&info->memory_free, &info->memory_total), "cudaMemGetInfo")) return 0;
    info->sm_count = device_info.multiProcessorCount;
    info->warp_size = device_info.warpSize;
    info->device_max_threads_per_block = device_info.maxThreadsPerBlock;
    info->device_max_threads_per_sm = device_info.maxThreadsPerMultiProcessor;
    info->kernel_max_threads_per_block = kernel_info.maxThreadsPerBlock;
    info->max_grid_x = device_info.maxGridSize[0];
    info->threads_per_block = threads;
    info->resident_blocks_per_sm = resident_blocks;
    info->registers_per_thread = kernel_info.numRegs;
    info->scratch_bytes_per_thread = (size_t)CUDA_HALF * sizeof(cfe);
    info->allocation_bytes_per_thread = info->scratch_bytes_per_thread + sizeof(cpt) + 32;
    info->fixed_allocation_bytes = (size_t)CUDA_HALF * sizeof(cpt) + (size_t)CUDA_MAX_OUT * 32
        + sizeof(*d_found) + sizeof(int) + (size_t)CUDA_MAX_DEGEN * 32 + 64;
    return 1;
}

extern "C" int cuda_scan_init(int device, int blocks, int threads,
                              char *why, size_t why_len) {
    if (g_ready) return 1;
    int n = 0;
    if (cudaGetDeviceCount(&n) != cudaSuccess || n <= 0) {
        snprintf(why, why_len, "видеокарт CUDA не найдено");
        return 0;
    }
    if (device < 0 || device >= n) {
        snprintf(why, why_len, "нет карты с номером %d (всего %d)", device, n);
        return 0;
    }
    CudaScanLaunchInfo limits;
    if (!cuda_scan_query_launch(device, threads, &limits, why, why_len)) return 0;
    if (threads > 0 && threads != limits.threads_per_block) {
        snprintf(why, why_len, "kern_scan: requested %d threads, legal selection is %d", threads, limits.threads_per_block);
        return 0;
    }
    const uint64_t requested_blocks = blocks > 0 ? (uint64_t)blocks
        : (uint64_t)limits.sm_count * (uint64_t)limits.resident_blocks_per_sm;
    if (!requested_blocks || requested_blocks > (uint64_t)limits.max_grid_x
        || requested_blocks > (uint64_t)INT32_MAX / (uint64_t)limits.threads_per_block) {
        snprintf(why, why_len, "kern_scan: block/thread count exceeds device or host indexing limits"); return 0;
    }
    g_blocks = (int)requested_blocks;
    g_threads = limits.threads_per_block;
    g_maxout  = CUDA_MAX_OUT;      // одно число с engine.cpp -- см. cuda_scan.h
    const size_t nthr = (size_t)g_blocks * g_threads;
    if (nthr > (SIZE_MAX - limits.fixed_allocation_bytes) / limits.allocation_bytes_per_thread
        || limits.fixed_allocation_bytes + nthr * limits.allocation_bytes_per_thread > limits.memory_free) {
        snprintf(why, why_len, "kern_scan: insufficient free memory for the requested working buffers"); return 0;
    }

    // Вдвое меньше, чем было: второй массив (обращения разностей) больше не
    // нужен -- см. cpt_batch_visit. При 262144 нитях это гигабайт видеопамяти,
    // который просто освободился.
    const size_t work_bytes = nthr * CUDA_HALF * sizeof(cfe);
    CU_TRY(cudaMalloc(&d_work,    work_bytes));
    CU_TRY(cudaMalloc(&d_gt,      CUDA_HALF * sizeof(cpt)));
    CU_TRY(cudaMalloc(&d_base,    nthr * sizeof(cpt)));
    CU_TRY(cudaMalloc(&d_basekey, nthr * 32));
    CU_TRY(cudaMalloc(&d_out,     (size_t)g_maxout * 32));
    CU_TRY(cudaMalloc(&d_found,   sizeof(*d_found)));
    CU_TRY(cudaMalloc(&d_degen,   (size_t)CUDA_MAX_DEGEN * 32));
    CU_TRY(cudaMalloc(&d_ndegen,  sizeof(int)));

    // Закреплённая память хоста -- см. пояснение к h_base_pin.
    CU_TRY(cudaHostAlloc(&h_gt_pin,   CUDA_HALF * sizeof(cpt), cudaHostAllocDefault));
    CU_TRY(cudaHostAlloc(&h_base_pin, nthr * sizeof(cpt),      cudaHostAllocDefault));
    CU_TRY(cudaHostAlloc(&h_bkey_pin, nthr * 32,               cudaHostAllocDefault));

    // Эти два жили по заходу: выделялись и освобождались каждый раз. Мелочь по
    // объёму (64 байта), но при продолжении их не заполняли -- и ядро получало
    // свежевыделенный мусор вместо шага перебора. Теперь они постоянные, как и
    // всё прочее состояние карты, и заполняются всегда.
    CU_TRY(cudaMalloc(&d_skey_p, 32));
    CU_TRY(cudaMalloc(&d_step_p, 32));
    // Считаем ВСЁ, что заняли. Прежде в сумму не входили d_found, d_degen,
    // d_ndegen (мелочь) и фильтр (до 2 ГБ) -- а число печатается в engine.cpp
    // как «под счёт N МБ видеопамяти» и вводило в заблуждение ровно там, где
    // память критична. Фильтр прибавляется отдельно, в cuda_scan_set_targets:
    // на этот момент его размер ещё неизвестен.
    g_vram = limits.fixed_allocation_bytes + nthr * limits.allocation_bytes_per_thread;

    memset(g_step, 0, 32);
    g_step[31] = 1;
    g_ready = true;
    return 1;
}

static void packed_buffers_release();
extern "C" void cuda_scan_shutdown(void) {
    packed_buffers_release();
    g_tables_ok = false;
    cudaFree(d_work); cudaFree(d_gt); cudaFree(d_base); cudaFree(d_basekey);
    cudaFree(d_targ); cudaFree(d_bloom); cudaFree(d_out); cudaFree(d_found);
    cudaFree(d_degen); cudaFree(d_ndegen); d_degen = nullptr; d_ndegen = nullptr;
    cudaFreeHost(h_gt_pin);   h_gt_pin   = nullptr;
    cudaFreeHost(h_base_pin); h_base_pin = nullptr;
    cudaFreeHost(h_bkey_pin); h_bkey_pin = nullptr;
    cudaFree(d_skey_p); d_skey_p = nullptr;
    cudaFree(d_step_p); d_step_p = nullptr;
    d_work = nullptr; d_base = nullptr; d_out = nullptr;
    d_gt = nullptr; d_basekey = nullptr; d_targ = nullptr; d_bloom = nullptr;
    g_bmask = 0; d_found = nullptr;
    // ЦЕЛЕЙ БОЛЬШЕ НЕТ -- И СЧЁТЧИК ОБЯЗАН ЭТО ОТРАЖАТЬ.
    //
    // g_ntargets не сбрасывался, а проверка в начале cuda_scan_run звучит
    // как "!g_ready || g_ntargets <= 0". После повторного cuda_scan_init
    // (g_ready снова true) она пропускала вызов вперёд с ненулевым
    // g_ntargets и НУЛЕВЫМИ d_targ/d_bloom: ядро читало targ[t] по нулевому
    // адресу. Ошибка липкая -- весь контекст CUDA в процессе после неё мёртв,
    // и счёт навсегда сваливался на процессор.
    g_ntargets = 0;
    g_ready = false;
    g_chain_ok = false;
    g_target_vram = 0;
    g_vram = 0;
}

extern "C" int cuda_scan_set_step(const uint8_t step32[32], char *why, size_t why_len) {
    if (!g_ready) { snprintf(why, why_len, "cuda_scan_init не вызывался"); return 0; }
    memcpy(g_step, step32, 32);
    g_chain_ok = false;          // прежнее состояние карты посчитано другим шагом
    return 1;
}

extern "C" int cuda_scan_set_targets(const uint8_t *x32, int n,
                                     const uint64_t *bloom, uint64_t block_mask,
                                     char *why, size_t why_len) {
    if (!g_ready) { snprintf(why, why_len, "cuda_scan_init не вызывался"); return 0; }
    if (n <= 0)   { snprintf(why, why_len, "целей нет"); return 0; }
    size_t target_bytes = 0;
    if (bloom) {
        if (block_mask == UINT64_MAX || !cuda_scan_checked_array_bytes(
                block_mask + 1, BLOOM_BLOCK_WORDS * sizeof(uint64_t), &target_bytes)) {
            snprintf(why, why_len, "CUDA: Bloom allocation size overflow"); return 0;
        }
    } else if (!x32 || (uint64_t)n > SIZE_MAX / 32
               || !cuda_scan_checked_array_bytes((uint64_t)n, sizeof(uint64_t), &target_bytes)) {
        snprintf(why, why_len, "CUDA: invalid target array or allocation size"); return 0;
    }

    if (d_targ)  { cudaFree(d_targ);  d_targ  = nullptr; }
    if (d_bloom) { cudaFree(d_bloom); d_bloom = nullptr; }
    g_vram -= g_target_vram;
    g_target_vram = 0;
    // С этой секунды целей нет, пока не выделены новые. Иначе провал
    // cudaMalloc ниже оставлял бы g_ntargets от прошлого удачного вызова при
    // уже обнулённых указателях -- то же нулевое разыменование в ядре.
    g_ntargets = 0;
    g_bmask = 0;
    size_t free_bytes = 0, total_bytes = 0;
    const cudaError_t memory_error = cudaMemGetInfo(&free_bytes, &total_bytes);
    if (memory_error != cudaSuccess) {
        snprintf(why, why_len, "cudaMemGetInfo(targets): %s", cudaGetErrorString(memory_error)); return 0;
    }
    if (target_bytes > free_bytes || target_bytes > SIZE_MAX - g_vram) {
        snprintf(why, why_len, "CUDA: targets require %zu MiB, free %zu MiB", target_bytes >> 20, free_bytes >> 20); return 0;
    }

    if (bloom) {
        // ФИЛЬТР ВМЕСТО ЦЕЛЕЙ.
        //
        // Сами цели на карту не идут вовсе: фильтр отвечает "может быть", а
        // сверяет хост, у которого база и так есть. Это не только быстрее --
        // это единственный способ работать с большой базой: миллиард целей по
        // 32 байта -- 32 ГБ, чего нет ни на одной карте, а фильтр на тот же
        // миллиард занимает около 3 ГБ.
        const size_t bytes = target_bytes;
        if (cudaMalloc(&d_bloom, bytes) != cudaSuccess) {
            snprintf(why, why_len, "не хватило видеопамяти под фильтр (%zu МБ)",
                     bytes >> 20);
            return 0;
        }
        if (cudaMemcpy(d_bloom, bloom, bytes, cudaMemcpyHostToDevice) != cudaSuccess) {
            cudaFree(d_bloom); d_bloom = nullptr;
            snprintf(why, why_len, "не удалось передать фильтр на карту");
            return 0;
        }
        g_bmask    = block_mask;
        g_ntargets = n;
        g_vram    += bytes;          // фильтр -- тоже видеопамять, и не малая
        g_target_vram = bytes;
        return 1;
    }

    // Малое число целей: перебор по старшим лимбам, как было.
    uint64_t *hi = (uint64_t *)malloc((size_t)n * sizeof(uint64_t));
    if (!hi) { snprintf(why, why_len, "не хватило памяти"); return 0; }
    for (int i = 0; i < n; i++) {
        uint64_t v = 0;
        for (int b = 0; b < 8; b++) v = (v << 8) | x32[(size_t)i * 32 + b];
        hi[i] = v;
    }
    if (cudaMalloc(&d_targ, (size_t)n * sizeof(uint64_t)) != cudaSuccess) {
        free(hi);
        snprintf(why, why_len, "не хватило видеопамяти под %d целей", n);
        return 0;
    }
    // Единственный непроверенный вызов CUDA во всём файле -- был. Отказ здесь
    // оставлял бы d_targ выделенным, но пустым: ядро сравнивало бы ключи с
    // мусором и не находило ничего, молча.
    const cudaError_t e = cudaMemcpy(d_targ, hi, (size_t)n * sizeof(uint64_t),
                                     cudaMemcpyHostToDevice);
    free(hi);
    if (e != cudaSuccess) {
        cudaFree(d_targ); d_targ = nullptr;
        snprintf(why, why_len, "не удалось передать цели на карту: %s",
                 cudaGetErrorString(e));
        return 0;
    }
    g_ntargets = n;
    g_target_vram = target_bytes;
    g_vram += target_bytes;
    return 1;
}

// Что стоил последний заход. Числа заполняет cuda_scan_run; читает их
// engine.cpp, чтобы напечатать первую строку о скорости.
static double   g_last_prep_ms = 0;   // подготовка начальных точек на процессоре
static double   g_last_kern_ms = 0;   // сам счёт на карте, включая пересылку
static uint64_t g_last_threads = 0;
static uint64_t g_last_rounds  = 0;

static uint8_t g_degen_host[CUDA_MAX_DEGEN * 32];
static int     g_ndegen = 0;

// Центры вырожденных пакетов последнего захода. Возвращает, сколько их
// было; копирует не больше max. Если их оказалось больше CUDA_MAX_DEGEN,
// возвращённое число больше max -- вызывающий обязан считать это отказом
// (в жизни такого не бывает: вырожденный пакет -- один на 2^250 ключей).
extern "C" int cuda_scan_take_degenerate(uint8_t *out32, int max) {
    // Копируем не больше того, что реально лежит в буфере: g_ndegen теперь
    // может быть больше CUDA_MAX_DEGEN (в этом весь смысл -- сообщить о
    // переполнении), а сам буфер по-прежнему на 256 записей.
    int n = g_ndegen < max ? g_ndegen : max;
    if (n > CUDA_MAX_DEGEN) n = CUDA_MAX_DEGEN;
    if (n > 0 && out32) memcpy(out32, g_degen_host, (size_t)n * 32);
    return g_ndegen;
}

extern "C" void cuda_scan_last_cost(double *prep_ms, double *kern_ms,
                                    uint64_t *threads, uint64_t *rounds) {
    if (prep_ms) *prep_ms = g_last_prep_ms;
    if (kern_ms) *kern_ms = g_last_kern_ms;
    if (threads) *threads = g_last_threads;
    if (rounds)  *rounds  = g_last_rounds;
}

extern "C" void cuda_scan_layout(int *blocks, int *threads, size_t *vram_used) {
    if (blocks)    *blocks    = g_blocks;
    if (threads)   *threads   = g_threads;
    if (vram_used) *vram_used = g_vram;
}

extern "C" uint64_t cuda_scan_run(const uint8_t first_key[32], uint64_t keys,
                                  uint8_t *out_keys, int max_out, int *found) {
    if (found) *found = 0;
    if (!g_ready || g_ntargets <= 0) return 0;
    // Public-backend guard as well as the engine's exact CPU fallback.
    // In particular, do not label points modulo n with wrapped 256-bit keys.
    if (!pkh_cuda_scalar::launch_fits(first_key, g_step, keys,
                                     g_blocks, g_threads, CUDA_HALF)) {
        g_chain_ok = false;
        g_ndegen = 0;
        return 0;
    }
#if PKH_PROFILE_NOFILTER
    static bool profile_notice = false;
    if (!profile_notice) {
        fprintf(stderr, "PROFILE ONLY: membership is disabled; this build does NOT search for targets.\n");
        profile_notice = true;
    }
#endif

    // Часы на весь вызов -- чтобы отличить время, потраченное ЗДЕСЬ, от
    // времени, которое движок тратит вокруг. Без этого «40 % мимо ядра»
    // остаётся числом без адреса.
    const auto t_call0 = std::chrono::steady_clock::now();

    // ЧИСЛО БЛОКОВ ПОДБИРАЕТСЯ ПОД ЗАКАЗАННЫЙ УЧАСТОК, А НЕ БЕРЁТСЯ ВСЕГДА
    // МАКСИМАЛЬНОЕ.
    //
    // Нить проходит CUDA_SPAN ключей за круг, кругов делается rounds -- и
    // раньше rounds просто округлялось вверх, а лишнее отбрасывал вызывающий.
    // На числах этой машины: участок 2^28 = 4 194 304 пакета, нитей 1 343 488,
    // отношение 3.12 -- значит 4 круга и 343.9 млн посчитанных ключей вместо
    // 268.4 млн нужных. ЧЕТВЕРТЬ РАБОТЫ КАРТЫ ВЫБРАСЫВАЛАСЬ.
    //
    // Исправление точное: число кругов оставляем прежним (меньше нельзя --
    // не хватит на участок), а блоков берём ровно столько, чтобы за эти круги
    // вышел заказанный объём. Для того же примера: 4 круга по 4096 блоков
    // дают 4*4096*256*64 = 268 435 456 -- ровно 2^28, без остатка. Заодно
    // нитей стало меньше, а подготовка стоит на нить, не на ключ.
    //
    // Занимать меньше блоков, чем выделено, безопасно: память под нити
    // выделена под максимум при cuda_scan_init, каждая нить адресуется
    // сквозным номером, и незанятые ячейки просто не читаются.
    const uint64_t per_thread_keys = (uint64_t)CUDA_SPAN;
    const uint64_t total_batches   = (keys + per_thread_keys - 1) / per_thread_keys;
    const size_t   nthr_max        = (size_t)g_blocks * g_threads;
    if (total_batches == 0 || nthr_max == 0) return 0;

    const uint64_t rounds = (total_batches + nthr_max - 1) / nthr_max;
    if (rounds == 0) return 0;

    // БЛОКИ ОКРУГЛЯЮТСЯ ВНИЗ, А НЕ ВВЕРХ. ЭТО НЕ МЕЛОЧЬ.
    //
    // Округление вверх давало заход, который МОГ пройти чуть больше ключей,
    // чем заказано. Лишнее ядро не считало -- оно выходит из цикла по краю
    // участка, -- но выходили не все нити и не в одном месте: часть нитей
    // обрывалась раньше остальных, и их точки оставались не там, где начнётся
    // следующий заход. Состояние на карте становилось непригодным для
    // продолжения, и подготовку приходилось считать заново каждый раз.
    //
    // Округление вниз даёт заход РОВНО кругов*нитей*SPAN ключей, не больше
    // заказанного. Тогда край участка не задевается вовсе, все нити проходят
    // все круги, и состояние на карте годится как начало следующего захода.
    //
    // Цена -- недобор меньше кругов*нитей ключей на заход, около сотой доли
    // процента; остаток забирает следующий вызов, функция возвращает ровно
    // столько, сколько прошла. Взамен исчезает подготовка на 29 мс.
    uint64_t need_blocks = total_batches / (rounds * (uint64_t)g_threads);
    if (need_blocks < 1)                  need_blocks = 1;   // меньше блока нельзя
    if (need_blocks > (uint64_t)g_blocks) need_blocks = (uint64_t)g_blocks;

    const int    use_blocks = (int)need_blocks;
    const size_t nthr       = (size_t)use_blocks * g_threads;

    // Буферы захода -- постоянные и закреплённые, выделены при инициализации.
    cpt     *h_gt     = h_gt_pin;
    cpt     *h_base   = h_base_pin;
    uint8_t *h_bkey   = h_bkey_pin;
    cpt      h_stride;
    uint8_t  h_skey[32];
    if (!h_gt || !h_base || !h_bkey) return 0;
    // Годится ли то, что уже лежит на карте. Все условия -- в комментарии к
    // g_chain_ok выше.
    const bool chain = g_chain_ok
                    && nthr == g_chain_nthr
                    && memcmp(first_key, g_chain_next, 32) == 0
                    && memcmp(g_step,    g_chain_step, 32) == 0;
    // Сверка стоит ровно столько же, сколько подготовка, поэтому она не
    // бесконечная: первых нескольких десятков продолжений достаточно, чтобы
    // увидеть расхождение, если оно есть, и не утопить экран в строках.
    static int  chain_checks_left = -1;      // -1 = ещё не смотрели переменную
    if (chain_checks_left < 0)
        chain_checks_left = (getenv("PKH_CHAIN_CHECK") != nullptr) ? 32 : 0;
    const bool chain_check = chain && chain_checks_left > 0;

    // ПОЧЕМУ ПРОДОЛЖЕНИЕ НЕ СОСТОЯЛОСЬ.
    //
    // Условий четыре, и молчаливый отказ от продолжения выглядит точно так
    // же, как его отсутствие: программа считает верно, просто медленно. Раз
    // уж проверку попросили, пусть она заодно называет причину -- иначе
    // подбирать приходится вслепую.
    static int chain_why_left = -1;
    if (chain_why_left < 0)
        chain_why_left = (getenv("PKH_CHAIN_CHECK") != nullptr) ? 8 : 0;
    if (!chain && chain_why_left > 0) {
        chain_why_left--;
        printf("  PKH_CHAIN_CHECK: продолжения нет --%s%s%s%s (нитей %zu, было %zu)\n",
               g_chain_ok ? "" : " состояние не сохранялось",
               (g_chain_ok && nthr != g_chain_nthr) ? " другое число нитей" : "",
               (g_chain_ok && memcmp(first_key, g_chain_next, 32) != 0) ? " начало не совпало" : "",
               (g_chain_ok && memcmp(g_step, g_chain_step, 32) != 0) ? " другой шаг" : "",
               nthr, g_chain_nthr);
        fflush(stdout);
    }

    const auto t_prep0 = std::chrono::steady_clock::now();

    // ОБЩАЯ ЧАСТЬ ПОДГОТОВКИ. Таблица кратных и точка-шаг зависят только от
    // шага перебора и числа нитей, поэтому считаются один раз на заход и до
    // всякого разбиения на куски. Стоят они немного: 128 сложений точек и одно
    // умножение на скаляр против миллионов сложений в начальных точках.
    const bool reuse_tables = g_tables_ok && nthr == g_tables_nthr
                           && memcmp(g_step, g_tables_step, 32) == 0;
    if (reuse_tables) {
        h_stride = g_tables_stride;
        memcpy(h_skey, g_tables_skey, 32);
    } else if (!cuda_host_tables((int)nthr, g_step, h_gt, CUDA_HALF, &h_stride, h_skey)) {
        g_tables_ok = false; g_chain_ok = false;
        return 0;
    }
    if (chain) h_stride = g_chain_stride;

    // Сверка продолжения (PKH_CHAIN_CHECK) считает начальные точки целиком --
    // ей нужно с чем сравнивать состояние карты.
    if (chain_check) {
        if (!cuda_host_bases((int)nthr, first_key, g_step, CUDA_HALF, h_base, h_bkey)) {
            g_chain_ok = false;
            return 0;
        }
    }

    // СВЕРКА СОСТОЯНИЯ, когда её попросили через PKH_CHAIN_CHECK.
    //
    // Считаем подготовку как обычно и сравниваем с тем, что оставило на карте
    // прошлое ядро. Расхождение означает, что продолжение сдвигает ключи --
    // самая тихая из возможных ошибок, поэтому проверка прямая и побайтная.
    if (chain_check) {
        cpt     *v_base = (cpt *)malloc(nthr * sizeof(cpt));
        uint8_t *v_bkey = (uint8_t *)malloc(nthr * 32);
        long bad_pt = 0, bad_key = 0;
        if (v_base && v_bkey
            && cudaMemcpy(v_base, d_base,    nthr * sizeof(cpt), cudaMemcpyDeviceToHost) == cudaSuccess
            && cudaMemcpy(v_bkey, d_basekey, nthr * 32,          cudaMemcpyDeviceToHost) == cudaSuccess) {
            for (size_t i = 0; i < nthr; i++) {
                if (memcmp(&v_base[i], &h_base[i], sizeof(cpt)) != 0) bad_pt++;
                if (memcmp(v_bkey + i*32, h_bkey + i*32, 32) != 0)    bad_key++;
            }
            chain_checks_left--;
            printf("  PKH_CHAIN_CHECK [%d]: нитей %zu, точек не сошлось %ld, ключей %ld%s\n",
                   32 - chain_checks_left, nthr, bad_pt, bad_key,
                   (bad_pt || bad_key) ? "   <-- ПРОДОЛЖЕНИЕ НЕВЕРНО" : "   -- совпало");
            if (chain_checks_left == 0)
                printf("  PKH_CHAIN_CHECK: 32 продолжения проверено, дальше не проверяю\n");
            fflush(stdout);
        } else {
            chain_checks_left--;
            printf("  PKH_CHAIN_CHECK: не удалось прочитать состояние карты\n");
        }
        free(v_base); free(v_bkey);
    }

    // КАЖДЫЙ ВЫЗОВ CUDA ПРОВЕРЯЕТСЯ, И ЛЮБАЯ ОШИБКА -- ЭТО "НОЛЬ ПРОВЕРЕНО".
    //
    // Раньше результаты cudaMalloc/cudaMemcpy не смотрели вовсе, после запуска
    // ядра не вызывался cudaGetLastError, а при ошибке синхронизации
    // печаталась строка -- и функция ВСЁ РАВНО возвращала "проверено keys".
    // Двигатель считал участок пройденным, счётчик рос, скорость на экране
    // была отличной, а ключей не проверялось ни одного. Ошибки CUDA к тому
    // же липкие: раз случившись (сторожевой таймер, нехватка ресурсов), они
    // повторяются в каждом следующем вызове -- и каждый выглядел бы успехом.
    // Возврат нуля включает у двигателя громкий отказ.
    uint8_t *d_skey = d_skey_p;
    uint8_t *d_step = d_step_p;
    bool up_ok = true;
    // Мелочь -- синхронно и всегда: 9 КБ таблицы и два ключа по 32 байта. При
    // продолжении таблица та же, но перезаписать её дешевле, чем помнить, не
    // изменился ли шаг с прошлого раза.
    if (!reuse_tables) {
    up_ok = up_ok && cudaMemcpy(d_gt,   h_gt,   CUDA_HALF * sizeof(cpt), cudaMemcpyHostToDevice) == cudaSuccess;
    up_ok = up_ok && cudaMemcpy(d_skey, h_skey, 32, cudaMemcpyHostToDevice) == cudaSuccess;
    up_ok = up_ok && cudaMemcpy(d_step, g_step, 32, cudaMemcpyHostToDevice) == cudaSuccess;
        g_tables_ok = up_ok;
        if (up_ok) {
            g_tables_nthr = nthr; g_tables_stride = h_stride;
            memcpy(g_tables_step, g_step, 32); memcpy(g_tables_skey, h_skey, 32);
        }
    }
    up_ok = up_ok && cudaMemset(d_found,  0, sizeof(*d_found)) == cudaSuccess;
    up_ok = up_ok && cudaMemset(d_ndegen, 0, sizeof(int)) == cudaSuccess;
    if (!up_ok) {
        fprintf(stderr, "  CUDA: не удалось передать данные на карту: %s\n",
                cudaGetErrorString(cudaGetLastError()));
        g_chain_ok = false;
        return 0;
    }

    // =======================================================================
    //  КОНВЕЙЕР: ПОДГОТОВКА СЛЕДУЮЩЕГО КУСКА ИДЁТ, ПОКА КАРТА СЧИТАЕТ ЭТОТ.
    //
    //  Замер показал: подготовка 34 мс, счёт 246 мс, и все 34 мс карта стоит.
    //  Убрать подготовку нельзя -- начальные точки нитей кто-то должен
    //  посчитать. Но её можно СПРЯТАТЬ.
    //
    //  Заход режется на куски по нитям. Готовим первый кусок, отправляем его
    //  считать и, не дожидаясь, готовим второй -- карта в это время занята.
    //  Время захода становится (подготовка / число кусков) + счёт вместо
    //  подготовка + счёт: ничем не прикрытой остаётся только подготовка
    //  первого куска.
    //
    //  Очередей две, а не по одной на кусок: больше двух ничего не добавляет
    //  (передача и счёт и так перекрываются), а очередей на карте не бесконечно.
    //
    //  При продолжении подготовки нет вовсе -- всё нужное уже лежит на карте,
    //  и заход идёт одним ядром, как раньше.
    // =======================================================================
    cudaError_t err = cudaSuccess;
    bool synced = false;
    auto t_first_launch = std::chrono::steady_clock::now();

    if (chain) {
        kern_scan<<<use_blocks, g_threads>>>(d_gt, d_work, d_base, d_basekey,
                                       h_stride, d_skey, 0, rounds, (uint64_t)nthr, keys,
                                       d_targ, g_ntargets, d_bloom, g_bmask, d_step,
                                       d_out, g_maxout, d_found,
                                       d_degen, CUDA_MAX_DEGEN, d_ndegen);
        err = cudaGetLastError();
        t_first_launch = std::chrono::steady_clock::now();
    } else {
        // СКОЛЬКО КУСКОВ -- ЗАМЕР, А НЕ РАССУЖДЕНИЕ.
        //
        // На этой карте (RTX 5090 Laptop, HALF=512, maxrregcount=128):
        //     1 кусок  -- 5 153 960 755 ключей/с   (конвейер выключен)
        //     4 куска  -- 5 325 961 833            +3.3 %
        //     8 кусков -- 5 350 813 423            +3.8 %
        //
        // Кривая гнётся: вчетверо мельче дало 3.3 %, ввосьмеро -- 3.8 %.
        // Дальше дробить бессмысленно: прячется всё меньший остаток
        // подготовки, а сетка каждого куска мельчает.
        int nch = 8;
        if (const char* e = getenv("PKH_CHUNKS")) {
            const int v = atoi(e);
            if (v >= 1 && v <= 16) nch = v;
        }
        if (nch > use_blocks) nch = use_blocks;
        if (nch < 1) nch = 1;

        cudaStream_t st[2] = { nullptr, nullptr };
        const bool st_ok = (cudaStreamCreate(&st[0]) == cudaSuccess)
                        && (cudaStreamCreate(&st[1]) == cudaSuccess);
        if (!st_ok) nch = 1;      // без очередей просто нечего совмещать

        int done_blocks = 0;
        for (int c = 0; c < nch; c++) {
            const int    blk = (c == nch - 1) ? (use_blocks - done_blocks)
                                              : (use_blocks / nch);
            const size_t lo  = (size_t)done_blocks * (size_t)g_threads;
            const int    n_c = blk * g_threads;
            if (blk <= 0) break;

            // Ключ, с которого начинается ЭТОТ отрезок нитей: каждая нить
            // проходит SPAN ключей, значит отрезок начинается на lo*SPAN
            // ключей дальше начала захода.
            uint8_t fk[32];
            key_add_mul(fk, first_key, g_step, (uint64_t)lo * (uint64_t)CUDA_SPAN);

            if (!cuda_host_bases(n_c, fk, g_step, CUDA_HALF,
                                 h_base + lo, h_bkey + lo * 32)) {
                err = cudaErrorInvalidValue; break;
            }

            cudaStream_t sc = st_ok ? st[c & 1] : (cudaStream_t)0;
            if (cudaMemcpyAsync(d_base + lo, h_base + lo,
                                (size_t)n_c * sizeof(cpt),
                                cudaMemcpyHostToDevice, sc) != cudaSuccess) {
                err = cudaGetLastError(); break;
            }
            if (cudaMemcpyAsync(d_basekey + lo * 32, h_bkey + lo * 32,
                                (size_t)n_c * 32,
                                cudaMemcpyHostToDevice, sc) != cudaSuccess) {
                err = cudaGetLastError(); break;
            }

            kern_scan<<<blk, g_threads, 0, sc>>>(d_gt, d_work, d_base, d_basekey,
                                       h_stride, d_skey, (int)lo,
                                       rounds, (uint64_t)nthr, keys,
                                       d_targ, g_ntargets, d_bloom, g_bmask, d_step,
                                       d_out, g_maxout, d_found,
                                       d_degen, CUDA_MAX_DEGEN, d_ndegen);
            err = cudaGetLastError();
            if (err != cudaSuccess) break;

            if (c == 0) t_first_launch = std::chrono::steady_clock::now();
            done_blocks += blk;
        }

        if (err == cudaSuccess && done_blocks != use_blocks) err = cudaErrorInvalidValue;

        // ЖДЁМ ДО КОНЦА ДАЖЕ ПРИ ОШИБКЕ, и только потом рушим очереди.
        //
        // Если один из кусков не запустился, предыдущие уже считают и пишут в
        // те же буферы. Уйти отсюда, не дождавшись их, значит отдать буферы
        // следующему вызову, пока в них ещё пишет карта. Ошибка от синхронизации
        // при этом не должна затирать ПЕРВУЮ причину отказа -- она и есть та,
        // о которой стоит рассказать.
        const cudaError_t sync_err = cudaDeviceSynchronize();
        if (err == cudaSuccess) err = sync_err;
        if (st[0]) cudaStreamDestroy(st[0]);
        if (st[1]) cudaStreamDestroy(st[1]);
        synced = true;
    }

    // Ошибка САМОГО ЗАПУСКА (негодная конфигурация, нехватка ресурсов) через
    // cudaDeviceSynchronize не видна -- только через cudaGetLastError.
    if (!synced && err == cudaSuccess) err = cudaDeviceSynchronize();
    const auto t_kern1 = std::chrono::steady_clock::now();
    const auto t_prep1 = t_first_launch;

    // РАЗДЕЛЬНЫЙ ЗАМЕР: ПОДГОТОВКА ОТДЕЛЬНО, СЧЁТ ОТДЕЛЬНО.
    //
    // Без него "заход занял 810 мс" не говорит ничего: непонятно, ждала ли
    // карта процессор или считала сама. Ровно на этом вопросе здесь уже один
    // раз потеряли время -- скорость на экране была скоростью подготовки, а
    // карту винили зря. Два числа стоят пары строк и снимают догадки.
    g_last_prep_ms = std::chrono::duration<double, std::milli>(t_prep1 - t_prep0).count();
    g_last_kern_ms = std::chrono::duration<double, std::milli>(t_kern1 - t_prep1).count();
    g_last_threads = (uint64_t)nthr;
    g_last_rounds  = rounds;

    int nf = 0;
    unsigned long long raw_found = 0;
    bool got = (err == cudaSuccess);
    if (got) {
        got = cudaMemcpy(&raw_found, d_found, sizeof(raw_found), cudaMemcpyDeviceToHost) == cudaSuccess;
        nf = cuda_candidate_count_clamped(raw_found, g_maxout, max_out);
        if (got && nf > 0)
            got = cudaMemcpy(out_keys, d_out, (size_t)nf * 32, cudaMemcpyDeviceToHost) == cudaSuccess;
        // Вырожденные центры -- на хост, в свой буфер; читает их
        // cuda_scan_take_degenerate.
        int nd = 0;
        if (got) got = cudaMemcpy(&nd, d_ndegen, sizeof(int), cudaMemcpyDeviceToHost) == cudaSuccess;
        // ПОЛНОЕ ЧИСЛО СОХРАНЯЕТСЯ, ОБРЕЗАЕТСЯ ТОЛЬКО КОПИРОВАНИЕ.
        //
        // Счётчик обрезался ДО сохранения, и cuda_scan_take_degenerate уже
        // никогда не возвращал больше 256. А cuda_scan.h обещает обратное и
        // на этом обещании построена проверка в engine.cpp ("if (nd > 256)"):
        // она была недостижима, и предупреждение о переполнении не
        // напечаталось бы ни разу. Хост досчитывал первые 256 пакетов и молча
        // пропускал остальные -- по CUDA_SPAN ключей каждый. Дыра в покрытии
        // ровно того сорта, ради устранения которого весь этот механизм и
        // написан.
        if (nd < 0) nd = 0;
        g_ndegen = got ? nd : 0;
        const int ncopy = nd < CUDA_MAX_DEGEN ? nd : CUDA_MAX_DEGEN;
        if (got && ncopy > 0)
            got = cudaMemcpy(g_degen_host, d_degen, (size_t)ncopy * 32, cudaMemcpyDeviceToHost) == cudaSuccess;
        if (!got) err = cudaGetLastError();
    }
    if (!got) {
        g_chain_ok = false;      // что осталось на карте после отказа -- неизвестно
        fprintf(stderr, "  CUDA: ядро не отработало: %s\n", cudaGetErrorString(err));
        if (found) *found = 0;
        return 0;                       // НЕ "проверено keys": ничего не проверено
    }

    if (found) *found = nf;

    // РОВНО СТОЛЬКО, СКОЛЬКО ПРОСИЛИ.
    //
    // Здесь возвращалось rounds*nthr*SPAN -- всё, что карта МОГЛА пройти. При
    // участке, не кратном пакету, это больше заказанного, и вызывающему
    // приходилось обрезать число самому. Теперь ядро само не выходит за край,
    // значит и число проверенных ключей известно точно.
    const uint64_t could = rounds * nthr * per_thread_keys;

    // ЗАПОМИНАЕМ СОСТОЯНИЕ -- только когда заход прошёл ровно столько, сколько
    // мог. При обрезке по краю участка часть нитей вышла из цикла досрочно, и
    // их точки остались не там, где начинается следующий заход.
    if (could <= keys) {
        g_chain_ok   = true;
        g_chain_nthr = nthr;
        g_chain_stride = h_stride;
        memcpy(g_chain_step, g_step, 32);
        // Следующий ключ: first_key + could*step -- именно could, потому что
        // ровно столько ключей заход и прошёл, и ровно на столько сдвинулись
        // точки нитей.
        {
            uint64_t mul = could, carry = 0;
            uint8_t nxt[32];
            memcpy(nxt, first_key, 32);
            for (int j = 31; j >= 0; j--) {
                const uint64_t add = (uint64_t)g_step[j] * mul + carry;
                const uint64_t sm  = (uint64_t)nxt[j] + (add & 0xFF);
                nxt[j] = (uint8_t)sm;
                carry  = (add >> 8) + (sm >> 8);
            }
            memcpy(g_chain_next, nxt, 32);
        }
    } else {
        g_chain_ok = false;
    }

    // РАЗБОР ЗАХОДА ПО ЧАСТЯМ, когда попросили PKH_CHAIN_CHECK.
    //
    // Три числа: подготовка, ядро вместе с передачей, и весь вызов целиком.
    // Разница между третьим и суммой первых двух -- это то, что тратится в
    // самой этой функции помимо счёта: выделения, чтение результатов.
    // А разница между этим числом и временем, которое видит движок, -- то,
    // что тратится уже вне карты.
    {
        static int cost_left = -1;
        if (cost_left < 0)
            cost_left = (getenv("PKH_CHAIN_CHECK") != nullptr) ? 8 : 0;
        if (cost_left > 0) {
            cost_left--;
            const double all_ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t_call0).count();
            printf("  PKH_COST: ключей %llu, нитей %zu, кругов %llu | подготовка %.1f мс,"
                   " ядро %.1f мс, вызов %.1f мс | продолжение %s\n",
                   (unsigned long long)could, nthr, (unsigned long long)rounds,
                   g_last_prep_ms, g_last_kern_ms, all_ms, chain ? "да" : "нет");
            fflush(stdout);
        }
    }

    return (could < keys) ? could : keys;
}
#include "packed_probe_impl.cuh"
#include "packed_scan_impl.cuh"
