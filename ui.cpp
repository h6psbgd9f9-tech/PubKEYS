#include "hybrid_mode.h"
// ============================================================================
//  ui.cpp — оборудование, ввод, меню запуска.
// ============================================================================
#include "ui.h"
#include "engine.h"   // g_gpu_active: карта найдена и поднята
#include "targets.h"
#include "traverse.h"
#include "search.h"
#include "notify.h"
#include "interval_digits.h"
#include "digit_rotation.h"
#include "single_instance.h"
#include "shape_checkpoint_restore.h"

#include <cstdlib>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <thread>
#if defined(__APPLE__)
  #include <sys/sysctl.h>
  #include <unistd.h>
#elif defined(_WIN32)
  #include <intrin.h>
#else
  #include <unistd.h>
#endif

// ОПРЕДЕЛЕНИЕ ОБОРУДОВАНИЯ
// ========================================================
HwInfo g_hw;
static bool ui_noninteractive = false;
static bool ui_preloaded_database = false;
void ui_set_noninteractive(bool enabled) { ui_noninteractive = enabled; }
void ui_set_preloaded_database(bool enabled) { ui_preloaded_database = enabled; }

// Block duration is only an estimate; the running search reports its own rate.
static const double ZONE_SPEED_ESTIMATE = 3.0e9;

static std::string zone_human_time(double seconds) {
    char buffer[64];
    if (seconds < 1e-3)      snprintf(buffer,sizeof buffer,"%.0f мкс",  seconds*1e6);
    else if (seconds < 1.0)  snprintf(buffer,sizeof buffer,"%.0f мс",   seconds*1e3);
    else if (seconds < 90.0) snprintf(buffer,sizeof buffer,"%.1f с",    seconds);
    else if (seconds < 5400.0)      snprintf(buffer,sizeof buffer,"%.1f мин", seconds/60);
    else if (seconds < 172800.0)    snprintf(buffer,sizeof buffer,"%.1f ч",   seconds/3600);
    else if (seconds < 5259492.0)   snprintf(buffer,sizeof buffer,"%.1f сут", seconds/86400);
    else if (seconds < 63113904.0)  snprintf(buffer,sizeof buffer,"%.1f мес", seconds/2629746.0);
    else                            snprintf(buffer,sizeof buffer,"%.1f лет", seconds/31556952.0);
    return buffer;
}

static std::string zone_pad(const std::string& text, size_t width) {
    size_t glyphs = 0;
    for (unsigned char c : text) if ((c & 0xC0) != 0x80) ++glyphs;
    return glyphs >= width ? text : text + std::string(width - glyphs, ' ');
}

static void zone_size_table(int chosen) {
    printf("\n  Зонный режим меняет по одному старшему нибблу вокруг базы.\n"
           "  Младшие N нибблов перебираются блоком из 16^N ключей.\n"
           "  Каждая база проходит всю исходную окрестность, затем выбирается новая база.\n"
           "  Битовые интервалы чередуются после целой окрестности одной базы.\n"
           "  После исходных окрестностей проверяются оставшиеся участки, без повторов.\n"
           "  На границах проверяется только часть блока внутри диапазона.\n"
           "  Размер блока независим от волнового режима.\n\n");
    printf("   N   ключей в блоке         время блока*\n");
    for (int n = 3; n <= 15; ++n) {
        const double keys = std::pow(16.0, (double)n);
        char keys_text[32]; snprintf(keys_text,sizeof keys_text,"%.0f",keys);
        printf("  %2d   %-22s %s%s\n", n, keys_text,
               zone_pad(zone_human_time(keys/ZONE_SPEED_ESTIMATE),12).c_str(),
               n == chosen ? "   <- сейчас" : "");
    }
    printf("  * при 3 млрд ключей/с; на вашей карте счёт покажет точную скорость.\n");
}

static void hybrid_size_table(int top_nibble, int chosen) {
    printf("\n  Ключ делится на A | B. A целиком выбирается случайно и остаётся\n"
           "  неизменной, пока B перебирается подряд от начала до конца.\n"
           "  После этого выбирается новая A и перебор B начинается заново.\n"
           "  A равномерна среди допустимых префиксов; повторы возможны.\n\n"
           "   N (B)  ключей в блоке       время блока*  значащих HEX-разрядов A\n");
    for (int n = 3; n <= 15; ++n) {
        const int a_digits = (std::max)(0, top_nibble + 1 - n);
        const double keys = std::pow(16.0, (double)n);
        char keys_text[32]; snprintf(keys_text, sizeof keys_text, "%.0f", keys);
        printf("  %2d     %-22s %s %2d%s\n", n, keys_text,
               zone_pad(zone_human_time(keys / ZONE_SPEED_ESTIMATE), 12).c_str(),
               a_digits, n == chosen ? "   <- сейчас" : "");
    }
    printf("  * при 3 млрд ключей/с; точную скорость покажет поиск.\n"
           "  На границах проверяется только часть блока внутри интервала.\n"
           "  Число значащих разрядов A зависит от длины ключа и длины B.\n"
           "  В узком интервале B сохраняет выбранную длину, но его значения\n"
           "  ограничены интервалом. Если допустима одна A, её блок пройдёт один раз.\n");
}

static const char* zone_digit_word(int count) {
    const int tens = count % 100, units = count % 10;
    return units == 1 && tens != 11 ? "разряд"
        : units >= 2 && units <= 4 && (tens < 12 || tens > 14) ? "разряда" : "разрядов";
}

void detect_hardware() {
    HwInfo &h = g_hw;
    int lc = (int)std::thread::hardware_concurrency();
    h.cpu_logical = (lc > 0) ? lc : 1;

#if defined(__APPLE__)
    {
        char buf[256] = {}; size_t n = sizeof(buf);
        if (sysctlbyname("machdep.cpu.brand_string", buf, &n, nullptr, 0) == 0 && buf[0])
            h.cpu_name = buf;
        int v = 0; n = sizeof(v);
        if (sysctlbyname("hw.perflevel0.logicalcpu", &v, &n, nullptr, 0) == 0) h.cpu_perf = v;
        v = 0; n = sizeof(v);
        if (sysctlbyname("hw.perflevel1.logicalcpu", &v, &n, nullptr, 0) == 0) h.cpu_eff = v;
    }
#elif defined(_WIN32)
    {
        int r[4] = {0,0,0,0}; char buf[49] = {};
        __cpuid(r, (int)0x80000000);
        if ((unsigned)r[0] >= 0x80000004u) {
            for (int i = 0; i < 3; i++) { __cpuid(r, (int)(0x80000002 + i)); memcpy(buf + i*16, r, 16); }
            buf[48] = 0;
            const char *s = buf; while (*s == ' ') ++s;
            if (*s) h.cpu_name = s;
        }
    }
#else
    {
        std::ifstream f("/proc/cpuinfo");
        std::string line;
        const char *keys[] = { "model name", "Hardware", "cpu model", "Processor" };
        while (std::getline(f, line)) {
            for (const char *k : keys) {
                if (line.rfind(k, 0) != 0) continue;
                auto p = line.find(':');
                if (p == std::string::npos) continue;
                std::string v = trim(line.substr(p + 1));
                if (v.empty() || v.find_first_not_of("0123456789") == std::string::npos) continue;
                h.cpu_name = v;
                break;
            }
            if (h.cpu_name != "неизвестен") break;
        }
    }
#endif
    // На процессорах со смешанными ядрами экономичные вдвое-втрое медленнее.
    // Поток на медленном ядре не ускоряет счёт, а лишь отнимает пропускную
    // способность памяти у прочих — по умолчанию предлагаем только быстрые.
    h.rec_threads = (h.cpu_perf > 0) ? h.cpu_perf : h.cpu_logical;
    if (h.rec_threads < 1) h.rec_threads = 1;
}

void print_hw_info() {
    std::cout << "  ОБОРУДОВАНИЕ\n";
    std::cout << "    Процессор : " << g_hw.cpu_name << "\n";
    std::cout << "    Ядра      : " << g_hw.cpu_logical << " логических";
    if (g_hw.cpu_perf > 0)
        std::cout << "  (" << g_hw.cpu_perf << " производительных, "
                  << g_hw.cpu_eff << " экономичных)";
    std::cout << "\n";
#ifdef PKH_HAVE_CUDA
    // Совет о числе потоков печатается только тогда, когда он к делу: при
    // счёте на карте потоков всё равно один, и "советуем 24" рядом со
    // "считает карта" -- два противоречащих указания подряд.
    if (g_gpu_active) {
        gpu_backend_print();
    } else {
        std::cout << "    Советуем  : " << g_hw.rec_threads << " потоков\n";
        std::cout << "    Видеокарта: не найдена, счёт на процессоре\n";
    }
#else
    std::cout << "    Советуем  : " << g_hw.rec_threads << " потоков\n";
#endif
    std::cout << "\n";
}

// ========================================================
// ВСПОМОГАТЕЛЬНЫЙ ВВОД
// ========================================================
// ВОПРОС ДА/НЕТ: ENTER ТЕПЕРЬ ЧТО-ТО ЗНАЧИТ, И ЭТО НАПИСАНО.
//
// Было так: ввод числа принимал пустую строку как значение по умолчанию и
// подсказывал его в тексте, а вопрос да/нет пустую строку молча пропускал и
// задавался заново. Со стороны программа просто переспрашивала без причины,
// и понять, что она вообще ждёт, было нельзя.
//
// Теперь у каждого вопроса есть ответ по умолчанию, он подписан прямо в
// строке, и Enter его выбирает. Умолчания подобраны так, чтобы случайный
// Enter не делал ничего неприятного: продолжить -- да, а вот «продолжить БЕЗ
// уведомлений» -- нет, потому что уходить на ночь без связи человек обычно
// не собирался.
bool ask_yes_no(const std::string &prompt, bool def_yes) {
    if (ui_noninteractive) return true;
    while (true) {
        std::cout << prompt << "\n"
                  << "  1 - Да" << (def_yes ? "   (ENTER)" : "") << "\n"
                  << "  2 - Нет" << (def_yes ? "" : "  (ENTER)") << "\n> ";
        std::string line;
        if (!single_instance_read_line(std::cin, line)) {
            std::cout << "\n  Ввод закончился, беру ответ по умолчанию: "
                      << (def_yes ? "да" : "нет") << "\n";
            return def_yes;
        }
        line = trim(line);
        if (line.empty()) return def_yes;          // Enter -- это ответ, а не молчание
        if (line[0] == '1') return true;
        if (line[0] == '2') return false;
        std::cout << "Неверный ввод, повторите.\n";
    }
}

