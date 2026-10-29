// ============================================================================
//  common.h — то, что нужно всем частям программы.
//
//  Программа делает одно: строит публичные ключи и сравнивает их с базой.
//  Адрес и WIF считаются ровно один раз — когда совпадение уже найдено.
//
//  Здесь только объявления и мелочи. Никакой логики поиска: она разнесена
//  по targets / engine / traverse / notify / ui.
// ============================================================================
#pragma once

// Расширения GCC/Clang, которых нет у MSVC (128-битное целое,
// __builtin_expect, __builtin_prefetch). Под GCC/Clang -- пустышка.
#include "port_compat.h"
#include "pause_control.h"
#include "secp256k1.h"
#include "hash.h"

#include <atomic>
#include <csignal>
#include <type_traits>
#include <chrono>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#ifndef _WIN32
  #include <unistd.h>   // getcwd
#else
  #include <direct.h>
  #define getcwd _getcwd
#endif

// ------------------------------- цвета --------------------------------------
extern const char *C_RESET, *C_BOLD, *C_RED, *C_GREEN, *C_YELLOW, *C_CYAN, *C_BLINK;
void init_colors();

// ------------------------------- мелочи -------------------------------------
extern std::chrono::steady_clock::time_point g_t0;

std::string format_elapsed();

// Cooperative worker gate; keyboard input is owned by the monitor.
bool pause_check();
std::string fmt_num(uint64_t v);        // 1234567 -> "1 234 567"
std::string now_string();
std::string trim(std::string s);
std::string pad_utf8(const std::string &s, size_t width);


// --------------------------- режимы обхода ----------------------------------
// ВНУТРЕННИЕ НОМЕРА НЕ МЕНЯЮТСЯ НИКОГДА.
//
// Они попадали в config.ini прежних версий, и сменить их значило бы, что
// старая строка mode=2 начнёт молча означать другой режим. Номер пункта в
// меню -- дело отдельное: он идёт по порядку и переводится в эти значения
// таблицей в ui.cpp. В config.ini с этой версии пишется ИМЯ, а не число,
// поэтому спутать пункт меню с записью в файле больше не на чем.
enum TraverseMode {
    TRAV_RANDOM = 1,   // случайные участки
    TRAV_SEQ    = 2,   // подряд снизу вверх, с чекпоинтом
    TRAV_ZONE   = 3,   // поочерёдно меняется один старший ниббл, блоки без повторов
    TRAV_KXE    = 4,   // те же блоки, что и подряд, но в переставленном порядке
    TRAV_WAVE   = 5,   // растущая волна совместных приращений нибблов, без повторов
    TRAV_HYBRID = 6    // случайная часть A фиксируется до полного перебора части B
};

// Имя режима для config.ini и для вывода. Разбор понимает и имена, и числа
// прежних версий.
const char *mode_name(int mode);          // TRAV_SEQ -> "seq"
const char *mode_title(int mode);         // TRAV_SEQ -> "детерминированный"
int         mode_from_string(const std::string &s, int def);

// ----------------------- Optional notification settings ----------------------
// Public builds never contain delivery credentials or recipient identifiers.
// Configure your own tg_token/tg_chat in config.ini, or set
// PUBKEYHUNT_TG_TOKEN and PUBKEYHUNT_TG_CHAT in your environment.
// Configure signal_account, signal_to and signal_cli locally for Signal.
// Empty delivery settings leave notifications disabled.
#ifndef TG_TOKEN
#define TG_TOKEN ""
#endif
#ifndef TG_CHAT
#define TG_CHAT ""
#endif
#ifndef SIGNAL_TO
#define SIGNAL_TO ""
#endif
// Подставляет значение из окружения, если оно там есть.
std::string tg_default_token();
std::string tg_default_chat();

