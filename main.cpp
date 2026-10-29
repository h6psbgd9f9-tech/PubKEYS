#include "worker_threads.h"
#include "minute_speed.h"
#include "search_completion.h"
#include "search_lifecycle.h"
#include "search_status_sample.h"
#include <future>
#include "digit_rotation.h"
// ============================================================================
//  PubkeyHunt — поиск приватного ключа по публичному ключу.
//
//  Поиска ПО АДРЕСУ в программе нет. Он был на раннем этапе, потом его
//  убрали: сравнение по адресу требует SHA-256 и RIPEMD-160 на каждый ключ и
//  режет скорость примерно вшестеро. Упоминания в заголовках и в меню от той
//  поры оставались и обещали то, чего программа не делает.
//
//      приватный ключ k  ->  k*G  ->  (X, Y)  ->  04||X||Y  либо 02/03||X
//                                                      |
//                                         сравнение с базой
//                                                      |
//                                    совпадение -> адрес, WIF -> Found.txt
//
//  При поиске по публичному ключу хеширования в цикле нет вовсе: адрес
//  считается один раз, уже при находке.
//
//  Разбор по файлам:
//    common.h    общие типы, конфигурация, глобальное состояние
//    util.cpp    цвета, время, форматирование
//    config.cpp  config.ini
//    targets.*   база целей и предфильтр
//    engine.*    горячий цикл
//    traverse.*  нарезка диапазона, чекпоинт, журнал участков
//    notify.*    экран, Found.txt, Telegram
//    ui.*        оборудование, меню
//    selftest.*  проверки и замер
// ============================================================================
#include "common.h"
#include "targets.h"
#include "engine.h"
#include "traverse.h"
#include "interval_progress.h"
#include "packed_traversal.h"
#include "wave_gpu_batch.h"
#include "wave_gpu_prefetch.h"
#include "wave_gpu_parallel.h"
#include "shape_checkpoint_commit.h"
#include "shape_checkpoint_restore.h"
#include "zone_mode.h"
#include "hybrid_mode.h"
#include "search.h"
#include "notify.h"
#include "ui.h"
#include "tests/app/selftest.h"
#include "kxe_sass_backend.h"
#include "platform.h"
#include "single_instance.h"
#include "cli_launch_policy.h"
#include "pult_control.h"

#include <csignal>
#include <cstring>
#include <iostream>
#include <memory>
#include <random>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <shellapi.h>
#pragma comment(lib, "shell32.lib")
#endif
extern std::atomic<uint64_t> g_dbg_written;
#ifndef PKH_HAVE_CUDA
// The common console status also builds in a CPU-only application. No CUDA
// controller exists in that build, so its local status starts at full power.
static std::atomic<int> g_gpu_power_pct{100};
#endif
#ifdef USE_SECP
  #include "scan_secp.h"
#endif

#if defined(__APPLE__)
  #include <mach-o/dyld.h>
#endif
#ifndef _WIN32
  #include <unistd.h>
  #include <limits.h>
#endif

