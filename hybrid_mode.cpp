#include "minute_speed.h"
#include "batch_prefetch.h"
#include "hybrid_mode.h"
#include "common.h"
#include "engine.h"
#include "notify.h"
#include "platform.h"
#include "traverse.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <random>

namespace {
pkh_hybrid::HybridSearch search;

void hybrid_error(const std::string& why) {
    std::lock_guard<std::mutex> output(g_mtx_cout);
    MinuteSpeed::LogScope speed_log;
    std::cerr << "  Гибридный поиск остановлен: " << why << ".\n"
                 "  Незавершённый заход не засчитан.\n";
    set_stop();
}

void log_piece(std::ofstream& log, const pkh_hybrid::HybridPiece& piece,
               uint64_t& written, uint64_t& last_ordinal) {
    if (!log || written >= DBG_LIMIT || piece.info.ordinal == last_ordinal) return;
    u256 first, last;
    fe_from_be32(first, piece.key);
    u256_add_u64(last, first, piece.count - 1);
    uint8_t final_bytes[32]; u256_to_be32(final_bytes, last);
    log << piece.info.ordinal << ',' << to_hex(piece.info.raw.b, 32) << ','
        << to_hex(piece.key, 32) << ',' << to_hex(final_bytes, 32) << ',' << piece.count << '\n';
    last_ordinal = piece.info.ordinal;
    ++written;
}
} // namespace

bool hybrid_mode_initialize(std::string& why) {
    uint8_t low[32], high[32], seed[32];
    u256_to_be32(low, g_range_start); u256_to_be32(high, g_range_end);
    try {
        std::random_device random;
        for (int w = 0; w < 8; ++w) {
            const uint32_t value = random();
            for (int b = 0; b < 4; ++b) seed[w * 4 + b] = (uint8_t)(value >> (24 - b * 8));
        }
    } catch (const std::exception& e) {
        why = std::string("не удалось получить зерно: ") + e.what();
        return false;
    }
    return search.init(g_cfg.hybrid_nibbles, low, high, seed, checkpoint_search_identity(), why);
}

bool hybrid_mode_restore(std::string& why) { return search.load(search.checkpoint_path(), why); }
void hybrid_mode_reset() { search.reset(); }
std::string hybrid_mode_checkpoint_path() { return search.checkpoint_path(); }
pkh_hybrid::HybridStatus hybrid_mode_status() { return search.status(); }

void hybrid_mode_describe() {
    const auto state = search.status();
    std::cout << "  Гибридный: часть A целиком выбирается случайно и остаётся неизменной,\n"
              << "  пока часть B из младших " << state.skip << " HEX-разрядов перебирается подряд.\n"
              << "  Полный блок B: " << state.block_keys << " ключей. Затем выбирается новая A.\n"
                 "  На краях блок подрезается строго по заданному интервалу.\n"
                 "  Префиксы A могут повторяться; полное покрытие не гарантируется.\n";
    if (state.single_prefix)
        std::cout << "  В интервале только одна A: её допустимая часть B проверяется однократно.\n";
    pkh_hybrid::HybridBatch preview;
    std::string why;
    if (!search.prepare_batch(1, state.block_keys, preview, why) || preview.pieces.empty()) return;
    const auto& first = preview.pieces.front();
    u256 key, last; fe_from_be32(key, first.key); u256_add_u64(last, key, first.count - 1);
    uint8_t last_bytes[32]; u256_to_be32(last_bytes, last);
    std::cout << "  Ближайший отрезок проверит " << first.count << " ключей подряд:\n"
              << "    от " << to_hex(first.key, 32) << "\n"
              << "    до " << to_hex(last_bytes, 32) << "\n";
}

void hybrid_mode_save_checkpoint() {
    if (!mode_has_checkpoint()) return;
    if (!search.status().initialized) return;
    std::string why;
    if (search.save(search.checkpoint_path(), why)) return;
    static std::atomic<int64_t> last_warning{0};
    const int64_t now = (int64_t)std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    int64_t previous = last_warning.load();
    if ((!previous || now - previous >= 60) && last_warning.compare_exchange_strong(previous, now)) {
        std::lock_guard<std::mutex> output(g_mtx_cout);
    MinuteSpeed::LogScope speed_log;
        std::cerr << "  Гибридный checkpoint НЕ сохранён: " << why << ".\n";
    }
}