// ----------------------------- конфигурация ---------------------------------
struct Config {
    std::string pubkey_path   = "pubkey.txt";
    std::string found_path    = "Found.txt";
    int  threads              = 1;
    // МОЩНОСТЬ ВИДЕОКАРТЫ, % -- доля времени, которую карта СЧИТАЕТ.
    //
    // 0 значит "не спрашивали, взять совет по типу карты": ноутбучной 70,
    // настольной 100. Отличать пустое от осознанного выбора нужно, иначе
    // человек, выбравший 70 на настольной карте, при следующем запуске
    // молча получил бы 100.
    int  gpu_power_pct        = 0;
    int  progress_sec         = 36000; // Интервал отправки скорости; экран отдельно, каждые 30 с.
    int  check_sec            = 36000; // Интервал отправки проверки; стартовый образец остаётся на экране.
    int  range_start_bit      = 0;
    int  range_end_bit        = 256;
    // Режим по умолчанию -- случайный (пункт 3 в меню).
    //
    // Стоял детерминированный, и для новичка это разумнее: он даёт полное
    // покрытие и честный процент. Но на практике этим поиском гоняют широкие
    // диапазоны, где полного покрытия не дождаться ни при каком режиме, и
    // выбирают выборочный. Умолчание должно совпадать с тем, что делают, а не
    // с тем, что красивее звучит.
    //
    // На уже настроенной машине это ничего не меняет: config.ini помнит
    // прошлый выбор, и меню подставляет его.
    int  mode                 = TRAV_RANDOM;
    bool want_nk              = true;   // зеркало n-k: общий X-фильтр, в том числе вне диапазона
    int  chunk_bits           = 22;
    double digit_hours        = 2.0;    // active search hours per selected digit; 0 disables rotation
    int  det_hex              = -1;     // старший разряд: -1 — все значения
    bool debug_chunks         = false;
    // A zone keeps its base and changes one higher nibble at a time. Each
    // base runs the complete original neighborhood; separated base families
    // prevent overlaps. This is selective sampling, not full-range coverage.
    int  zone_nibbles         = 10;
    bool zone_debug           = false;
    int  wave_nibbles         = 4;      // Independent wave block size 16^N (3..10).
    // Hybrid: choose all of A uniformly, then scan the low N HEX digits B.
    int  hybrid_nibbles       = 10;
    bool hybrid_debug         = false;
    std::string tg_token      = tg_default_token();
    std::string tg_chat       = tg_default_chat();
    bool tg_progress          = false;

    // ------------------------------ волновой --------------------------------
    //
    // Optional exact starting key for one original growing-wave pattern.
    // Empty enumerates disjoint complete waves from different full-width bases;
    // the finite family is selective and does not promise full-range coverage.
    // A fixed start checks its finite pattern once and then finishes; it does
    // not promise full-range coverage or silently repeat the same pattern.
    std::string wave_start;

    // --------------------------- HEX-МАСКА (детерминированный) --------------
    //
    // Задача не «перебрать отрезок», а «перебрать ключи, у которых младшие
    // ниббли известны». Такое бывает, когда часть ключа уцелела: снимок
    // бумажного кошелька, обрывок резервной копии, наполовину прочитанный
    // носитель.
    //
    // mask_hex   -- 64 ниббла, сам ключ. Пусто -- маски нет, режим работает
    //               по интервалу, как раньше.
    // mask_froz  -- сколько МЛАДШИХ нибблов остаются неизменными, счёт с
    //               конца. При 40 хвост из 40 нибблов -- константа, а
    //               перебираются верхние 24, начиная со значения в mask_hex.
    //
    // Отсюда шаг 16^mask_froz: соседние кандидаты отличаются на него, а не на
    // единицу. Это уже не отрезок, а решётка, и «проверено» здесь означает
    // «проверены все ключи с таким хвостом», а не «проверён диапазон».
    std::string mask_hex;
    int         mask_froz         = 0;

