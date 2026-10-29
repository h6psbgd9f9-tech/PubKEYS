// ============================================================================
//  traverse.h — как обходится диапазон и чем это подтверждается.
// ============================================================================
#pragma once
#include "common.h"

#include <cstring>
#include <random>
#include <string>

// ============================================================================
//  НОМЕР УЧАСТКА — 256 БИТ
//
//  Был uint64_t. Перестановка KXE работала на полных 256 битах, а счётчик,
//  который её кормит, выдавал не больше 2^64 разных номеров — то есть при
//  широком диапазоне часть блоков не получала номера вовсе и не доставалась
//  никому. Программа честно об этом предупреждала, но предупреждение — не
//  решение.
//
//  Теперь номер полный. Счётчик защищён мьютексом, а не атомарный: 256 бит
//  атомарно не увеличить, зато участок считается миллисекундами и дольше, так
//  что взятие замка раз на участок не стоит ничего.
//
//  Хранение — 32 байта, старший байт первым. Это не прихоть: при таком
//  порядке обычное побайтовое сравнение совпадает с числовым, и номера можно
//  класть в std::set без отдельного компаратора.
// ============================================================================
struct ChunkIdx {
    uint8_t b[32] = {};

    bool operator<(const ChunkIdx &o) const { return memcmp(b, o.b, 32) < 0; }
    bool operator==(const ChunkIdx &o) const { return memcmp(b, o.b, 32) == 0; }

    void from_u64(uint64_t v) {
        memset(b, 0, 32);
        for (int i = 0; i < 8; i++) b[31 - i] = (uint8_t)(v >> (8 * i));
    }
    void from_u256(const u256 v) { u256_to_be32(b, v); }
    void to_u256(u256 out) const { fe_from_be32(out, b); }

    // +1 с переносом. Переполнение 2^256 недостижимо: блоков не больше 2^246.
    void inc() {
        for (int i = 31; i >= 0; i--) if (++b[i]) break;
    }
    std::string hex() const { return to_hex(b, 32); }
    bool from_hex_str(const std::string &s) {
        if (s.size() != 64) return false;
        u256 t;
        if (!hex_to_u256(s.c_str(), t)) return false;
        from_u256(t);
        return true;
    }
};

// Счётчик выдачи участков. next() отдаёт очередной номер и сдвигает себя.
class ChunkCounter {
public:
    void next(ChunkIdx &out) {
        std::lock_guard<std::mutex> lk(m_);
        out = v_;
        v_.inc();
    }
    void set(const ChunkIdx &x) { std::lock_guard<std::mutex> lk(m_); v_ = x; }
    ChunkIdx get() { std::lock_guard<std::mutex> lk(m_); return v_; }
private:
    std::mutex m_;
    ChunkIdx   v_;
};

extern ChunkCounter &g_chunk_seq;

// Сколько участков попадает в журнал: дальше файл только растёт впустую.
static const uint64_t DBG_LIMIT = 10000;

// Границы участка по его номеру. Отдельной функцией нарочно: ею пользуется
// и рабочий цикл, и тест покрытия — иначе проверять было бы нечего.
bool chunk_bounds(const ChunkIdx &idx, u256 out_start, uint64_t &out_count);
// Requested block size for the current mode: zone and wave each have their
// own size; other modes use 2^chunk_bits. Actual tail lengths are clipped.
uint64_t chunk_keys();

// СМЕЩЕНИЕ ОБРАЗЦА ВНУТРИ УЧАСТКА.
//
// Одна функция на две стороны -- на блок ПРОВЕРКА (экран, Telegram) и на
// журнал участков. Пока их было две, экран и журнал показывали РАЗНЫЕ ключи
// одного и того же блока: журнал -- начало, выровненное по 4096 и потому
// вечно с тремя нулями на конце, экран -- ключ изнутри со случайным хвостом.
// Оба верны, но сверить их между собой было нельзя, и разница выглядела
// ошибкой. Теперь обе стороны берут один и тот же ключ.
uint64_t sample_offset(const u256 k0, uint64_t count);

