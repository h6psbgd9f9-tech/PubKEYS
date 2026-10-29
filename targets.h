// ============================================================================
//  targets.h — что именно ищем.
//
//  Ключ поиска — 32 байта. Для публичных ключей это координата X, для
//  адресов — HASH160, дополненный нулями. Структура базы одна и та же:
//  отсортированный массив плюс дешёвый предфильтр по старшим битам.
// ============================================================================
#pragma once
#include "common.h"
#include "bloom.h"
#include "platform.h"   // plat_wide: путь с кириллицей

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <set>

// БАЗА ПУБЛИЧНЫХ КЛЮЧЕЙ
//
// Сравнение идёт по координате X, и притом прямо в лимбах — в том же
// представлении, в котором её выдаёт арифметика кривой. Ни сериализации в
// байты, ни хеширования на горячем пути не остаётся: на ключ приходится
// одно обращение к фильтру Блума.
//
// Y полностью определяется координатой X с точностью до знака, поэтому
// хранить его незачем — достаточно чётности (префикс 02 или 03).
// Ключам k и n−k отвечает один и тот же X при разной чётности: это и есть
// «зеркало», которое включается пунктом меню.
// ========================================================
struct PubEntry {
    uint64_t x[4];
    uint8_t  par;   // битовая маска: 1 — есть 02, 2 — есть 03
    bool operator<(const PubEntry &o) const {
        for (int i = 3; i >= 0; i--) {
            if (x[i] != o.x[i]) return x[i] < o.x[i];
        }
        return false;
    }
    bool same_x(const PubEntry &o) const {
        return x[0]==o.x[0] && x[1]==o.x[1] && x[2]==o.x[2] && x[3]==o.x[3];
    }
};

bool point_from_pubkey(const std::string &hex, Point &out);

class PubDB {
public:
    std::vector<PubEntry> items;

    // Предфильтр. Раньше здесь стоял фильтр Блума с тремя murmur-хешами:
    // девять умножений и три обращения к памяти на КАЖДЫЙ ключ — дороже,
    // чем вся арифметика кривой на этот же ключ.
    // Координата X случайного ключа распределена равномерно, поэтому
    // перемешивать её незачем: старшие биты сами по себе годятся в индекс.
    //   * до 8 целей  — сравнение старшего лимба прямо в регистрах.
    //                   Сравнивается ОДИН лимб из четырёх, поэтому строго
    //                   говоря ложное срабатывание возможно -- с вероятностью
    //                   около 2^-64 на цель. Точность обеспечивает confirm(),
    //                   он сверяет все 256 бит;
    //   * дальше      — битовая карта по старшим битам, одно обращение.
    std::vector<uint64_t> bloom;     // блочный фильтр, см. bloom.h
    uint64_t block_mask = 0;
    uint64_t small_x3[8];
    int      n_small = 0;
    size_t   n_comp = 0, n_uncomp = 0, n_xonly = 0;

