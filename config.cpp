#include "worker_threads.h"
// ============================================================================
//  config.cpp — config.ini: чтение, запись, приведение значений в разумные рамки.
// ============================================================================
#include <iostream>
#include <cctype>
#include "common.h"
#include "digit_rotation.h"
#include "notify.h"

#include <cstdlib>
#include <cerrno>
#include <sstream>
#include <fstream>
#include "platform.h"

// ЧИСЛО ИЗ ФАЙЛА -- ЛИБО НАСТОЯЩЕЕ, ЛИБО ПРЕЖНЕЕ.
//
// atoi не отличает "0" от мусора: пустая строка, "нет", "1e6", "0x1f" -- всё
// это ноль, и ошибку не отличить от осознанно записанного нуля. Для det_hex
// это стоило бы дорого и тихо: умолчание -1 означает «все значения старшего
// разряда», а ноль -- «только разряд 0», то есть одну шестнадцатую
// пространства. Программа сузила бы диапазон вшестнадцатеро и показывала бы
// честный процент от суженного -- ни одного признака ошибки за месяц счёта.
//
// Здесь: не разобралось -- оставляем прежнее значение и говорим вслух.
static bool parse_int(const std::string &key, const std::string &v, int &dst) {
    if (v.empty()) {
        std::cout << "  config.ini: у ключа " << key
                  << " пустое значение — оставлено " << dst << "\n";
        return false;
    }
    errno = 0;
    char *end = nullptr;
    const long n = strtol(v.c_str(), &end, 10);
    while (end && (*end == ' ' || *end == '\t')) end++;
    if (errno == ERANGE || !end || *end != '\0' || n < -2147483647L || n > 2147483647L) {
        std::cout << "  config.ini: " << key << "=" << v
                  << " — не число, оставлено " << dst << "\n";
        return false;
    }
    dst = (int)n;
    return true;
}

// Тот же разбор для флагов 0/1: «да» вместо 1 не должно молча означать «нет».
static void parse_flag(const std::string &key, const std::string &v, bool &dst) {
    int t = dst ? 1 : 0;
    if (parse_int(key, v, t)) dst = (t != 0);
}

// Токен берётся из окружения, а в исходнике его нет вовсе. Пустая строка
// означает «уведомления выключены» -- программа при этом работает как обычно.
std::string tg_default_token() {
    const char *e = getenv("PUBKEYHUNT_TG_TOKEN");
    return e ? std::string(e) : std::string(TG_TOKEN);
}
std::string tg_default_chat() {
    const char *e = getenv("PUBKEYHUNT_TG_CHAT");
    return e ? std::string(e) : std::string(TG_CHAT);
}

Config g_cfg;

// ========================================================

// ---------------------------- имена режимов ---------------------------------
//
// В файле стоит имя, а не число. Причина простая: номер пункта в меню и
// внутреннее значение -- разные вещи (пункты идут по порядку и сгруппированы
// по смыслу, внутренние значения заморожены навсегда ради совместимости).
// Пока в config.ini лежало число, эти две нумерации было не различить на
// глаз: строка mode=4 читалась как «пункт 4», хотя это KXE, то есть пункт 2.
// Имя не путается ни с чем.
const char *mode_name(int mode) {
    switch (mode) {
        case TRAV_SEQ:   return "seq";
        case TRAV_KXE:   return "kxe";
        case TRAV_WAVE:  return "wave";
        case TRAV_ZONE:  return "zone";
        case TRAV_HYBRID:return "hybrid";
        case TRAV_RANDOM:return "random";
        default:         return "seq";
    }
}

const char *mode_title(int mode) {
    switch (mode) {
        case TRAV_SEQ:   return "детерминированный";
        case TRAV_KXE:   return "KXE (перестановочный)";
        case TRAV_WAVE:  return "волновой";
        case TRAV_ZONE:  return "зональный";
        case TRAV_HYBRID:return "гибридный";
        case TRAV_RANDOM:return "случайный";
        default:         return "детерминированный";
    }
}

int mode_from_string(const std::string &s, int def) {
    std::string v = trim(s);
    if (v.empty()) return def;
    if (v == "seq"    || v == "det")  return TRAV_SEQ;
    if (v == "kxe")                   return TRAV_KXE;
    if (v == "wave")                  return TRAV_WAVE;
    if (v == "zone")                  return TRAV_ZONE;
    if (v == "hybrid")                return TRAV_HYBRID;
    if (v == "random" || v == "rnd")  return TRAV_RANDOM;
    // Файлы прежних версий хранили число -- и это ВНУТРЕННЕЕ значение,
    // а не номер пункта меню. Читаем как есть.
    if (v[0] >= '1' && v[0] <= '6' && v.size() == 1) {
        int n = v[0] - '0';
        if (n >= TRAV_RANDOM && n <= TRAV_HYBRID) return n;
    }
    return def;
}