void debug_log_chunk(int thread, const ChunkIdx &idx, const u256 start, uint64_t count);
// У каких режимов позиция вообще что-то значит.
//
// Детерминированный и KXE сохраняют подтверждённый номер участка. Волновой
// и зональный сохраняют зерно сбалансированного обхода, номер участка и
// точное число ключей его завершённого префикса. Случайный режим сохранённой
// позиции не имеет.
// ============================================================================
//  HEX-МАСКА
//
//  Ключи вида K + j*16^N: младшие N нибблов заморожены, верхние перебираются
//  от значения в K и вверх. Это РЕШЁТКА, а не отрезок, и говорить о ней надо
//  соответственно: «проверены все ключи с таким хвостом», а не «проверён
//  диапазон».
//
//  Маска работает ВНУТРИ интервала 2^a..2^b, поэтому реальное число
//  кандидатов -- пересечение, и его надо посчитать до старта: две настройки
//  легко задать так, что общего у них не останется ни одного ключа.
// ============================================================================
extern u256 g_mask_step;    // 16^N
extern u256 g_mask_first;   // первый кандидат, попавший в интервал
extern u256 g_mask_total;   // сколько кандидатов внутри интервала

std::string u256_to_dec(const u256 v);

bool mask_active();
// Пересчитывает шаг, первый ключ и число кандидатов по g_cfg и границам.
// Вызывать после compute_range(). Возвращает false, если пересечение пусто.
bool mask_compute();
// Разметка ключа скобками: что перебирается, что константа.
std::string mask_layout(const std::string &hex, int froz);
// Подпись под разметкой: скобка каждой части ровно под ней.
std::string mask_legend(int froz);

bool mode_has_checkpoint();
class PubDB;
// Hash the effective, sorted and merged targets once, before offering resume.
// Uses constant extra memory. False leaves checkpoint loading/saving disabled.
bool checkpoint_prepare_identity(const PubDB &db, std::string &why);
// Exact completed scalar/candidate count for a finite traversal watermark.
bool checkpoint_completed_keys(const ChunkIdx &seq, u256 out);
// Used to account exactly for KXE's possibly short last block after resume.
bool kxe_tail_precedes(const ChunkIdx &seq);
// Wave and zone retain complete original nibble motions around full-width
// base points. An exact separating check prevents overlaps between complete
// patterns. Production then checks the exact complement of that family,
// so all eligible candidates in the selected interval are covered once.
bool unique_blocks_mode();
bool traversal_full_coverage();
int traversal_block_bits();
bool permuted_tail_precedes(const ChunkIdx &seq);
void save_checkpoint();
bool load_checkpoint(ChunkIdx &seq);
std::string checkpoint_path();   // имя файла для текущей конфигурации
// Decoded shape state stays staged until the resume UI accepts it. Seed is
// restored by load_checkpoint before any following ordinal is reconstructed.
bool checkpoint_loaded_shape(ChunkIdx &seq, u256 completed, uint64_t *seed = nullptr);
void checkpoint_clear_loaded_shape();

// ----------------------- учёт ЗАВЕРШЁННЫХ участков ---------------------------
//
// Чекпоинт обязан хранить не «докуда раздали», а «докуда ДОСЧИТАНО».
// Участки выдаются по порядку, но заканчиваются вразнобой: пока поток 0
// молотит участок 50, соседи успевают закрыть 51..107. Позиция, полученная
// вычитанием числа потоков из счётчика выдачи, в таком раскладе указывает
// на 100 -- то есть ВЫШЕ незакрытого пятидесятого. После восстановления
// участок 50 не переберёт никто, и в покрытии останется дыра, о которой
// программа не подозревает. А режим при этом называется детерминированным и
// обещает полный перебор.
//
// Поэтому: участок засчитывается только когда досчитан до конца, а
// сохраняется наименьший НЕзавершённый номер. Всё ниже него закрыто
// заведомо; что-то выше могло быть посчитано дважды -- это безобидно, в
// отличие от пропуска.
struct SearchContext;   // определён в search.h