// ЧТО СДЕЛАЕТ ENTER — ПИШЕТСЯ САМО, А НЕ РУКАМИ В КАЖДОМ ВОПРОСЕ.
//
// Пустая строка означала значение по умолчанию с самого начала, но сказано
// об этом было далеко не везде: у одних вопросов в тексте вручную дописано
// «ENTER = 300», у других -- ничего, и человек жал Enter вслепую, не зная,
// что выберет программа. «Что запускаем» -- как раз такой случай.
//
// Дописывать подсказку руками в каждый вопрос -- значит рано или поздно
// где-то её забыть, а ещё разойтись с настоящим значением (так уже было:
// в тексте стояло 300, подставлялось другое). Поэтому подсказку печатает
// сама функция, из того самого числа, которое и подставит.
int ask_int(const std::string &prompt, int def_val, int min_val, int max_val) {
    while (true) {
        std::cout << prompt << "  (ENTER = " << def_val << ")\n> ";
        std::string line;
        if (!single_instance_read_line(std::cin, line)) return def_val;
        line = trim(line);
        if (line.empty()) return def_val;
        try {
            int v = std::stoi(line);
            if (v >= min_val && v <= max_val) return v;
        } catch (...) {}
        std::cout << "Неверное значение, диапазон " << min_val << ".." << max_val << ".\n";
    }
}

std::string ask_string(const std::string &prompt, const std::string &def_val) {
    std::cout << prompt << " [" << def_val << "]\n> ";
    std::string line;
    if (!single_instance_read_line(std::cin, line)) return def_val;
    line = trim(line);
    return line.empty() ? def_val : line;
}

// ========================================================
// МЕНЮ ЗАПУСКА
// ========================================================
void print_menu_header() {
    std::cout << C_BOLD << C_CYAN << std::string(60, '=') << "\n";
    std::cout << "    PubkeyHunt  —  поиск приватного ключа по PUBLIC KEY\n";
    std::cout << "  secp256k1, пакетное сложение, симметричный пакет\n";
    std::cout << "  5 режимов обхода  |  зоны и волна по разрядам  |  Signal\n";
    std::cout << std::string(60, '=') << C_RESET << "\n\n";
    print_hw_info();
}

// v << lb, где v помещается в четыре бита, а lb не больше 252.
static void u256_shifted(u256 out, uint64_t v, int lb) {
    u256_zero(out);
    if (lb < 0 || lb >= 256) return;
    int li = lb >> 6, off = lb & 63;
    out[li] = v << off;
    if (off && li + 1 < 4) out[li + 1] = v >> (64 - off);
}

// Маска из lb младших единиц.
static void u256_low_mask(u256 out, int lb) {
    u256_zero(out);
    for (int i = 0; i < 4; i++) {
        int lo = i * 64;
        if (lb <= lo) break;
        int n = lb - lo;
        out[i] = (n >= 64) ? ~0ULL : ((1ULL << n) - 1);
    }
}

// СТАРШИЙ РАЗРЯД СУЖАЕТ ДИАПАЗОН, А НЕ ПРАВИТ КЛЮЧИ.
//
// Прежде выбранное значение вставлялось в уже готовый ключ функцией
// apply_top_nibble. Для случайного режима это ещё как-то работало, а для
// детерминированного и KXE -- нет: там номер участка отображается в ключ
// взаимно однозначно, и правка ключа поверх этого отображения рушит
// биекцию. Разные номера начинают давать один и тот же участок, часть
// пространства не достаётся никому, а программа продолжает считать
// проценты так, будто идёт по всему диапазону. Именно поэтому вызова
// apply_top_nibble в ветках SEQ и KXE не было вовсе -- но настройка при
// этом показывалась в меню и молча не действовала.
//
// Правильное место для этого выбора -- границы. Фиксированный разряд на
// позиции g_top_ni означает ровно один непрерывный отрезок:
//
//     [ v << lb ,  (v << lb) | (2^lb - 1) ]
//
// где lb -- младший бит разряда. Пересекаем его с заданным диапазоном и
// дальше работаем как обычно. Все четыре режима становятся честными
// разом: последовательный и KXE полностью покрывают суженный отрезок,
// случайный распределяется по нему равномерно, зональный кладёт в него
// свои базы. Никакой правки ключей после нарезки не требуется.
static void narrow_range_by_top_nibble() {
    if (g_cfg.det_hex < 0) return;
    int lb = 252 - 4 * g_top_ni;               // младший бит старшего разряда
    if (lb < 0) lb = 0;

    u256 lo, mask, hi;
    u256_shifted(lo, (uint64_t)(g_cfg.det_hex & 0xF), lb);
    u256_low_mask(mask, lb);
    for (int i = 0; i < 4; i++) hi[i] = lo[i] | mask[i];

    u256 ns, ne;
    u256_set(ns, g_range_start);
    u256_set(ne, g_range_end);
    if (u256_cmp(lo, ns) > 0) u256_set(ns, lo);
    if (u256_cmp(hi, ne) < 0) u256_set(ne, hi);

    if (u256_cmp(ns, ne) > 0) {
        // Пустое пересечение: разряд не встречается в заданном диапазоне.
        // Молча оставить настройку нельзя -- поиск шёл бы по нулю ключей и
        // выглядел бы как мгновенно завершившийся.
        std::cout << "\n  Старший разряд " << "0123456789abcdef"[g_cfg.det_hex & 0xF]
                  << " не встречается в диапазоне [2^" << g_cfg.range_start_bit
                  << ", 2^" << g_cfg.range_end_bit << ").\n"
                     "  Ограничение по разряду снято.\n";
        g_cfg.det_hex = -1;
        return;
    }
    u256_set(g_range_start, ns);
    u256_set(g_range_end,   ne);
}

// ПОКРЫТИЕ — ОДНОЙ СТРОКОЙ.
//
// Здесь стоял разбор на десять строк: сколько участков, почему счётчик их
// не назовёт, чем лечится. Всё это правда, но читать её при каждом запуске
// незачем — решение принимается один раз, а стена текста заслоняет саму
// сводку. Оставляем главное: какого рода результат даст этот прогон и
// сколько он займёт. Ничего не запрещаем: поиск запускается в любом случае.
static std::string coverage_line() {
    if (g_cfg.mode == TRAV_HYBRID)
        return "внутри B — последовательно; A — случайная выборка, повторы возможны";
    if (g_cfg.mode == TRAV_WAVE && !g_cfg.wave_start.empty())
        return "растущий узор от заданной точки, затем остаток всего диапазона без повторов";
    if (g_cfg.mode == TRAV_WAVE)
        return "целые исходные волны по очереди битовых интервалов; весь диапазон без повторов";
    if (g_cfg.mode == TRAV_ZONE)
        return "целые исходные зоны по очереди битовых интервалов; весь диапазон без повторов";
    if (!traversal_full_coverage())
        return "выборка — повторы и пропуски, «диапазон проверен» не наступит";

    // ДОПУЩЕНИЕ О СКОРОСТИ НАЗЫВАЕТСЯ ВСЛУХ.
    //
    // Здесь стояло 35 млн/с на поток с пометкой «замер на M5». Замеры на
    // этой же машине дают 30-32: один поток -- 32.1, два потока -- 60.3, то
    // есть 30.2 на поток. Комментарий утверждал измерение, которого не было,
    // и оценка выходила оптимистичнее правды на шестую часть: 6.5 часов там,
    // где на деле 7.6.
    //
    // Само по себе это мелочь -- счёт идёт на степени двойки. Не мелочь то,
    // что число подавалось как знание. Поэтому: константа приведена к
    // измеренному, а допущение печатается рядом с ответом, чтобы его можно
    // было сверить со строкой keys/s прямо во время счёта.
    //
    // Линейности по потокам тут нет: у M5 четыре производительных ядра и
    // шесть экономичных, и после четырёх потоков прибавка меньше. Оценка
    // считает линейно и потому сверху -- при большом числе потоков она
    // оптимистична.
    const long double PER_THREAD = 30e6L;
    const long double rate = PER_THREAD * (g_cfg.threads > 0 ? g_cfg.threads : 1);
    // Считаем по тому, что будет проверено: при маске это кандидаты решётки,
    // а не ширина интервала. Иначе для 40 замороженных нибблов выходило бы
    // «10^60 лет» там, где на деле часы -- и человек отказался бы от прогона,
    // который занял бы вечер.
    long double items;
    if (mask_active()) {
        items = u256_to_ld(g_mask_total);
    } else {
        u256 span; u256_sub(span, g_range_end, g_range_start);
        items = u256_to_ld(span) + 1.0L;
    }
    long double secs = items / rate;
    long double y = secs / 3.15e7L;

    char t[64];
    if (secs < 3600)    snprintf(t, sizeof t, "%.0Lf минут", secs / 60);
    else if (secs < 86400) snprintf(t, sizeof t, "%.1Lf часов", secs / 3600);
    else if (y < 1)     snprintf(t, sizeof t, "%.1Lf дней", secs / 86400);
    else if (y < 1000)  snprintf(t, sizeof t, "%.0Lf лет", y);
    else                snprintf(t, sizeof t, "%.0Le лет", y);

    // Оговорки про «неполное покрытие» больше нет: номер участка стал
    // 256-битным, и любой диапазон обходится целиком. Единственный предел
    // теперь — время, и оно тут же названо.
    char asum[160];
#ifdef PKH_HAVE_CUDA
    if (g_gpu_active) {
        // СКОРОСТЬ КАРТЫ НЕ ВЫДУМЫВАЕТСЯ.
        //
        // Для процессора здесь стоит измеренное число, и рядом с ним автор
        // отдельно оговорил, что подавать неизмеренное как знание нельзя.
        // Для карты такого замера ещё нет ни одного, поэтому оценка честно
        // не даётся: она появится сама, когда пройдёт первый вывод скорости.
        return std::string("полное, основной обход без повторов — время станет "
                           "известно после первой строки скорости");
    }
#endif
    snprintf(asum, sizeof asum, "  (счёт из 30 млн/с на поток x %d)",
             g_cfg.threads > 0 ? g_cfg.threads : 1);
    if (mask_active())
        return std::string("все ключи с этим хвостом, по одному разу — примерно ")
             + t + asum;
    return std::string("полное, основной обход без повторов — примерно ") + t + asum;
}