bool config_ini_exists() { std::ifstream f("config.ini"); return f.good(); }

void load_config_ini(Config &c) {
    std::ifstream f("config.ini");
    std::string line;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        size_t e = line.find('=');
        if (e == std::string::npos) continue;
        std::string k = trim(line.substr(0, e)), v = trim(line.substr(e + 1));
        // Retired solver settings must not select a mode or alter the remaining
        // database search. Existing INI files remain readable without a rewrite.
        if (k == "startup_mode" || k.rfind("kangaroo_", 0) == 0) continue;
        // Примечание в конце строки отсекается: файл теперь читают и правят
        // руками, а «seq   # так надо» иначе было бы прочитано целиком и
        // молча отброшено как непонятное имя режима.
        // РЕШЁТКА НАЧИНАЕТ КОММЕНТАРИЙ ТОЛЬКО ПОСЛЕ ПРОБЕЛА.
        //
        // Был список исключений: у всех ключей, кроме tg_token и tg_chat,
        // всё после '#' отсекалось. Список исключений -- это обещание не
        // забыть его пополнить, и оно уже нарушено: archive_pass=Мой#Пароль
        // молча превращался в "Мой". Архив собирался с одним паролем, а
        // человек помнил другой -- и узнал бы об этом ровно тогда, когда
        // архив с находкой не открылся бы. То же с notify_cmd, где решётка
        // законна.
        //
        // Правило вместо списка: комментарий -- это '#' в начале значения
        // либо после пробела, как в ini и в оболочке. «seq   # так надо»
        // по-прежнему читается как seq, а решётка внутри слова остаётся на
        // месте. Забыть тут больше нечего.
        {
            size_t hash_pos = std::string::npos;
            for (size_t i = 0; i < v.size(); i++) {
                if (v[i] != '#') continue;
                if (i == 0 || v[i-1] == ' ' || v[i-1] == '\t') { hash_pos = i; break; }
            }
            if (hash_pos != std::string::npos) v = trim(v.substr(0, hash_pos));
        }
        if      (k == "pubkey")      c.pubkey_path  = v;
        else if (k == "found")       c.found_path   = v;
        else if (k == "threads")     parse_int(k, v, c.threads);
        else if (k == "digit_hours") { if (!parse_digit_hours(v, c.digit_hours)) std::cerr << "  Некорректное время смены цифры: оставлено прежнее.\n"; }
        else if (k == "gpu_power")   parse_int(k, v, c.gpu_power_pct);
        else if (k == "progress")    parse_int(k, v, c.progress_sec);
        else if (k == "check")       parse_int(k, v, c.check_sec);
        else if (k == "range_start") parse_int(k, v, c.range_start_bit);
        else if (k == "range_end")   parse_int(k, v, c.range_end_bit);
        else if (k == "mode")        c.mode         = mode_from_string(v, c.mode);
        else if (k == "want_nk")     parse_flag(k, v, c.want_nk);
        else if (k == "chunk")       parse_int(k, v, c.chunk_bits);
        else if (k == "det_hex") {
            if (!parse_top_hex_digit(v, c.det_hex))
                std::cerr << "  Некорректная HEX-цифра: оставлено прежнее значение.\n";
        }
        else if (k == "debug")       parse_flag(k, v, c.debug_chunks);
        else if (k == "wave_nibbles") {
            int n = c.wave_nibbles;
            if (parse_int(k, v, n)) {
                if (n >= 3 && n <= 10) c.wave_nibbles = n;
                else std::cout << "  config.ini: wave_nibbles должен быть 3..10 — прежнее значение сохранено\n";
            }
        }
        else if (k == "zone_nibbles") {
            int n = c.zone_nibbles;
            if (parse_int(k, v, n)) {
                if (n >= 3 && n <= 15) c.zone_nibbles = n;
                else std::cout << "  config.ini: zone_nibbles должен быть 3..15 — прежнее значение сохранено\n";
            }
        }
        else if (k == "zone_debug")  parse_flag(k, v, c.zone_debug);
        else if (k == "hybrid_nibbles") {
            int n = c.hybrid_nibbles;
            if (parse_int(k, v, n)) {
                if (n >= 3 && n <= 15) c.hybrid_nibbles = n;
                else std::cout << "  config.ini: hybrid_nibbles должен быть 3..15 — прежнее значение сохранено\n";
            }
        }
        else if (k == "hybrid_debug") parse_flag(k, v, c.hybrid_debug);
        else if (k == "tg_token")    c.tg_token     = v;
        else if (k == "tg_chat")     c.tg_chat      = v;
        else if (k == "tg_progress") parse_flag(k, v, c.tg_progress);
        else if (k == "archive_pass") c.archive_pass = v;
        else if (k == "notify_cmd")  c.notify_cmd   = v;
        else if (k == "wave_start")  c.wave_start   = v;
        // Маска сохраняется вместе с числом замороженных нибблов: по
        // отдельности ни то, ни другое ничего не значит, а имя чекпоинта
        // строится из обоих.
        else if (k == "mask_hex")    c.mask_hex     = v;
        else if (k == "mask_froz")   parse_int(k, v, c.mask_froz);
        else if (k == "signal_account") c.signal_account = v;
        else if (k == "signal_to")      c.signal_to      = v;
        else if (k == "signal_cli")     c.signal_cli     = v;
        else if (k == "signal_progress") parse_flag(k, v, c.signal_progress);
        // Опечатка в имени ключа проглатывалась молча: «treads=8» означало
        // счёт в один поток при полной уверенности, что их восемь.
        else std::cout << "  config.ini: неизвестный ключ «" << k << "» — пропущен\n";
    }
}

