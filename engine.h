// ============================================================================
//  engine.h — горячий цикл.
// ============================================================================
#pragma once
#include "common.h"

// Пакет собственной математики. Для libsecp256k1 свои размеры — см. scan_secp.h.
extern const int HALF;
extern const int SPAN;   // 2*HALF: ровно столько ключей за пакет,
                         // покрывается [c-HALF, c+HALF-1]. Совпадает с
                         // SECP_SPAN -- иначе два backend'а сканируют разное.
extern Point GT[];        // GT[i] = (i+1)*G
extern Point GSTEP;       // SPAN*G

// Шаг перебора ключей. По умолчанию 1 -- подряд, как было всегда.
// HEX-маска задаёт 16^N. Ставить до запуска потоков: таблицы общие.
extern u256 g_step;
extern u256 g_step_half;      // HALF*S
extern u256 g_step_span;      // SPAN*S
void set_scan_step(const u256 s);

void build_table();
void scan_chunk(const u256 k0, uint64_t count);

#ifdef PKH_HAVE_CUDA
// Счёт на видеокарте. Включается на запуске: если карта нашлась и
// инициализировалась, scan_chunk сам пойдёт по ней; иначе -- по процессору,
// и программа об этом скажет.
//
// Карта возвращает КАНДИДАТОВ, подтверждение делает хост -- см. пояснение
// в engine.cpp. Поэтому процессорная часть нужна и при работающей карте.
#include "gpu_info.h"
extern bool     g_gpu_active;
extern GpuInfo  g_gpu_info;
extern GpuPlan  g_gpu_plan;
extern int      g_gpu_blocks, g_gpu_threads;
extern int      g_gpu_count;      // сколько карт NVIDIA нашлось всего
extern std::atomic<int> g_gpu_power_pct;  // доля времени под счётом, % (100 = без пауз)
extern size_t   g_gpu_vram;
void gpu_backend_print();       // справка о карте для раздела ОБОРУДОВАНИЕ
bool gpu_backend_start(std::string &why);
void gpu_backend_push_step();   // шаг известен только после меню
void gpu_backend_stop();
// Ключи и чистое время ядра, накопленные с прошлого вызова (обнуляет).
void gpu_interval_rate(uint64_t *keys, double *seconds);
// Текущие частота (МГц), температура и мощность (Вт) -- через nvidia-smi.
// Нули, если недоступно. Медленно (десятки мс): звать раз в интервал.
void gpu_now(int *sm_mhz, int *temp_c, int *power_w);
struct PackedTraversalPlan;
struct PackedTraversalBatch;
size_t packed_traversal_capacity();
bool scan_packed_configure(const PackedTraversalPlan &plan);
// False publishes no work and requires exact replay of the claimed IDs.
bool scan_packed_traversal(const PackedTraversalPlan &plan,const PackedTraversalBatch &batch);

// Exact starts produced by the host zone traversal. The GPU only scans the
// supplied consecutive segments; it does not select or mutate any HEX digit.
// Segment lengths are multiples of CUDA_SPAN, up to 65536 keys. Shorter tails
// use scan_chunk. A false result publishes no keys or candidates, so the caller
// must replay every supplied segment before committing its traversal state.
size_t zone_scan_capacity(uint64_t segment_keys);
bool scan_zone_configure(uint64_t segment_keys);
bool scan_zone_batch(const uint8_t *starts32, int count, uint64_t segment_keys,
                     uint8_t *sample32 = nullptr);
#endif

// Честный поштучный просмотр n ключей начиная с from: края участков и
// вырожденные пакеты. Открыт ради самопроверки.
// count=false -- не прибавлять к g_count: так пересчитываются ключи, которые
// карта уже засчитала как пройденные (вырожденный пакет).
void scan_slow_range(const u256 from, uint64_t n, bool count = true);