// MAIN
// ========================================================
// SIGTERM keeps the graceful shutdown path; console copy must not stop work.
static bool interactive_search = false;
// A loaded target snapshot can be reused only while its source file is pinned
// against writes/deletes. The handle is acquired before parsing, and every
// requested path is opened again to compare actual file identity before reuse.
class PultDatabaseSnapshot {
public:
    PultDatabaseSnapshot() = default;
    ~PultDatabaseSnapshot() { close(); }
    PultDatabaseSnapshot(const PultDatabaseSnapshot&) = delete;
    PultDatabaseSnapshot& operator=(const PultDatabaseSnapshot&) = delete;
    PultDatabaseSnapshot(PultDatabaseSnapshot&& other) noexcept { swap(other); }
    PultDatabaseSnapshot& operator=(PultDatabaseSnapshot&& other) noexcept {
        if (this != &other) { close(); swap(other); }
        return *this;
    }
    bool open(const std::string& path, std::string& error) {
#ifdef _WIN32
        close();
        handle_ = CreateFileW(plat_wide(path).c_str(), GENERIC_READ, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            error = GetLastError() == ERROR_SHARING_VIOLATION
                ? "База целей открыта для записи; закройте редактор базы и повторите применение настроек"
                : "Не удалось открыть неизменяемый снимок базы целей";
            return false;
        }
        if (!GetFileInformationByHandle(handle_, &identity_) ||
            (identity_.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || GetFileType(handle_) != FILE_TYPE_DISK) {
            error = "Не удалось проверить файл базы целей"; close(); return false;
        }
        const DWORD size = GetFinalPathNameByHandleW(handle_, nullptr, 0, FILE_NAME_NORMALIZED);
        if (!size) { error = "Не удалось определить путь снимка базы целей"; close(); return false; }
        std::vector<wchar_t> name(static_cast<size_t>(size) + 1);
        const DWORD written = GetFinalPathNameByHandleW(handle_, name.data(), static_cast<DWORD>(name.size()), FILE_NAME_NORMALIZED);
        if (!written || written >= name.size()) { error = "Не удалось определить путь снимка базы целей"; close(); return false; }
        canonical_path_.assign(name.data(), written);
        return true;
#else
        (void)path; (void)error;
        return false; // POSIX deliberately retains the full-reload path.
#endif
    }
    bool same_file(const PultDatabaseSnapshot& other) const {
#ifdef _WIN32
        return handle_ != INVALID_HANDLE_VALUE && other.handle_ != INVALID_HANDLE_VALUE
            && identity_.dwVolumeSerialNumber == other.identity_.dwVolumeSerialNumber
            && identity_.nFileIndexHigh == other.identity_.nFileIndexHigh
            && identity_.nFileIndexLow == other.identity_.nFileIndexLow
            && _wcsicmp(canonical_path_.c_str(), other.canonical_path_.c_str()) == 0;
#else
        (void)other; return false;
#endif
    }
private:
    void close() noexcept {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE; canonical_path_.clear(); identity_ = {};
#endif
    }
    void swap(PultDatabaseSnapshot& other) noexcept {
#ifdef _WIN32
        std::swap(handle_, other.handle_); std::swap(identity_, other.identity_);
        canonical_path_.swap(other.canonical_path_);
#else
        (void)other;
#endif
    }
#ifdef _WIN32
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    BY_HANDLE_FILE_INFORMATION identity_{};
    std::wstring canonical_path_;
#endif
};
struct PultRuntime {
    pult::Control control;
    pult::Settings pending;
    pult::Settings staged;
    pult::Settings accepted_request;
    bool has_accepted_request = false;
    bool apply_pending = false;
    bool staging = false;
    bool backend_prepared = false;
    bool initial_sample_written = false;
    bool manual_pause = false;
    bool completed = false;
    bool search_started = false;
    PultDatabaseSnapshot database_snapshot;
    bool database_loaded = false;
    bool backend_targets_ready = false;
    bool notifications_checked = false;
    uint64_t database_loads = 0, database_reuses = 0;
    uint64_t previous_count = 0;
    std::string error;
};

// Called by the owning main thread during preparation/after joins, or by its
// sole monitor while g_cfg is immutable. Failure affects resume metadata only.
static void pult_publish_resume(PultRuntime* panel) noexcept {
    if(!panel||!panel->database_loaded||!panel->has_accepted_request||panel->staging)return;
    try {
        Config effective=g_cfg;
        effective.gpu_power_pct=g_gpu_power_pct.load(std::memory_order_relaxed);
        const bool exact=mode_has_checkpoint();
        if(panel->control.export_resume_settings(effective,exact?checkpoint_search_identity():std::string{},
                exact?checkpoint_path():std::string{}))return;
    } catch(...) {}
    panel->control.invalidate_resume();
    std::fputs("  Не удалось сохранить параметры продолжения; поиск продолжается.\n",stderr);
}

// During a search only its monitor calls this. The owning thread can call it
// again after joining that monitor and all workers.
static void pult_dispatch(PultRuntime& panel, const pult::Command& command,
                          Config* remembered = nullptr, MinuteSpeed* speed = nullptr) {
    panel.control.acknowledge(command.revision);
    panel.error.clear();
    if (command.action == "stop") {
        panel.apply_pending = false;
        set_stop();
    } else if (command.action == "pause") {
        panel.manual_pause = true;
        g_pause.request();
        if (speed) speed->set_paused(true, true);
    } else if (command.action == "resume") {
        panel.manual_pause = false;
        g_pause.resume();
        if (speed) speed->set_paused(g_pause.automatic_requested());
    } else if (command.action == "power") {
        g_gpu_power_pct.store(command.power, std::memory_order_relaxed);
        if (remembered) remembered->gpu_power_pct = command.power;
        if (speed) speed->set_power(command.power);
        if (panel.apply_pending) panel.pending.config.gpu_power_pct = command.power;
        pult_publish_resume(&panel);
    } else if (command.action == "apply") {
        pult::Settings replacement;
        if (!panel.control.read_settings(command.settings_revision, panel.control.config(), replacement, panel.error)) {
            panel.control.reject_settings(command.settings_revision);
            return;
        }
        std::error_code ec;
        if (!std::filesystem::is_regular_file(std::filesystem::u8path(replacement.config.pubkey_path), ec)) {
            panel.error = "The selected public-key database does not exist";
            panel.control.reject_settings(command.settings_revision);
            return;
        }
#ifdef PKH_HAVE_CUDA
        const bool gpu_active = g_gpu_active;
#else
        const bool gpu_active = false;
#endif
        // Never coalesce while another requested configuration is pending:
        // queued B -> A must still replace pending B with the user's latest A.
        if (!panel.apply_pending && !panel.staging && panel.database_loaded && panel.has_accepted_request
            && pult::same_editable_settings(replacement.config, panel.accepted_request.config,
                panel.control.config(), g_gpu_power_pct.load(std::memory_order_relaxed), gpu_active)) {
            PultDatabaseSnapshot requested_database;
            std::string snapshot_error;
            if (requested_database.open(replacement.config.pubkey_path, snapshot_error)
                && panel.database_snapshot.same_file(requested_database)) {
                panel.control.acknowledge_noop(replacement.revision);
                // The next ordinary status publication acknowledges this
                // request without a fabricated zero-rate measurement.
                return;
            }
        }
        panel.pending = std::move(replacement);
        panel.apply_pending = true;
        request_interval_stop();
    }
}

static bool pult_write_initial_sample(PultRuntime& panel) {
    const std::string sample = search_status_sample();
    if (sample.empty()) return false;
    u256 key; if (!hex_to_u256(sample.c_str(), key)) return false;
    const Point point = ec_mul_G(key); if (point.inf) return false;
    uint8_t x[32], y[32], compressed[33], uncompressed[65];
    fe_to_be32(x, point.x); fe_to_be32(y, point.y);
    compressed[0] = static_cast<uint8_t>(2 | (point.y[0] & 1));
    memcpy(compressed + 1, x, 32);
    uncompressed[0] = 4; memcpy(uncompressed + 1, x, 32); memcpy(uncompressed + 33, y, 32);
    return panel.control.initial_sample({{"private_key", sample}, {"x", to_hex(x,32)},
        {"y", to_hex(y,32)}, {"pubc", to_hex(compressed,33)}, {"pubu", to_hex(uncompressed,65)}});
}
class FoundWorkExitDrain {
public:
    ~FoundWorkExitDrain() noexcept {
        // Findings are already durable. Let their optional background copies
        // finish on normal return and exception paths without holding workers.
        try {
            wait_found_postprocessing(45000);
            wait_background_notifications(45000);
        } catch (...) {}
    }
};
static void app_signal_handler(int sig) {
    // MSVC resets a CRT signal handler after delivery.
    std::signal(sig, app_signal_handler);
#ifdef _WIN32
    if (sig == SIGINT) return;
#endif
    set_stop();
}
#ifdef _WIN32
#include "console_interrupt_guard.h"
#endif

// ============================================================================
//  РАБОЧИЙ КАТАЛОГ — ТАМ, ГДЕ ЛЕЖИТ ПРОГРАММА
//
//  Все пути в настройках относительные: pubkey.txt, Found.txt, config.ini,
//  чекпоинты, журнал. Отсчитывались они от каталога, ОТКУДА ЗАПУСТИЛИ, а не
//  от того, где программа лежит. Запуск из другой папки давал:
//
//      Не удалось открыть файл публичных ключей: pubkey.txt
//      Искали в каталоге: [application directory]
//
//  хотя pubkey.txt лежал рядом с самим бинарником. Запускалка это скрывала --
//  она первым делом делает cd в свою папку, -- поэтому при двойном щелчке всё
//  работало, а при запуске из терминала по пути ломалось.
//
//  ПРАВИЛО: СВОЯ ПАПКА ПЕРВАЯ, ВСЕГДА.
//
//  Первая версия этой правки была умнее и потому хуже: она переходила в свою
//  папку только если в текущей ничего подходящего нет. Стоило в домашнем
//  каталоге остаться config.ini от прежнего неудачного запуска -- и программа
//  оставалась там же, продолжая не находить базу. Ровно тот случай, когда
//  условие «если вдруг» превращает понятное поведение в непредсказуемое.
//
//  Теперь без условий: программа работает там, где лежит. Полный путь,
//  указанный в меню, от этого не страдает -- он абсолютный, и текущий каталог
//  на него не влияет.
// ============================================================================
// exe_dir и переход в свой каталог живут в platform.cpp: способ узнать путь
// к самой программе у трёх систем разный, а поведение должно быть одним.
static void chdir_to_program_if_needed() {
    const std::string dir = plat_exe_dir();
    if (dir.empty()) {
        std::cout << C_YELLOW
                  << "  Не удалось определить, где лежит программа —\n"
                     "  файлы будут искаться в текущем каталоге.\n" << C_RESET;
        return;
    }
    if (plat_chdir(dir))
        std::cout << "  Рабочий каталог: " << dir << "\n"
                     "  (программа работает там, где лежит; полный путь в меню\n"
                     "   по-прежнему можно указать любой)\n\n";
    else
        std::cout << C_YELLOW << "  Не удалось перейти в " << dir
                  << " — файлы будут искаться в текущем каталоге.\n" << C_RESET;
}

// Called only after all workers and the monitor have joined. Each digit keeps
// its existing checkpoint identity; a partially processed chunk is replayed.
static bool prepare_rotated_interval(const std::string& configured_wave_start,
                                     const VolatileIntervalProgress& volatile_progress) {
    compute_range();
    if (g_cfg.mode == TRAV_WAVE && !configured_wave_start.empty()) {
        u256 fixed;
        if (!hex_to_u256(configured_wave_start.c_str(), fixed)) return false;
        uint8_t bytes[32]; u256_to_be32(bytes, fixed);
        std::string hex = to_hex(bytes, 32);
        hex[g_top_ni] = "0123456789abcdef"[g_cfg.det_hex];
        hex_to_u256(hex.c_str(), fixed);
        if (u256_cmp(fixed, g_range_start) < 0) u256_set(fixed, g_range_start);
        if (u256_cmp(fixed, g_range_end) > 0) u256_set(fixed, g_range_end);
        u256_to_be32(bytes, fixed); g_cfg.wave_start = to_hex(bytes, 32);
    }
    g_chunk_seq.set(ChunkIdx{});
    checkpoint_reset(g_search, ChunkIdx{});
    g_search.resumed_items = 0;
    g_zone_idx.store(-1);
    { std::lock_guard<std::mutex> lock(g_search.mtx_sample); g_search.sample_valid = false; }
    std::string why;
    bool restored = false;
    if (g_cfg.mode == TRAV_HYBRID) {
        if (!hybrid_mode_initialize(why)) { std::cerr << why << "\n"; return false; }
    } else {
        wave_set_seed((uint64_t)std::chrono::steady_clock::now().time_since_epoch().count());
        ChunkIdx seq;
        uint64_t saved_seed=0;u256 saved_checked{};
        const bool remembered=unique_blocks_mode()
            &&volatile_progress.recall_exact(g_cfg.det_hex,seq,saved_seed,saved_checked);
        if(remembered)wave_set_seed(saved_seed);
        if (remembered || load_checkpoint(seq)) {
            if(unique_blocks_mode()) {
                if(remembered)compute_total_blocks256();
                else {
                    ChunkIdx loaded;
                    if(!checkpoint_loaded_shape(loaded,saved_checked)||!(loaded==seq))
                        throw std::runtime_error("Не восстановлен подтверждённый счётчик обхода");
                }
                if(memcmp(seq.b,g_total_blocks256,32)>0)
                    throw std::runtime_error("Позиция продолжения вне текущего обхода");
                apply_shape_checkpoint_state(g_search,seq,saved_checked);
                checkpoint_clear_loaded_shape();
            } else {
            g_chunk_seq.set(seq); checkpoint_reset(g_search, seq);
            u256 completed;
            g_search.resumed_items = checkpoint_completed_keys(seq, completed) ? u256_to_ld(completed) : 0;
            }
            restored = true;
        }
    }
    // Shape family size must match the effective seed/start used by workers.
    // Sequential and KXE checkpoint paths retain their existing calculation.
    if (unique_blocks_mode()) compute_total_blocks256();
    // Preserve even a run interrupted before its first completed logical block.
    if (unique_blocks_mode()) save_checkpoint();
    std::cout << (restored ? "  Продолжаем с подтверждённой позиции цифры.\n" : "  Новая позиция для этой цифры.\n");
    uint8_t low[32], high[32]; u256_to_be32(low, g_range_start); u256_to_be32(high, g_range_end);
    std::cout << "  Границы: " << to_hex(low, 32) << " .. " << to_hex(high, 32) << "\n";
    return !g_stop.load();
}

static int print_command_line_help() {
    plat_console_utf8();
    printf("PubkeyHunt\n\n"
           "  ./pubkeyhunt                     интерактивное меню\n"
           "  ./pubkeyhunt pubkey.txt Found.txt   то же, с готовыми путями\n"
           "  ./pubkeyhunt --test              самопроверка\n"
           "  ./pubkeyhunt --bench [потоков] [целей]\n"
           "                                   замер скорости; целей по\n"
           "                                   умолчанию 1 -- для боевого\n"
           "                                   числа берите размер своей базы\n"
           "\n"
           "Настройки сохраняются в config.ini, позиция — в checkpoint_<режим>_<от>_<до>_<участок>.dat\n"
           "(у каждой конфигурации свой файл, они не мешают друг другу).\n");
    return 0;
}

// Windows main(char**) receives paths in the active ANSI code page. The panel
// writes UTF-8 settings and the filesystem APIs below expect UTF-8 too, so take
// its two path arguments directly from Windows' original Unicode command line.
static std::string pult_path_argument_utf8(int index, const char* fallback) {
#ifdef _WIN32
    int count = 0;
    LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!wide) throw std::runtime_error("Cannot read the panel's Unicode command line");
    if (index < 0 || index >= count) {
        LocalFree(wide);
        throw std::runtime_error("Missing panel path argument");
    }
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide[index], -1,
        nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        LocalFree(wide);
        throw std::runtime_error("Cannot convert the panel path to UTF-8");
    }
    std::string result(static_cast<size_t>(size), '\0');
    const int written = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide[index], -1,
        result.data(), size, nullptr, nullptr);
    LocalFree(wide);
    if (written != size) throw std::runtime_error("Cannot convert the panel path to UTF-8");
    result.pop_back();
    return result;