// Caller contract: publish only after every candidate in this ordinal was
// checked and every finding committed. An interrupted/uncertain ordinal must
// remain uncommitted. A proven empty clipped/already-covered ordinal is complete
// and must be published too. Repeating a completed ordinal is idempotent.
void     chunk_done(SearchContext &ctx, const ChunkIdx &idx);
void     chunk_done(SearchContext &ctx, const ChunkIdx &idx, uint64_t completed_keys);
ChunkIdx checkpoint_seq(SearchContext &ctx);
void     checkpoint_reset(SearchContext &ctx, const ChunkIdx &seq);
void     checkpoint_reset(SearchContext &ctx, const ChunkIdx &seq, const u256 completed_keys);
// Coherent count and watermark. False means an unweighted shape completion
// was published and its exact candidate count is consequently unknown.
bool     checkpoint_snapshot(SearchContext &ctx, ChunkIdx &seq, u256 completed_keys);
// Completed out-of-order ordinals above the watermark, not active workers.
size_t   chunks_in_flight(SearchContext &ctx);

// ------------------------- старший разряд ключа ------------------------------
extern int g_top_ni;        // позиция старшего значащего ниббла, 0 = самый левый
extern int g_top_vmin, g_top_vmax;
extern int g_zone_first, g_zone_count;

// Wave's block size. The legacy ZONE_BLOCK name is retained for its CUDA
// layout; the replacement zone walk uses zone_block_keys() independently.
extern uint64_t ZONE_BLOCK;
extern int g_skip_low;

// Call before workers; rebuilds wave tables for the selected block size.
void set_block_nibbles(int nib);

void compute_top_nibble();
// Rebuild from actual closed scalar bounds, after any explicit range narrowing.
void compute_zone_plan();
void apply_top_nibble(uint8_t k[32]);

// Перестановка номеров блоков: обратима, значит без повторов.
uint64_t kxe_permute(uint64_t x, uint64_t total, uint64_t seed);
// Пересчитывает g_total_blocks256 из текущих g_range_start/g_range_end/
// traversal_block_bits(). Вызывать там же, где считается g_total_chunks (compute_range
// в ui.cpp) -- то есть сразу после того, как границы диапазона известны.
void compute_total_blocks256();

// Число блоков когда-то приходилось сверять с разрядностью счётчика: он был
// uint64_t, и при широком диапазоне часть блоков не получала номера. Обе
// проверки -- blocks_fit_u64() и blocks_bits() -- вместе с сообщением о
// неполном покрытии убраны: счётчик стал 256-битным, сверять больше не с чем.
void     zone_set_seed(uint64_t s);
// Ограниченный разряд зон: позиция СЛЕВА (-1 -- шагать нечем) и его пределы.
// Берутся из интервала после всех сужений, поэтому могут не совпасть с
// g_top_ni/g_top_vmin/g_top_vmax, которые считаются от исходных 2^a..2^b.
int zone_top_position();
int zone_top_min();
int zone_top_max();
// Current effective target/settings identity shared with the zone checkpoint.
std::string checkpoint_search_identity();

// Обрезает участок по [g_range_start, g_range_end]: начало при необходимости
// поднимается до дна диапазона, длина укорачивается по верху. Возвращает
// число ключей, которое законно проверять; 0 -- участок целиком вне
// диапазона. Нужна случайному и зональному режимам, где длина участка не
// вытекает из нарезки.
uint64_t clamp_chunk_to_range(u256 key, uint64_t count);
// ============================================================================
//  ЗОННЫЙ РЕЖИМ: КАК В BITPAZL
// Случайный старт, затем одна случайная допустимая замена выше блока.
// Младшие skip разрядов полностью перебираются внутри выровненного блока;
// его края обрезаются по диапазону. Нет циклов по 16 и новых случайных баз.
// Production saves the complete chain and RNG in ZoneSearch's checkpoint.
struct ZoneInfo {
    bool valid = false;
    int skip = 0;
    uint64_t block_keys = 0, count = 0, ordinal = 0;
    int pos = -1, zone = -1; // changed position from left, -1 for initial block
    int value = -1;
    u256 raw{}, base{};      // aligned block key and preceding aligned key
};

// Independent configured production block size. The finite partition clips
// its last block even when the complete interval is smaller than one block.
int zone_block_nibbles();
uint64_t zone_block_keys();