void hybrid_mode_worker() {
    const auto initial = search.status();
    if (!initial.initialized) { hybrid_error("поиск не инициализирован"); return; }
    uint64_t piece_keys = (std::min)(initial.block_keys, UINT64_C(65536));
    size_t capacity = 1;
    bool packed = false;
#ifdef PKH_HAVE_CUDA
    if (g_gpu_active && piece_keys >= (uint64_t)(2 * CUDA_HALF)) {
        capacity = zone_scan_capacity(piece_keys);
        packed = capacity && scan_zone_configure(piece_keys);
        if (!packed) capacity = 1;
    }
#endif
    if (!packed) piece_keys = (std::min)(initial.block_keys, UINT64_C(1048576));
    {
        std::lock_guard<std::mutex> output(g_mtx_cout);
    MinuteSpeed::LogScope speed_log;
        if (packed) std::cout << "  Гибридный: небольшие отрезки объединяются в один заход видеокарты.\n";
    }
    std::ofstream log;
    uint64_t logged = 0, last_logged = UINT64_MAX;
    if (g_cfg.hybrid_debug) {
        log.open(plat_wide("hybrid_debug.csv").c_str(), std::ios::trunc);
        if (log) log << "block,prefix_A_zero_B,piece_first,piece_last,count\n";
        else { std::lock_guard<std::mutex> output(g_mtx_cout);
    MinuteSpeed::LogScope speed_log;
            std::cerr << "  ВНИМАНИЕ: не удалось открыть hybrid_debug.csv\n"; }
    }
    BatchPrefetch<pkh_hybrid::HybridSearch,pkh_hybrid::HybridBatch> prefetch(search,capacity,piece_keys,
        packed && !g_cfg.hybrid_debug && !std::getenv("PKH_DISABLE_PREFETCH"));
    while (!g_stop.load(std::memory_order_relaxed)) {
        pause_check();
        if (g_stop.load(std::memory_order_relaxed)) break;
        pkh_hybrid::HybridBatch batch;
        std::string why;
        if (!prefetch.take(batch, why)) { hybrid_error(why); return; }
        if (batch.pieces.empty()) {
            if (!search.commit_batch(batch, why)) hybrid_error(why);
            else { std::lock_guard<std::mutex> output(g_mtx_cout);
    MinuteSpeed::LogScope speed_log;
                std::cout << "  Гибридный поиск завершён: допустимый интервал проверен\n"
                             "  либо достигнут предел счётчика блоков.\n"; }
            break;
        }
        prefetch.start_after(batch);
        const uint64_t before = g_count.load(std::memory_order_relaxed);
        bool packed_complete = false;
#ifdef PKH_HAVE_CUDA
        if (packed && g_gpu_active) {
            const auto& starts=batch.full_starts;
            // The packed backend publishes no counts or candidates on failure.
            packed_complete = starts.empty() || scan_zone_batch(starts.data(), (int)(starts.size() / 32), piece_keys);
        }
#endif
        // Avoid revisiting every full piece after a successful packed scan.
        // Keep the exact fallback for clipped tails and unsuccessful attempts.
        if (!packed_complete || batch.full_starts.size()/32 != batch.pieces.size())
        for (const auto& piece : batch.pieces) {
            if (g_stop.load(std::memory_order_relaxed)) break;
            if (packed_complete && piece.count == piece_keys) continue;
            u256 key; fe_from_be32(key, piece.key); scan_chunk(key, piece.count);
        }
        if (g_stop.load(std::memory_order_relaxed)) break;
        if (g_count.load(std::memory_order_relaxed) - before != batch.keys) {
            hybrid_error("число проверенных ключей не совпало с подготовленным заходом"); return;
        }
        // Publish the random state and exact intra-block offset only after the
        // entire attempt and all CPU confirmations/local finding saves finished.
        if (!search.commit_batch(batch, why)) { hybrid_error(why); return; }
        if (log.is_open()) {
            for (const auto& piece : batch.pieces)
                log_piece(log, piece, logged, last_logged);
            if (log) log.flush();
        }
    }
}