    bool load(const std::string &path) {
        // plat_wide, а не сам path: этот путь человек набирает в меню, и на
        // Windows узкий конструктор потока прочитал бы байты UTF-8 в кодовой
        // странице системы. Файл с кириллицей в пути не открылся бы, а
        // сообщение об ошибке ниже показало бы правильное имя -- искать
        // причину пришлось бы долго.
        std::ifstream f(plat_wide(path).c_str());
        if (!f) {
            // Относительный путь ищется от текущего каталога, а не от места,
            // где лежит программа. Показываем каталог явно: иначе непонятно,
            // куда именно класть файл.
            char cwd[1024] = {0};
            if (!getcwd(cwd, sizeof(cwd) - 1)) strcpy(cwd, "?");
            std::cout << C_RED << "\n  Не удалось открыть файл публичных ключей: "
                      << path << C_RESET << "\n"
                      << "  Искали в каталоге: " << cwd << "\n"
                      << "  Положите файл сюда либо укажите полный путь в меню.\n\n";
            return false;
        }
        // На большой базе рост вектора через push_back стоит дороже самого
        // разбора: 50 млн записей — это 2 ГБ, и каждое удвоение копирует всё
        // целиком. Считаем строки одним быстрым проходом и резервируем точно.
        {
            std::ifstream cnt(plat_wide(path).c_str(), std::ios::binary);
            if (cnt) {
                std::vector<char> buf(1 << 20);
                size_t lines = 0;
                while (cnt.read(buf.data(), (std::streamsize)buf.size()) || cnt.gcount()) {
                    const size_t got = (size_t)cnt.gcount();
                    for (size_t i = 0; i < got; i++) if (buf[i] == '\n') lines++;
                    if (!cnt) break;
                }
                if (lines) items.reserve(lines + 16);
            }
        }

        std::string line;
        size_t bad = 0, ln = 0;
        while (std::getline(f, line)) {
            ln++;
            // Метка порядка байт от Блокнота Windows. Без её снятия первая
            // строка файла, сохранённого «как есть» в Блокноте, отвергалась
            // как «не распознано» -- одна цель теряется, а диагностика
            // указывает не на ту причину.
            if (ln == 1 && line.size() >= 3 &&
                (unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB &&
                (unsigned char)line[2] == 0xBF)
                line.erase(0, 3);
            line = trim(line);
            if (line.empty() || line[0] == '#') continue;
            // Формат "адрес<TAB>publичный_ключ" -- как пишет
            // extract_pubkeys.py в pubkeys_found.txt. Берём то, что ПОСЛЕ
            // таба (сам ключ), адрес не трогаем -- он нам не нужен, X и так
            // определяется по самому ключу.
            size_t tab = line.find('\t');
            if (tab != std::string::npos) {
                line = trim(line.substr(tab + 1));
            } else {
                // Обычный wallets.txt: один ключ на строку, возможен мусор
                // после пробела/запятой/точки с запятой -- берём первое слово.
                size_t sp = line.find_first_of(" \t,;");
                if (sp != std::string::npos) line = line.substr(0, sp);
            }
            if (!add_hex(line)) {
                if (bad < 5)
                    fprintf(stderr, "  строка %zu: не распознано — %s\n", ln, line.c_str());
                bad++;
            }
        }
        if (items.empty()) {
            fprintf(stderr, "%s  В файле %s нет ни одного публичного ключа.%s\n",
                    C_RED, path.c_str(), C_RESET);
            return false;
        }
        size_t read = n_comp + n_uncomp + n_xonly;
        build();
        // Формы считаются при чтении, а итог — после склейки одинаковых X:
        // сжатый и несжатый вид одного ключа дают ровно одну цель. Пока в
        // строке стояло только «ключей N», числа не сходились и выглядели
        // как ошибка. Поэтому показываем и прочитанное, и уникальное.
            std::cout << "  Публичных ключей : " << fmt_num(items.size()) << " уникальных X";
            if (read != items.size())
                std::cout << "  (прочитано строк " << read << ", одинаковые X слиты)";
            std::cout << "\n  Из них           : сжатых " << n_comp
                      << ", несжатых " << n_uncomp << ", только X " << n_xonly << "\n";
        if (n_small)
            std::cout << "  Предфильтр       : прямое сравнение " << n_small
                      << " целей в регистрах\n";
        else {
            std::cout << "  Предфильтр       : Bloom " << (size_t)bloom_mb() << " МБ, "
                      << (size_t)bits_per_target() << " бит на ключ (блочный, "
                      << (block_mask + 1) << " блоков)";
            if (bloom_capped())
                std::cout << "  (упёрлись в предел " << BLOOM_CAP_MB << " МБ)";
            std::cout << "\n";
        }
        if (bad) std::cout << "  Пропущено строк  : " << bad << "\n";
        return true;
    }

    // "02..."/"03..." (66 hex), "04..." (130 hex) или голый X (64 hex)
    bool add_hex(const std::string &s) {
        for (char ch : s) if (!isxdigit((unsigned char)ch)) return false;
        // Validate once on input; the GPU/CPU lookup loop remains unchanged.
        Point parsed;
        if (!point_from_pubkey(s.size() == 64 ? "02" + s : s, parsed)) return false;
        PubEntry e{};
        std::string xs;
        if (s.size() == 66 && (s[0]=='0') && (s[1]=='2' || s[1]=='3')) {
            xs = s.substr(2);
            e.par = (s[1] == '2') ? 1 : 2;
            n_comp++;
        } else if (s.size() == 130 && s[0]=='0' && s[1]=='4') {
            xs = s.substr(2, 64);
            // чётность Y — по последней шестнадцатеричной цифре
            int last = isdigit((unsigned char)s.back()) ? s.back()-'0'
                                                        : tolower(s.back())-'a'+10;
            e.par = (last & 1) ? 2 : 1;
            n_uncomp++;
        } else if (s.size() == 64) {
            xs = s;
            e.par = 3;            // чётность неизвестна — принимаем обе
            n_xonly++;
        } else return false;

        u256 xv;
        if (!hex_to_u256(xs.c_str(), xv)) return false;
        memcpy(e.x, xv, 32);
        items.push_back(e);
        return true;
    }

    void build() {
        std::sort(items.begin(), items.end());
        // Склейка записей с одинаковым X, объединяя маски чётности. Делается
        // НА МЕСТЕ: второй вектор на 50 млн целей — это лишние 2 ГБ пика,
        // ровно в тот момент, когда память и так занята полностью.
        {
            size_t w = 0;
            for (size_t r = 0; r < items.size(); r++) {
                if (w && items[w - 1].same_x(items[r])) items[w - 1].par |= items[r].par;
                else items[w++] = items[r];
            }
            items.resize(w);
            // shrink_to_fit НЕ бесплатен: в libstdc++/libc++/MSVC он выделяет
            // новый буфер, переносит и освобождает старый -- то есть даёт
            // ровно тот двойной пик по памяти, ради ухода от которого склейка
            // и делалась на месте. Для 50 млн целей это 2 ГБ против 4 ГБ.
            // Ужимаем только там, где выигрыш заметен, а база невелика.
            const size_t cap = items.capacity();
            if (cap && w * 8 < cap * 7 && cap < (size_t)8000000)
                items.shrink_to_fit();
        }

        bloom.clear();
        n_small = 0;
        // До восьми целей фильтр не нужен вовсе: сравнение старших лимбов
        // идёт в регистрах. Сравнивается только старший лимб, так что ложное
        // срабатывание не исключено совсем -- оно лишь невероятно (порядка
        // 2^-64 на цель), а точную проверку делает confirm() по всем 256 битам.
        if (items.size() <= 8) {
            for (size_t i = 0; i < items.size(); i++) small_x3[i] = items[i].x[3];
            n_small = (int)items.size();
            return;
        }
        uint64_t blocks = bloom_blocks_for(items.size());
        block_mask = blocks - 1;
        bloom.assign((size_t)blocks * BLOOM_BLOCK_WORDS, 0);
        for (const auto &it : items) bloom_add(bloom.data(), block_mask, it.x);
    }

    // Сколько памяти занял фильтр и сколько битов пришлось на цель.
    double bloom_mb() const { return bloom.size() * 8.0 / 1048576.0; }
    double bits_per_target() const {
        return items.empty() ? 0.0 : (double)bloom.size() * 64.0 / (double)items.size();
    }
    // Упёрлись ли в потолок размера. Сравнивать надо с САМИМ потолком, а не
    // с числом целей: старая проверка items > слов фильтра срабатывала уже
    // на полутора миллионах целей и врала про предел в 2 ГБ.
    bool bloom_capped() const {
        return bloom.size() * 64ULL >= (1ULL << 34);   // 2 ГБ в битах
    }

    // Адрес блока фильтра для этой абсциссы: нужен, чтобы подтянуть строку
    // кэша заранее. nullptr, когда фильтра нет (до восьми целей).
    inline const uint64_t *block_ptr(const fe x) const {
        return bloom.empty() ? nullptr
             : bloom.data() + bloom_block_of(x, block_mask) * BLOOM_BLOCK_WORDS;
    }

    // Горячий путь: только «может быть». Точную проверку делает confirm().
    inline bool maybe(const fe x) const {
        if (n_small) {
            const uint64_t v = x[3];
            for (int i = 0; i < n_small; i++) if (v == small_x3[i]) return true;
            return false;
        }
        return bloom_maybe(bloom.data(), block_mask, x) != 0;
    }

    // Cold path: bit 1 is k, bit 2 is n-k. Both target parities may exist.
    int confirm_mask(const fe x, int parity) const {
        PubEntry key{};
        memcpy(key.x, x, 32);
        auto it = std::lower_bound(items.begin(), items.end(), key);
        if (it == items.end() || !it->same_x(key)) return 0;
        uint8_t want = (parity == 0) ? 1 : 2;      // 0 -> 02, 1 -> 03
        return ((it->par & want) ? 1 : 0) | ((it->par & (want ^ 3)) ? 2 : 0);
    }

    // Legacy single-result API for callers that only need existence/parity.
    int confirm(const fe x, int parity) const {
        const int mask = confirm_mask(x, parity);
        return (mask & 1) ? 1 : (mask & 2);
    }
};

extern PubDB &g_db;

// ========================================================

bool push_targets_to_backend();
