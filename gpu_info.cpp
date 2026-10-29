// ============================================================================
//  gpu_info.cpp — опрос видеокарты и расчёт раскладки запуска.
//
//  Файл собирается ВСЕГДА, в том числе там, где CUDA нет вовсе: тогда работает
//  разбор nvidia-smi, а если нет и его — программа честно говорит, что карты
//  не видит. Это нужно, чтобы меню и советник можно было посмотреть и
//  проверить на любой машине, а не только на той, где стоит видеокарта.
//
//  Когда файл собран с -DPKH_HAVE_CUDA, добавляется опрос через CUDA Runtime
//  API — он точнее и не зависит от того, что печатает утилита.
// ============================================================================
#include "gpu_info.h"
#include "platform.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <algorithm>
#include <cctype>

#ifdef PKH_HAVE_CUDA
  #include <cuda_runtime.h>
#endif



// ---------------------------------------------------------------------------
//  ЯДЕР В МУЛЬТИПРОЦЕССОРЕ
//
//  Свойство архитектуры, а не модели. Все карты с одной вычислительной
//  способностью устроены здесь одинаково, поэтому таблица короткая и стареет
//  медленно -- новая запись нужна раз в поколение, а не на каждую модель.
//
//  Неизвестное будущее значение не выдумывается: берётся 128 как самое
//  распространённое у свежих поколений, и вызывающий получает known = false,
//  чтобы сказать об этом человеку. Молча подставить число и выдать основанный
//  на нём совет было бы хуже, чем признаться.
// ---------------------------------------------------------------------------
int gpu_cores_per_sm(int major, int minor, bool *known) {
    struct Row { int maj, min, cores; };
    static const Row t[] = {
        { 3, 0, 192 }, { 3, 2, 192 }, { 3, 5, 192 }, { 3, 7, 192 },   // Kepler
        { 5, 0, 128 }, { 5, 2, 128 }, { 5, 3, 128 },                   // Maxwell
        { 6, 0,  64 }, { 6, 1, 128 }, { 6, 2, 128 },                   // Pascal
        { 7, 0,  64 }, { 7, 2,  64 },                                  // Volta
        { 7, 5,  64 },                                                 // Turing
        { 8, 0,  64 },                                                 // Ampere GA100
        { 8, 6, 128 }, { 8, 7, 128 }, { 8, 9, 128 },                   // Ampere/Ada
        { 9, 0, 128 },                                                 // Hopper
        {10, 0, 128 }, {10, 1, 128 }, {12, 0, 128 },                   // Blackwell
    };
    for (const Row &r : t)
        if (r.maj == major && r.min == minor) { if (known) *known = true; return r.cores; }
    if (known) *known = false;
    return 128;
}

// --------------------------- разбор nvidia-smi ------------------------------
//
// Запасной путь: работает без CUDA SDK, достаточно драйвера. Просим утилиту
// выдать поля через запятую -- так разбор не зависит от языка и оформления
// её обычного вывода.
// Запуск утилиты и «куда девать ошибки» -- через слой систем: у cmd.exe нет
// ни popen, ни /dev/null, и раньше здесь стояла заглушка, из-за которой опрос
// видеокарты не работал бы ровно на Windows.
static std::string run_capture(const std::string &cmd) { return plat_run_capture(cmd); }