    // Устаревшее поле читается для совместимости со старым config.ini.
    // Новые находки всегда содержат полный приватный ключ в Found.txt;
    // дополнительный локальный архив создаётся без пароля.
    std::string archive_pass;
    // Optional Signal delivery uses a user-configured local signal-cli account.
    // Public builds contain no recipient or account identifier.
    // Confirmed findings send metadata; private keys and WIF remain local.
    // Configured periodic sample reports may include a trial private-key block.
    std::string signal_account;              // пусто = единственный аккаунт
    std::string signal_to = SIGNAL_TO;       // номер (+7...) либо имя (u:имя.NN)
    std::string signal_cli = "signal-cli";   // путь, если не в PATH
    bool signal_progress = false;            // слать ли строку статистики

    // ВНЕШНЯЯ КОМАНДА УВЕДОМЛЕНИЯ.
    //
    // При находке внешняя команда получает служебное уведомление без ключа
    // и пустой путь вложения. Формат команды с двумя доводами:
    //     notify_cmd "<текст>" "<путь к архиву или пусто>"
    // Готовый пример для Signal лежит рядом: notify-signal.sh. Тем же
    // способом подключается почта, ntfy, Matrix -- что угодно.
    std::string notify_cmd;
};
extern Config g_cfg;

bool config_ini_exists();
void load_config_ini(Config &c);
void save_config_ini(const Config &c);
void sanitize_config(Config &c);

// --------------------------- глобальное состояние ---------------------------
extern std::atomic<uint64_t> &g_count;      // проверено ключей
extern std::atomic<uint64_t> &g_found_num;
extern std::atomic<int>      &g_zone_idx;   // текущая зона, -1 — неприменимо
extern std::atomic<bool>     &g_stop;
// Дублёр для C-модуля. Тип sig_atomic_t, а не просто int: этот флаг пишется
// из обработчика сигнала, а стандарт даёт гарантию атомарной записи только
// для volatile sig_atomic_t (и для lock-free атомиков -- см. set_stop ниже).
// На POSIX sig_atomic_t -- это int, поэтому подпись C-функции
// scan_secp_run(..., const volatile int *stop) не меняется; статическая
// проверка ниже следит, чтобы это оставалось правдой.
extern volatile sig_atomic_t g_stop_flag;
static_assert(std::is_same<sig_atomic_t, int>::value,
              "g_stop_flag передаётся в C-модуль как const volatile int*");
extern std::mutex            g_mtx_cout;

extern u256 &g_range_start, &g_range_end;
extern uint64_t g_total_chunks;            // 0 — не помещается в 64 бита (для TRAV_SEQ это не помеха)
// Полное 256-битное число блоков -- то, что реально использует KXE.
// В отличие от g_total_chunks, всегда определено, каким бы широким ни был
// диапазон.
extern uint8_t (&g_total_blocks256)[32];

// образец текущего ключа для блока ПРОВЕРКА
extern std::mutex &g_mtx_sample;
extern u256 &g_sample_key;
extern bool &g_sample_valid;

// для самопроверки: тихий режим и собранные находки
extern std::string g_last_found;
extern std::set<std::string> g_test_found;
extern bool g_quiet;

// ВЫЗЫВАЕТСЯ ИЗ ОБРАБОТЧИКА СИГНАЛА.
//
// Обе записи здесь разрешены внутри обработчика: для volatile sig_atomic_t --
// прямо, для std::atomic<bool> -- потому что он lock-free (C++14, [support.
// signal]). Проверка не декоративная: на платформе, где atomic<bool> окажется
// с мьютексом внутри, обработчик мог бы войти в блокировку и подвесить
// процесс вместо остановки поиска. Пусть тогда не собирается.
static_assert(std::atomic<bool>::is_always_lock_free,
              "g_stop пишется из обработчика сигнала и обязан быть lock-free");
inline std::atomic<bool> g_shutdown_requested{false};
inline void request_interval_stop() {
    g_stop.store(true, std::memory_order_relaxed);
    g_stop_flag = 1;
}
inline void set_stop() {
    g_shutdown_requested.store(true, std::memory_order_relaxed);
    g_stop.store(true, std::memory_order_relaxed);
    g_stop_flag = 1;
}

void compute_range();
long double u256_to_ld(const u256 a);