void compute_range() {
    u256_zero(g_range_start);
    u256_zero(g_range_end);
    int a = g_cfg.range_start_bit, b = g_cfg.range_end_bit;
    // начало = 2^a (при a = 0 это 1: ключ 0 недопустим),
    // конец  = 2^b - 1 включительно
    if (a > 0) g_range_start[a >> 6] = 1ULL << (a & 63);
    else       u256_from_u64(g_range_start, 1);
    for (int i = 0; i < b && i < 256; i++) g_range_end[i >> 6] |= 1ULL << (i & 63);
    if (b >= 256) {                                   // обрезаем по порядку кривой
        u256 nm1; u256_set(nm1, EC_N);
        u256 one; u256_from_u64(one, 1);
        u256_sub(nm1, nm1, one);
        u256_set(g_range_end, nm1);
    }

    // Позиция старшего разряда зависит только от границ в битах, поэтому
    // считается здесь же -- до сужения, которое ею пользуется.
    compute_top_nibble();
    narrow_range_by_top_nibble();
    compute_zone_plan();

    // Маска считается ПОСЛЕ границ: пересечение зависит от них обоих.
    const bool mask_ok = mask_compute();
    (void)mask_ok;

    uint64_t chunk = UINT64_C(1) << traversal_block_bits();
    if (mask_active()) {
        // Участков столько, сколько КАНДИДАТОВ, а не сколько ключей в отрезке.
        if (g_mask_total[3] == 0 && g_mask_total[2] == 0 && g_mask_total[1] == 0)
            g_total_chunks = g_mask_total[0] / chunk + (g_mask_total[0] % chunk != 0);
        else
            g_total_chunks = 0;
        compute_total_blocks256();
        return;
    }

    u256 span; u256_sub(span, g_range_end, g_range_start);
    if (span[3] == 0 && span[2] == 0 && span[1] == 0)
        g_total_chunks = span[0] / chunk + 1;
    else
        g_total_chunks = 0;   // не помещается в 64 бита -- не помеха для TRAV_SEQ/TRAV_KXE,
                               // оба справляются с этим случаем своими средствами
    compute_total_blocks256();
}