static std::string trim_sp(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// «123 MiB» -> 123, «N/A» -> 0. Единицы не разбираем: у nvidia-smi для
// запрошенных полей они постоянны, а угадывать по суффиксу значило бы
// молча ошибиться там, где формат однажды поменяется.
static long num_or_zero(const std::string &s) {
    std::string t = trim_sp(s);
    if (t.empty() || t[0] == '[' || t.rfind("N/A", 0) == 0) return 0;
    return strtol(t.c_str(), nullptr, 10);
}

static std::vector<std::string> split_csv(const std::string &line) {
    std::vector<std::string> v;
    std::string cur;
    for (char c : line) {
        if (c == ',') { v.push_back(trim_sp(cur)); cur.clear(); }
        else cur += c;
    }
    v.push_back(trim_sp(cur));
    return v;
}

// Открыто для теста: разбор отделён от запуска утилиты, поэтому его можно
// проверить на заранее записанном выводе, не имея видеокарты.
bool gpu_parse_smi(const std::string &csv, std::vector<GpuInfo> &out);

// "MX" -- это семейство (MX150, MX550), а не две буквы подряд. Проверка
// подстрокой объявляла ноутбучной любую карту, в имени которой эти буквы
// встретились внутри слова, и молча срезала мощность до 70%.
static bool is_mx_model(const std::string &low) {
    for (size_t p = low.find("mx"); p != std::string::npos; p = low.find("mx", p + 1)) {
        const bool left_ok  = (p == 0) || !isalnum((unsigned char)low[p - 1]);
        const size_t after  = p + 2;
        // За "mx" должны идти цифры модели: "mx150", "mx 550".
        size_t q = after;
        while (q < low.size() && low[q] == ' ') q++;
        const bool right_ok = q < low.size() && isdigit((unsigned char)low[q]);
        if (left_ok && right_ok) return true;
    }
    return false;
}

bool gpu_parse_smi(const std::string &csv, std::vector<GpuInfo> &out) {
    std::istringstream is(csv);
    std::string line;
    bool any = false;
    while (std::getline(is, line)) {
        line = trim_sp(line);
        if (line.empty()) continue;
        std::vector<std::string> f = split_csv(line);
        if (f.size() < 12) continue;

        GpuInfo g;
        g.source        = GPU_SRC_SMI;
        g.index         = (int)num_or_zero(f[0]);
        g.name          = f[1];
        g.driver        = f[2];
        g.mem_total     = (size_t)num_or_zero(f[3]) * 1024ull * 1024ull;
        g.mem_free      = (size_t)num_or_zero(f[4]) * 1024ull * 1024ull;
        g.clock_mhz     = (int)num_or_zero(f[5]);
        g.temp_now      = (int)num_or_zero(f[6]);
        g.power_now_w   = (int)num_or_zero(f[7]);
        g.power_limit_w = (int)num_or_zero(f[8]);
        g.power_max_w   = (int)num_or_zero(f[9]);
        g.cc_major      = (int)num_or_zero(f[10]);
        g.cc_minor      = (int)num_or_zero(f[11]);
        if (g.cc_major) g.cores_per_sm = gpu_cores_per_sm(g.cc_major, g.cc_minor);

        // Ноутбучный вариант распознаётся по имени -- другого признака у
        // nvidia-smi нет. Это единственное место, где мы смотрим на название,
        // и ошибка тут стоит недорого: она лишь сделает совет осторожнее.
        std::string low = g.name;
        std::transform(low.begin(), low.end(), low.begin(), ::tolower);
        g.is_mobile = low.find("laptop") != std::string::npos ||
                      low.find("mobile") != std::string::npos ||
                      low.find(" max-q") != std::string::npos ||
                      // "mx" отдельным словом, а не подстрокой: иначе любое
                      // имя, где эти две буквы встретились подряд, объявлялось
                      // ноутбучным и мощность резалась до 70% ни за что.
                      is_mx_model(low);
        out.push_back(g);
        any = true;
    }
    return any;
}

bool gpu_detect(std::vector<GpuInfo> &out) {
    out.clear();

    // ПОДСТАВНЫЕ ДАННЫЕ ДЛЯ ПРОВЕРКИ.
    //
    // Нужны по простой причине: советник пишется там, где видеокарты нет, и
    // увидеть его вывод иначе невозможно -- а печать с выравниванием и
    // пересчётом мегабайт ошибается ровно так же тихо, как всё остальное.
    //
    // Переменная окружения, а не настройка в config.ini: это не режим работы,
    // а способ посмотреть. Строка -- ровно та, что выдаёт nvidia-smi.
    //
    //   PKH_GPU_FAKE="0, NVIDIA GeForce RTX 4090, 550.54, 24564, 23800, 2520, 41, 35, 450, 600, 8, 9"
    //
    // Когда подстановка сработала, об этом печатается предупреждение: цифры,
    // выданные за настоящие, хуже отсутствия цифр.
    if (const char *fake = getenv("PKH_GPU_FAKE")) {
        if (*fake && gpu_parse_smi(std::string(fake) + "\n", out)) {
            std::cout << "  ВНИМАНИЕ: данные видеокарты взяты из PKH_GPU_FAKE, "
                         "это не настоящая карта.\n";
            return true;
        }
    }

#ifdef PKH_HAVE_CUDA
    // ГЛАВНЫЙ ПУТЬ: спрашиваем у самой карты.
    int n = 0;
    if (cudaGetDeviceCount(&n) == cudaSuccess && n > 0) {
        for (int i = 0; i < n; i++) {
            cudaDeviceProp p{};
            if (cudaGetDeviceProperties(&p, i) != cudaSuccess) continue;
            GpuInfo g;
            g.source       = GPU_SRC_CUDA;
            g.index        = i;
            g.name         = p.name;
            g.cc_major     = p.major;
            g.cc_minor     = p.minor;
            g.sm_count     = p.multiProcessorCount;
            g.cores_per_sm = gpu_cores_per_sm(p.major, p.minor);
            // Частота: НЕ через p.clockRate.
            //
            // Это поле объявляли устаревшим несколько выпусков подряд, а в
            // CUDA 13 убрали из cudaDeviceProp совсем -- сборка падает на
            // C2039 "clockRate не является членом cudaDeviceProp".
            // cudaDeviceGetAttribute -- та замена, на которую NVIDIA всё это
            // время и указывала. Ошибку опроса не считаем поводом бросить
            // карту: частота нужна советнику только для прикидки, и ноль он
            // разбирает сам.
            {
                int khz = 0;
                if (cudaDeviceGetAttribute(&khz, cudaDevAttrClockRate, i) == cudaSuccess)
                    g.clock_mhz = khz / 1000;
            }
            g.mem_total    = p.totalGlobalMem;
            g.is_display   = (p.integrated != 0);
            {
                size_t fr = 0, tt = 0;
                if (cudaSetDevice(i) == cudaSuccess &&
                    cudaMemGetInfo(&fr, &tt) == cudaSuccess) g.mem_free = fr;
            }
            std::string low = g.name;
            std::transform(low.begin(), low.end(), low.begin(), ::tolower);
            g.is_mobile = low.find("laptop") != std::string::npos ||
                          low.find("mobile") != std::string::npos ||
                          low.find("max-q")  != std::string::npos;
            out.push_back(g);
        }
    }
    // Мощность и тепловые пределы CUDA API не отдаёт -- дочитываем из smi.
    if (!out.empty()) {
        std::vector<GpuInfo> smi;
        std::string csv = run_capture(
            "nvidia-smi --query-gpu=index,name,driver_version,memory.total,memory.free,"
            "clocks.max.sm,temperature.gpu,power.draw,power.limit,power.max_limit,"
            "compute_cap,compute_cap --format=csv,noheader,nounits "
            + std::string(plat_devnull_2()));
        if (!csv.empty() && gpu_parse_smi(csv, smi)) {
            for (GpuInfo &g : out)
                for (const GpuInfo &s : smi)
                    if (s.index == g.index) {
                        g.power_now_w   = s.power_now_w;
                        g.power_limit_w = s.power_limit_w;
                        g.power_max_w   = s.power_max_w;
                        g.temp_now      = s.temp_now;
                        g.driver        = s.driver;
                    }
        }
        return true;
    }
#endif

    // ЗАПАСНОЙ ПУТЬ: без CUDA SDK, только драйвер.
    std::string csv = run_capture(
        "nvidia-smi --query-gpu=index,name,driver_version,memory.total,memory.free,"
        "clocks.max.sm,temperature.gpu,power.draw,power.limit,power.max_limit,"
        "compute_cap,compute_cap --format=csv,noheader,nounits "
        + std::string(plat_devnull_2()));
    if (!csv.empty() && gpu_parse_smi(csv, out)) return true;

    return false;
}

// ---------------------------------------------------------------------------
//  РАСКЛАДКА ЗАПУСКА
//
//  Всё считается из свойств карты, а не из её названия.
//
//  Нитей в блоке: 256. Не «побольше» и не «поменьше» -- у этой задачи на
//  ключ приходится несколько сотен байт состояния (координаты, разности,
//  накопитель пакетного обращения), и при 512 нитях на блок регистров уже не
//  хватает, а занятость падает. 256 -- обычная золотая середина для
//  арифметики с большим состоянием.
//
//  Блоков: кратно числу мультипроцессоров, чтобы работа легла ровно. Меньше
//  мультипроцессоров занять нельзя -- часть карты будет простаивать; сильно
//  больше бессмысленно, планировщику хватает нескольких волн.
// ---------------------------------------------------------------------------
GpuPlan gpu_scan_plan(const CudaScanLaunchInfo &i, size_t target_bytes) {
    GpuPlan p;
    auto reject = [&](const char *message) {
        p.blocks = 0;
        p.keys_in_flight = 0;
        p.vram_needed = 0;
        p.warnings.push_back(message);
        return p;
    };
    if (i.sm_count <= 0 || i.warp_size <= 0 || i.threads_per_block < i.warp_size
        || i.threads_per_block % i.warp_size != 0 || i.resident_blocks_per_sm <= 0
        || i.threads_per_block > i.device_max_threads_per_block
        || i.threads_per_block > i.kernel_max_threads_per_block
        || i.device_max_threads_per_sm / i.threads_per_block < i.resident_blocks_per_sm
        || i.max_grid_x <= 0)
        return reject("CUDA: неизвестные или несовместимые пределы ядра/устройства");
    if (!i.memory_free || !i.memory_total || i.memory_free > i.memory_total)
        return reject("CUDA: свободная видеопамять неизвестна или равна нулю");
    if (!i.scratch_bytes_per_thread || !i.allocation_bytes_per_thread
        || i.scratch_bytes_per_thread > i.allocation_bytes_per_thread)
        return reject("CUDA: неверный размер рабочего состояния потока");

    p.threads_per_block = i.threads_per_block;
    p.resident_blocks_per_sm = i.resident_blocks_per_sm;
    p.queued_resident_waves = 8;
    const size_t safety = (size_t)64 * 1024 * 1024;
    // One scratch array now remains in the kernel. Spending 1/8 of free
    // memory on it preserves the previous thread budget (1/4 for two arrays),
    // while accounting below reports/checks the actual single-array bytes.
    const size_t scratch_budget = i.memory_free / 8;
    if (target_bytes > i.memory_free || safety > i.memory_free - target_bytes
        || i.fixed_allocation_bytes > i.memory_free - target_bytes - safety)
        return reject("CUDA: недостаточно памяти для целей и постоянных буферов");
    const size_t variable_budget = i.memory_free - target_bytes - safety - i.fixed_allocation_bytes;
    const uint64_t tpb = (uint64_t)i.threads_per_block;
    uint64_t fit_blocks = (uint64_t)(scratch_budget / i.scratch_bytes_per_thread) / tpb;
    const uint64_t allocation_fit = (uint64_t)(variable_budget / i.allocation_bytes_per_thread) / tpb;
    if (fit_blocks > allocation_fit) fit_blocks = allocation_fit;
    // cuda_host_tables/bases and several host-side loops take signed int.
    const uint64_t int_thread_limit = (uint64_t)INT32_MAX / tpb;
    if (fit_blocks > int_thread_limit) fit_blocks = int_thread_limit;
    if (fit_blocks > (uint64_t)i.max_grid_x) fit_blocks = (uint64_t)i.max_grid_x;
    // int inputs are at most INT32_MAX; multiplying by waves last avoids
    // overflow even for deliberately invalid extreme CPU test inputs.
    const uint64_t resident = (uint64_t)i.sm_count * (uint64_t)i.resident_blocks_per_sm;
    const uint64_t wanted = resident > UINT64_MAX / 8 ? UINT64_MAX : resident * 8;
    uint64_t blocks = fit_blocks < wanted ? fit_blocks : wanted;
    if (blocks >= (uint64_t)i.sm_count) blocks -= blocks % (uint64_t)i.sm_count;
    if (!blocks) return reject("CUDA: памяти недостаточно даже для одного блока");
    p.blocks = (int)blocks;
    const size_t threads = (size_t)blocks * (size_t)i.threads_per_block;
    p.keys_in_flight = (uint64_t)threads * (uint64_t)CUDA_SPAN;
    // threads * allocation bytes is bounded by variable_budget above.
    p.vram_needed = i.fixed_allocation_bytes + threads * i.allocation_bytes_per_thread + target_bytes;
    if (blocks < wanted)
        p.notes.push_back("Число блоков ограничено рабочей памятью или пределом сетки");
    p.notes.push_back("Резидентных блоков на SM: " + std::to_string(i.resident_blocks_per_sm)
                     + "; предел очереди: " + std::to_string(p.queued_resident_waves) + " резидентных волн");
    return p;
}

GpuPlan gpu_plan(const GpuInfo &g, GpuLoad load, size_t bytes_per_key) {
    GpuPlan p;
    p.threads_per_block = 256;

    // БЕЗ ЧИСЛА МУЛЬТИПРОЦЕССОРОВ РАСКЛАДКИ НЕ БЫВАЕТ.
    //
    // Здесь стояла подстановка «нет числа -- берём 8». Для RTX 4090 это
    // давало 8 блоков вместо 512 и «0 МБ» видеопамяти. Рядом печаталось
    // предупреждение, но числа-то выглядели настоящими -- а человек читает
    // числа, а не оговорки. Выдуманная раскладка хуже её отсутствия.
    //
    // nvidia-smi числа мультипроцессоров не отдаёт вовсе, и вывести его не из
    // чего: у карт с одинаковым именем оно разное (настольная и ноутбучная),
    // а таблицы моделей мы завести не хотим -- она устареет.
    //
    // Поэтому: нет числа -- нет плана. blocks остаётся нулём, и печать об этом
    // говорит прямо.
    if (g.sm_count <= 0) {
        p.warnings.push_back("Раскладку не посчитать: nvidia-smi не сообщает число "
                             "мультипроцессоров, а выдумывать его нельзя");
        p.warnings.push_back("Соберите с CUDA (make cuda) — тогда число придёт "
                             "от самой карты и раскладка станет точной");
        return p;
    }
    const int sm = g.sm_count;

    // Блоков на мультипроцессор -- сколько волн держим в полёте.
    int waves = 4;
    if (load == LOAD_GENTLE) waves = 1;
    if (load == LOAD_MAX)    waves = 8;
    p.blocks = sm * waves;

    p.keys_in_flight = (uint64_t)p.blocks * p.threads_per_block;
    p.vram_needed    = (size_t)p.keys_in_flight * bytes_per_key;

    // ПАМЯТЬ РЕЖЕТ РАСКЛАДКУ, А НЕ НАОБОРОТ.
    //
    // Если запрошенное не влезает, уменьшаем число блоков, а не молча
    // запускаемся с надеждой: нехватка видеопамяти проявляется не отказом, а
    // падением посреди счёта через час работы.
    const size_t avail = g.mem_free ? g.mem_free : g.mem_total;
    if (avail) {
        // Оставляем запас: под базу целей, фильтр и то, что уже занял монитор.
        const size_t reserve = g.is_display ? (size_t)512 * 1024 * 1024
                                            : (size_t)256 * 1024 * 1024;
        const size_t budget = avail > reserve ? avail - reserve : 0;
        if (p.vram_needed > budget && bytes_per_key) {
            const uint64_t fit = budget / bytes_per_key;
            int nb = (int)(fit / (uint64_t)p.threads_per_block);
            if (nb < 1) nb = 1;
            if (nb < p.blocks) {
                p.blocks = nb;
                p.keys_in_flight = (uint64_t)p.blocks * p.threads_per_block;
                p.vram_needed    = (size_t)p.keys_in_flight * bytes_per_key;
                p.warnings.push_back("Раскладка урезана по видеопамяти: "
                                     "запрошенное не помещалось");
            }
        }
    }

    // МОЩНОСТЬ. Ограничение мощности -- единственный настоящий рычаг: оно
    // снижает и нагрев, и износ, и шум, стоя всего нескольких процентов
    // скорости на этой задаче (она упирается в арифметику, а не в память).
    switch (load) {
        case LOAD_GENTLE: p.power_pct = 60;  break;
        case LOAD_NORMAL: p.power_pct = 80;  break;
        case LOAD_MAX:    p.power_pct = 100; break;
    }
    if (g.is_mobile && p.power_pct > 70) {
        p.power_pct = 70;
        p.notes.push_back("Карта ноутбучная: предел мощности снижен — "
                          "у ноутбуков теплоотвод рассчитан на короткие всплески, "
                          "а не на сутки счёта");
    }

    p.notes.push_back("Нитей в блоке 256: на ключ приходится сотни байт состояния, "
                      "при 512 не хватает регистров и занятость падает");
    p.notes.push_back("Блоков " + std::to_string(p.blocks) + " = " +
                      std::to_string(sm) + " мультипроцессоров x " +
                      std::to_string(waves) + " волн");
    return p;
}

// ------------------------------- печать -------------------------------------
// Мелкие величины в мегабайтах округляются в «0 МБ» -- число, которое
// сообщает неправду о том, что памяти не нужно вовсе.
static std::string fmt_mb(size_t b) {
    char t[64];
    if (b >= (size_t)1024 * 1024 * 1024)
        snprintf(t, sizeof t, "%.1f ГБ", (double)b / (1024.0 * 1024 * 1024));
    else if (b >= (size_t)1024 * 1024)
        snprintf(t, sizeof t, "%.0f МБ", (double)b / (1024.0 * 1024));
    else
        snprintf(t, sizeof t, "%.0f КБ", (double)b / 1024.0);
    return t;
}

void gpu_print(const GpuInfo &g) {
    std::cout << "    Видеокарта : " << g.name;
    if (g.is_mobile) std::cout << "  (ноутбучная)";
    std::cout << "\n";

    if (g.cc_major) {
        bool known = false;
        gpu_cores_per_sm(g.cc_major, g.cc_minor, &known);
        std::cout << "    Способность: " << g.cc_major << "." << g.cc_minor;
        if (!known)
            std::cout << "  (этому коду ещё не знакома — ядер на мультипроцессор"
                         " взято 128 по образцу свежих поколений)";
        std::cout << "\n";
    }
    if (g.sm_count > 0) {
        std::cout << "    Ядра CUDA  : " << g.cuda_cores()
                  << "  (" << g.sm_count << " мультипроцессоров x "
                  << g.cores_per_sm << ")\n";
    }
    if (g.mem_total) {
        std::cout << "    Память     : " << fmt_mb(g.mem_total);
        if (g.mem_free) std::cout << ", свободно " << fmt_mb(g.mem_free);
        std::cout << "\n";
    }
    if (g.clock_mhz) std::cout << "    Частота    : " << g.clock_mhz << " МГц\n";
    if (g.power_limit_w) {
        std::cout << "    Мощность   : предел " << g.power_limit_w << " Вт";
        if (g.power_max_w > g.power_limit_w)
            std::cout << " (можно поднять до " << g.power_max_w << ")";
        if (g.power_now_w) std::cout << ", сейчас " << g.power_now_w << " Вт";
        std::cout << "\n";
    }
    if (g.temp_now) std::cout << "    Температура: " << g.temp_now << " C\n";
    if (!g.driver.empty()) std::cout << "    Драйвер    : " << g.driver << "\n";

    std::cout << "    Сведения   : "
              << (g.source == GPU_SRC_CUDA ? "CUDA API (точные)"
                 : g.source == GPU_SRC_SMI ? "nvidia-smi (без числа мультипроцессоров)"
                                           : "нет") << "\n";
}

void gpu_print_plan(const GpuInfo &g, const GpuPlan &p, GpuLoad load) {
    const char *lname = (load == LOAD_GENTLE) ? "бережно"
                      : (load == LOAD_MAX)    ? "на полную" : "обычно";
    if (p.blocks <= 0) {
        std::cout << "\n  --- Как запускать (" << lname << ") ---\n"
                  << "    Посчитать нельзя.\n";
        for (const std::string &s : p.warnings) std::cout << "      ! " << s << "\n";
        return;
    }
    std::cout << "\n  --- Как запускать (" << lname << ") ---\n"
              << "    Блоков          : " << p.blocks << "\n"
              << "    Нитей в блоке   : " << p.threads_per_block << "\n"
              << "    Ключей в полёте : " << p.keys_in_flight << "\n"
              << "    Видеопамяти     : " << fmt_mb(p.vram_needed) << "\n"
              << "    Предел мощности : " << p.power_pct << "% от штатного";
    if (g.power_limit_w)
        std::cout << "  (" << (g.power_limit_w * p.power_pct / 100) << " Вт)";
    std::cout << "\n";

    for (const std::string &s : p.notes)    std::cout << "      - " << s << "\n";
    for (const std::string &s : p.warnings) std::cout << "      ! " << s << "\n";
}