void save_config_ini(const Config &c) {
    // В config.ini попадает токен Telegram -- значит, это файл с секретом, и
    // читать его всем пользователям машины незачем. Права закрываются до
    // создания потока записи, чтобы не оставлять окна.
    // ЧЕРЕЗ ВРЕМЕННЫЙ ФАЙЛ, КАК ЧЕКПОИНТ.
    //
    // Прежде здесь был ofstream без флагов: он обнуляет файл сразу, а пишет
    // постепенно. Смерть процесса между этими двумя событиями (на Windows это
    // перезагрузка по обновлению, которая происходит сама) оставляла обрубок,
    // и вместе с хвостом пропадали signal_*, notify_cmd и токен -- то есть
    // ВСЕ уведомления. Поиск после этого шёл как обычно, и сообщить о находке
    // было бы некому.
    std::ostringstream f;
    f << "# PubkeyHunt — сохранено " << now_string() << "\n"
      << "pubkey="      << c.pubkey_path    << "\n"
      << "found="       << c.found_path     << "\n"
      << "threads="     << c.threads        << "\n"
      << "gpu_power="   << c.gpu_power_pct  << "\n"
      << "progress="    << c.progress_sec   << "\n"
      << "check="       << c.check_sec      << "\n"
      << "range_start=" << c.range_start_bit<< "\n"
      << "range_end="   << c.range_end_bit  << "\n"
      << "# mode: seq — детерминированный, kxe — перестановочный,\n"
         "#       random — случайный, zone — зональный, wave — волновой, hybrid — гибридный\n"
      << "mode="        << mode_name(c.mode)<< "\n"
      << "want_nk="     << (c.want_nk ? 1 : 0) << "\n"
      << "chunk="       << c.chunk_bits     << "\n"
      << "det_hex="     << c.det_hex        << "\n"
      << "digit_hours=" << format_digit_hours(c.digit_hours) << "\n"
      << "debug="       << (c.debug_chunks ? 1 : 0) << "\n"
      << "# Зонный шаг: 16^N ключей подряд; один HEX-разряд выше блока меняется случайно.\n"
      << "wave_nibbles=" << c.wave_nibbles << "\n"
      << "zone_nibbles=" << c.zone_nibbles << "\n"
      << "zone_debug="   << (c.zone_debug ? 1 : 0) << "\n"
      << "# Гибрид: A выбирается целиком случайно, B перебирается подряд до конца.\n"
      << "hybrid_nibbles=" << c.hybrid_nibbles << "\n"
      << "hybrid_debug=" << (c.hybrid_debug ? 1 : 0) << "\n"
      << "tg_token="    << c.tg_token       << "\n"
      << "tg_chat="     << c.tg_chat        << "\n"
      << "tg_progress=" << (c.tg_progress ? 1 : 0) << "\n"
      << "# Устаревшее поле: новые находки сохраняются в Found-файле и архиве без пароля.\n"
      << "archive_pass=" << c.archive_pass << "\n"
      << "# Внешнее уведомление: команда получает <текст> и <путь к архиву>.\n"
      << "# Пример для Signal лежит рядом: notify-signal.sh\n"
      << "# Откуда начинает волновой режим. Пусто = случайная точка.\n"
      << "wave_start="   << c.wave_start   << "\n"
      << "mask_hex="     << c.mask_hex     << "\n"
      << "mask_froz="    << c.mask_froz    << "\n"
      << "notify_cmd="   << c.notify_cmd   << "\n"
      << "# Signal. Привязка: signal-cli link -n \"PubkeyHunt\" (доп. устройство).\n"
      << "# signal_to можно поставить свой же номер -- придёт в «Заметки».\n"
      << "signal_account="  << c.signal_account << "\n"
      << "signal_to="       << c.signal_to      << "\n"
      << "signal_cli="      << c.signal_cli     << "\n"
      << "signal_progress=" << (c.signal_progress ? 1 : 0) << "\n";

    const std::string tmp = "config.ini.tmp";
    secure_create(tmp);
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        if (!o) { std::cerr << "  config.ini не сохранён: не создать " << tmp << "\n"; return; }
        const std::string s = f.str();
        o.write(s.data(), (std::streamsize)s.size());
        o.flush();
        o.close();                  // ошибка записи может вылезти только здесь
        if (!o) {
            std::cerr << "  config.ini не сохранён: ошибка записи\n";
            std::remove(tmp.c_str());
            return;
        }
    }
    if (!plat_fsync_file(tmp) || !plat_replace_file(tmp, "config.ini")) {
        std::cerr << "  config.ini НЕ сохранён — настройки останутся прежними.\n";
        std::remove(tmp.c_str());
        return;
    }
    plat_fsync_dir(".");
    secure_create("config.ini");
}

