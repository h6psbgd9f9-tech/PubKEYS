// ============================================================================
//  gpu_selftest.cpp — проверка определения видеокарты и советника.
//
//  ЧТО ЗДЕСЬ ПРОВЕРЯЕТСЯ И ЧТО НЕТ.
//
//  Видеокарты у сборочной машины может не быть вовсе, и обычно её нет. Но
//  проверяемого тут всё равно достаточно: разбор вывода nvidia-smi и вся
//  арифметика совета -- чистые функции от чисел. Им безразлично, откуда эти
//  числа взялись, поэтому подставляем записанные заранее и сверяем ответ.
//
//  Проверить НЕЛЬЗЯ одного: что cudaGetDeviceProperties вернёт то, что мы
//  ожидаем. Это выяснится на первой настоящей карте, и об этом сказано прямо
//  в выводе программы -- строка «Сведения: CUDA API / nvidia-smi / нет».
//
//  Числа мультипроцессоров ниже взяты для ПРОВЕРКИ, а не для работы: сама
//  программа их не знает и знать не должна, она спрашивает карту.
// ============================================================================
#include "gpu_info.h"
#include "platform.h"   // plat_console_utf8: кодировка консоли
#include "cuda_scan.h"   // CUDA_HALF: одно число с ядром

#include <iostream>
#include <string>
#include <vector>

bool gpu_parse_smi(const std::string &csv, std::vector<GpuInfo> &out);