bool run_startup_menu(int argc, char **argv) {
    detect_hardware();
    print_menu_header();

    bool from_file = ui_noninteractive;
    if (!ui_noninteractive && config_ini_exists()) {
        // ФАЙЛ ЧИТАЕТСЯ ВСЕГДА, ДАЖЕ ПРИ ОТВЕТЕ «НЕТ».
        //
        // Раньше при «Нет» файл не читался вовсе, настройки брались встроенные,
        // а в конце меню config.ini ПЕРЕЗАПИСЫВАЛСЯ из них. Всё, о чём меню не
        // спрашивает -- путь к signal-cli, номера Signal, токен Telegram, --
        // при этом затиралось значениями по умолчанию. Человек вписывал номер
        // в файл, запускал, отвечал «Нет» (потому что хотел сменить режим
        // поиска) и получал стёртый файл, ни о чём не будучи предупреждён.
        //
        // Теперь «Нет» означает ровно то, что написано: заново спросить про
        // ПАРАМЕТРЫ ПОИСКА. Всё остальное сохраняется, как и было в файле.
        load_config_ini(g_cfg);
        // ЗНАЧЕНИЯ ИЗ ФАЙЛА ПРИВОДЯТСЯ В ПОРЯДОК СРАЗУ, а не в конце меню:
        // ниже они подставляются как умолчания в вопросы, и файл с
        // gpu_power=30, набранный руками, ушёл бы в счёт как есть.
        sanitize_config(g_cfg);
#ifdef PKH_HAVE_CUDA
        if (g_gpu_active) {
            // ТО, ЧТО С КАРТОЙ ОБЯЗАТЕЛЬНО, СТАВИТСЯ НА ОБЩЕМ ПУТИ.
            //
            // Три вещи стояли только в ветке ручного ввода, и ответ «Да»
            // (он же ENTER) их обходил:
            //   * потоков = 1. cuda_scan_* держит одно состояние на процесс;
            //     файл от процессорного запуска с threads=8 дал бы восемь
            //     потоков, бьющих одну карту, -- перезаписанные буферы и
            //     потерянные кандидаты;
            //   * мощность из файла. Карта поднимается ДО чтения файла, и
            //     сохранённые 80% молча заменялись советом (100);
            //   * участок не мельче 2^28 -- иначе подготовка съедает счёт.
            g_cfg.threads = 1;
            g_gpu_power_pct = g_cfg.gpu_power_pct > 0 ? g_cfg.gpu_power_pct
                                                      : (g_gpu_info.is_mobile ? 70 : 100);
            // Порог 28, а не 30: 30 -- совет меню, а ниже 28 участок уже
            // заведомо вреден (подготовка длиннее счёта). Осознанные 28-29
            // из файла не трогаем.
            if (g_cfg.chunk_bits < 28) g_cfg.chunk_bits = 28;
        }
#endif
        if (ask_yes_no("Использовать предыдущие настройки?", true)) {
            std::cout << "\nЗагружены настройки из config.ini.\n";
            from_file = true;
        } else {
            std::cout << "\nСпрошу заново про поиск. Уведомления (Telegram, Signal)\n"
                         "останутся как в config.ini -- меню про них не спрашивает.\n";
        }
    }

    if (!from_file) {
        std::string def_pub = (argc > 1) ? argv[1] : g_cfg.pubkey_path;
        g_cfg.pubkey_path = ask_string("Введите путь к pubkey.txt", def_pub);

        const int max_threads = (g_hw.cpu_logical > 0) ? g_hw.cpu_logical : 16;
        int def_threads = g_hw.rec_threads;
        if (def_threads > max_threads) def_threads = max_threads;
#ifdef PKH_HAVE_CUDA
        if (g_gpu_active) {
            // С картой вопрос лишён смысла: cuda_scan_* держит одно состояние
            // на процесс, значит кормит её ровно один поток. Спрашивать и
            // потом молча переписывать ответ -- худшее из возможного.
            g_cfg.threads = 1;   // сказано в шапке ОБОРУДОВАНИЕ, здесь молчим
        } else
#endif
        g_cfg.threads = ask_int("Введите количество потоков (1-" +
                                std::to_string(max_threads) + ", советуем " +
                                std::to_string(def_threads) + ")",
                                def_threads, 1, max_threads);   // ENTER допишет ask_int

        // ПОДСКАЗКА СОБИРАЕТСЯ ИЗ САМОГО ЗНАЧЕНИЯ, А НЕ ПИШЕТСЯ РУКАМИ.
        //
        // Было: "ENTER = 300 сек" и "ENTER = 3600 сек" -- голые литералы. На
        // ENTER подставлялось g_cfg.progress_sec, то есть значение из
        // common.h, а на экране стояло число, набранное отдельно. Поменяв
        // константу и пересобрав, пользователь видел прежние 300 и 3600 и
        // делал единственный разумный вывод: правка не подействовала. Хотя
        // подставлялось уже новое.
        //
        // Текст, который повторяет значение, обязан из него и получаться.
        // Подсказку «ENTER = ...» дописывает сам ask_int из подставляемого
        // значения -- вручную её тут больше нет. Прежде текст и значение
        // жили порознь и однажды разошлись: на экране 300, подставлялось
        // другое, и правка выглядела недействующей.
#ifdef PKH_HAVE_CUDA
        // МОЩНОСТЬ КАРТЫ СПРАШИВАЕМ, А НЕ СТАВИМ МОЛЧА.
        //
        // Раньше в шапке стояло "70% от штатного" -- и это было вдвойне
        // неудачно. Во-первых, поставлено перед фактом: настольная карта
        // рассчитана на сутки полной нагрузки, и сбавлять ей нечего.
        // Во-вторых, строка ВРАЛА: настоящий предел мощности ставится через
        // nvidia-smi -pl, для этого нужны права администратора, а на
        // ноутбучных картах он часто заперт прошивкой -- программа ничего не
        // ставила и поставить не могла. Печаталась рекомендация, похожая на
        // действие.
        //
        // Теперь число значит ровно то, что программа действительно делает:
        // долю времени, которую карта считает. Остальное она стоит. Это
        // работает без всяких прав, и цена названа прямо -- скорость падает
        // на ту же долю.
        if (g_gpu_active) {
            const bool mob = g_gpu_info.is_mobile;
            std::cout << "\n--- Мощность видеокарты ---\n"
                      << "  Карта : " << g_gpu_info.name
                      << (mob ? "  (ноутбучная)" : "  (настольная)") << "\n";
            if (g_gpu_count > 1)
                std::cout << "  Найдено: " << g_gpu_count
                          << " карт NVIDIA, считает первая\n";
            std::cout << "  Число — доля времени, которую карта СЧИТАЕТ; остальное\n"
                         "  она стоит. 100 — без пауз, самый быстрый счёт.\n"
                         "  Меньше — тише и холоднее, и ровно во столько же раз\n"
                         "  медленнее. Разумные значения: 70, 80, 85, 99, 100.\n";
            if (mob)
                std::cout << "  Карта ноутбучная: её теплоотвод рассчитан на короткие\n"
                             "  всплески, а не на сутки счёта — поэтому советуем 70.\n";
            else
                std::cout << "  Карта настольная: она рассчитана на долгую полную\n"
                             "  нагрузку — сбавлять незачем, советуем 100.\n";

            const int def_pow = g_cfg.gpu_power_pct > 0 ? g_cfg.gpu_power_pct
                                                        : (mob ? 70 : 100);
            g_cfg.gpu_power_pct = ask_int("Мощность запуска, % (50-100)",
                                          def_pow, 50, 100);
            g_gpu_power_pct = g_cfg.gpu_power_pct;
        }
#endif

        g_cfg.progress_sec = ask_int("Введите интервал отправки скорости в Telegram, сек",
                                     g_cfg.progress_sec, 1, 36000000);
        g_cfg.check_sec    = ask_int("Введите интервал отправки проверки в Telegram, сек",
                                     g_cfg.check_sec, 1, 36000000);

        std::string def_found = (argc > 2) ? argv[2] : g_cfg.found_path;
        g_cfg.found_path = ask_string("Введите файл найденных совпадений", def_found);

        // ПОРЯДОК ВОПРОСОВ: СНАЧАЛА ЧТО ИЩЕМ, ПОТОМ ГДЕ.
        //
        // Диапазон и старший разряд спрашивались ДО выбора режима. Для
        // HEX-маски это неверный порядок: границы перебора вытекают из
        // самой маски -- из того, какая часть ключа объявлена переменной и
        // где при этом лежит ключ целиком. Спросив границы раньше, мы
        // заставляли отвечать наугад, а потом молча считали пересечение с
        // ответом, данным вслепую.
        //
        // Поэтому блоки переехали ниже, за выбор режима и маски. Здесь
        // остаётся только предварительный расчёт: меню режимов показывает
        // число разрядов для зонального, и без границ его не посчитать.
        // Границы берутся из config.ini или из умолчаний -- ровно те, что
        // будут, если пользователь ничего не поменяет; после ввода всё
        // пересчитывается, а доступность зонального перепроверяется.
        sanitize_config(g_cfg);
        compute_range();
        compute_top_nibble();


        // Keep saved mode values and menu numbers stable. Coverage is shown
        // beside each option because only finite bijective modes close a range.
        std::cout << "\n--- Режим перебора ---\n"
                  << "\n    1 - Детерминированный: подряд снизу вверх, без повторов\n"
                  << "    2 - KXE: блоки в перемешанном порядке, без повторов\n"
                  << "    3 - Случайный: выборка, повторы и пропуски возможны\n"
                  << "    4 - Зонный: меняется один старший ниббл, без повторов\n"
                  << "    5 - Волновой: растущий узор совместных приращений, без повторов\n"
                  << "    6 - Гибридный (A целиком случайная и фиксируется;\n"
                     "                    B перебирается подряд, затем новая A)\n"
                  << "\n  Режимы 1 и 2 проверяют весь выбранный диапазон.\n"
                     "  Волна от заданной точки начинает с её исходного узора.\n"
                     "  Зоны и случайные старты волны проверяют полные окрестности разных баз.\n"
                     "  Затем проверяются оставшиеся участки диапазона, без повторов.\n"
                     "  В режимах 4 и 5 новые базы выбираются по очереди битовых интервалов.\n"
                     "  Единица чередования — целый исходный узор одной базы.\n"
                     "  Режимы 1, 2, 4 и 5 проверяют весь выбранный диапазон.\n"
                     "  Размеры блоков волны и зонного режима задаются независимо.\n"
                     "  Режимы 3 и 6 не гарантируют полного покрытия.\n";
        // ПУНКТ МЕНЮ И ВНУТРЕННЕЕ ЗНАЧЕНИЕ -- РАЗНЫЕ ВЕЩИ.
        //
        // Пункты идут по порядку и сгруппированы по смыслу: сначала два
        // режима с полным покрытием, потом два выборочных. Внутренние же
        // значения TRAV_* заморожены навсегда -- они попали в config.ini
        // прежних версий, и менять их значит молча переназначить чужие
        // настройки. Поэтому здесь стоит таблица перевода, а не совпадение.
        //
        // В config.ini с этой версии пишется имя (seq/kxe/random/zone), так
        // что спутать пункт меню с записью в файле больше не на чем.
        static const int MENU_TO_MODE[6] = {
            TRAV_SEQ,      // пункт 1
            TRAV_KXE,      // пункт 2
            TRAV_RANDOM,   // пункт 3
            TRAV_ZONE,     // пункт 4
            TRAV_WAVE,     // пункт 5
            TRAV_HYBRID    // пункт 6
        };
        int def_item = 1;
        for (int i = 0; i < 6; i++) if (MENU_TO_MODE[i] == g_cfg.mode) def_item = i + 1;
        g_cfg.mode = MENU_TO_MODE[ask_int("Режим", def_item, 1, 6) - 1];

#ifdef PKH_HAVE_CUDA
        // ЗОНАЛЬНЫЙ И ВОЛНОВОЙ РЕЖИМЫ С КАРТОЙ ХОДЯТ КРУПНЫМ БЛОКОМ.
        //
        // Раньше здесь стояло предупреждение: с картой эти два режима идут
        // порядка 80 тысяч ключей/с вместо сотен миллионов, берите режим
        // 1, 2 или 3. Предупреждение было верным, но лечило не то: блок в
        // 4096 ключей -- три младших ниббла -- писался под процессор, где
        // пакет считается за микросекунды. Карте за один заход нужны сотни
        // миллионов ключей, и на четырёх тысячах она простаивала 99.9%
        // времени. Виноват был размер блока, а не режим.
        //
        // The selected wave suffix is independent of the GPU launch size.
        // Four-nibble blocks additionally support the packed fast path.
        if (g_gpu_active && g_cfg.mode == TRAV_WAVE) {
            std::cout << "\n  Волна: размер блока 16^N выберите ниже.\n"
                         "  Блоки проходят без повторов; видеокарта считает ключи внутри блока.\n";
        }
#endif

        // Проверка доступности зонального переехала ниже, за ввод границ:
        // здесь они ещё не спрошены, и g_zone_count относился бы к прошлому
        // запуску, а не к тому, что человек только что набрал.

        // ------------------------- HEX-МАСКА ----------------------------
        //
        // Спрашивается только у детерминированного: полное покрытие -- это
        // единственный режим, где «перебрать всё с таким хвостом» вообще
        // что-то значит. У выборочных режимов маска была бы украшением.
        if (g_cfg.mode == TRAV_SEQ) {
            std::cout << "\n  --- Что задаём ---\n"
                      << "    1 - Интервал 2^a..2^b   (перебор подряд, как раньше)\n"
                      << "    2 - HEX-маска           (младшие ниббли ключа известны)\n";
            const int def_src = g_cfg.mask_hex.empty() ? 1 : 2;
            const int src = ask_int("Что задаём", def_src, 1, 2);
            if (src == 1) {
                g_cfg.mask_hex.clear();
                g_cfg.mask_froz = 0;
            } else {
                for (;;) {
                    std::string h = ask_string("Ключ, 64 знака HEX", g_cfg.mask_hex);
                    // Пробелы внутри терпим: ключ часто переносят по кускам.
                    std::string c;
                    for (char ch : h) if (!isspace((unsigned char)ch)) c += ch;
                    u256 probe;
                    if (c.size() == 64 && hex_to_u256(c.c_str(), probe)) {
                        g_cfg.mask_hex = c;
                        break;
                    }
                    std::cout << "  Нужно ровно 64 шестнадцатеричных знака"
                              << " (введено " << c.size() << ").\n";
                }
                const int def_fr = g_cfg.mask_froz > 0 ? g_cfg.mask_froz : 40;
                g_cfg.mask_froz = ask_int(
                    "Сколько МЛАДШИХ нибблов не меняются (1-64, счёт с конца)",
                    def_fr, 1, 64);

                // РАЗМЕТКА ПОКАЗЫВАЕТСЯ СРАЗУ.
                //
                // Число «сколько нибблов заморожено» читается двояко, и
                // ошибка в его понимании не выглядит ошибкой: программа
                // честно переберёт не то пространство и ничего не найдёт.
                // Поэтому смысл виден в момент ввода, а не в документации.
                const int vary = 64 - g_cfg.mask_froz;
                (void)vary;
                std::cout << "\n  " << mask_layout(g_cfg.mask_hex, g_cfg.mask_froz) << "\n"
                          << "  " << mask_legend(g_cfg.mask_froz) << "\n"
                          << "  Шаг между кандидатами: 16^" << g_cfg.mask_froz << "\n\n";
            }
        } else {
            g_cfg.mask_hex.clear();
            g_cfg.mask_froz = 0;
        }

        // ------------------- ГДЕ ПЕРЕБИРАТЬ -----------------------------
        //
        // При маске границы предлагает сама программа: она знает, какая
        // часть ключа переменная и где лежит ключ целиком. Спрашивать их
        // вслепую до маски было бессмысленно -- см. пояснение выше.
        if (mask_active()) {
            u256 K; hex_to_u256(g_cfg.mask_hex.c_str(), K);
            int hb = -1;
            for (int i = 255; i >= 0; i--) if (u256_bit(K, i)) { hb = i; break; }
            if (hb < 0) hb = 0;

            // Верх решётки: переменная часть вся из f, хвост прежний.
            u256 kmax; u256_set(kmax, K);
            for (int i = 4 * g_cfg.mask_froz; i < 256; i++) kmax[i >> 6] |= 1ULL << (i & 63);
            int hbmax = 255;
            for (int i = 255; i >= 0; i--) if (u256_bit(kmax, i)) { hbmax = i; break; }

            uint8_t kb[32], mb[32];
            u256_to_be32(kb, K); u256_to_be32(mb, kmax);
            std::cout << "\n--- Где перебирать ---\n"
                      << "  Переменная часть идёт от " << g_cfg.mask_hex.substr(0, 64 - g_cfg.mask_froz)
                      << " и выше.\n"
                      << "  Первый кандидат : " << to_hex(kb, 32) << "\n"
                      << "  Последний       : " << to_hex(mb, 32) << "\n"
                      << "  Значит все кандидаты лежат в [2^" << hb
                      << ", 2^" << (hbmax + 1) << ").\n"
                      << "  Можно сузить: перебор пойдёт только по пересечению.\n";
            g_cfg.range_start_bit =
                ask_int("От 2^", hb, 0, hb);
            g_cfg.range_end_bit =
                ask_int("До 2^", hbmax + 1, g_cfg.range_start_bit + 1, 256);
            sanitize_config(g_cfg);
            compute_range();
            compute_top_nibble();

            // Старший разряд при маске не спрашивается: маска уже задаёт
            // форму ключа целиком, и второе, независимое сужение той же
            // строки могло бы вычеркнуть все кандидаты разом -- причём
            // молча, двумя по отдельности разумными настройками.
            g_cfg.det_hex = -1;
        } else {
        // Подсказка про ENTER раньше обещала «весь простор: ENTER = от 0 до
        // 256». Это было правдой ровно один раз -- при первом запуске с
        // чистым конфигом. ask_int() подставляет по ENTER то, что лежит в
        // g_cfg, а туда границы попадают из config.ini с прошлого запуска.
        // Стоило один раз выбрать 70 -- и подсказка начинала врать, причём
        // молча: человек жмёт ENTER, ожидая всё пространство, и получает
        // прошлый диапазон. Теперь написано, что ENTER делает на самом деле,
        // а «весь простор» назван числами, которые для этого надо ввести.
        std::cout << "\n--- Диапазон ключей ---\n"
                  << "  Ключи будут лежать в [2^x, 2^y).\n"
                  << "  Головоломке N отвечает диапазон [2^(N-1), 2^N).\n"
                  << "  Всё пространство -- это 0 и 256, их надо ввести.\n"
                  << "  ENTER оставляет значение в скобках: оно с прошлого запуска.\n";
        g_cfg.range_start_bit = ask_int("От 2^", g_cfg.range_start_bit, 0, 255);
        int def_hi = (g_cfg.range_end_bit > g_cfg.range_start_bit)
                     ? g_cfg.range_end_bit : g_cfg.range_start_bit + 1;
        g_cfg.range_end_bit = ask_int("До 2^", def_hi, g_cfg.range_start_bit + 1, 256);

        // Границы и производные от них величины считаем сразу после ввода
        // диапазона: ниже по меню они уже нужны — и позиция старшего разряда,
        // и число блоков, от которого зависит доступность KXE. Пока
        // compute_range() вызывался только в самом конце, KXE всегда
        // показывался недоступным, сколько бы блоков ни было на деле.
        sanitize_config(g_cfg);
        compute_range();
        compute_top_nibble();
        static const char HEXD[] = "0123456789abcdef";
        std::cout << "\n--- Старший разряд ключа ---\n"
                  << "  Пространство можно поделить на части по старшему значащему\n"
                  << "  разряду ключа — удобно, когда счёт идёт на нескольких машинах.\n"
                  << "  Позиция разряда в строке HEX: " << g_top_ni << "\n"
                  << "  Допустимые значения: " << g_top_vmin << ".." << g_top_vmax
                  << "  (" << HEXD[g_top_vmin] << ".." << HEXD[g_top_vmax] << ")\n"
                  << "  Вводите HEX: 0–9, a–f (также принимаются 10–15).\n"
                  << "  ENTER — прежний выбор; -1 — весь диапазон.\n";
        {
            int defd = g_cfg.det_hex;
            if (defd >= 0 && (defd < g_top_vmin || defd > g_top_vmax)) defd = -1;
            for (;;) {
                const std::string previous = defd < 0 ? "-1" : std::string(1, HEXD[defd]);
                int selected = defd;
                if (parse_top_hex_digit(ask_string("Разряд HEX", previous), selected) &&
                    (selected == -1 || (selected >= g_top_vmin && selected <= g_top_vmax))) {
                    g_cfg.det_hex = selected; break;
                }
                std::cout << "  Выберите допустимую HEX-цифру или -1.\n";
            }
            if (g_cfg.det_hex >= 0) {
                std::cout << "  Время работы на каждой цифре; затем следующая по кругу. 0 — без смены.\n";
                while (!parse_digit_hours(ask_string("Время в интервале, часов", format_digit_hours(g_cfg.digit_hours)), g_cfg.digit_hours))
                    std::cout << "  Введите число часов от 0 до 8760 (например, 2 или 0,5).\n";
            }
        }
        }

        // Selecting a fixed top digit changes the actual zone domain.
        compute_range();

        if (g_cfg.mode == TRAV_WAVE) {
            std::cout << "\n--- Размер блока волнового режима ---\n"
                         "  Волна меняет один старший ниббл, затем два вместе, затем три и далее.\n"
                         "  Приращения идут по модулю 16, без переноса между нибблами.\n"
                         "  Младшие N нибблов перебираются блоком из 16^N ключей.\n"
                         "  От каждой базы выполняется весь исходный узор, без пропуска фаз.\n"
                         "  Без заданного старта затем выбирается новая целая база.\n"
                         "  После целого узора очередь получает следующий битовый интервал.\n"
                         "  Волны разных баз не пересекаются; после них проверяется остаток диапазона.\n"
                         "  От заданной точки сначала проверяется её узор, затем остальные участки.\n"
                         "  На границе диапазона блок обрезается. Большой блок дольше завершается.\n"
                         "  Размер независим от блока зонного режима.\n"
                         "  При новом запуске начинается новый проход.\n";
            std::cout << "\n   N    Ключей в полном блоке         Время блока*\n"
                         "  ----  ---------------------------  ------------\n";
            for (int n = 3; n <= 10; ++n) {
                const uint64_t keys = UINT64_C(1) << (4 * n);
                const std::string count = fmt_num(keys);
                printf("  %2d    %s  %s%s\n", n, zone_pad(count,27).c_str(),
                       zone_pad(zone_human_time((double)keys/ZONE_SPEED_ESTIMATE),12).c_str(),
                       n == g_cfg.wave_nibbles ? "  <- выбрано" : "");
            }
            std::cout << "  * Оценка при 3 млрд ключей/с, как в BitPazl.\n"
                         "    Фактическое время зависит от скорости поиска и мощности.\n\n";
            g_cfg.wave_nibbles = ask_int("Размер блока волны: 16^N ключей (N)",
                                         g_cfg.wave_nibbles, 3, 10);
        } else if (g_cfg.mode == TRAV_ZONE) {
            zone_size_table(g_cfg.zone_nibbles);
            g_cfg.zone_nibbles = ask_int("Размер зонного блока: 16^N ключей (N)",
                                         g_cfg.zone_nibbles, 3, 15);
        } else if (g_cfg.mode == TRAV_HYBRID) {
            const int top_nibble = g_cfg.range_end_bit > 0
                ? (g_cfg.range_end_bit - 1) / 4 : 0;
            hybrid_size_table(top_nibble, g_cfg.hybrid_nibbles);
            g_cfg.hybrid_nibbles = ask_int("Разрядов в части B (N)",
                                          g_cfg.hybrid_nibbles, 3, 15);
            g_cfg.hybrid_debug = ask_yes_no("Записывать первые блоки A | B в файл?",
                                           g_cfg.hybrid_debug);
        } else {
            int def_chunk = g_cfg.chunk_bits;
#ifdef PKH_HAVE_CUDA
            if (g_gpu_active) {
                // РАЗМЕР УЧАСТКА С КАРТОЙ ДОЛЖЕН БЫТЬ НАМНОГО БОЛЬШЕ.
                //
                // Перед каждым заходом хост считает начальные точки для всех
                // нитей карты -- при раскладке 328x256 это 83 968 точек.
                // Замер: 100 мс на заход. Стоимость эта разовая, но она
                // приходится на УЧАСТОК: при 2^22 (4.19 млн ключей) выходит
                // потолок около 42 млн ключей/с независимо от того, насколько
                // быстра сама карта -- она просто ждёт.
                //
                // 2^28 -- это 268 млн ключей на заход, те же 100 мс
                // размазываются в несколько раз тоньше.
                //
                // ПОЧЕМУ ТЕПЕРЬ 2^30, А НЕ 2^28. Число нитей выросло с 84
                // тысяч до 1.3 млн, и подготовка выросла вместе с ним: она
                // стоит НА НИТЬ. Её распараллелили на все ядра процессора,
                // но совсем бесплатной она не стала, а платится по-прежнему
                // один раз на участок. При 2^28 карте достаётся около 0.14 с
                // счёта на каждую подготовку -- пятая часть времени уходит
                // мимо; при 2^30 счёт длится около 0.6 с, и доля падает
                // примерно до 6%.
                //
                // Выше не идём из-за сторожевого таймера драйвера Windows:
                // одно ядро выполняется целиком за один запуск, и запуск
                // дольше двух секунд драйвер сбрасывает. 0.6 с оставляют
                // трёхкратный запас; 2^32 его бы съели.
                if (def_chunk < 30) def_chunk = 30;
                std::cout << "  С картой участок берётся крупнее: подготовка "
                             "начальных точек делается\n"
                             "  один раз на участок, и на мелком участке "
                             "карта ждёт процессор.\n";
            }
#endif
            g_cfg.chunk_bits = ask_int("Размер участка на поток = 2^N ключей (10-40)",
                                       def_chunk, 10, 40);
        }

        // X(k) == X(n-k): предфильтр общий. Основной k остаётся внутри
        // заданного интервала, зеркало n-k проверяется и за его пределами.
        // Кому нужно отключить зеркало -- строка want_nk= в config.ini.
        g_cfg.want_nk = true;

        if (g_cfg.mode != TRAV_HYBRID) {
        const bool full_cov = traversal_full_coverage();
        std::cout << "\n--- Журнал участков ---\n"
                  << "  Первые " << DBG_LIMIT << " участков будут записаны в debug_chunks.txt:\n"
                  << "  номер, поток, начальный и конечный ключ, публичный ключ.\n";
        if (full_cov)
            std::cout << "  Блоки не пересекаются. Соседние строки разных окрестностей\n"
                         "  могут относиться к разным частям диапазона. Журнал показывает\n"
                         "  первые блоки; весь диапазон закрывает конечный обход.\n";
        else
            std::cout << "  Покрытие по нему проверить НЕЛЬЗЯ: в выборочном режиме его\n"
                         "  нет по устройству. Журнал здесь о другом — видно, куда легли\n"
                         "  участки и не сбились ли они в одно место.\n";
        g_cfg.debug_chunks = ask_yes_no("Вести журнал участков?", g_cfg.debug_chunks);
        if (g_cfg.mode == TRAV_ZONE) g_cfg.zone_debug = g_cfg.debug_chunks;
        }

        // Токен в исходнике не хранится: он приходит из config.ini или из
        // переменных PUBKEYHUNT_TG_TOKEN / PUBKEYHUNT_TG_CHAT. На экран не
        // выводится. Если его нет -- уведомления просто выключены.
        if (g_cfg.tg_token.empty() || g_cfg.tg_chat.empty()) {
            std::cout << "\n--- Telegram ---\n"
                         "  Токен не задан, уведомления выключены. Чтобы включить:\n"
                         "    export PUBKEYHUNT_TG_TOKEN=... ; export PUBKEYHUNT_TG_CHAT=...\n"
                         "  либо строки tg_token= и tg_chat= в config.ini.\n";
            g_cfg.tg_token.clear();
        } else {
            // ТОКЕН ЕСТЬ -- ЗНАЧИТ УВЕДОМЛЕНИЯ НУЖНЫ, ВОПРОСА БОЛЬШЕ НЕТ.
            //
            // Токен задают вручную: через config.ini или переменные
            // окружения. Задав его, человек уже ответил "да" -- спрашивать
            // второй раз незачем. Выключить можно, убрав токен, или строкой
            // tg_progress= в config.ini.
            //
            // tg_progress по умолчанию false и раньше не включался нигде,
            // кроме ручной правки config.ini -- согласие здесь означало
            // только "не выключать токен", а строка прогресса на Telegram
            // не уходила никогда, сколько бы ни было отвечено "Да".
            g_cfg.tg_progress = true;
        }
    }

    sanitize_config(g_cfg);
    set_block_nibbles(g_cfg.wave_nibbles);

#ifdef PKH_HAVE_CUDA
    if (ui_noninteractive && g_gpu_active) {
        g_cfg.threads = 1;
        g_gpu_power_pct = g_cfg.gpu_power_pct > 0 ? g_cfg.gpu_power_pct
                                                : (g_gpu_info.is_mobile ? 70 : 100);
        if (g_cfg.chunk_bits < 28) g_cfg.chunk_bits = 28;
    }
#endif

    // A single host worker preserves deterministic committed block order;
    // the GPU itself still runs its normal parallel threads.
    if (g_cfg.mode == TRAV_WAVE || g_cfg.mode == TRAV_ZONE || g_cfg.mode == TRAV_HYBRID)
        g_cfg.threads = 1;
    if (g_cfg.mode == TRAV_ZONE && g_cfg.zone_debug) g_cfg.debug_chunks = true;

    // ПАПКА RESULT СОЗДАЁТСЯ НА ОБЩЕМ ПУТИ, А НЕ В ВЕТКЕ ВВОДА НАСТРОЕК.
    //
    // Сначала этот блок стоял внутри «изменить настройки». При ответе
    // «использовать предыдущие» он не выполнялся вовсе -- а в config.ini уже
    // записано found=RESULT/Found.txt. На свежей копии проекта (в архиве
    // пустой папки нет) это давало: программа считает сутки, находит ключ и
    // не может его записать. Узнать об этом в такую минуту -- худшее время
    // из возможных.
    //
    // Ровно та же ошибка уже была с compute_top_nibble() парой строк ниже:
    // расчёт стоял только в одной ветке, и при «прежних настройках»
    // зональный режим молча вырождался. Поэтому теперь тут же, на общем
    // пути, где проходят оба ответа.
    {
        std::string why;
        if (!ensure_result_dir(why)) {
            std::cout << "\n  " << why << "\n"
                      << "  Находки будут записаны рядом с программой.\n";
            // Путь не подставляем: каталога нет, и RESULT/Found.txt не
            // откроется. Пусть лучше ляжет рядом, чем никуда.
            if (g_cfg.found_path.rfind("RESULT/", 0) == 0)
                g_cfg.found_path = g_cfg.found_path.substr(7);
        } else {
            g_cfg.found_path = found_in_result(g_cfg.found_path);
        }
    }

    compute_range();
    // ИСПРАВЛЕНО: compute_top_nibble() стоял ТОЛЬКО в ветке ручного ввода.
    // При ответе «использовать предыдущие настройки» оставались значения по
    // умолчанию g_zone_count = 0 и g_batches_per_cycle = 1, и зональный режим
    // молча вырождался: ниббл не менялся никогда, каждый участок был просто
    // новой случайной базой. Теперь считается всегда.
    compute_top_nibble();
    if (g_cfg.mode == TRAV_WAVE && !g_cfg.wave_start.empty()) {
        u256 fixed_start;
        if (!hex_to_u256(g_cfg.wave_start.c_str(), fixed_start) ||
            u256_cmp(fixed_start, g_range_start) < 0 || u256_cmp(fixed_start, g_range_end) > 0) {
            std::cout << "\n  wave_start должен быть HEX-ключом внутри выбранного диапазона.\n"
                         "  Это точка старта одного растущего узора, без повторов.\n"
                         "  Исправьте wave_start в config.ini или оставьте пустым.\n";
            return false;
        }
        std::cout << "\n  Волна начинается с заданного ключа wave_start.\n"
                     "  Проверяется один растущий узор; после него поиск завершается.\n"
                     "  Полное покрытие диапазона при заданном старте не обещается.\n";
    }

    uint8_t sb[32], eb[32];
    u256_to_be32(sb, g_range_start);
    u256_to_be32(eb, g_range_end);

    std::cout << "\n" << C_BOLD << std::string(60, '=') << "\n"
              << " Конфигурация\n"
              << " pubkey.txt    : " << g_cfg.pubkey_path << "\n"
              << " FOUND         : " << g_cfg.found_path  << "\n"
#ifdef PKH_HAVE_CUDA
              << (g_gpu_active
                    ? std::string(" Считает       : видеокарта (процессор подтверждает кандидатов)\n")
                    : std::string(" Потоков       : ") + std::to_string(g_cfg.threads) +
                      " из " + std::to_string(g_hw.cpu_logical) + "\n")
#else
              << " Потоков       : " << g_cfg.threads << " из " << g_hw.cpu_logical << "\n"
#endif
#ifdef PKH_HAVE_CUDA
              // Мощность стоит в сводке рядом со скоростью нарочно: эти две
              // строки читают вместе, и если счёт идёт с паузами, знать об
              // этом надо до, а не после первой строки статистики.
              << (g_gpu_active
                    ? " Мощность карты: " + std::to_string(g_gpu_power_pct.load()) + "%" +
                      (g_gpu_power_pct >= 100 ? std::string("  (без пауз)")
                                              : std::string("  (с паузами)")) + "\n"
                    : std::string())
#endif
              << " Telegram, скорость : " << g_cfg.progress_sec << " сек\n"
              << " Telegram, проверка : " << g_cfg.check_sec    << " сек\n"
              << " Экран              : обновление каждые 30 сек\n"
              << " Проверка сборки    : CPU-тесты и контрольные суммы; GPU-проверка этой сборки не выполнялась\n"
              << " Диапазон      : от 2^" << g_cfg.range_start_bit
                                 << " до 2^" << g_cfg.range_end_bit << " - 1 включительно\n"
              << ((g_cfg.mode == TRAV_RANDOM || g_cfg.mode == TRAV_ZONE
                    || g_cfg.mode == TRAV_WAVE || g_cfg.mode == TRAV_HYBRID)
                    // При случайном обходе никакого «начала» нет: участки
                    // выбираются наугад по всему диапазону. Строка «Начало:
                    // ...0001» создавала впечатление, что счёт пойдёт снизу.
                    ? " Границы       : " + to_hex(sb, 32) + "\n"
                      "                 " + to_hex(eb, 32) + "\n"
                    : " Начало        : " + to_hex(sb, 32) + "\n"
                      " Конец         : " + to_hex(eb, 32) + "\n");
    if (g_cfg.range_end_bit >= 256)
        std::cout << " Фактически    : последний допустимый ключ — n-1 (порядок secp256k1 минус 1)\n";
    std::cout << " HEX-позиций   : " << 64-g_top_ni << "; старшая принимает "
              << "0123456789abcdef"[g_top_vmin] << ".." << "0123456789abcdef"[g_top_vmax]
              << (g_cfg.det_hex>=0 ? " (выбрана только одна часть диапазона)\n" : " (det_hex=-1: весь диапазон)\n");
    if (g_cfg.mode==TRAV_WAVE)
        std::cout << " Волна         : один ниббл, затем два вместе, затем три и далее\n"
                  << " Приращения    : по модулю 16, без переноса; блоки без повторов\n"
                  << " Старт         : " << (g_cfg.wave_start.empty()
                        ? "новая целая база после каждого полного узора" : "первый узор от заданного ключа") << "\n"
                  << " Покрытие      : после исходных волн добор оставшихся участков; весь диапазон без повторов\n";
    if(g_cfg.mode==TRAV_WAVE && !g_cfg.wave_start.empty())
        std::cout << " Разрядности   : заданный старт сохраняет прежний порядок; чередование выключено\n";
    else if(g_cfg.mode==TRAV_WAVE || g_cfg.mode==TRAV_ZONE)
        std::cout << " Разрядности   : каждая непустая битовая часть получает целый узор по очереди\n"
                     " Баланс        : равные очереди целых узоров; время и число ключей зависят от их размера\n"
                     " Границы       : степени 2^x..2^y сохраняются, пустые и завершённые части пропускаются\n";
    bool resume_launch_confirmed = false;
    if (g_cfg.mode == TRAV_ZONE) {
        const double keys = std::pow(16.0, (double)g_cfg.zone_nibbles);
        char keys_text[32]; snprintf(keys_text, sizeof keys_text, "%.0f", keys);
        std::cout << " Зонный обход  : по одному старшему нибблу вокруг каждой базы\n"
                  << " Смена базы    : после всех исходных зон выбирается новая целая база\n"
                  << " Повторы       : окрестности разных баз не пересекаются\n"
                  << " Зонный блок   : 16^" << g_cfg.zone_nibbles << " = " << keys_text
                  << " ключей подряд (~" << zone_human_time(keys / ZONE_SPEED_ESTIMATE)
                  << " при 3 млрд/с)\n"
                  << " Границы       : проверяется только часть блока внутри диапазона\n"
                  << " Покрытие      : после исходных зон добор оставшихся участков без повторов\n"
                  << " Завершение    : после проверки всего выбранного диапазона\n";
    }
    if (g_cfg.mode == TRAV_WAVE || g_cfg.mode == TRAV_ZONE)
        std::cout << " Продолжение   : зерно и подтверждённые участки восстанавливаются из чекпоинта\n"
                     " Новый проход  : только при отказе от сохранённой позиции\n"
                     " HEX-таймер    : переход ждёт завершения начатых блоков, чтобы не повторять их\n";
    if (g_cfg.want_nk)
        std::cout << " Зеркальные цели: для каждого k проверяется n-k, в том числе вне диапазона.\n"
                     "                 Это не полный обход остальных ключей вне диапазона.\n";
    if ((g_cfg.mode == TRAV_SEQ || g_cfg.mode == TRAV_KXE || unique_blocks_mode()) && g_cfg.want_nk)
        std::cout << "                 При k и n-k внутри диапазона одна цель может\n"
                     "                 совпасть при дополнительной проверке зеркала ещё раз.\n";
    if (g_cfg.mode == TRAV_HYBRID) {
        const double keys = std::pow(16.0, (double)g_cfg.hybrid_nibbles);
        char keys_text[32]; snprintf(keys_text, sizeof keys_text, "%.0f", keys);
        std::cout << " Часть A       : весь допустимый префикс выбирается случайно и фиксируется\n"
                  << " Часть B       : " << g_cfg.hybrid_nibbles << " младших HEX-"
                  << zone_digit_word(g_cfg.hybrid_nibbles) << ", до " << keys_text
                  << " ключей подряд (~" << zone_human_time(keys / ZONE_SPEED_ESTIMATE)
                  << " при 3 млрд/с)\n"
                  << " Смена A       : только после полного перебора допустимой части B\n"
                  << " Новый запуск  : начинается с новой части A, прежняя позиция не загружается\n"
                  << " Границы       : каждый блок обрезается по заданному интервалу\n"
                  << " Исполнение    : один управляющий поток; GPU использует все рабочие потоки карты\n";
    }
    {
        interval_digits::Key lo{}, hi{};
        u256_to_be32(lo.data(), g_range_start); u256_to_be32(hi.data(), g_range_end);
        std::cout << " Допустимые HEX: позиции считаются справа, от 1 до 64;\n";
        for (int pos = 63; pos >= 0; ) {
            const uint16_t mask = interval_digits::allowed_digits(lo, hi, pos);
            int end = pos;
            while (end > 0 && interval_digits::allowed_digits(lo, hi, end - 1) == mask) --end;
            std::cout << "                 " << (64 - pos);
            if (end != pos) std::cout << ".." << (64 - end);
            std::cout << ": ";
            for (int v = 0; v < 16; ++v) if (mask & (1u << v)) std::cout << "0123456789abcdef"[v];
            std::cout << "\n";
            pos = end - 1;
        }
        std::cout << "                 Допустимость полного ключа проверяется по обеим границам.\n";
        if (mask_active())
            std::cout << "                 Выбранная HEX-маска дополнительно ограничивает эти цифры.\n";
    }
    if (g_cfg.mode == TRAV_WAVE)
        std::cout << " Блок волны    : 16^" << g_cfg.wave_nibbles << " = "
                  << fmt_num(UINT64_C(1) << (4*g_cfg.wave_nibbles)) << " ключей\n";
    else if (g_cfg.mode != TRAV_ZONE && g_cfg.mode != TRAV_HYBRID)
        std::cout << " Участок       : 2^" << g_cfg.chunk_bits << " ключей\n";
    std::cout
              // Название, а не номер: номер пункта меню и значение в
              // config.ini -- разные нумерации, и печатать здесь любую из них
              // значит подсказывать не то в половине случаев.
              << " Режим         : " << mode_title(g_cfg.mode)
              << "  (в config.ini: mode=" << mode_name(g_cfg.mode) << ")\n"
              << " Старший разряд: " << (g_cfg.det_hex >= 0
                    ? std::string(1, "0123456789abcdef"[g_cfg.det_hex]) +
                      "  (позиция " + std::to_string(g_top_ni) + ")"
                    : std::string("все значения")) << "\n"
              // КАКОЙ БИНАРНИК СЕЙЧАС РАБОТАЕТ.
              //
              // Без этих двух строк отличить свежую сборку от вчерашней можно
              // было только по косвенным признакам -- по формулировке в шапке
              // меню. Один прогон уже был принят за проверку исправлений,
              // хотя выполнялся прежний бинарник. Дата сборки и backend
              // снимают вопрос сразу.
              // ПЕРЕНОСИМОСТЬ ВИДНА СРАЗУ.
              //
              // Бинарник часто копируют на другую машину вместе с базой. Если
              // он собран с подстройкой под процессор сборочной машины, на
              // чужой он может умереть с «Illegal instruction» -- мгновенно и
              // без объяснений. Узнать об этом лучше здесь, на своей машине,
              // чем там, где ничего не выяснить.
              //
              // Замер, чтобы решение было осознанным: на этом горячем цикле
              // подстройка не даёт ничего (60 против 58 млн/с -- шум). Цикл
              // состоит из 64-битных умножений и сложений, векторизовать там
              // нечего, поэтому переносимая сборка ничего не стоит.
              << " Собрано       : " << __DATE__ << " " << __TIME__
              << (PKH_NATIVE_BUILD
                    ? "  (под ЭТОТ процессор — на другой машине может не пойти)"
                    : "  (переносимая — пойдёт на любой машине той же системы)")
              << "\n"
              << " Математика    : "
#ifdef USE_SECP
              << "libsecp256k1 (быстрее)"
#else
              << "своя (медленнее примерно вдвое)"
#endif
              << "\n"
              << " Зеркало n-k   : " << (g_cfg.want_nk ? "включено, в том числе вне диапазона" : "выключено") << "\n"
              << (g_cfg.mode == TRAV_HYBRID
                    ? std::string(" Журнал A | B  : ") + (g_cfg.hybrid_debug ? "hybrid_debug.csv" : "выключен") + "\n"
                    : std::string(" Журнал участков: ") + (g_cfg.debug_chunks ? "debug_chunks.txt" : "выключен") + "\n")
              << " Telegram      : " << (g_cfg.tg_token.empty() ? "выключен"
                                        : (g_cfg.tg_progress ? "включён + статистика" : "включён")) << "\n"
              << " Signal        : " << (!signal_enabled() ? "выключен"
                                        : (g_cfg.signal_progress ? "включён + статистика" : "включён")) << "\n"
              << " Находки       : приватный ключ в Found-файле; архив без пароля\n"
              << " Покрытие      : " << coverage_line() << "\n"
              << std::string(60, '=') << C_RESET << "\n\n";

    // Resume is tied to the effective targets that this process will search.
    // Load once here, before offering a saved position; main reuses this database.
    single_instance_check_stop();
    if (ui_preloaded_database) std::cout << "  База целей сохранена в памяти; используется проверенный неизменяемый снимок.\n";
    else {
        std::cout << "  Загрузка базы...\n";
        if (!g_db.load(g_cfg.pubkey_path)) return false;
    }
    single_instance_check_stop();
    std::string checkpoint_why;
    if (!ui_preloaded_database && !checkpoint_prepare_identity(g_db, checkpoint_why)) {
        single_instance_check_stop();
        std::cout << "  Не удалось подготовить поиск: " << checkpoint_why << "\n";
        return false;
    }
    single_instance_check_stop();

    if (g_cfg.mode == TRAV_HYBRID) {
        std::string why;
        if (!hybrid_mode_initialize(why)) {
            std::cout << "  Гибридный поиск не подготовлен: " << why << "\n";
            return false;
        }
        hybrid_mode_describe();
    } else if (mode_has_checkpoint()) {
        ChunkIdx seq;
        // ИМЯ ФАЙЛА НАЗЫВАЕТСЯ ВСЕГДА, А НЕ ТОЛЬКО ПРИ УДАЧЕ.
        //
        // Раньше при отсутствии сохранённой позиции программа молчала. Со
        // стороны это неотличимо от «продолжение не сработало»: счёт пошёл с
        // нуля, а почему -- неизвестно. Между тем причина чаще всего простая
        // и правильная: сменили диапазон, размер участка или старший разряд --
        // значит, это ДРУГАЯ задача, и файл у неё свой.
        ChunkIdx zero;
        auto fresh_shape_start=[&]() {
            if(!unique_blocks_mode())return;
            checkpoint_clear_loaded_shape();g_chunk_seq.set(zero);
            checkpoint_reset(g_search,zero);g_search.resumed_items=0;
            wave_set_seed((uint64_t)std::chrono::steady_clock::now().time_since_epoch().count()^UINT64_C(0x5deece66d));
            compute_total_blocks256();
        };
        const bool loaded=load_checkpoint(seq);
        if (!loaded || (seq == zero && !unique_blocks_mode())) {
            fresh_shape_start();
            std::cout << "  Чекпоинт: " << checkpoint_path() << " — новый, счёт с начала.\n"
                      << "  У каждой базы целей и сочетания параметров обхода\n"
                      << "  свой файл: сменив их, вы получите отдельный чекпоинт,\n"
                      << "  а прежний останется цел.\n\n";
        } else {
            // Номер участка 256-битный, поэтому печатается шестнадцатерично, а
            // «пройдено ключей» считается только когда оно осмысленно помещается
            // в разрядность вывода.
            // ЧТО ЭТОТ НОМЕР ЗНАЧИТ -- ПИШЕТСЯ РЯДОМ.
            //
            // Раньше печатался только 256-битный номер участка в HEX. Прочесть
            // его нельзя: это ни ключ, ни проценты, ни количество, а человек
            // видит строку из шестидесяти четырёх знаков и должен как-то
            // соотнести её с «почти 20%», которые помнит с прошлого запуска.
            // Считаем за него -- теми же функциями, которыми пойдёт обход,
            // чтобы показ не мог разойтись с делом.
            std::cout << "  Сохранённая позиция: участок " << seq.hex() << "\n"
                      << "  Файл: " << checkpoint_path() << "\n";
            {
                u256 si; seq.to_u256(si);

                // Всё считается в 256 битах и печатается через u256_to_dec.
                //
                // Было через long double с приведением к unsigned long long:
                // при диапазоне [2^0, 2^256) участков выходит порядка 2.8e70,
                // в 64 бита это не лезет, и на экран попадало
                // «18446744073709551615» -- предел типа, выданный за число
                // участков. Мусор, похожий на данные, хуже отсутствия строки.
                u256 done;
                const bool exact_done = checkpoint_completed_keys(seq, done);
                if(unique_blocks_mode()&&!exact_done)
                    throw std::runtime_error("В чекпоинте отсутствует подтверждённый счётчик ключей");
                if (!exact_done) { u256 ck; u256_from_u64(ck, chunk_keys());
                  // done = si * chunk_keys(), в 256 битах
                  u256_zero(done);
                  u128 c = 0;
                  for (int i = 0; i < 4; i++) {
                      u128 v = (u128)si[i] * ck[0] + c;
                      done[i] = (uint64_t)v; c = v >> 64; } }

                std::cout << "  Это участков  : " << u256_to_dec(si);
                if(!unique_blocks_mode())std::cout << "  (по " << chunk_keys() << " ключей)";
                std::cout << "\n  " << (!exact_done ? "Номинально    : " :
                                      mask_active() ? "Кандидатов    : " : "Ключей        : ")
                          << u256_to_dec(done);

                // ДОЛЯ ПУТИ ОСМЫСЛЕНА НЕ ВЕЗДЕ.
                //
                // У выборочных режимов процент полного диапазона не отражает
                // завершение выбранных окрестностей. Печатали его потому, что
                // формула считалась одна на
                // все режимы -- ровно та ошибка, за которую строку «Пройдено»
                // уже убирали из выборочных режимов на экране.
                const bool full = traversal_full_coverage();
                if (full) {
                    long double tot;
                    if (mask_active()) tot = u256_to_ld(g_mask_total);
                    else { u256 sp; u256_sub(sp, g_range_end, g_range_start);
                           tot = u256_to_ld(sp) + 1.0L; }
                    if (tot < 1.0L) tot = 1.0L;
                    // Зажим сверху, как в print_progress (notify.cpp). Без него
                    // чекпоинт от прогона с бо́льшим chunk давал «10345.6789%
                    // пути»: соседняя ветка это уже умела, а эта -- нет.
                    long double d = u256_to_ld(done);
                    if (d > tot) d = tot;
                    char pc[64];
                    snprintf(pc, sizeof pc, "%.4Lf", d / tot * 100.0L);
                    std::cout << "  — " << pc << "% пути";
                }
                std::cout << "\n";
                if (!full) {
                    if (unique_blocks_mode())
                        std::cout << "  Доли покрытия нет : конечное семейство окрестностей проверяет выборку диапазона\n";
                    else
                        std::cout << "  Доли пути нет : режим выборочный, конца у обхода не бывает\n";
                }

                u256 k0; uint64_t cnt = 0;
                int preview_zone = -1;
                std::mt19937_64 preview_rng(0);
                if (full && next_chunk(seq, k0, cnt, preview_zone, preview_rng) && cnt) {
                    uint8_t kb[32]; u256_to_be32(kb, k0);
                    std::cout << "  Продолжим с   : " << to_hex(kb, 32) << "\n";
                }
            }
            if (ask_yes_no("Продолжить с этой позиции?", true)) {
                resume_launch_confirmed = true;
                // Водяной знак завершённости тоже переносится: иначе первый
                // же chunk_done(seq+3) лёг бы в множество "досчитано выше",
                // знак остался бы на нуле, и следующий чекпоинт откатил бы
                // позицию в самое начало.
                if(unique_blocks_mode()) {
                    ChunkIdx loaded_seq;u256 confirmed;
                    if(!checkpoint_loaded_shape(loaded_seq,confirmed)||!(loaded_seq==seq))
                        throw std::runtime_error("Не восстановлено состояние подтверждённых участков");
                    apply_shape_checkpoint_state(g_search,seq,confirmed);
                    checkpoint_clear_loaded_shape();
                } else {
                g_chunk_seq.set(seq);
                checkpoint_reset(g_search, seq);
                // Чтобы «Пройдено» означало весь путь, а не только этот запуск.
                { u256 completed;
                  g_search.resumed_items = checkpoint_completed_keys(seq, completed)
                      ? u256_to_ld(completed) : 0.0L; }
                }
            } else {
                fresh_shape_start();
            }
        }
    }

    // ПЕРЕСЕЧЕНИЕ МАСКИ С ИНТЕРВАЛОМ НАЗЫВАЕТСЯ ДО СТАРТА.
    //
    // Маска и границы задаются порознь, и согласовать их легко забыть: ключ
    // с головой "c69f..." просто не существует в [2^39, 2^40). Без этой
    // проверки программа стартовала бы, показывала скорость, считала
    // проценты -- и не проверила бы НИ ОДНОГО ключа, ничем себя не выдав.
    if (mask_active()) {
        const bool empty = (g_mask_total[0] | g_mask_total[1]
                          | g_mask_total[2] | g_mask_total[3]) == 0;
        if (empty) {
            std::cout << "\n  МАСКА И ИНТЕРВАЛ НЕ ПЕРЕСЕКАЮТСЯ: проверять нечего.\n"
                      << "  Ключ  " << g_cfg.mask_hex << "\n"
                      << "  с шагом 16^" << g_cfg.mask_froz
                      << " не даёт ни одного значения внутри [2^"
                      << g_cfg.range_start_bit << ", 2^" << g_cfg.range_end_bit << ").\n"
                      << "  Поменяйте границы либо сам ключ.\n\n";
            return false;
        }
        uint8_t fb[32]; u256_to_be32(fb, g_mask_first);
        std::cout << "\n  Маска         : " << mask_layout(g_cfg.mask_hex, g_cfg.mask_froz) << "\n"
                  << "                  " << mask_legend(g_cfg.mask_froz) << "\n"
                  << "  Первый ключ   : " << to_hex(fb, 32) << "\n"
                  << "  Кандидатов    : " << u256_to_dec(g_mask_total)
                  << "  (шаг 16^" << g_cfg.mask_froz << ")\n"
                  << "  Покрытие      : все ключи с этим хвостом внутри интервала,\n"
                  << "                  а не весь интервал -- это решётка, не отрезок.\n\n";
    }

    if (!resume_launch_confirmed && !ask_yes_no("Начать поиск?", true)) return false;
    single_instance_check_stop();
    if (!ui_noninteractive) save_config_ini(g_cfg);
    return true;
}

// ========================================================
