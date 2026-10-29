// ============================================================================
//  notify.h — куда уходит находка: экран, Found.txt, Telegram.
// ============================================================================
#pragma once
#include "common.h"

struct FoundInfo {
    std::string priv_hex, wif_c, wif_u, pub_c, pub_u, addr_c, addr_u, x_hex, y_hex;
    bool mirror = false;
};

FoundInfo make_found(const u256 key, const fe px, const fe py, int kind);
void report_found(const u256 key, const fe px, const fe py, int kind);
// Explicit-range entry point for independently verified kangaroo results.
// Returns true only after the local result has been committed durably.
// Invalid/out-of-range source candidates have no side effects. An enabled
// verified n-k mirror may be outside the source interval.
bool report_found_in_range(const u256 key, const fe px, const fe py, int kind,
                           const u256 minimum, const u256 maximum);

// Создаёт файл с правами 0600 (только владелец). Для всего, что содержит
// секреты: Found.txt, config.ini с токеном.
void secure_create(const std::string &path);

// Папка RESULT рядом с программой: архивы находок и Found.txt.
std::string result_dir();
bool        ensure_result_dir(std::string &why);
std::string found_in_result(const std::string &name);

// Дополнительный локальный архив с данными находки без пароля. Возвращает
// путь к созданному файлу либо пустую строку, если создать не удалось.
std::string make_found_archive(const FoundInfo &a, std::string &why,
                               const std::string* custom_payload = nullptr);
// Compatibility API: новые архивы без пароля, возвращает пустую строку.
std::string archive_password(const FoundInfo &a);
// Путь к 7z (7zz или 7z), пусто -- не найден.
std::string seven_zip_binary();
// Сообщить при запуске, если дополнительную архивную копию нечем создать.
void archive_warn_weak();
void tg_send_document(const std::string &path, const std::string &caption);
// Готовая команда отправки файла в Telegram -- нужна запасному каналу Signal.
std::string tg_document_cmd(const std::string &path, const std::string &caption);
std::string tg_text_cmd(const std::string &text);
// Архив остаётся локально; во внешние каналы идёт только факт находки.
// Второй аргумент оставлен для совместимости и игнорируется.
void deliver_found(const std::string &zipf, const std::string &pass);
// Внешняя команда из config.ini: notify_cmd "<текст>" "<путь к архиву>"
void notify_external(const std::string &text, const std::string &file);

// Signal через signal-cli. Настраивается так же, как Telegram, в config.ini.
bool signal_enabled();
bool signal_check();
// Local launcher/version check only; never sends or links an account.
bool signal_client_available();
// Готов ли Signal доставить архив: получатель прописан И signal-cli отвечает.
// Результат проверки при запуске запоминается -- в минуту находки проверять
// связь заново нельзя.
void signal_set_ready(bool v);
bool signal_ready();                                   // проверка связи до поиска
void signal_send_text(const std::string &text, bool background = true);
void signal_send_document(const std::string &path, const std::string &caption);

// Telegram. Пустой токен отключает отправку. Находки уведомляют только
// о локальном сохранении; приватные ключи и вложения не передаются.
void tg_send_text(const std::string &text);
bool tg_check();

void print_progress(int progress_sec, bool console_output = true);
void print_check(bool send_notifications = true, bool console_output = true);

#ifdef PKH_PERIODIC_REPORT_TEST
#include <functional>
// Captures periodic reports only. Test builds never call their real transport,
// even when no sink is set; delivery of findings is outside this hook's scope.
void set_periodic_report_test_sink(
    std::function<void(const std::string& channel, const std::string& text)> sink);
#endif

// Normal-exit drain for Windows Signal/archive/background notifications.
unsigned pending_background_notifications();
bool wait_background_notifications(unsigned timeout_ms = 45000);

// Optional PNG/archive/metadata work runs after the durable Found commit.
// A bounded exit wait never changes whether a finding is considered saved.
unsigned pending_found_postprocessing();
bool wait_found_postprocessing(unsigned timeout_ms = 45000);
#ifdef PKH_FOUND_POSTPROCESS_TEST
#include <functional>
// Isolated regression hook, called without queue or search locks held.
void set_found_postprocess_test_hook(std::function<void()> hook);
#endif