static int fails = 0;
// Ширина в ЗНАКАХ, а не в байтах: подписи кириллические, буква занимает два
// байта. Считая байты, столбец уезжает ровно вдвое -- эта ловушка в проекте
// уже срабатывала на разметке HEX-маски.
static int u8len(const std::string &s) {
    int n = 0;
    for (unsigned char c : s) if ((c & 0xC0) != 0x80) n++;
    return n;
}
static void check(bool ok, const std::string &name, const std::string &detail) {
    std::cout << "  [" << (ok ? "OK " : "FAIL") << "] " << name;
    for (int i = u8len(name); i < 42; i++) std::cout << ' ';
    std::cout << detail << "\n";
    if (!ok) fails++;
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

    std::cout << "\n=== определение видеокарты и советник ===\n\n";

    // ---------------------------------------------------------------- разбор
    {
        // Строка ровно того вида, что выдаёт
        //   nvidia-smi --query-gpu=... --format=csv,noheader,nounits
        const std::string csv =
            "0, NVIDIA GeForce RTX 4090, 550.54.14, 24564, 23800, 2520, 41, 35.2, 450, 600, 8, 9\n"
            "1, NVIDIA GeForce RTX 3050 Laptop GPU, 550.54.14, 4096, 3900, 1740, 38, 12.0, 60, 80, 8, 6\n";
        std::vector<GpuInfo> v;
        const bool ok = gpu_parse_smi(csv, v);
        check(ok && v.size() == 2, "разбор nvidia-smi: две карты",
              "нашлось " + std::to_string(v.size()));
        if (v.size() == 2) {
            check(v[0].name == "NVIDIA GeForce RTX 4090", "имя первой", v[0].name);
            check(v[0].mem_total == 24564ull * 1024 * 1024, "память в байтах",
                  std::to_string(v[0].mem_total / (1024 * 1024)) + " МБ");
            check(v[0].power_limit_w == 450 && v[0].power_max_w == 600,
                  "предел мощности и потолок",
                  std::to_string(v[0].power_limit_w) + " / " + std::to_string(v[0].power_max_w));
            check(v[0].cc_major == 8 && v[0].cc_minor == 9, "вычислительная способность",
                  std::to_string(v[0].cc_major) + "." + std::to_string(v[0].cc_minor));
            // Ноутбучность распознаётся по имени -- единственное место, где мы
            // на имя смотрим, и ошибка тут лишь сделает совет осторожнее.
            check(!v[0].is_mobile && v[1].is_mobile, "ноутбучный вариант распознан",
                  v[1].name);
        }
    }

    // ------------------------------------------------- ядер в мультипроцессоре
    {
        bool k1 = false, k2 = false, k3 = false;
        const int a = gpu_cores_per_sm(8, 6, &k1);    // Ampere
        const int b = gpu_cores_per_sm(7, 5, &k2);    // Turing
        const int c = gpu_cores_per_sm(99, 9, &k3);   // такого нет и не будет скоро
        check(a == 128 && k1, "ядер на SM: 8.6", std::to_string(a));
        check(b == 64  && k2, "ядер на SM: 7.5", std::to_string(b));
        // Неизвестное будущее значение не выдумывается молча: known = false,
        // и программа обязана об этом сказать человеку.
        check(c == 128 && !k3, "ядер на SM: незнакомая способность",
              "взято " + std::to_string(c) + ", known=false");
    }

    // -------------------------------------------------------------- советник
    // Байт на нить. Число проверок ниже от него не зависит по существу --
    // они проверяют арифметику gpu_plan, -- но показ раскладки в конце
    // зависит, и с прежними 256 он обещал «42 МБ» на RTX 5090 там, где счёт
    // занимает больше гигабайта. Держим то же значение, что и gpu_main.cpp:
    // d_work = 2*CUDA_HALF*32, d_base = 72, d_basekey = 32.
    const size_t BPK = (size_t)2 * CUDA_HALF * 32 + 72 + 32;

    {
        // Настольная карта: блоков должно быть кратно мультипроцессорам.
        GpuInfo g; g.sm_count = 128; g.cores_per_sm = 128;
        g.mem_total = 24ull << 30; g.mem_free = 23ull << 30;
        g.power_limit_w = 450;
        GpuPlan p = gpu_plan(g, LOAD_NORMAL, BPK);
        check(p.blocks % g.sm_count == 0, "блоков кратно мультипроцессорам",
              std::to_string(p.blocks) + " при " + std::to_string(g.sm_count) + " SM");
        check(p.threads_per_block == 256, "нитей в блоке", std::to_string(p.threads_per_block));
        check(p.power_pct == 80, "мощность при обычной нагрузке",
              std::to_string(p.power_pct) + "%");
    }

    {
        // Бережный и предельный режимы обязаны различаться, иначе выбор
        // в меню был бы украшением.
        GpuInfo g; g.sm_count = 128; g.cores_per_sm = 128;
        g.mem_total = 24ull << 30; g.mem_free = 23ull << 30;
        GpuPlan a = gpu_plan(g, LOAD_GENTLE, BPK);
        GpuPlan b = gpu_plan(g, LOAD_MAX,    BPK);
        check(a.blocks < b.blocks && a.power_pct < b.power_pct,
              "бережно слабее, чем на полную",
              std::to_string(a.blocks) + "/" + std::to_string(a.power_pct) + "% против "
            + std::to_string(b.blocks) + "/" + std::to_string(b.power_pct) + "%");
    }

    {
        // ПАМЯТЬ РЕЖЕТ РАСКЛАДКУ. Нехватка видеопамяти проявляется не отказом
        // при запуске, а падением через час счёта -- поэтому раскладка обязана
        // ужаться заранее, а не надеяться.
        // Памяти должно НЕ ХВАТАТЬ по-настоящему, иначе проверять нечего.
        // Первая version этого теста брала 1 ГБ свободных при нужде в 85 МБ и
        // требовала ужатия -- то есть требовала того, чего быть не должно.
        // Числа ниже пересчитаны под настоящий BPK: раньше здесь стояло
        // «1360 блоков x 256 нитей x 256 байт = 85 МБ», и вся проверка
        // опиралась на заниженную в 32 раза величину.
        GpuInfo g; g.sm_count = 170; g.cores_per_sm = 128;
        g.mem_total = 2ull << 30; g.mem_free = 300ull << 20;
        GpuPlan p = gpu_plan(g, LOAD_MAX, BPK);
        const bool fits  = p.vram_needed <= g.mem_free;
        const bool small = p.blocks < g.sm_count * 8;      // ужалось от запрошенного
        bool warned = false;
        for (const std::string &w : p.warnings)
            if (w.find("видеопам") != std::string::npos) warned = true;
        check(fits && small && warned, "раскладка ужата по видеопамяти",
              std::to_string(p.blocks) + " блоков вместо " +
              std::to_string(g.sm_count * 8) + ", нужно " +
              std::to_string(p.vram_needed / (1024 * 1024)) + " МБ");
    }

    {
        // И обратное: когда памяти вдоволь, ужимать НЕЛЬЗЯ. Иначе прошлый
        // тест проходил бы всегда, ничего не проверяя.
        GpuInfo g; g.sm_count = 170; g.cores_per_sm = 128;
        g.mem_total = 32ull << 30; g.mem_free = 31ull << 30;
        GpuPlan p = gpu_plan(g, LOAD_MAX, BPK);
        check(p.blocks == g.sm_count * 8 && p.warnings.empty(),
              "памяти вдоволь — раскладка не тронута",
              std::to_string(p.blocks) + " блоков, предупреждений "
            + std::to_string(p.warnings.size()));
    }

    {
        // Ноутбучная карта: предел мощности обязан опуститься даже на «полную».
        // У ноутбуков теплоотвод рассчитан на всплески, а не на сутки счёта.
        GpuInfo g; g.sm_count = 16; g.cores_per_sm = 128;
        g.mem_total = 4ull << 30; g.mem_free = 3800ull << 20;
        g.is_mobile = true; g.power_limit_w = 60;
        GpuPlan p = gpu_plan(g, LOAD_MAX, BPK);
        check(p.power_pct <= 70, "ноутбук: мощность снижена",
              std::to_string(p.power_pct) + "% (было бы 100 у настольной)");
    }

    {
        // БЕЗ ЧИСЛА МУЛЬТИПРОЦЕССОРОВ РАСКЛАДКИ БЫТЬ НЕ ДОЛЖНО.
        //
        // Сперва здесь проверялось обратное: что совет всё-таки выдан, лишь с
        // оговоркой. Так и было сделано -- и на RTX 4090 без CUDA API выходило
        // «8 блоков, 0 МБ»: числа выдуманные, но с виду настоящие, а человек
        // читает числа, а не оговорки.
        //
        // Теперь раскладка не выдаётся вовсе, и проверка требует именно этого.
        GpuInfo g; g.source = GPU_SRC_SMI;
        g.mem_total = 8ull << 30; g.mem_free = 7ull << 30;
        GpuPlan p = gpu_plan(g, LOAD_NORMAL, BPK);
        bool said = false;
        for (const std::string &w : p.warnings)
            if (w.find("мультипроцессор") != std::string::npos) said = true;
        check(p.blocks == 0 && said, "без CUDA API: раскладки НЕ выдаём",
              p.blocks == 0 ? "блоков 0, оговорка есть"
                            : "ВЫДУМАЛ раскладку: " + std::to_string(p.blocks));
    }

    std::cout << "\n";
    if (fails) std::cout << "Провалено проверок: " << fails << "\n";
    else       std::cout << "Все проверки пройдены.\n";

    // ПОКАЗ РАСКЛАДКИ НА ПРИМЕРАХ.
    //
    // Не проверка, а именно показ: числа мультипроцессоров ниже взяты для
    // наглядности и программе неизвестны -- она спрашивает их у карты.
    std::cout << "\n=== как это выглядит на разных картах ===\n";
    struct Ex { const char *name; int sm; int cc_maj, cc_min; size_t gb; bool mob; int pw; };
    const Ex ex[] = {
        { "RTX 3050 Laptop", 16,  8, 6,  4, true,  60 },
        { "RTX 3060",        28,  8, 6, 12, false, 170 },
        { "RTX 4090",       128,  8, 9, 24, false, 450 },
        { "RTX 5090",       170, 12, 0, 32, false, 575 },
    };
    for (const Ex &e : ex) {
        GpuInfo g;
        g.source = GPU_SRC_CUDA;  g.name = e.name;
        g.sm_count = e.sm;  g.cc_major = e.cc_maj;  g.cc_minor = e.cc_min;
        g.cores_per_sm = gpu_cores_per_sm(e.cc_maj, e.cc_min);
        g.mem_total = e.gb << 30;  g.mem_free = (e.gb << 30) - (1ull << 30);
        g.is_mobile = e.mob;  g.power_limit_w = e.pw;
        GpuPlan p = gpu_plan(g, LOAD_NORMAL, BPK);
        char line[256];
        snprintf(line, sizeof line,
                 "  %-16s %3d SM  %5d ядер  блоков %4d  ключей %8llu  %s  мощность %3d%%",
                 e.name, e.sm, g.cuda_cores(), p.blocks,
                 (unsigned long long)p.keys_in_flight,
                 (p.vram_needed >= (1u<<20)
                    ? (std::to_string(p.vram_needed >> 20) + " МБ").c_str()
                    : (std::to_string(p.vram_needed >> 10) + " КБ").c_str()),
                 p.power_pct);
        std::cout << line << "\n";
    }

    // Заодно показываем, что видит программа на ЭТОЙ машине.
    std::cout << "\n=== что найдено здесь ===\n";
    std::vector<GpuInfo> real;
    if (gpu_detect(real)) {
        for (const GpuInfo &g : real) {
            gpu_print(g);
            gpu_print_plan(g, gpu_plan(g, LOAD_NORMAL, BPK), LOAD_NORMAL);
        }
    } else {
        std::cout << "  Видеокарты NVIDIA не найдено (ни CUDA API, ни nvidia-smi).\n"
                     "  Это ожидаемо на MacBook и на сборочной машине без карты:\n"
                     "  проверки выше от наличия карты не зависят.\n";
    }
    return fails ? 1 : 0;
}