#else
    (void)index;
    return fallback ? fallback : "";
#endif
}

static int application_main(int argc, char **argv, PultRuntime* panel = nullptr) {
    const bool panel_restart = panel && panel->search_started;
    // После получения права запуска настраиваем кодировку консоли.
    //
    // Весь вывод программы кириллический. На Windows консоль по умолчанию в
    // 866 или 1251, и без этого вызова человек увидит не текст, а мусор -- и
    // не сможет прочитать даже сообщение о том, что пошло не так. На POSIX
    // вызов ничего не делает.
    plat_console_utf8();

    init_colors();
    // sync_with_stdio НЕ отключаем: в программе есть и printf, и std::cout,
    // с раздельными буферами их вывод перемешивается не в том порядке
    // (сообщение о загрузке базы уезжало в самый конец сеанса).
    g_t0 = std::chrono::steady_clock::now();

    // Install interruption protection before preparing the search or menu.
    signal(SIGINT, app_signal_handler);
#ifdef SIGTERM
    signal(SIGTERM, app_signal_handler);
#endif
#ifdef _WIN32
    if (!SetConsoleCtrlHandler(ignore_copy_interrupt, TRUE)) {
        std::cerr << "Cannot install console stop handler: " << GetLastError() << "\n";
        return 1;
    }
#endif

    // Делается ДО разбора доводов и до меню: иначе относительные пути из
    // config.ini уже были бы истолкованы не от того каталога.
    if (!panel) chdir_to_program_if_needed();

    if (!panel || !panel->backend_prepared) {
    build_table();
#ifdef USE_SECP
    if (!scan_secp_init()) {
        std::cerr << "Не удалось построить таблицы libsecp256k1\n";
        return 1;
    }
#endif

#ifdef PKH_HAVE_CUDA
    // ЗАПУСК КАРТЫ -- ДО МЕНЮ.
    //
    // Порядок важен для человека, а не для машины: меню печатает шапку
    // "ОБОРУДОВАНИЕ" и советует число потоков. Пока опрос карты стоял ПОСЛЕ
    // меню, совет был про процессор ("24 потока"), а через минуту программа
    // сообщала, что потоков будет 1 и считает карта. Совет устаревал раньше,
    // чем его дочитывали.
    //
    // Шаг перебора здесь ещё не известен -- HEX-маска задаётся в меню, --
    // поэтому он уходит на карту отдельно, из set_scan_step.
    {
        std::string why;
        if (!gpu_backend_start(why))
            std::cout << "  Видеокарта не задействована: " << why << "\n"
                      << "  Счёт пойдёт на процессоре.\n\n";
        else
            // Logical zone/wave blocks stay short; the worker packs many of
            // them into one GPU launch. CPU startup retains its 4096-key block.
            // Set before the menu computes traversal/checkpoint identity.
            set_block_nibbles(g_cfg.wave_nibbles);
    }
#endif

    if (panel) panel->backend_prepared = true;
    }
    single_instance_check_stop();
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--test") return self_test();
        if (a == "--range-test") return range_self_test();
        if (a == "--strict-range-test") return strict_range_self_test();
        if (a == "--bench") {
            int th = (i + 1 < argc && argv[i+1][0] != '-') ? atoi(argv[++i]) : 1;
            if (th < 1) th = 1;
            // Второе число -- сколько целей положить в базу. Без него замер
            // идёт с одной целью, то есть по короткому пути предфильтра, и
            // показывает скорость, недостижимую на боевой базе.
            uint64_t nt = 1;
            if (i + 1 < argc && argv[i+1][0] >= '0' && argv[i+1][0] <= '9')
                nt = strtoull(argv[++i], nullptr, 10);
            if (nt < 1) nt = 1;
            return bench(th, 5, nt);
        }
    }

    single_instance_check_stop();
    interactive_search = !panel;
    ui_set_noninteractive(panel != nullptr);
    PultDatabaseSnapshot opening_database;
    bool reuse_database = false;
    if (panel) {
#ifdef _WIN32
        if (!opening_database.open(g_cfg.pubkey_path, panel->error)) {
            std::cout << "  " << panel->error << "\n";
            return 1;
        }
        reuse_database = panel->database_loaded && panel->database_snapshot.same_file(opening_database);
#endif
        if (!reuse_database) {
            // PubDB::load appends. Invalidate the cache before releasing its
            // memory; a failed replacement must force a safe rollback reload.
            panel->database_loaded = false;
            panel->backend_targets_ready = false;
            g_db = PubDB{};
        }
        panel->control.status(panel->staging ? "applying" : "starting", panel->previous_count,
            g_found_num.load(), g_gpu_power_pct.load(), 0, {}, panel->error,
            reuse_database ? "preparing_mode" : "loading_database",
            reuse_database ? "База целей уже подготовлена; применяются параметры режима"
                           : "Загрузка и проверка базы публичных ключей");
    }
    ui_set_preloaded_database(reuse_database);
    pult::Settings requested_settings;
    if (panel) requested_settings = panel->staging ? panel->staged
        : panel->has_accepted_request ? panel->accepted_request : panel->control.settings();
    if (!run_startup_menu(argc, argv)) {
        std::cout << "Отменено.\n";
        return panel ? 1 : 0;
    }
    if (panel) {
        if (!panel->has_accepted_request || panel->staging) {
            panel->accepted_request = requested_settings;
            panel->has_accepted_request = true;
        }
        if (reuse_database) ++panel->database_reuses;
        else {
            panel->database_snapshot = std::move(opening_database);
            panel->database_loaded = true;
            ++panel->database_loads;
        }
        auto effective = panel->staging ? panel->staged : panel->control.settings();
        effective.config = g_cfg;
        if (panel->staging) panel->initial_sample_written = false;
        panel->staging = false;
        panel->control.applied(effective);
        panel->control.status(panel_restart ? "applying" : "starting", panel->previous_count,
            g_found_num.load(), g_gpu_power_pct.load(), 0, {}, panel->error,
            "preparing_mode", "Подготовка выбранного режима и данных видеокарты");
        pult::Command command; std::string error;
        while (panel->control.next_command(command, error)) {
            pult_dispatch(*panel, command);
            if (g_shutdown_requested.load() || panel->apply_pending) return 0;
        }
        if (!error.empty()) panel->error = error;
    }

    single_instance_check_stop();
    archive_warn_weak();   // пока ещё можно поставить 7z и перезапустить


    // The menu loads and fingerprints the actual database before checkpoint
    // continuation is offered. Reuse that immutable database here.
    if (!panel || !panel->backend_targets_ready) {
        if (!push_targets_to_backend()) return 1;
        if (panel) panel->backend_targets_ready = true;
    }
    single_instance_check_stop();

    if (panel) panel->control.status(panel_restart ? "applying" : "starting", panel->previous_count,
        g_found_num.load(), g_gpu_power_pct.load(), 0, {}, panel->error,
        "checking_notifications", "Проверка настроенных каналов уведомлений");
    if ((!panel || !panel->notifications_checked) && !g_cfg.tg_token.empty()) {
        std::cout << "  Проверяю связь с Telegram...\n";
        if (!tg_check() && !ask_yes_no("Продолжить без уведомлений?", false)) return 1;
    }

    // Check notification availability before scanning starts. Found records
    // and password-free archives remain local; channels receive only a notice.
    single_instance_check_stop();
    if ((!panel || !panel->notifications_checked) && signal_enabled()) {
        std::cout << "  Проверяю связь с Signal...\n";
        // Remember readiness without testing the connection on a finding.
        const bool sig_ok = signal_check();
        signal_set_ready(sig_ok);
        if (!sig_ok && !ask_yes_no("Продолжить без уведомлений Signal?", false)) return 1;
        if (!sig_ok)
            std::cout << "  Signal недоступен; локальное сохранение находок продолжит работать.\n";
    }
    if (panel) {
        panel->notifications_checked = true;
        // Signal may normalize its executable path during the initial check.
        // Keep that already checked route in subsequent panel snapshots.
        auto checked = panel->control.settings(); checked.config = g_cfg;
        panel->control.applied(checked);
    }

    // Зерно волны ставится от часов ТОЛЬКО если его не принесли из чекпоинта.
    // Иначе продолжение уводило бы к другим ключам, рапортуя о продолжении.
    // ШАГ ПЕРЕБОРА СТАВИТСЯ ДО ЗАПУСКА ПОТОКОВ.
    //
    // set_scan_step перестраивает таблицы кратных, общие на весь процесс.
    // При маске шаг равен 16^N, без маски -- единице, то есть прежней
    // программе. Ставится здесь, после меню: раньше маска ещё не известна.
    {
        u256 st;
        if (mask_active()) u256_set(st, g_mask_step);
        else               u256_from_u64(st, 1);
        set_scan_step(st);
    }

    if (!wave_seed_initialized())
        wave_set_seed((uint64_t)std::chrono::steady_clock::now().time_since_epoch().count() ^ 0x5DEECE66DULL);
    if (unique_blocks_mode()) compute_total_blocks256();
    if (unique_blocks_mode()) save_checkpoint();

    single_instance_check_stop();
    pause_start(&g_stop);
    if (panel && panel->manual_pause) g_pause.request();
    pult_publish_resume(panel);
    if (panel) panel->control.status(panel_restart ? "applying" : "starting", panel->previous_count,
        g_found_num.load(), g_gpu_power_pct.load(), 0, {}, panel->error,
        "starting_workers", panel->manual_pause ? "Запуск выбранного режима; ручная пауза сохранится"
                                                : "Запуск выбранного режима; поиск продолжится автоматически");
    g_t0 = search_now();
    {
        uint8_t sb2[32], eb2[32];
        u256_to_be32(sb2, g_range_start);
        u256_to_be32(eb2, g_range_end);
        if (!panel_restart) tg_send_text("PubkeyHunt: поиск начат\n"
                     "Время: " + now_string() + "\n"
                     "Целей: " + std::to_string(g_db.items.size()) + "\n"
                     "Диапазон: 2^" + std::to_string(g_cfg.range_start_bit) +
                     " .. 2^" + std::to_string(g_cfg.range_end_bit) + "\n"
                     "Потоков: " + std::to_string(g_cfg.threads));
    }
    if (!plat_keep_awake(true))
        throw std::runtime_error("Windows не приняла защиту от сна и приостановки поиска");
    std::cout << "\n" << C_GREEN << "  Поиск начат. Пробел — пауза, Q, затем Y — остановить; Ctrl+C защищён.\n  + / - — мощность, шаг 5%, пределы 50–100%.\n  Без питания от сети — автоматическая пауза; при возвращении — продолжение с прежней мощностью.\n" << C_RESET
              << "  Начало поиска: " << now_string() << "\n"
              << "  Защита от сна и приостановки поиска включена; экран гаснет по таймеру Windows.\n"
              << "  Совпадения сохраняются; поиск продолжается по выбранному режиму до конца его обхода.\n"
              << (unique_blocks_mode()
                    ? "  В каждой части сначала исходные волны/зоны, затем оставшиеся участки без повторов.\n"
                    : "")
              << (unique_blocks_mode() && !(g_cfg.mode==TRAV_WAVE && !g_cfg.wave_start.empty())
                    ? "  Битовые интервалы чередуются после целого исходного узора одной базы.\n"
                    : "")
              << "  После закрытия программы обычное энергосбережение восстановится.\n"
                 "  Время и скорость замеров: RESULT/performance.csv.\n\n";


    Config remembered_settings = g_cfg;
    DigitRotation rotation(g_top_vmin, g_top_vmax, mask_active() ? -1 : g_cfg.det_hex,
                           g_cfg.digit_hours * 3600.0);
    const std::string configured_wave_start = g_cfg.wave_start;
    VolatileIntervalProgress volatile_progress;
    uint64_t previous_intervals = 0;
    int search_result = 0;
    bool range_fully_checked = false;
    for (;;) {
    if (g_shutdown_requested.load()) break;
    if (panel) panel->search_started = true;
    std::atomic<bool> rotate_due{false};
    if (rotation.enabled())
        std::cout << "  Текущая цифра: " << "0123456789abcdef"[g_cfg.det_hex]
                  << "; время: " << format_digit_hours(g_cfg.digit_hours) << " ч.\n";
#ifdef PKH_HAVE_CUDA
    PackedTraversalPlan packed_plan;
    const bool packed_ready=g_cfg.mode==TRAV_WAVE&&g_gpu_active&&g_cfg.threads==1
        &&packed_traversal_prepare(packed_plan)&&scan_packed_configure(packed_plan);
    if(packed_ready)std::cout << "  Волна: блоки по " << fmt_num(ZONE_BLOCK) << " ключей объединяются для видеокарты.\n";
    const uint64_t wave_piece=(std::min)(chunk_keys(),UINT64_C(65536));
    const size_t wave_capacity=unique_blocks_mode()&&g_gpu_active
        &&scan_zone_configure(wave_piece)?zone_scan_capacity(wave_piece):0;
    const bool wave_segmented=!packed_ready&&wave_capacity!=0;
    if(wave_segmented)std::cout << "  Обход без повторов: GPU-пакеты для " << traversal_block_bits()/4
        << " HEX-разрядов; порция " << fmt_num(wave_piece) << " ключей.\n";
#endif
    // После запуска рабочих потоков остановка идёт через set_stop(), join и чекпоинт.
    single_instance_check_stop();
    {
        std::lock_guard<std::mutex> lock(g_mtx_sample);
        u256_zero(g_sample_key);
        g_sample_valid = false;
    }
    MinuteSpeed minute_speed(search_now(), g_gpu_power_pct.load(), "RESULT/performance.csv",
        std::string("mode=") + mode_name(g_cfg.mode) +
        ";wave_nibbles=" + std::to_string(g_cfg.wave_nibbles) +
        ";zone_nibbles=" + std::to_string(g_cfg.zone_nibbles) +
        ";hybrid_nibbles=" + std::to_string(g_cfg.hybrid_nibbles),
        [] {return MinuteSpeed::Counters{g_count.load(std::memory_order_relaxed),
                                         g_found_num.load(std::memory_order_relaxed)};},
        search_status_sample);
    rotation.start(search_now());
    std::vector<std::thread> pool;
    SearchThreadFailure thread_failure;
    ThreadJoinScope workers_guard(pool, [] { set_stop(); });
    for (int t = 0; t < g_cfg.threads; t++) {
        pool.emplace_back([&, t]() {
          thread_failure.run([&]() {
            SearchPause::Worker pause_worker(g_pause);
            if (g_cfg.mode == TRAV_HYBRID) { hybrid_mode_worker(); return; }
            std::mt19937_64 rng((uint64_t)std::chrono::steady_clock::now()
                                    .time_since_epoch().count()
                                ^ (0x9E3779B97F4A7C15ULL * (uint64_t)(t + 1)));
#ifdef PKH_HAVE_CUDA
            bool use_packed=packed_ready;
            WaveGpuCursor wave_cursor;
            const bool parallel_wave=wave_segmented&&wave_piece==4096&&chunk_keys()==4096
                &&!g_cfg.debug_chunks;
            WaveGpuParallel wave_parallel(parallel_wave?4:1);
            WaveGpuBatch wave_batch;
            PackedTraversalBatch pending;
            struct PackedClaim {PackedTraversalBatch batch;bool ok=false;};
            std::future<PackedClaim> next_packed;
            const bool overlap_packed=!g_cfg.debug_chunks && !std::getenv("PKH_DISABLE_PREFETCH");
            WaveGpuPrefetch wave_prefetch([&]() {
                WaveGpuBatch prepared;
                if(parallel_wave){
                    wave_parallel.fill(prepared,wave_capacity,wave_piece,
                        [&](ChunkIdx& id){
                            if(g_stop.load(std::memory_order_relaxed)||rotate_due.load(std::memory_order_relaxed))return false;
                            g_chunk_seq.next(id);
                            return memcmp(id.b,g_total_blocks256,32)<0;
                        },
                        [&](const ChunkIdx& id,u256 key,uint64_t& count){
                            // Unique traversal uses no RNG; retain one per CPU
                            // worker so this adapter never shares mutable state.
                            static thread_local std::mt19937_64 metadata_rng(1);
                            int zone=-1;
                            return next_chunk(id,key,count,zone,metadata_rng);
                        });
                    return prepared;
                }
                wave_cursor.fill(prepared,wave_capacity,wave_piece,
                    [&](ChunkIdx &id,u256 key,uint64_t &count) {
                        // A timed shape rotation stops new logical claims. The
                        // cursor retains and drains every piece of its current ID.
                        if(g_stop.load(std::memory_order_relaxed)||rotate_due.load(std::memory_order_relaxed))return false;
                        g_chunk_seq.next(id);
                        int zone=-1;
                        if(!next_chunk(id,key,count,zone,rng))return false;
                        if(zone>=0)g_zone_idx.store(zone,std::memory_order_relaxed);
                        if(g_cfg.debug_chunks&&count)debug_log_chunk(t,id,key,count);
                        return true;
                    });
                return prepared;
            },overlap_packed);
            const size_t packed_capacity=packed_traversal_capacity();
            auto claim_packed=[&packed_plan,packed_capacity]() {
                PackedClaim result;
                result.ok=packed_traversal_claim(packed_plan,g_chunk_seq,packed_capacity,result.batch);
                return result;
            };
            auto log_packed_id=[&](const ChunkIdx &id){
                if(!g_cfg.debug_chunks||g_dbg_written.load(std::memory_order_relaxed)>DBG_LIMIT)return;
                u256 key; WaveInfo info; wave_block_key(id,key,info);
                const uint64_t count=info.duplicate?0:clamp_chunk_to_range(key,ZONE_BLOCK);
                debug_log_wave(t,id,key,info,count);
            };
            auto replay_packed_id=[&](const ChunkIdx &id){
                u256 key; WaveInfo info; wave_block_key(id,key,info);
                const uint64_t count=info.duplicate?0:clamp_chunk_to_range(key,ZONE_BLOCK);
                if(g_cfg.debug_chunks)debug_log_wave(t,id,key,info,count);
                g_zone_idx.store(info.phase,std::memory_order_relaxed);
                scan_chunk(key,count);
                if(!g_stop.load(std::memory_order_relaxed)) {
                    if(unique_blocks_mode())chunk_done(g_search,id,count);
                    else chunk_done(g_search,id);
                }
            };
#endif
            while (!g_stop.load()) {
                pause_check();
                if (g_stop.load()) break;
#ifdef PKH_HAVE_CUDA
                if(wave_segmented){
                    try { wave_prefetch.take(wave_batch); }
                    catch(const std::exception& error) {
                        std::lock_guard<std::mutex> output(g_mtx_cout);
                        MinuteSpeed::LogScope speed_log;
                        std::cerr << "  Подготовка блоков остановлена: " << error.what() << "\n";
                        set_stop();return;
                    }
                    // A finite traversal returns no more blocks. Leave only
                    // this worker; other workers may still be finishing theirs.
                    if(wave_batch.pieces.empty()&&wave_batch.completed.empty())break;
                    require_shape_completion_counts(wave_batch);
                    const bool full_done=wave_batch.full_starts.empty()||
                        (g_gpu_active&&scan_zone_batch(wave_batch.full_starts[0].data(),
                            (int)wave_batch.full_starts.size(),wave_piece));
                    // A failed packed attempt publishes nothing. Replay only this
                    // launch, never pieces already completed in previous launches.
                    for(const auto &part:wave_batch.pieces){
                        if(g_stop.load(std::memory_order_relaxed))break;
                        if(full_done&&part.count==wave_piece)continue;
                        u256 key;fe_from_be32(key,part.start.data());scan_chunk(key,part.count);
                    }
                    if(!g_stop.load(std::memory_order_relaxed))
                        commit_completed_batch(g_search,wave_batch,true);
                    continue;
                }
                if(use_packed&&(g_gpu_active||next_packed.valid())){
                    PackedClaim claimed;
                    try {claimed=next_packed.valid()?next_packed.get():claim_packed();}
                    catch(const std::exception& error) {
                        std::lock_guard<std::mutex> output(g_mtx_cout);
                        MinuteSpeed::LogScope speed_log;
                        std::cerr << "  Подготовка волны остановлена: " << error.what() << "\n";
                        set_stop();return;
                    }
                    pending=std::move(claimed.batch);
                    if(!claimed.ok){
                        // Allocation/unsupported preparation fails before an ID is claimed.
                        use_packed=false;
                    }else{
                        if(overlap_packed && pending.fast && g_gpu_active) {
                            // Claimed IDs are speculative. Only chunk_done advances
                            // the persisted watermark; dropped look-ahead is replayed.
                            try {next_packed=std::async(std::launch::async,claim_packed);}
                            catch(const std::exception&) {} // continue synchronously
                        }
                        if(pending.fast&&g_gpu_active&&scan_packed_traversal(packed_plan,pending)){
                            for(const auto &id:pending.edge_ids){
                                if(g_stop.load())break;
                                u256 key;WaveInfo info;wave_block_key(id,key,info);
                                const uint64_t count=info.duplicate?0:clamp_chunk_to_range(key,ZONE_BLOCK);
                                if(count)scan_chunk(key,count);
                            }
                            for(const auto&id:pending.pending_ids)log_packed_id(id);
                            // A completed launch may still be replayed after a stop; never
                            // move the watermark past work whose completion is uncertain.
                            if(!g_stop.load(std::memory_order_relaxed))
                                for(const auto&id:pending.pending_ids)chunk_done(g_search,id);
                        }else{
                            // The failed packed attempt delivered neither counts nor hits.
                            // Reconstruct exact starts, but retain GPU batching. One
                            // rejected translation must not cause thousands of tiny launches.
                            const uint64_t piece=(std::min)(ZONE_BLOCK,UINT64_C(65536));
                            if(g_gpu_active&&scan_zone_configure(piece)){
                                static bool explained=false;
                                if(!explained){
                                    std::lock_guard<std::mutex> output(g_mtx_cout);
                                    MinuteSpeed::LogScope speed_log;
                                    std::cerr << "  Волна: переход на пакеты точных начал ("
                                        << (pending.fast?"GPU-пакет не принят":"граница диапазона") << ").\n";
                                    explained=true;
                                }
                                WaveGpuCursor fallback;WaveGpuBatch batch;size_t next=0;
                                do{
                                    fallback.fill(batch,zone_scan_capacity(piece),piece,
                                        [&](ChunkIdx &id,u256 key,uint64_t &count){
                                            if(next==pending.pending_ids.size()||g_stop.load())return false;
                                            id=pending.pending_ids[next++];WaveInfo info;
                                            wave_block_key(id,key,info);
                                            count=info.duplicate?0:clamp_chunk_to_range(key,ZONE_BLOCK);
                                            if(g_cfg.debug_chunks)debug_log_wave(t,id,key,info,count);
                                            return true;
                                        });
                                    const bool done=batch.full_starts.empty()||scan_zone_batch(
                                        batch.full_starts[0].data(),(int)batch.full_starts.size(),piece);
                                    for(const auto &part:batch.pieces){
                                        if(g_stop.load())break;
                                        if(done&&part.count==piece)continue;
                                        u256 key;fe_from_be32(key,part.start.data());scan_chunk(key,part.count);
                                    }
                                    if(!g_stop.load())commit_completed_batch(g_search,batch,unique_blocks_mode());
                                }while(next<pending.pending_ids.size()&&!g_stop.load());
                                // Prefetch contains only host metadata; restore the GPU
                                // translation table before consuming its next packet.
                                if(!g_stop.load())scan_packed_configure(packed_plan);
                            }else{
                                for(const auto&id:pending.pending_ids){
                                    if(g_stop.load(std::memory_order_relaxed))break;
                                    replay_packed_id(id);
                                }
                            }
                        }
                        continue;
                    }
                }
#endif
                u256 k;
                uint64_t n = 0;
                int zone = -1;
                // Shape rotation drains the active logical block instead of
                // interrupting it and replaying its already checked prefix.
                if(unique_blocks_mode()&&rotate_due.load(std::memory_order_relaxed))break;
                ChunkIdx idx; g_chunk_seq.next(idx);
#if defined(PKH_HAVE_CUDA) && defined(_WIN32)
                if (g_cfg.mode == TRAV_KXE && kxe_sass_requested()) {
                    if (memcmp(idx.b, g_total_blocks256, 32) >= 0) break;
                    if (!kxe_sass_scan(idx)) break;
                    // Production SASS is disabled until complete interval
                    // coverage is verified; requests use the standard backend.
                    if (!g_stop.load(std::memory_order_relaxed)) chunk_done(g_search, idx);
                    continue;
                }
#endif
                if (!next_chunk(idx, k, n, zone, rng)) {
                    // ДИАПАЗОН КОНЧИЛСЯ У ЭТОГО ПОТОКА -- И ТОЛЬКО У НЕГО.
                    //
                    // Было set_stop(): поток, забравший последний участок,
                    // поднимал общий флаг остановки, и все ОСТАЛЬНЫЕ потоки
                    // бросали свои участки недосчитанными -- горячий цикл
                    // проверяет g_stop на каждом пакете. При восьми потоках и
                    // участке 2^22 это до семи участков, брошенных в
                    // произвольном месте, то есть до 29 млн непроверенных
                    // ключей ровно в конце диапазона -- при том, что программа
                    // пишет «Остановлено» и заявляет полное покрытие.
                    //
                    // Теперь поток просто выходит сам. Общий флаг ставится в
                    // main после join всех потоков -- тогда и поток вывода
                    // закончит работу.
                    break;
                }
                if (zone >= 0) g_zone_idx.store(zone, std::memory_order_relaxed);
                // Журнал ведётся в любом режиме. Раньше вызов стоял только в
                // ветке последовательного обхода, и при случайном режиме файл
                // просто не появлялся — при включённом в меню журнале.
                if (g_cfg.debug_chunks && (!unique_blocks_mode() || n)) debug_log_chunk(t, idx, k, n);
                scan_chunk(k, n);

                // УЧАСТОК ЗАСЧИТЫВАЕТСЯ, ТОЛЬКО ЕСЛИ ДОСЧИТАН ДО КОНЦА.
                //
                // Горячий цикл проверяет g_stop на каждом пакете и при
                // остановке бросает участок посередине. Засчитать такой
                // участок значит соврать чекпоинту: после восстановления
                // недосчитанный хвост не переберёт никто.
                //
                // Обратная ошибка безобидна: если флаг подняли уже после
                // того, как участок закрылся, он просто пересчитается заново
                // при следующем запуске. Лишняя работа против пропуска --
                // выбор очевидный.
                if (!g_stop.load(std::memory_order_relaxed)) {
                    if(unique_blocks_mode())chunk_done(g_search,idx,n);
                    else chunk_done(g_search,idx);
                }
            }
          }, [] { set_stop(); });
        });
    }

    // Экран обновляется каждые 30 с; отдельные интервалы относятся к уведомлениям.
    std::thread ui;
    ThreadJoinScope ui_guard(ui, [] { set_stop(); });
    ui = std::thread([&]() {
      thread_failure.run([&]() {
        auto last_prog = search_now();
        auto last_chk  = last_prog;
        auto last_minute = last_prog;
        auto last_save = last_prog;
        bool first_sample_pending=true;
        auto last_panel_status = std::chrono::steady_clock::now();
        auto last_panel_sample = last_panel_status - std::chrono::seconds(30);
        uint64_t last_panel_count = g_count.load();
        std::string panel_sample;
        bool panel_pause_saved = false;
        while (!g_stop.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (g_stop.load()) break;
            if (panel) {
                pult::Command command; std::string error;
                // Commands are immutable and processed in revision order.
                if (panel->control.next_command(command, error))
                    pult_dispatch(*panel, command, &remembered_settings, &minute_speed);
                else if (!error.empty()) panel->error = error;
                if (!g_pause.requested()) panel_pause_saved = false;
                else if (!panel_pause_saved && g_pause.ready()) {
                    if (mode_has_checkpoint()) save_checkpoint();
                    pult_publish_resume(panel);
                    panel_pause_saved = true;
                }
                const auto wall_now = std::chrono::steady_clock::now();
                if (!panel->initial_sample_written)
                    panel->initial_sample_written = pult_write_initial_sample(*panel);
                if (wall_now-last_panel_sample >= std::chrono::seconds(30)) {
                    panel_sample = search_status_sample();
                    if (!panel_sample.empty()) last_panel_sample = wall_now;
                }
                if (wall_now-last_panel_status >= std::chrono::milliseconds(500) || g_stop.load()) {
                    const auto count = g_count.load();
                    const double seconds = std::chrono::duration<double>(wall_now-last_panel_status).count();
                    const double rate = g_pause.requested() || count<last_panel_count || seconds<=0
                        ? 0 : static_cast<double>(count-last_panel_count)/seconds;
                    const std::string phase = panel->apply_pending ? "applying"
                        : g_shutdown_requested.load() ? "stopping"
                        : g_pause.requested() ? (g_pause.ready() ? "paused" : "pausing") : "running";
                    panel->control.status(phase, panel->previous_count + previous_intervals + count,
                        g_found_num.load(), g_gpu_power_pct.load(), rate, panel_sample, panel->error,
                        phase, panel->apply_pending ? "Завершаются текущие пакеты и сохраняется позиция; затем настройки применятся автоматически" : "");
                    last_panel_status=wall_now;last_panel_count=count;
                }
                if (g_stop.load()) break;
            }
            const PauseEvent event = pause_poll([&](int key) {
                if (key != '+' && key != '=' && key != '-' && key != '_') return false;
                const int previous = g_gpu_power_pct.load(std::memory_order_relaxed);
                const int next = power_after_key(previous, key);
                if (next == previous) return true;
                std::lock_guard<std::mutex> output(g_mtx_cout);
                MinuteSpeed::LogScope speed_log;
                g_gpu_power_pct.store(next, std::memory_order_relaxed);
                remembered_settings.gpu_power_pct = next;
                minute_speed.set_power(next);
                std::cout << "  Мощность: " << previous << "% -> " << next << "%\n";
                if (!panel) save_config_ini(remembered_settings);
                return true;
            });
            if (event != PauseEvent::none) {
                std::lock_guard<std::mutex> output(g_mtx_cout);
                MinuteSpeed::LogScope speed_log;
                if (event == PauseEvent::power_lost) {
                    minute_speed.set_paused(true, true);
                    std::cout << "\n  Питание от сети отключено. Завершаю текущие пакеты и ставлю поиск на паузу.\n"
                              << "  Сохранена мощность " << g_gpu_power_pct.load() << "%; продолжение автоматически после возвращения питания.\n";
                } else if (event == PauseEvent::power_restored) {
                    const bool manual = g_pause.manual_requested();
                    minute_speed.set_paused(manual);
                    std::cout << (manual ? "\n  Питание от сети восстановлено. Ручная пауза сохранена.\n"
                                         : "\n  Питание от сети восстановлено. Продолжаем с текущей позиции.\n");
                    if (!manual) std::cout << "  Мощность: " << g_gpu_power_pct.load() << "%.\n";
                } else if (event == PauseEvent::requested) {
                    minute_speed.set_paused(true, true);
                    std::cout << "\n  Пауза запрошена: завершаю текущие вычислительные пакеты.\n";
                } else if (event == PauseEvent::paused) {
                    minute_speed.set_paused(true);
                    std::cout << "\n  ПАУЗА. Вычисления остановлены. Проверено ключей: "
                              << fmt_num(g_count.load()) << "\n"
                              << (g_pause.automatic_requested()
                                  ? "  Ожидаю питание от сети; выбранная мощность сохранена. Q, затем Y — остановить.\n"
                                  : "  + / - — мощность без снятия паузы; другая клавиша — продолжить. Q, затем Y — остановить; Ctrl+C защищён.\n");
                } else {
                    minute_speed.set_paused(false);
                    std::cout << "  Продолжаем с текущей позиции.\n";
                }
                std::cout.flush();
            }
            if(event==PauseEvent::paused && mode_has_checkpoint()){
                save_checkpoint();
                pult_publish_resume(panel);
            }
            if (g_pause.requested()) continue;
            auto now = search_now();
            if(first_sample_pending) {
                bool ready=false;
                {std::lock_guard<std::mutex> lock(g_search.mtx_sample);ready=g_search.sample_valid;}
                if(ready) {print_check(false);first_sample_pending=false;}
            }
            if (rotation.due(now) && !rotate_due.load(std::memory_order_relaxed)) {
                rotate_due.store(true);
                if(unique_blocks_mode()) {
                    std::lock_guard<std::mutex> output(g_mtx_cout);
                    MinuteSpeed::LogScope speed_log;
                    std::cout << "\n  Время HEX-цифры истекло: завершаю начатые блоки перед переходом.\n"
                                 "  Это сохраняет обход без повторов; управление паузой и остановкой доступно.\n";
                } else {
                    request_interval_stop();
                    break;
                }
            }
            if (now - last_minute >= std::chrono::seconds(30)) {
                if (!plat_keep_awake(true))
                    throw std::runtime_error("Windows не приняла обновление защиты от сна");
                std::lock_guard<std::mutex> output(g_mtx_cout);
                minute_speed.update(now, g_count.load(), g_found_num.load());
                last_minute = now;
            }
            if (std::chrono::duration_cast<std::chrono::seconds>(now - last_prog).count()
                    >= g_cfg.progress_sec) {
                print_progress(g_cfg.progress_sec, false);
                last_prog = now;
            }
            if (std::chrono::duration_cast<std::chrono::seconds>(now - last_chk).count()
                    >= g_cfg.check_sec) {
                print_check(true, false);
                last_chk = now;
            }
            // Saving progress must not depend on the notification check interval.
            if (now - last_save >= std::chrono::seconds(300)) {
                if (mode_has_checkpoint()) save_checkpoint();
                pult_publish_resume(panel);
                last_save = search_now();
            }
        }
      }, [] { set_stop(); });
    });

    for (auto &th : pool) th.join();
    // Capture natural completion before the internal stop wakes the monitor.
    // Out-of-order claims and an interrupted final chunk do not prove coverage.
    const ChunkIdx committed = checkpoint_seq(g_search);
    const bool interval_exhausted = pkh_completion::range_fully_checked(
        true, g_stop.load() || g_shutdown_requested.load(), false,
        committed.b, g_total_blocks256);
    range_fully_checked = pkh_completion::range_fully_checked(
        traversal_full_coverage(),
        g_stop.load() || g_shutdown_requested.load(), rotation.enabled(),
        committed.b, g_total_blocks256);
    request_interval_stop();
    ui.join();
    // Finite tiny searches may finish before the monitor's first 50 ms tick.
    if (panel && !panel->initial_sample_written)
        panel->initial_sample_written = pult_write_initial_sample(*panel);
    g_pause.resume();
    minute_speed.finish(g_count.load(), g_found_num.load());
    if (mode_has_checkpoint()) save_checkpoint();
    pult_publish_resume(panel);
    thread_failure.rethrow_after_join();
    if(unique_blocks_mode()&&rotation.enabled()) {
        ChunkIdx position;u256 confirmed;
        if(!checkpoint_snapshot(g_search,position,confirmed))
            throw std::runtime_error("Не подтверждён счётчик завершённых участков");
        volatile_progress.remember_exact(g_cfg.det_hex,position,wave_get_seed(),confirmed);
    }
    previous_intervals += g_count.load();
    if (g_shutdown_requested.load() || (panel && panel->apply_pending) || !rotation.enabled()) break;
    const int previous_digit = g_cfg.det_hex;
    const bool completed = unique_blocks_mode() ? interval_exhausted : !rotate_due.load();
    if (!rotation.advance(completed)) {
        break;
    }
    g_cfg.det_hex = rotation.current();
    std::cout << "\n  " << (completed ? "Часть проверена" : "Время истекло")
              << ". Переход " << "0123456789abcdef"[previous_digit] << " -> "
              << "0123456789abcdef"[g_cfg.det_hex] << "; по "
              << format_digit_hours(g_cfg.digit_hours) << " ч на интервал.\n";
    // External stop is latched separately and must never be cleared by rotation.
    g_stop.store(false); g_stop_flag = 0;
    if (g_shutdown_requested.load()) { set_stop(); break; }
    g_count.store(0);
    if (!prepare_rotated_interval(configured_wave_start,volatile_progress)) { search_result = 1; set_stop(); break; }
    pult_publish_resume(panel);
    } // digit intervals
    g_count.store(previous_intervals);

    // Apply ends this configuration's worker interval, while the owning
    // process continues into preparation of the next one. Do not announce a
    // terminal search stop (or run final notifications) for that transition.
    if (panel && panel->apply_pending && !g_shutdown_requested.load()) {
        panel->completed = false;
        panel->control.status("applying", panel->previous_count + g_count.load(),
            g_found_num.load(), g_gpu_power_pct.load(), 0, {}, panel->error,
            "preparing_configuration", panel->manual_pause
                ? "Позиция сохранена; применяются настройки, ручная пауза сохранится"
                : "Позиция сохранена; применяются настройки, поиск продолжится автоматически");
        std::cout << "\n  Позиция сохранена. Применяются настройки; "
                  << (panel->manual_pause ? "ручная пауза сохранится.\n" : "поиск продолжится автоматически.\n");
        return search_result;
    }

    const bool completed_range = range_fully_checked && !g_shutdown_requested.load();
    if (panel) panel->completed = completed_range;
    tg_send_text(std::string(completed_range ? "PubkeyHunt: диапазон полностью проверен\n"
                                           : "PubkeyHunt: поиск остановлен\n") +
                 "Время работы: " + format_elapsed() + "\n"
                 "Проверено ключей: " + fmt_num(g_count.load()) + "\n"
                 "Найдено: " + std::to_string(g_found_num.load()));

    std::cout << "\n" << std::string(60, '=') << "\n"
              << (completed_range ? "  Диапазон полностью проверен. Время работы: "
                                  : "  Остановлено. Время работы: ") << format_elapsed() << "\n"
              << "  Проверено ключей: " << fmt_num(g_count.load()) << "\n"
              << "  Найдено совпадений: " << g_found_num.load() << "\n"
              << (completed_range ? "  Все ключи выбранного диапазона проверены. Завершение не связано с находкой.\n"
                                  : "")
              << std::string(60, '=') << "\n";
    return search_result;
}

