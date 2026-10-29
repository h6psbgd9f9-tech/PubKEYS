#include "minute_speed.h"
#include "batch_prefetch.h"
#include "zone_mode.h"
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
pkh_zone::ZoneSearch search;

void zone_error(const std::string& why) {
    std::lock_guard<std::mutex> output(g_mtx_cout);
    MinuteSpeed::LogScope speed_log;
    std::cerr << "  Зональная цепочка остановлена: " << why << ".\n"
                 "  Незавершённый заход не переносится в checkpoint.\n";
    set_stop();
}

void log_piece(std::ofstream& log, const pkh_zone::ZonePiece& piece,
               uint64_t block_keys, uint64_t& written, uint64_t& last_ordinal) {
    if (!log || written >= DBG_LIMIT || piece.info.ordinal == last_ordinal) return;
    u256 first, last;
    fe_from_be32(first, piece.info.raw.b);
    // The canonical lower block may start before the interval. Log its clipped
    // start, and skip a resumed piece that begins in the middle of this block.
    const uint64_t count = clamp_chunk_to_range(first, block_keys);
    if (!count) return;
    uint8_t first_bytes[32]; u256_to_be32(first_bytes, first);
    if (std::memcmp(piece.key, first_bytes, 32) != 0) return;
    u256_add_u64(last, first, count - 1);
    uint8_t final_bytes[32]; u256_to_be32(final_bytes, last);
    log << piece.info.ordinal << ','
        << (piece.info.position < 0 ? 0 : 64 - piece.info.position) << ','
        << to_hex(piece.info.base.b, 32) << ',' << to_hex(first_bytes, 32)
        << ',' << to_hex(final_bytes, 32) << ',' << piece.info.value << ',' << count << '\n';
    last_ordinal = piece.info.ordinal;
    ++written;
}
} // namespace

bool zone_mode_initialize(std::string& why) {
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
    return search.init(g_cfg.zone_nibbles, low, high, seed, checkpoint_search_identity(), why);
}

bool zone_mode_restore(std::string& why) { return search.load(search.checkpoint_path(), why); }
void zone_mode_reset() { search.reset(); }
std::string zone_mode_checkpoint_path() { return search.checkpoint_path(); }
pkh_zone::ZoneStatus zone_mode_status() { return search.status(); }

void zone_mode_describe() {
    const auto state = search.status();
    std::cout << "  Зонный: случайный ключ внутри интервала, затем один случайный\n"
                 "  HEX-разряд заменяется другим допустимым символом 0..9, a..f.\n"
              << "  Каждый шаг проверяет БЛОК из " << state.block_keys << " ключей: младшие "
              << state.skip << " HEX-разрядов\n"
                 "  перебираются целиком, замена выбирается среди разрядов выше них.\n"
                 "  Границы проверяются по полному ключу; на краях блок подрезается.\n"
                 "  Если допустимых замен нет, цепочка завершается. Покрытие не гарантируется.\n";
    pkh_zone::ZoneBatch preview;
    std::string why;
    if (!search.prepare_batch(1, state.block_keys, preview, why) || preview.pieces.empty()) return;
    const auto& first = preview.pieces.front();
    u256 key, last; fe_from_be32(key, first.key); u256_add_u64(last, key, first.count - 1);
    uint8_t last_bytes[32]; u256_to_be32(last_bytes, last);
    std::cout << "  Ближайший отрезок проверит " << first.count << " ключей подряд:\n"
              << "    от " << to_hex(first.key, 32) << "\n"
              << "    до " << to_hex(last_bytes, 32) << "\n";
    auto walker = preview.next;
    walker.current = first.info.raw;
    const uint64_t positions = walker.valid_positions() & ~((UINT64_C(1) << state.skip) - 1);
    int mutable_digits = 0;
    for (uint64_t remaining = positions; remaining; remaining &= remaining - 1) ++mutable_digits;
    std::cout << "  Разрядов под замену: " << mutable_digits << " (позиции выше блока);\n"
              << "  младшие " << state.skip << " разрядов перебираются подряд внутри блока.\n";
}

void zone_mode_save_checkpoint() {
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
        std::cerr << "  Зональный checkpoint НЕ сохранён: " << why << ".\n";
    }
}

void zone_mode_worker() {
    const auto initial = search.status();
    if (!initial.initialized) { zone_error("цепочка не инициализирована"); return; }
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
        if (packed) std::cout << "  Зонный: небольшие отрезки объединяются в один заход видеокарты.\n";
    }
    std::ofstream log;
    uint64_t logged = 0, last_logged = UINT64_MAX;
    if (g_cfg.zone_debug) {
        log.open(plat_wide("zone_debug.csv").c_str(), std::ios::trunc);
        if (log) log << "step,digit_position_right,previous,block_first,block_last,value,count\n";
        else { std::lock_guard<std::mutex> output(g_mtx_cout);
    MinuteSpeed::LogScope speed_log;
            std::cerr << "  ВНИМАНИЕ: не удалось открыть zone_debug.csv\n"; }
    }
    BatchPrefetch<pkh_zone::ZoneSearch,pkh_zone::ZoneBatch> prefetch(search,capacity,piece_keys,
        packed && !g_cfg.zone_debug && !std::getenv("PKH_DISABLE_PREFETCH"));
    while (!g_stop.load(std::memory_order_relaxed)) {
        pause_check();
        if (g_stop.load(std::memory_order_relaxed)) break;
        pkh_zone::ZoneBatch batch;
        std::string why;
        if (!prefetch.take(batch, why)) { zone_error(why); return; }
        if (batch.pieces.empty()) {
            if (!search.commit_batch(batch, why)) zone_error(why);
            else { std::lock_guard<std::mutex> output(g_mtx_cout);
    MinuteSpeed::LogScope speed_log;
                std::cout << "  Зональная цепочка завершена: допустимых замен больше нет.\n"; }
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
        // A fully packed batch has already checked every piece on the GPU.
        // Only clipped tails or a failed GPU attempt need per-piece replay.
        if (!packed_complete || batch.full_starts.size()/32 != batch.pieces.size())
        for (const auto& piece : batch.pieces) {
            if (g_stop.load(std::memory_order_relaxed)) break;
            if (packed_complete && piece.count == piece_keys) continue;
            u256 key; fe_from_be32(key, piece.key); scan_chunk(key, piece.count);
        }
        if (g_stop.load(std::memory_order_relaxed)) break;
        if (g_count.load(std::memory_order_relaxed) - before != batch.keys) {
            zone_error("число проверенных ключей не совпало с подготовленным заходом"); return;
        }
        // Publish the random state and exact intra-block offset only after the
        // entire attempt and all CPU confirmations/local finding saves finished.
        if (!search.commit_batch(batch, why)) { zone_error(why); return; }
        if (log.is_open()) {
            for (const auto& piece : batch.pieces)
                log_piece(log, piece, initial.block_keys, logged, last_logged);
            if (log) log.flush();
        }
    }
}