void sanitize_config(Config &c) {
    c.threads = bounded_worker_threads(c.threads);
    // 0 -- это "спросить/взять совет", поэтому не трогаем. Осознанный выбор
    // держим в разумных пределах: ниже половины смысла нет (пауза длиннее
    // счёта), выше ста не бывает.
    if (!std::isfinite(c.digit_hours) || c.digit_hours < 0 || c.digit_hours > 8760) c.digit_hours = 2.0;
    if (c.gpu_power_pct != 0) {
        if (c.gpu_power_pct < 50)  c.gpu_power_pct = 50;
        if (c.gpu_power_pct > 100) c.gpu_power_pct = 100;
    }
    if (c.progress_sec < 1)   c.progress_sec = 1;
    if (c.check_sec    < 1)   c.check_sec = 1;
    if (c.range_start_bit < 0)   c.range_start_bit = 0;
    if (c.range_start_bit > 255) c.range_start_bit = 255;
    if (c.range_end_bit <= c.range_start_bit) c.range_end_bit = c.range_start_bit + 1;
    if (c.range_end_bit > 256) c.range_end_bit = 256;
    if (c.mode < TRAV_RANDOM || c.mode > TRAV_HYBRID) c.mode = TRAV_RANDOM;
    if (c.det_hex < -1 || c.det_hex > 15) c.det_hex = -1;
    if (c.chunk_bits < 10) c.chunk_bits = 10;
    if (c.chunk_bits > 40) c.chunk_bits = 40;
    if (c.wave_nibbles < 3 || c.wave_nibbles > 10) c.wave_nibbles = 4;
    if (c.zone_nibbles < 3 || c.zone_nibbles > 15) c.zone_nibbles = 10;
    if (c.hybrid_nibbles < 3 || c.hybrid_nibbles > 15) c.hybrid_nibbles = 10;
    // МАСКА ПРОВЕРЯЕТСЯ ЗДЕСЬ, ИНАЧЕ ДИАГНОЗ БУДЕТ ЛОЖНЫЙ.
    //
    // config.ini правят руками. Строка не из 64 знаков или не HEX -- и
    // mask_compute() возвращает false, а меню объявляет «МАСКА И ИНТЕРВАЛ НЕ
    // ПЕРЕСЕКАЮТСЯ». Причина при этом не в границах, и человек будет
    // подбирать диапазон, пока не сдастся. Лучше сказать про строку.
    if (!c.mask_hex.empty()) {
        std::string t;
        for (char ch : c.mask_hex) if (!isspace((unsigned char)ch)) t += ch;
        bool ok = (t.size() == 64);
        if (ok) for (char ch : t)
            if (!isxdigit((unsigned char)ch)) { ok = false; break; }
        if (!ok) {
            std::cout << "  config.ini: mask_hex должен быть 64 знака HEX"
                      << " (сейчас " << t.size() << ") — маска отключена.\n";
            c.mask_hex.clear();
            c.mask_froz = 0;
        } else {
            c.mask_hex = t;
            if (c.mask_froz < 1 || c.mask_froz > 64) {
                std::cout << "  config.ini: mask_froz вне 1..64 (сейчас "
                          << c.mask_froz << ") — маска отключена.\n";
                c.mask_hex.clear();
                c.mask_froz = 0;
            }
        }
    } else if (c.mask_froz) {
        c.mask_froz = 0;                       // без ключа число ничего не значит
    }

    if (c.pubkey_path.empty()) c.pubkey_path = "pubkey.txt";
    if (c.found_path.empty())  c.found_path  = "Found.txt";
}

// ========================================================