static int run_application_sessions(int argc, char **argv, PultRuntime* panel) {
    int result = 0;
    for (;;) {
        result = application_main(argc, argv, panel);
        if (!panel) break;
        if (result && panel->staging && !g_shutdown_requested.load()) {
            // A database can exist yet fail to load. Keep the old accepted
            // revision and restore its checkpoint rather than exit on an
            // invalid replacement configuration.
            panel->control.reject_settings(panel->staged.revision);
            panel->staging = false;
            const std::string preparation_error = panel->error;
            panel->error = "New settings could not be prepared; previous settings restored";
            if (!preparation_error.empty()) panel->error += ": " + preparation_error;
            g_cfg = panel->control.config();
            g_chunk_seq.set(ChunkIdx{});
            checkpoint_reset(g_search, ChunkIdx{});
            checkpoint_clear_loaded_shape();
            g_search.resumed_items = 0;
            g_stop.store(false); g_stop_flag = 0;
            panel->control.status("applying", panel->previous_count, g_found_num.load(), g_gpu_power_pct.load(), 0,
                {}, panel->error);
            continue;
        }
        if (!panel->apply_pending || g_shutdown_requested.load()) {
            panel->control.status(result ? "error" : panel->completed ? "completed" : "stopped",
                panel->previous_count + g_count.load(), g_found_num.load(), g_gpu_power_pct.load(), 0,
                search_status_sample(), result && panel->error.empty()
                    ? "Search preparation or execution failed; see worker.log" : panel->error);
            break;
        }
        // application_main has joined both old thread groups and saved
        // their checkpoints. A queued Stop wins before any new GPU work.
        pult::Command command; std::string error;
        while (panel->control.next_command(command, error)) {
            pult_dispatch(*panel, command);
            if (g_shutdown_requested.load()) break;
        }
        if (g_shutdown_requested.load()) {
            panel->control.status("stopped", panel->previous_count + g_count.load(),
                g_found_num.load(), g_gpu_power_pct.load(), 0);
            break;
        }
        if (!error.empty()) panel->error = error;
        panel->previous_count += g_count.load();
        g_count.store(0);
        g_chunk_seq.set(ChunkIdx{});
        checkpoint_reset(g_search, ChunkIdx{});
        checkpoint_clear_loaded_shape();
        g_search.resumed_items = 0;
        g_zone_idx.store(-1);
        panel->staged = panel->pending;
        panel->staging = true;
        g_cfg = panel->staged.config;
        panel->apply_pending = false;
        panel->completed = false;
        g_gpu_power_pct.store(g_cfg.gpu_power_pct ? g_cfg.gpu_power_pct : g_gpu_power_pct.load());
        g_stop.store(false); g_stop_flag = 0;
        panel->control.status("applying", panel->previous_count, g_found_num.load(), g_gpu_power_pct.load(), 0);
    }
    return result;
}

