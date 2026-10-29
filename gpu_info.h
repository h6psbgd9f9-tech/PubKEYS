// ============================================================================
//  gpu_info.h — какая в машине видеокарта и как её лучше загрузить.
//
//  ЗДЕСЬ НЕТ СПИСКА МОДЕЛЕЙ, И ЭТО ГЛАВНОЕ РЕШЕНИЕ ЭТОГО ФАЙЛА.
//
//  Соблазн велик: завести таблицу «3050 — столько-то ядер, 4060 — столько-то,
//  5090 — столько-то» и раздавать советы по названию. Такая таблица устаревает
//  в день выхода следующей карты, а до того успевает соврать на каждом
//  ноутбучном варианте: у RTX 4060 Laptop и RTX 4060 разное всё, кроме имени.
//
//  Поэтому числа берутся у САМОЙ карты: cudaGetDeviceProperties отдаёт число
//  мультипроцессоров, объём памяти, частоты и вычислительную способность.
//  Совет считается из них. Новая карта, которой ещё нет на свете, будет
//  опрошена так же и получит осмысленный ответ без единой правки.
//
//  Единственная таблица, которая тут всё же нужна, — сколько ядер CUDA в
//  одном мультипроцессоре. Это свойство архитектуры, а не модели: у всех карт
//  с вычислительной способностью 8.6 их 128. Для неизвестного будущего
//  значения берётся ближайшее разумное и об этом честно сообщается.
// ============================================================================
#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <cstddef>
#include "cuda_scan.h"

// Откуда взяты сведения. Влияет на то, чему можно верить: CUDA API отдаёт
// всё, nvidia-smi — почти всё, отсутствие обоих означает, что советовать
// нечего и надо сказать об этом прямо, а не выдумывать.
enum GpuSource {
    GPU_SRC_NONE = 0,   // карты нет либо драйвер не отвечает
    GPU_SRC_SMI,        // разобран вывод nvidia-smi
    GPU_SRC_CUDA        // опрошен CUDA Runtime API
};

struct GpuInfo {
    int         index        = 0;
    std::string name;
    GpuSource   source       = GPU_SRC_NONE;

    int         cc_major     = 0;    // вычислительная способность, напр. 8.6
    int         cc_minor     = 0;
    int         sm_count     = 0;    // мультипроцессоров
    int         cores_per_sm = 0;    // ядер CUDA в одном мультипроцессоре
    int         clock_mhz    = 0;    // предельная частота ядра

    size_t      mem_total    = 0;    // всего видеопамяти, байт
    size_t      mem_free     = 0;    // свободно на момент опроса, байт

    int         power_now_w      = 0;
    int         power_limit_w    = 0;
    int         power_max_w      = 0;   // предел, до которого можно поднять

    int         temp_now         = 0;
    int         temp_slowdown    = 0;   // здесь карта начнёт сбрасывать частоты
    int         temp_shutdown    = 0;   // здесь выключится сама

    bool        is_mobile        = false;  // ноутбучный вариант
    bool        is_display       = false;  // на карте висит монитор

    std::string driver;
    std::string cuda_runtime;

    // Ядер CUDA всего: sm_count * cores_per_sm. Ноль, если неизвестно.
    int cuda_cores() const { return sm_count * cores_per_sm; }
};

// Насколько бережно загружать карту.
enum GpuLoad {
    LOAD_GENTLE = 0,   // тихо и холодно: ноутбук, ночь, общий компьютер
    LOAD_NORMAL,       // обычно: полная загрузка, разумный запас
    LOAD_MAX           // всё, что даёт карта
};

struct GpuPlan {
    int      blocks            = 0;
    int      threads_per_block = 0;
    uint64_t keys_in_flight    = 0;   // сколько ключей считается одновременно
    size_t   vram_needed       = 0;   // сколько это займёт видеопамяти
    int      power_pct         = 100; // рекомендуемый предел мощности, % от штатного
    int      resident_blocks_per_sm = 0;
    int      queued_resident_waves = 0;
    std::vector<std::string> notes;   // почему именно так
    std::vector<std::string> warnings;// на что обратить внимание
};

// Опрашивает все карты. Возвращает false, если не нашлось ни одной.
bool gpu_detect(std::vector<GpuInfo> &out);

// Считает раскладку запуска из свойств карты.
GpuPlan gpu_plan(const GpuInfo &g, GpuLoad load, size_t bytes_per_key);

// Pure arithmetic used by the running CUDA backend. target_bytes is an
// optional reservation for a known future target upload; existing targets
// are already excluded from info.memory_free.
GpuPlan gpu_scan_plan(const CudaScanLaunchInfo &info, size_t target_bytes = 0);

// Печать для меню.
void gpu_print(const GpuInfo &g);
void gpu_print_plan(const GpuInfo &g, const GpuPlan &p, GpuLoad load);

// Ядер в мультипроцессоре по вычислительной способности.
// known = false, если такая способность этому коду ещё не известна.
int gpu_cores_per_sm(int major, int minor, bool *known = nullptr);
