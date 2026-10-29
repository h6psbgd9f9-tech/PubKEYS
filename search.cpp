// ============================================================================
//  search.cpp — единственный экземпляр SearchContext и переходные псевдонимы.
//
//  Псевдонимы (g_range_start и прочие) -- не украшение, а способ провести
//  правку по частям. Обход и горячий цикл уже получают контекст явным
//  доводом; меню, уведомления и вывод продолжают обращаться к прежним именам,
//  которые теперь указывают внутрь того же экземпляра. Ни двойного состояния,
//  ни рассинхронизации при этом не возникает: значение одно, имён к нему два.
// ============================================================================
#include "search.h"

SearchContext g_search;

// Ссылки на поля единственного экземпляра. Для массива u256 это ссылка на
// массив: uint64_t (&)[4] -- законная и единственно верная форма.
u256 &g_range_start = g_search.range_start;
u256 &g_range_end   = g_search.range_end;

std::atomic<uint64_t> &g_count     = g_search.count;
std::atomic<uint64_t> &g_found_num = g_search.found_num;
std::atomic<int>      &g_zone_idx  = g_search.zone_idx;
std::atomic<bool>     &g_stop      = g_search.stop;

std::mutex &g_mtx_sample   = g_search.mtx_sample;
u256       &g_sample_key   = g_search.sample_key;
bool       &g_sample_valid = g_search.sample_valid;

PubDB &g_db = g_search.db;

ChunkCounter &g_chunk_seq = g_search.chunk_seq;

// Число блоков: раньше отдельный массив, теперь поле контекста.
uint8_t (&g_total_blocks256)[32] = g_search.total_blocks256;