#ifdef _WIN32
static void wait_for_search_completion(int result) noexcept {
    try {
        DWORD input_mode=0, output_mode=0;
        const bool error_exit=result!=0;
        if (!search_keep_console_open(interactive_search,
                GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &input_mode)!=0,
                GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &output_mode)!=0,
                g_shutdown_requested.load(), error_exit)) return;
#ifdef PKH_HAVE_CUDA
        gpu_backend_stop();
#endif
        plat_keep_awake(false);
        HANDLE input=GetStdHandle(STD_INPUT_HANDLE);
        FlushConsoleInputBuffer(input);
        std::cout << (error_exit ? "\n  Поиск завершён с ошибкой. Проверьте сообщение выше.\n"
                                : "\n  Поиск завершён. Итоги показаны выше.\n")
                  << "  Нажмите ENTER, чтобы закрыть окно.\n" << std::flush;
        while (!search_close_completion_wait(g_shutdown_requested.load(), error_exit)) {
            single_instance_check_stop();
            if (WaitForSingleObject(input, 100)!=WAIT_OBJECT_0) continue;
            INPUT_RECORD event{};DWORD count=0;
            if (!ReadConsoleInputW(input,&event,1,&count)) break;
            if (count && event.EventType==KEY_EVENT && event.Event.KeyEvent.bKeyDown
                && (event.Event.KeyEvent.wVirtualKeyCode==VK_RETURN
                    || event.Event.KeyEvent.wVirtualKeyCode==VK_ESCAPE)) break;
        }
    } catch (...) {}
}
#endif