// ============================================================================
//  ВОЛНОВОЙ РЕЖИМ
//
// Production next_chunk scans a finite block permutation; the shape helper
// below describes the retained legacy wave for isolated model comparisons.
//
//  От старта растёт волна: сначала крутится один младший разряд, 16
//  положений; потом два соседних в лад; потом три — и так до всех 64.
//  Последний ключ фазы становится стартом следующей.
//
//      фаза 1:  ...............[4]      один разряд
//      фаза 2:  ..............[2][4]    два в лад
//      фаза 3:  .............[c][1][c]  три в лад
//
//  Переворот f -> 0 БЕЗ переноса: каждый разряд живёт по модулю 16 сам по
//  себе. Это не сложение, а узор, и в этом вся его суть.
//
//  Состояний на старт: 1 + 64*15 = 961. Ключ считается СРАЗУ по номеру
//  состояния, без прокрутки всей волны:
//
//      разряд_i = старт_i + 15*max(0, фаза-1-i) + (шаг, если i < фаза)
//
//  Множитель 15 -- это то, что разряд набрал за каждую уже завершённую
//  фазу. Формула проверена против честной прокрутки в самотесте.
// Младшие разряды, которые перебирает сам блок: волна начинается выше них.
// Зонный режим задаёт свой размер независимо через zone_nibbles.
#define WAVE_SKIP_LOW g_skip_low

// Верхние границы, по которым отведены таблицы волны. Блок не может быть
// меньше трёх нибблов, значит разрядов под волну не больше 61, а фаз не
// больше 241. Таблицы отводятся по максимуму раз и навсегда; при более
// крупном блоке заполняется меньшая часть.
static const int WAVE_POS_MAX    = 62; // isolated 256-key packed benchmark
static const int WAVE_PHASES_MAX = 245;

struct WaveInfo {
    // A successful zero-length chunk retains this legacy ordinal/checkpoint.
    bool     duplicate = false;
    uint64_t duplicate_of = 0; // earlier ordinal WITHIN the same start
    uint64_t start_no = 0;   // какой по счёту старт
    int      phase    = 0;   // 1..242 сквозной номер фазы; 0 -- сам старт
    int      step     = 0;   // 1..15 внутри фазы
    int      steps    = 0;   // length of this phase, respects the top-digit radix
    int      top      = 63;  // highest changing nibble, counted from the right
    // Челнок и зеркало: одной фазы мало, чтобы понять картину.
    //   pass  1 низ-рост, 2 низ-сужение, 3 верх-рост, 4 верх-сужение
    //   width сколько разрядов работает СЕЙЧАС (1..61)
    //   high  фронт отсчитывается от старшего конца, а не от младшего
    int      pass     = 0;
    int      width    = 0;
    bool     high     = false;
    u256     start;          // ключ старта всей волны
    // База ТЕКУЩЕЙ фазы -- то есть её нулевой шаг, он же последний ключ
    // предыдущей фазы.
    //
    // Сначала журнал печатал в строке Baza старт всей волны, и на первой же
    // фазе это расходилось с показанным Curr: фаза 1 кончалась на ...[e]000,
    // а базой фазы 2 значилось ...[f]000, то есть исходное. Ключи при этом
    // считались верно -- врал только показ, и врал убедительно: разница на
    // один разряд выглядит как ошибка в арифметике, а не в подписи.
    u256     base;
};

// Зерно волнового режима сохраняется в его checkpoint. Зонный отдельно
// сохраняет зерно и полное состояние генератора непрерывной цепочки.
void     wave_set_seed(uint64_t s);
bool     wave_seed_initialized();
uint64_t wave_get_seed();
void     wave_seed_bytes(uint8_t out[32]);

uint64_t wave_states_per_start();   // сколько состояний на одну точку старта

void wave_block_key(const ChunkIdx &idx, u256 out_key, WaveInfo &info);
void debug_log_wave(int thread, const ChunkIdx &idx, const u256 key,
                    const WaveInfo &info, uint64_t count);

void zone_block_key(const ChunkIdx &idx, u256 out_key, ZoneInfo &info);

// Production shapes: complete original patterns rotate through the nonempty
// bit-length subintervals. Each local traversal also covers its exact remainder.
// The legacy adapters remain only for isolated comparison tests.
bool restored_shape_block(const ChunkIdx &idx, u256 out_key, uint64_t &out_count,
                          int &zone_out, WaveInfo *wave = nullptr, ZoneInfo *zone = nullptr);

// Следующий участок. false — обход закончен.
bool next_chunk(const ChunkIdx &idx, u256 out_key, uint64_t &out_count,
                int &zone_out, std::mt19937_64 &rng);