// Help and notification diagnostics cannot replace the current search.
// Every other launch requires an idle application instead of taking over.
int main(int argc, char **argv) {
    if (argc == 2 && std::string(argv[1]) == "--pult-protocol-version") {
        std::puts("PUBKEYS_PULT_PROTOCOL=1");
        return 0;
    }
    const bool pult_requested = argc > 1 && std::string(argv[1]) == "--pult-worker";
    if (cli_removed_kangaroo_requested(argc, argv)) {
        std::cerr << "PubKEYS: режим кенгуру удалён; команда не поддерживается. Ничего не запущено.\n";
        return 2;
    }
    if (cli_help_requested(argc, argv)) return print_command_line_help();
    FoundWorkExitDrain found_work_exit_drain;
    // Notification diagnostics do not acquire the search mutex, initialize CUDA,
    // rewrite settings, load the key database or touch checkpoints.
    if (argc > 1 && std::string(argv[1]) == "--check-notifications") {
        if (argc != 2) {
            std::cerr << "PubKEYS: --check-notifications не принимает дополнительные параметры; ничего не запущено.\n";
            return 2;
        }
        try {
            plat_console_utf8();
            init_colors();
            chdir_to_program_if_needed();
            if (!std::ifstream("config.ini").good()) {
                std::cerr << "PubKEYS: config.ini not found; no messages sent.\n";
                return 2;
            }
            load_config_ini(g_cfg);
            const bool telegram = !g_cfg.tg_token.empty() && !g_cfg.tg_chat.empty();
            const bool sig = signal_enabled();
            std::cout << "PubKEYS: проверка Telegram и Signal (тестовые сообщения).\n";
            if (!telegram) std::cout << "Telegram: не настроен.\n";
            if (!sig) std::cout << "Signal: не настроен.\n";
            const bool tg_ok = telegram && tg_check();
            const bool sig_ok = sig && signal_check();
            return tg_ok && sig_ok ? 0 : 2;
        } catch (const std::exception&) {
            std::cerr << "PubKEYS: notification check failed.\n";
            return 2;
        }
    }
#ifdef _WIN32
    const bool diagnostic = cli_exclusive_diagnostic(argc, argv);
    SingleInstanceGuard instance(L"Global\\PubKEYS.SingleInstance.v1", set_stop, "PubKEYS");
    if (!instance.acquired()) {
        std::cerr << "PubKEYS: не получено исключительное право запуска; "
                  << (diagnostic ? "служебная проверка" : "новый экземпляр")
                  << " не запущен. Команды остановки другим экземплярам не отправлялись. "
                     "Код Windows: " << instance.error() << "\n";
        return 11;
    }
#endif
    std::unique_ptr<PultRuntime> panel;
    try {
        single_instance_check_stop();
        if (pult_requested) {
            if (argc != 3 && !(argc == 5 && std::string(argv[3]) == "--pult-app-root")) {
                std::cerr << "PubKEYS: use --pult-worker <absolute session> [--pult-app-root <absolute app root>]\n";
                return 2;
            }
            const std::string session_directory = pult_path_argument_utf8(2, argv[2]);
            const std::string app_root = argc == 5 ? pult_path_argument_utf8(4, argv[4]) : plat_exe_dir();
            if (!std::filesystem::u8path(app_root).is_absolute() || !plat_chdir(app_root)) {
                std::cerr << "PubKEYS: cannot open the panel's application directory\n";
                return 2;
            }
            Config defaults;
            load_config_ini(defaults);
            sanitize_config(defaults);
            auto opening = std::make_unique<PultRuntime>();
            std::string error;
            if (!opening->control.open(session_directory, plat_exe_dir(), defaults, error)) {
                std::cerr << "PubKEYS: " << error << "\n";
                return 2;
            }
            panel = std::move(opening);
            g_cfg = panel->control.config();
            if (g_cfg.gpu_power_pct) g_gpu_power_pct.store(g_cfg.gpu_power_pct);
            panel->control.status("starting", 0, g_found_num.load(), g_gpu_power_pct.load(), 0);
        }
        const int result = run_application_sessions(argc, argv, panel.get());
        single_instance_check_stop();
#ifdef _WIN32
        wait_for_search_completion(result);
#endif
        return result;
    } catch (const std::exception& error) {
        set_stop();
        if (panel) panel->control.status("error", panel->previous_count + g_count.load(),
            g_found_num.load(), g_gpu_power_pct.load(), 0, {}, error.what());
        if ((interactive_search || panel) && mode_has_checkpoint()) save_checkpoint();
        pult_publish_resume(panel.get());
        plat_keep_awake(false);
        std::cerr << "PubKEYS: поиск остановлен: " << error.what() << "\n";
#ifdef _WIN32
        wait_for_search_completion(1);
#endif
        return 1;
    } catch (const SingleInstanceStop&) {
        std::cerr << "PubKEYS: работа завершена по запросу нового экземпляра.\n";
        return 0;
    } catch (...) {
        set_stop();
        if (panel) panel->control.status("error", panel->previous_count + g_count.load(),
            g_found_num.load(), g_gpu_power_pct.load(), 0, {}, "Worker thread failed");
        try { if ((interactive_search || panel) && mode_has_checkpoint()) save_checkpoint(); } catch (...) {}
        pult_publish_resume(panel.get());
        plat_keep_awake(false);
        std::fputs("PubKEYS: поиск остановлен из-за ошибки рабочего потока.\n",stderr);
#ifdef _WIN32
        wait_for_search_completion(1);
#endif
        return 1;
    }
}
