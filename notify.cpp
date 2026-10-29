#include "minute_speed.h"
#include "hybrid_mode.h"
// ============================================================================
//  notify.cpp — экран, Found.txt, Telegram, вывод прогресса.
// ============================================================================
#include "notify.h"
#include "found_card.h"
#include "found_display.h"
#include "found_persistence.h"
#include <sstream>
#include "platform.h"   // вся разница между системами -- только там
#include "targets.h"
#include "engine.h"
#include "search_range.h"
#include "search.h"      // resumed_items: процент считает и прежние запуски
#include "traverse.h"   // mask_active, g_mask_total: процент считается от кандидатов
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <set>
#include <thread>
#include <chrono>
#include <condition_variable>
#include <thread>
#include <deque>
#include <functional>

std::mutex g_mtx_file;
std::atomic<uint64_t> g_prev{0};

// TELEGRAM
//
// Токен и chat_id берутся из config.ini. Пустой токен — молча без отправки.
//
// В сообщение НЕ кладутся приватный ключ и WIF. Оно уходит через чужой
// сервер и навсегда оседает в истории чата, поэтому секретам там не место:
// в Telegram идёт только факт находки, время и адрес. Полные данные —
// на экране и в Found.txt на самой машине.
// ========================================================
// Создаёт файл, если его ещё нет, и закрывает права до 0600 (владелец).
// На Windows POSIX-прав нет -- там это тихо ничего не делает, и об этом
// сказано в предупреждении при находке.
// ============================================================================
//  ПАПКА RESULT
//
//  Находки складываются рядом с программой в отдельный каталог: архив, его
//  временный .txt и Found.txt. Смысл не в порядке на диске -- в том, что
//  всё, содержащее приватные ключи, лежит в ОДНОМ месте с известными
//  правами. Раньше оно рассыпалось по рабочей папке вперемешку с
//  исходниками, и «скопировать проект» означало скопировать и ключи.
//
//  Права 0700 -- те же, что у Found.txt: внутри приватные ключи, и каталог
//  со всеобщим доступом на чтение обессмысливал бы закрытые права файлов.
// ============================================================================
static const char *RESULT_DIR = "RESULT";

std::string result_dir() { return RESULT_DIR; }

bool ensure_result_dir(std::string &why) {
    if (plat_make_dir_private(RESULT_DIR)) return true;
    why = std::string("не удалось создать папку ") + RESULT_DIR;
    return false;
}

// Куда класть Found.txt. Голое имя -- значит в RESULT; путь со слэшем
// пользователь задал сам, и переносить его мы не вправе.
std::string found_in_result(const std::string &name) {
    if (name.empty()) return std::string(RESULT_DIR) + "/Found.txt";
    if (name.find('/') != std::string::npos) return name;
    // Путь Windows: с обратной косой или с буквой диска. Без этой проверки
    // C:\Users\me\Found.txt превращался в RESULT/C:\Users\me\Found.txt --
    // имя негодное, и узнавалось это в минуту находки, при записи.
    if (name.find('\\') != std::string::npos) return name;
    if (name.size() >= 2 && name[1] == ':' &&
        ((name[0] >= 'A' && name[0] <= 'Z') || (name[0] >= 'a' && name[0] <= 'z')))
        return name;
    return std::string(RESULT_DIR) + "/" + name;
}

void secure_create(const std::string &path) {
    // Что именно даёт «закрыть права», зависит от системы, и programma об
    // этом говорит вслух: см. plat_privacy_note() -- на Windows защита слабее.
    plat_make_file_private(path);
}

static void run_bg_with_retries(const std::string &cmd, const std::string &what,
                                const std::string &fallback = std::string(),
                                const std::string &fb_what  = std::string());
static int run_found_process(const std::string& command, unsigned timeout_ms);

bool tg_field_safe(const std::string &s) {
    // Токен и chat_id подставляются в командную строку. Сами по себе это не
    // чужой ввод, но случайная кавычка или перевод строки из битой копипасты
    // разорвёт команду — поэтому пропускаем только те символы, которые в
    // токенах Telegram действительно встречаются.
    if (s.empty()) return false;
    for (unsigned char ch : s) {
        bool ok = (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') ||
                  (ch >= 'a' && ch <= 'z') || ch == ':' || ch == '_' || ch == '-';
        if (!ok) return false;
    }
    return true;
}

// Команда отправки текста в Telegram отдельной строкой -- ею пользуется и
// обычная отправка, и запасной канал внутри фоновой цепочки повторов.
std::string tg_text_cmd(const std::string &text) {
    if (g_cfg.tg_token.empty() || g_cfg.tg_chat.empty()) return "";
    if (!tg_field_safe(g_cfg.tg_token) || !tg_field_safe(g_cfg.tg_chat)) return "";
    std::string enc;
    for (unsigned char ch : text) {
        if (ch == ' ') enc += '+';
        else if ((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') ||
                 (ch >= 'a' && ch <= 'z') || ch=='-' || ch=='_' || ch=='.') enc += (char)ch;
        else { char b[8]; snprintf(b, sizeof(b), "%%%02X", ch); enc += b; }
    }
    // Команда уходит в цепочку повторов, где на Windows об успехе судят по
    // ответу сервера (http_says_ok), а на POSIX -- по коду выхода. Одинарные
    // кавычки и grep здесь были Unix-only: на Windows запасной канал
    // "не проходил" при уже доставленном сообщении.
    const std::string url = "https://api.telegram.org/bot" + g_cfg.tg_token +
                            "/sendMessage?chat_id=" + g_cfg.tg_chat + "&text=" + enc;
#ifdef _WIN32
    return "curl -s -m 20 " + plat_quote(url);
#else
    return "curl -s -m 20 " + plat_quote(url) + " " + std::string(plat_devnull_2()) +
           " | grep -q '\"ok\":true'";
#endif
}

void tg_send_text(const std::string &text) {
    if (g_cfg.tg_token.empty() || g_cfg.tg_chat.empty()) return;
    if (!tg_field_safe(g_cfg.tg_token) || !tg_field_safe(g_cfg.tg_chat)) {
        std::cerr << "  Telegram: токен или chat_id содержит недопустимые символы, "
                     "отправка пропущена\n";
        return;
    }
    std::string enc;
    for (unsigned char ch : text) {
        if (ch == ' ') enc += '+';
        else if ((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') ||
                 (ch >= 'a' && ch <= 'z') || ch=='-' || ch=='_' || ch=='.') enc += (char)ch;
        else { char b[8]; snprintf(b, sizeof(b), "%%%02X", ch); enc += b; }
    }
    const std::string url = "https://api.telegram.org/bot" + g_cfg.tg_token +
                            "/sendMessage?chat_id=" + g_cfg.tg_chat + "&text=" + enc;
    // Отправка уходит в фон: сеть не должна задерживать поиск. Фоном
    // занимается слой систем -- на POSIX это хвостовой "&", на Windows
    // отдельный процесс без окна. Хвостовой "&" в cmd.exe не работает вовсе.
    plat_run_detached("curl -s -m 20 " + plat_quote(url) + " " +
                      plat_devnull_all());
}

// ============================================================================
//  АРХИВ С НАХОДКОЙ БЕЗ ПАРОЛЯ
//
//  По выбору пользователя полный ключ всегда сохраняется в Found.txt,
//  а дополнительная архивная копия открывается без пароля. Старое поле
//  archive_pass в конфигурации на сохранение новых находок не влияет.
// ============================================================================

// Пропускаем только то, что заведомо безопасно в командной строке. Адрес --
// base58; проверка стоит
// на случай, если сюда однажды попадёт что-то другое.
static bool shell_token_safe(const std::string &s) {
    if (s.empty() || s.size() > 200) return false;
    for (unsigned char ch : s) {
        bool ok = (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') ||
                  (ch >= 'a' && ch <= 'z') || ch == '.' || ch == '_' || ch == '-';
        if (!ok) return false;
    }
    return true;
}

// Compatibility API: new archives never have a password.
std::string archive_password(const FoundInfo & /*a*/) {
    return "";
}

// Какой 7z есть в системе. brew ставит его то как 7zz (sevenzip), то как
// 7z (p7zip) -- проверяем оба, а не гадаем.
std::string seven_zip_binary() {
    // Process-lifetime ownership also covers an optional worker finishing
    // during a bounded normal-exit drain. No static string is destroyed early.
    struct Cache {std::mutex mutex;std::string value;bool done=false;};
    static auto* const state=new Cache;
    std::lock_guard<std::mutex> lock(state->mutex);
    auto& cached=state->value;
    if (state->done) return cached;
    state->done = true;
    for (const char *b : { "7zz", "7z" })
#ifdef _WIN32
        if(run_found_process(std::string("where ")+b+" >NUL 2>NUL",2000)==0) {cached=b;break;}
#else
        if (plat_have_command(b)) { cached = b; break; }
#endif
#ifdef _WIN32
    // На Windows 7-Zip ставится в Program Files и в PATH НЕ попадает: `where
    // 7z` его не видит, программа откатывалась на zip, которого в Windows нет
    // вовсе, -- и архив с находкой не собирался никогда. Смотрим по обычным
    // местам установки; путь с пробелом берём в кавычки, cmd их понимает.
    if (cached.empty()) {
        for (const char *c : { "C:\\Program Files\\7-Zip\\7z.exe",
                               "C:\\Program Files (x86)\\7-Zip\\7z.exe" })
            if (plat_file_exists(c)) { cached = std::string("\"") + c + "\""; break; }
    }
#endif
    return cached;
}

// Compatibility entry point: only archive availability matters now.
void archive_warn_weak() {
    if (!seven_zip_binary().empty()) return;
#ifdef _WIN32
    std::cout << C_YELLOW
              << "\n  7-Zip не найден: дополнительный архив с находкой недоступен.\n"
                 "  Приватный ключ будет записан без пароля в " << g_cfg.found_path << ".\n"
              << C_RESET;
#endif
}

// Optional external work must never wait indefinitely. Windows children live
// in their own job, so timeout also closes any subprocess created by cmd.exe.
static int run_found_process(const std::string& command, unsigned timeout_ms) {
#ifdef _WIN32
    HANDLE job=CreateJobObjectW(nullptr,nullptr);
    if(!job)return -1;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if(!SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof limits)) {
        CloseHandle(job);return -1;
    }
    std::wstring line=L"cmd.exe /D /S /C \""+plat_wide(command)+L"\"";
    STARTUPINFOW startup{};startup.cb=sizeof startup;
    startup.dwFlags=STARTF_USESHOWWINDOW;startup.wShowWindow=SW_HIDE;
    PROCESS_INFORMATION process{};
    if(!CreateProcessW(nullptr,line.data(),nullptr,nullptr,FALSE,
        CREATE_NO_WINDOW|CREATE_SUSPENDED,nullptr,nullptr,&startup,&process)) {
        CloseHandle(job);return -1;
    }
    if(!AssignProcessToJobObject(job,process.hProcess)) {
        TerminateProcess(process.hProcess,1);
        CloseHandle(process.hThread);CloseHandle(process.hProcess);CloseHandle(job);return -1;
    }
    const bool resumed=ResumeThread(process.hThread)!=DWORD(-1);
    CloseHandle(process.hThread);
    const DWORD waited=resumed?WaitForSingleObject(process.hProcess,timeout_ms):WAIT_FAILED;
    DWORD code=1;
    if(waited==WAIT_OBJECT_0)GetExitCodeProcess(process.hProcess,&code);
    else {
        TerminateJobObject(job,ERROR_TIMEOUT);
        (void)WaitForSingleObject(process.hProcess,1000);
        code=ERROR_TIMEOUT;
    }
    CloseHandle(process.hProcess);CloseHandle(job);
    return static_cast<int>(code);
#else
    // POSIX search work is still isolated from this optional worker. The
    // production timeout/process-tree implementation above targets Windows.
    (void)timeout_ms;
    return std::system(command.c_str());
#endif
}

static std::string make_found_archive_with(const FoundInfo &a, std::string &why,
        const std::string* custom_payload, const std::string& sevenz,
        const std::string& directory) {
    why.clear();

    // Адрес идёт в ИМЯ ФАЙЛА, поэтому его проверяем по-прежнему строго.
    if (!shell_token_safe(a.addr_c)) { why = "адрес содержит недопустимые символы"; return ""; }

    // ИМЯ АРХИВА -- САМ АДРЕС, без приставок.
    // По имени файла сразу видно, к какому адресу он относится.
    // Имена внутри RESULT -- короткие; путь добавляется отдельно. Архиватор
    // запускается ИЗ этой папки, иначе 7z уложил бы внутрь архива и сам путь
    // "RESULT/", а он там ни к чему.
    const std::string base_txt = a.addr_c + ".txt";
    // Расширение отвечает используемому архиватору; оба формата без пароля.
    const std::string base_final = a.addr_c +
        (sevenz.empty() ? ".zip" : ".7z");
    const std::string base_zip = a.addr_c + ".pending" +
        (sevenz.empty() ? ".zip" : ".7z");

    if(!plat_make_dir_private(directory)) {why="не удалось создать каталог архива";return "";}

    const std::string txt  = directory + "/" + base_txt;
    const std::string zipf = directory + "/" + base_zip;
    const std::string final_zip = directory + "/" + base_final;

    // Открытый файл живёт ровно столько, сколько нужно архиватору, и создаётся
    // сразу с закрытыми правами: внутри приватный ключ.
    {
        secure_create(txt);
        std::ofstream f(plat_wide(txt).c_str(), std::ios::trunc);
        if (!f) { why = "не удалось создать " + txt; return ""; }
        if(custom_payload) f << *custom_payload;
        else f << "  Адрес (c)      : " << a.addr_c   << "\n"
          << "  Адрес (u)      : " << a.addr_u   << "\n"
          << "  PUBLIC KEY (c) : " << a.pub_c    << "\n"
          << "  PUBLIC KEY (u) : " << a.pub_u    << "\n"
          << "  X              : " << a.x_hex    << "\n"
          << "  Y              : " << a.y_hex    << "\n"
          << "  PRIVATE KEY    : " << a.priv_hex << "\n"
          << "  WIF (c)        : " << a.wif_c    << "\n"
          << "  WIF (u)        : " << a.wif_u    << "\n"
          << "  Зеркало n-k    : " << (a.mirror ? "да" : "нет") << "\n"
          << "  Время          : " << now_string() << "\n";
        f.flush();
        if (!f) { why = "не удалось записать " + txt; plat_remove_file(txt); return ""; }
    }

    plat_remove_file(zipf);        // архиватор дописал бы в существующий

    // This copy is intentionally unencrypted. Never pass a password, including
    // a deprecated archive_pass value inherited from an older configuration.
    std::string cmd;
    if (!sevenz.empty()) {
        cmd = "cd " + plat_quote(directory) + " && " + sevenz +
              " a -t7z -mx=1 " +
              base_zip + " " + base_txt + " " + plat_devnull_all();
    } else {
        // -j кладёт файл без пути, -q молчит.
        cmd = "cd " + plat_quote(directory) +
              " && zip -j -q " +
              base_zip + " " + base_txt + " " + plat_devnull_all();
    }
    int rc = run_found_process(cmd,30000);

    // Убираем только временный файл упаковщика. Полный Found.txt сохраняется.
    plat_remove_file(txt);

    if (rc != 0) {
        why = "не удалось запустить zip (код " + std::to_string(rc) + ")";
        plat_remove_file(zipf);
        return "";
    }
    secure_create(zipf);
    // Довести дополнительную копию до носителя, как и основной Found.txt.
    if (!plat_fsync_file(zipf) || !plat_replace_file(zipf, final_zip) ||
        !plat_fsync_file(final_zip)) {
        why = "не удалось подтвердить сохранение архива";
        return "";
    }
    return final_zip;
}

std::string make_found_archive(const FoundInfo &a, std::string &why,
                               const std::string* custom_payload) {
    return make_found_archive_with(a,why,custom_payload,seven_zip_binary(),result_dir());
}

// Команда отправки файла в Telegram отдельной строкой: ею пользуется и
// обычная отправка, и запасной канал Signal. Пока она была вписана внутрь
// tg_send_document, запасному каналу пришлось бы держать вторую копию -- а
// две копии одной команды однажды разойдутся.
std::string tg_document_cmd(const std::string &path, const std::string &caption) {
    if (g_cfg.tg_token.empty() || g_cfg.tg_chat.empty()) return "";
    if (!tg_field_safe(g_cfg.tg_token) || !tg_field_safe(g_cfg.tg_chat)) return "";
    // Путь содержит RESULT/, поэтому проверяем не весь целиком, а имя файла.
    {
        const size_t sl = path.rfind('/');
        const std::string base = (sl == std::string::npos) ? path : path.substr(sl + 1);
        const std::string dir  = (sl == std::string::npos) ? std::string() : path.substr(0, sl);
        if (!shell_token_safe(base)) return "";
        if (!dir.empty() && dir != result_dir()) return "";
    }
    std::string cap;
    for (char ch : caption) if (ch != '\'' && ch != '\\') cap += ch;

    std::string cmd = "curl -s -m 60 -F chat_id=" + g_cfg.tg_chat +
                      " -F document=@" + path;
    if (!cap.empty()) cmd += " -F caption=" + plat_quote(cap);
    cmd += " " + plat_quote("https://api.telegram.org/bot" + g_cfg.tg_token +
                            "/sendDocument");
    // Ответ разбирает http_says_ok, а не grep: см. пояснение при нём.
    return cmd;
}

void tg_send_document(const std::string &path, const std::string &caption) {
    const std::string cmd = tg_document_cmd(path, caption);
    if (cmd.empty()) return;
    // Тот же порядок, что и у Signal: в фон и с повторами.
    //
    // Раньше здесь стояло ожидание -- «знать о неудаче важнее скорости».
    // Рассуждение неверное: узнать о неудаче на экране некому, ради того и
    // затевались уведомления, что человека нет рядом.
    run_bg_with_retries(cmd, "Telegram/архив");
}

// ============================================================================
//  SIGNAL
//
//  Второй канал, устроенный так же, как Telegram: настройки в config.ini,
//  проверка связи до начала поиска, отправка текста и файла.
//
//  Разница по существу одна, и она-то и есть причина: в Telegram уходит
//  приватный ключ, а токен бота лежит открытой строкой в исходнике -- значит
//  доступ к переписке имеет всякий, у кого есть копия программы. У Signal
//  сквозное шифрование и никакого ключа доступа снаружи.
//
//  Работа идёт через signal-cli, привязанный к аккаунту как ДОПОЛНИТЕЛЬНОЕ
//  устройство (отдельный номер не нужен):
//      brew install signal-cli
//      signal-cli link -n "PubkeyHunt"
//  и показать выданную ссылку телефону как QR-код.
// ============================================================================

// Получатель бывает двух видов, и оба должны пройти проверку:
//    +71234567890  -- номер телефона;
//    u:имя.71      -- имя пользователя Signal (префикс u: обязателен, его
//                     понимает сам signal-cli).
// Строка уходит в командную строку, поэтому список допустимого узкий намеренно.
static bool recipient_safe(const std::string &s) {
    if (s.size() < 4 || s.size() > 64) return false;
    if (s[0] == '+') {
        if (s.size() < 6) return false;
        for (size_t i = 1; i < s.size(); i++)
            if (s[i] < '0' || s[i] > '9') return false;
        return true;
    }
    if (s.compare(0, 2, "u:") == 0) {
        for (size_t i = 2; i < s.size(); i++) {
            char c = s[i];
            bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                      (c >= 'A' && c <= 'Z') || c == '.' || c == '_';
            if (!ok) return false;
        }
        return s.size() > 2;
    }
    return false;
}

// Аккаунт -- всегда номер, и его можно не задавать вовсе.
static bool account_ok(const std::string &s) {
    if (s.empty()) return true;              // единственный аккаунт signal-cli выберет сам
    return recipient_safe(s) && s[0] == '+';
}

// Путь к signal-cli: обычные символы пути, без пробелов и кавычек.
//
// ПОЧЕМУ БЕЛЫЙ СПИСОК, А НЕ ЧЁРНЫЙ. Путь уходит в командную строку целиком.
// Перечислять опасное («не пускать & | > ...») -- значит однажды забыть один
// символ; перечислять разрешённое -- значит забыть можно только безобидное,
// и тогда программа откажется работать вслух, а не тихо выполнит чужое.
//
// ЧТО БЫЛО СЛОМАНО. Список разрешённого писался под Unix: '/', точка,
// подчёркивание, дефис. Обычный windows-путь C:\signal-cli\bin\signal-cli.bat
// содержит ':' и '\', которых в списке нет, -- и Signal отвергался ещё до
// первой попытки запуска, с сообщением «недопустимый путь». Ровно та же
// ошибка, что была с /dev/null и с одинарными кавычками: код Unix-only.
//
// ПРОБЕЛ ПО-ПРЕЖНЕМУ ЗАПРЕЩЁН, и это осознанно. Взять путь в кавычки мало:
// на Windows он идёт внутрь строки, которую cmd.exe разбирает ещё раз, и
// правила снятия кавычек там свои. Признать пробел -- отдельная работа с
// отдельной проверкой. Пока: signal-cli ставится в путь без пробелов
// (C:\signal-cli\...), и об этом говорится в подсказке при отказе.
static bool path_safe(const std::string &s) {
    if (s.empty() || s.size() > 300) return false;
    for (size_t i = 0; i < s.size(); i++) {
        const unsigned char ch = (unsigned char)s[i];
        bool ok = (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') ||
                  (ch >= 'a' && ch <= 'z') ||
                  ch == '/' || ch == '.' || ch == '_' || ch == '-';
#ifdef _WIN32
        // Разделитель каталогов Windows.
        if (ch == '\\') ok = true;
        // Двоеточие -- только как «C:», то есть вторым символом строки и
        // сразу после буквы. В любом другом месте это ADS-разделитель
        // (файл.txt:поток) или часть чужой конструкции -- не пускаем.
        if (ch == ':' && i == 1 &&
            ((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z'))) ok = true;
#endif
        if (!ok) return false;
    }
    return true;
}

// Достаточно получателя: аккаунт необязателен, если signal-cli привязан к
// единственному номеру -- тогда он подставит его сам.
bool signal_enabled() {
    return !g_cfg.signal_to.empty();
}

// ГОТОВ ЛИ SIGNAL НА САМОМ ДЕЛЕ.
//
// signal_enabled() говорит лишь о том, что получатель прописан в настройках.
// А доставить архив можно, только если signal-cli стоит и привязан -- это
// выясняет signal_check() при запуске. Ответ запоминаем: в минуту находки
// заново проверять связь нельзя, там дорога каждая секунда, да и проверка
// сама по себе может подвиснуть на сети.
static bool g_signal_ready = false;
void signal_set_ready(bool v) { g_signal_ready = v; }
bool signal_ready() { return signal_enabled() && g_signal_ready; }

// Одинарные кавычки вокруг текста, значит внутри их быть не должно.
static std::string sig_clean(const std::string &s) {
    std::string r;
    for (char ch : s) {
        if (ch == '\'' || ch == '\\') continue;
        // Перевод строки в cmd.exe -- разделитель команд: до signal-cli
        // доходила первая строка, остальные cmd пытался ВЫПОЛНИТЬ. Текст
        // проверки многострочный, поэтому это не гипотеза. Один пробел
        // вместо разрыва -- сообщение целое, только в одну строку.
        if (ch == '\n' || ch == '\r') { r += ' '; continue; }
#ifdef _WIN32
        // %ИМЯ% cmd раскрывает даже внутри кавычек, а '!' -- при включённом
        // отложенном раскрытии. В строке статистики есть "%" -- убираем оба.
        if (ch == '%' || ch == '!') continue;
#endif
        r += ch;
    }
    return r;
}

static std::string signal_cmd_base() {
    std::string s = g_cfg.signal_cli;
    if (!g_cfg.signal_account.empty()) s += " -a " + g_cfg.signal_account;
    return s + " send " + g_cfg.signal_to;
}

// ОТПРАВКА НИКОГДА НЕ ЗАДЕРЖИВАЕТ ПОИСК И НЕ СДАЁТСЯ ПОСЛЕ ОДНОГО ОТКАЗА.
//
// signal-cli поднимает JVM и работает секунды, а при плохой сети -- десятки
// секунд. Ждать этого незачем: ключ уже записан и в Found.txt, и в архив, и
// показан на экране, а счёт в это время простаивал бы.
//
// Про число попыток. Сначала их было две -- сразу и через полминуты. Для
// сообщения, которое бывает один раз, этого мало: домашний интернет пропадает
// и на час, и на ночь, а программа к тому времени давно ушла бы дальше.
// Теперь попытки идут с растущими промежутками и покрывают больше трёх часов:
//
//     сразу, +30 с, +2 мин, +10 мин, +30 мин, +1 ч, +1 ч, +1 ч
//
// Как только попытка удалась, остальные отменяются. Всё это делает сама
// оболочка в отдельной подоболочке: она переживает выход программы, поэтому
// попытки продолжатся, даже если поиск к тому времени остановят.
//
// ЖУРНАЛ. Фоновая отправка невидима: ушло или нет -- по экрану не понять.
// Поэтому каждая попытка пишет строку в notify.log рядом с программой.
static const char *NOTIFY_LOG = "notify.log";

#ifdef _WIN32
namespace {
struct BackgroundPending {
    std::mutex mutex;
    std::condition_variable changed;
    unsigned count = 0;
};
BackgroundPending& background_pending() {
    // Same process-lifetime ownership as the Telegram worker queue in BitPazl.
    static auto* const state = new BackgroundPending;
    return *state;
}
void finish_background() noexcept {
    auto& state = background_pending();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.count) --state.count;
    state.changed.notify_all();
}
struct BackgroundCompletion { ~BackgroundCompletion() { finish_background(); } };
}
#endif
unsigned pending_background_notifications() {
#ifdef _WIN32
    auto& state = background_pending();
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.count;
#else
    return 0; // POSIX delivery already runs in a separate shell process.
#endif
}
bool wait_background_notifications(unsigned timeout_ms) {
#ifdef _WIN32
    auto& state = background_pending();
    std::unique_lock<std::mutex> lock(state.mutex);
    const bool complete = state.changed.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                                [&] { return state.count == 0; });
    lock.unlock();
    if (!complete) {
        std::ofstream log(NOTIFY_LOG, std::ios::app);
        if (log) log << "Notification exit wait timed out; local results remain saved.\n";
    }
    return complete;
#else
    (void)timeout_ms;
    return true;
#endif
}

// ОТВЕТ TELEGRAM РАЗБИРАЕТСЯ ЗДЕСЬ, А НЕ ЧЕРЕЗ grep.
//
// Раньше команды заканчивались на `| grep -q '"ok":true'`. На Windows нет ни
// grep, ни одинарных кавычек в понимании cmd.exe, поэтому проверка не
// работала никогда -- и сообщала "не доставлено" при исправном токене.
// Теперь вывод curl забирается в строку и разбирается программой: одинаково
// на любой системе и без внешних средств.
static bool http_says_ok(const std::string &curl_cmd) {
    const std::string out = plat_run_capture(curl_cmd + " " + plat_devnull_2());
    return out.find("\"ok\":true") != std::string::npos ||
           out.find("\"ok\": true") != std::string::npos;
}

static void run_bg_with_retries(const std::string &cmd, const std::string &what,
                                const std::string &fallback, const std::string &fb_what) {
    // Задержки перед попытками, в секундах. Первая -- без задержки.
    const std::string delays = "0 30 120 600 1800 3600 3600 3600";

    // ЗАПАСНОЙ КАНАЛ ВСТРОЕН В САМУ ЦЕПОЧКУ ПОВТОРОВ.
    //
    // Проверки Signal при запуске мало: он отвечает при старте и отваливается
    // через сутки счёта -- ровно тогда, когда находка и случается. Тогда
    // архив уходил бы в очередь повторов и застревал там навсегда, а на
    // экране всё выглядело бы отправленным.
    //
    // Поэтому запасная команда живёт внутри той же фоновой оболочки: восемь
    // попыток исчерпаны -- отправляем другим каналом. Сюда попадает отправка
    // архива в Telegram; пароль запасным каналом НЕ идёт, иначе архив и ключ
    // от него окажутся в одной переписке.
    std::string tail =
        "echo \"$(date '+%Y-%m-%d %H:%M:%S') " + what + ": ВСЕ ПОПЫТКИ ИСЧЕРПАНЫ\" >> " + NOTIFY_LOG + "; ";
    if (!fallback.empty()) {
        tail +=
        "echo \"$(date '+%Y-%m-%d %H:%M:%S') " + what + ": пробую запасной канал (" + fb_what + ")\" >> " + NOTIFY_LOG + "; "
        "if " + fallback + " > /dev/null 2>&1; then "
        "  echo \"$(date '+%Y-%m-%d %H:%M:%S') " + fb_what + ": отправлено запасным каналом\" >> " + NOTIFY_LOG + "; "
        "else "
        "  echo \"$(date '+%Y-%m-%d %H:%M:%S') " + fb_what + ": ЗАПАСНОЙ КАНАЛ ТОЖЕ НЕ ПРОШЁЛ\" >> " + NOTIFY_LOG + "; "
        "fi; ";
    }

#ifdef _WIN32
    // ================== ВЕТКА ДЛЯ WINDOWS ==================================
    //
    // Выше построен скрипт для sh: for, if, sleep, date, $((n+1)), >>. В
    // cmd.exe нет ни одного из этих средств, поэтому там цепочка повторов
    // просто не запускалась -- вместе с ней не уходило и сообщение о находке.
    //
    // Здесь то же самое делает поток самой программы. Отличие от POSIX
    // назову честно: подоболочка переживает выход программы, а поток -- нет.
    // При обычном выходе ждём до 45 секунд завершения отправок, как очередь
    // Telegram в BitPazl. После тайм-аута оставшиеся повторы не переживут
    // выход процесса; локально сохранённый результат остаётся на диске. Зато
    // на POSIX ничего не изменилось -- там работает прежний путь.
    (void)tail;
    {
        const std::string c = cmd, w = what, fb = fallback, fbw = fb_what;
        static const bool drain_registered = [] {
            (void)background_pending();
            return std::atexit([] { (void)wait_background_notifications(45000); }) == 0;
        }();
        if (!drain_registered) {
            std::ofstream log(NOTIFY_LOG, std::ios::app);
            if (log) log << "Notification worker not started: cannot register exit drain.\n";
            return;
        }
        {
            auto& state = background_pending();
            std::lock_guard<std::mutex> lock(state.mutex);
            ++state.count;
        }
        try {
        std::thread([c, w, fb, fbw]() {
            BackgroundCompletion completion;
            try {
            static const int wait_s[] = {0, 30, 120, 600, 1800, 3600, 3600, 3600};
            const int tries = (int)(sizeof(wait_s) / sizeof(wait_s[0]));
            for (int n = 0; n < tries; n++) {
                if (wait_s[n])
                    std::this_thread::sleep_for(std::chrono::seconds(wait_s[n]));
                // КАК СУДИТЬ ОБ УСПЕХЕ -- ЗАВИСИТ ОТ ТОГО, КОГО ЗАПУСТИЛИ.
                //
                // Здесь для всех команд стояло http_says_ok -- поиск "ok":true
                // в выводе. Это ответ Telegram; signal-cli такого не печатает
                // никогда, и КАЖДАЯ отправка в Signal считалась неудачной:
                // сообщение уходило восемь раз за три часа, а архив с
                // находкой после восьми попыток шёл запасным каналом в
                // Telegram -- туда же, куда уже ушёл пароль. Разделение
                // каналов, ради которого всё строилось, отменялось ровно на
                // той системе, для которой писалось.
                //
                // curl -- по ответу сервера; всё остальное -- по коду выхода.
                const bool is_curl = (c.rfind("curl", 0) == 0);
                const bool ok = is_curl
                              ? http_says_ok(c)
                              : (plat_run_status(c + " " + plat_devnull_all()) == 0);
                std::ofstream lg(NOTIFY_LOG, std::ios::app);
                if (ok) {
                    if (lg) lg << now_string() << " " << w
                               << ": отправлено с попытки " << (n + 1) << "\n";
                    return;
                }
                if (lg) lg << now_string() << " " << w
                           << ": попытка " << (n + 1) << " не удалась\n";
            }
            std::ofstream lg(NOTIFY_LOG, std::ios::app);
            if (lg) lg << now_string() << " " << w << ": ВСЕ ПОПЫТКИ ИСЧЕРПАНЫ\n";
            if (!fb.empty()) {
                if (lg) lg << now_string() << " " << w
                           << ": пробую запасной канал (" << fbw << ")\n";
                // Запасной канал -- всегда Telegram, то есть curl: судим по
                // ответу сервера, а не по коду выхода (curl без --fail
                // возвращает 0 и на HTTP 400 -- при негодном токене).
                const bool fb_ok = (fb.rfind("curl", 0) == 0)
                                 ? http_says_ok(fb)
                                 : (plat_run_status(fb + " " + plat_devnull_all()) == 0);
                if (lg) lg << now_string() << " " << fbw
                           << (fb_ok ? ": отправлено запасным каналом\n"
                                     : ": ЗАПАСНОЙ КАНАЛ ТОЖЕ НЕ ПРОШЁЛ\n");
            }
            } catch (...) {
                std::ofstream log(NOTIFY_LOG, std::ios::app);
                if (log) log << "Notification worker failed; local results remain saved.\n";
            }
        }).detach();
        } catch (...) {
            finish_background();
            std::ofstream log(NOTIFY_LOG, std::ios::app);
            if (log) log << "Notification worker could not start; local results remain saved.\n";
        }
    }
#else
    std::string sh =
        "( n=0; for d in " + delays + "; do "
        "  n=$((n+1)); [ \"$d\" -gt 0 ] && sleep \"$d\"; "
        "  if " + cmd + " > /dev/null 2>&1; then "
        "    echo \"$(date '+%Y-%m-%d %H:%M:%S') " + what + ": отправлено с попытки $n\" >> " + NOTIFY_LOG + "; "
        "    exit 0; "
        "  else "
        "    echo \"$(date '+%Y-%m-%d %H:%M:%S') " + what + ": попытка $n не удалась\" >> " + NOTIFY_LOG + "; "
        "  fi; "
        "done; " + tail +
        ") > /dev/null 2>&1 &";

    // ВОЗВРАТ system() ЗДЕСЬ НЕ ЗНАЧИТ НИЧЕГО -- И ЭТО НАДО СКАЗАТЬ ЯВНО.
    //
    // Команда заканчивается на «&»: оболочка уходит в фон и возвращает 0
    // сразу, ещё до первой попытки отправки. Судить по этому числу об
    // успехе нельзя в принципе, а ждать настоящего результата нельзя тем
    // более -- ради этого фон и заводился: находка не должна стоять в
    // очереди за сетью. Настоящий итог всех восьми попыток пишется в
    // notify.log, там его и надо смотреть.
    //
    // Пустое `system(...)` компилятор помечает предупреждением, и оно
    // осмысленно: обычно возврат проверять надо. Поэтому не глушим его
    // прагмой, а забираем значение и объясняем, почему оно бесполезно.
    const int rc = system(sh.c_str());
    if (rc != 0) {
        // Сюда попадаем, только если оболочку не удалось запустить вовсе.
        std::ofstream lg(NOTIFY_LOG, std::ios::app);
        if (lg) lg << now_string() << " " << what
                   << ": не удалось запустить оболочку (код " << rc << ")\n";
    }
#endif
}

static void signal_run_bg(const std::string &cmd, const std::string &what) {
    run_bg_with_retries(cmd, "Signal/" + what);
}

void signal_send_text(const std::string &text, bool /*background*/) {
    if (!signal_enabled()) return;
    if (!account_ok(g_cfg.signal_account) || !recipient_safe(g_cfg.signal_to) ||
        !path_safe(g_cfg.signal_cli)) return;
    // plat_to_native: signal-cli -- это Java, и аргументы она читает как
    // UTF-16, уже переведённые системой из однобайтовой кодировки. Без этого
    // перевода русский текст приходил в Signal нечитаемым, хотя тот же самый
    // текст в Telegram был правильным. Подробности -- в platform.cpp.
    signal_run_bg(signal_cmd_base() + " -m " +
                  plat_quote(plat_to_native(sig_clean(text))), "весть");
}

// ============================================================================
//  КУДА УХОДИТ НАХОДКА -- РЕШАЕТСЯ ЗДЕСЬ, И ТОЛЬКО ЗДЕСЬ
//
//  Полный приватный ключ, WIF, карточка и архив остаются локально.
//  В настроенные каналы уходит только факт находки после записи Found.
//
//  Развилка собрана в одну функцию нарочно. Пока она была размазана по
//  report_found и signal_send_document, два места решали одно и то же -- и
//  разошлись бы при первой же правке.
// ============================================================================
void deliver_found(const std::string &zipf, const std::string & /*pass*/) {
    // Password removal is authorized for local files. Do not attach the
    // unencrypted private material to an external channel without separate
    // authorization: existing notifications carry only the finding status.
    const std::string notice = "PubKEYS: найдено совпадение; приватный ключ сохранён локально в Found.";
    tg_send_text(notice);
    signal_send_text(notice);
    notify_external(notice, "");
    std::cout << "  Архив без пароля сохранён локально: " << zipf << "\n"
                 "  Уведомление содержит только факт находки.\n";
}
void signal_send_document(const std::string &path, const std::string &caption) {
    if (!signal_enabled()) return;
    if (!account_ok(g_cfg.signal_account) || !recipient_safe(g_cfg.signal_to) ||
        !path_safe(g_cfg.signal_cli) || !path_safe(path)) return;

    // ВНИМАНИЕ НА ФЛАГИ: -a ДО слова send это аккаунт, -a ПОСЛЕ -- вложение.
    // Буква одна и та же, смысл разный; перепутать легко.
    //
    // Пустая подпись означает, что -m не ставится ВОВСЕ. Иначе ушёл бы пустой
    // пузырь сообщения рядом с файлом -- лишняя строка в переписке ровно там,
    // где мы от строк и избавлялись.
    // В САМ SIGNAL ПОДПИСЬ НЕ ИДЁТ НИКОГДА.
    //
    // Необязательная подпись используется только запасным каналом.
    // Сохранение находок эту отправку документов не вызывает.
    std::string cmd = signal_cmd_base() + " -a " + path;

    // ЗАПАСНОЙ КАНАЛ: Telegram, если Signal не прошёл ни с одной попытки.
    //
    // Signal отваливается не только при запуске -- он может ответить на
    // проверке и умереть через сутки, ровно к находке. Тогда архив висел бы в
    // очереди повторов, а на экране значился отправленным.
    //
    // Необязательная подпись передаётся только при её наличии.
    std::string fb = tg_document_cmd(path, "");
    if (!fb.empty() && !caption.empty()) {
        const std::string pt = tg_text_cmd(caption);
        if (!pt.empty()) fb += " && " + pt;
    }
    run_bg_with_retries(cmd, "Signal/архив", fb, caption.empty() ?
        "Telegram: архив (запасной)" : "Telegram: архив и подпись (запасной)");
}

// Проверка связи ДО поиска -- по той же причине, что и у Telegram: узнать об
// опечатке в номере через сутки значит не иметь уведомлений вовсе.
bool signal_client_available() {
    if (!path_safe(g_cfg.signal_cli)) {
        std::cout << C_RED << "  Signal: недопустимый путь к signal-cli.\n" << C_RESET;
        return false;
    }
    int status = system((g_cfg.signal_cli + " --version " + plat_devnull_all()).c_str());
#ifdef _WIN32
    // A copied EXE gets its own config.ini. If its default command is absent
    // from PATH, reuse the conventional local installation already present.
    if (status != 0 && g_cfg.signal_cli == "signal-cli") {
        const std::string installed = "C:\\signal-cli\\bin\\signal-cli.bat";
        if (std::ifstream(installed).good()) {
            const int installed_status = system((installed + " --version " + plat_devnull_all()).c_str());
            if (installed_status == 0) {
                g_cfg.signal_cli = installed;
                status = 0;
                std::cout << "  Signal: найден локальный клиент " << installed << "\n";
            }
        }
    }
#endif
    if (status == 0) return true;
    std::cout << C_RED << "  Signal: не удалось запустить клиент (--version).\n" << C_RESET
              << "    Путь: " << g_cfg.signal_cli << "; код завершения: " << status << "\n"
                 "    Проверьте этот путь, установленный клиент signal-cli и Java.\n"
                 "    Клиент: https://github.com/AsamK/signal-cli/releases\n"
                 "    В config.ini указывается signal_cli=полный_путь_к_клиенту.\n";
    return false;
}
bool signal_check() {
    if (!signal_enabled()) return true;
    if (!recipient_safe(g_cfg.signal_to)) {
        std::cout << C_RED << "  Signal: получатель должен быть номером +71234567890 "
                             "либо именем вида u:имя.71\n" << C_RESET;
        return false;
    }
    if (!account_ok(g_cfg.signal_account)) {
        std::cout << C_RED << "  Signal: аккаунт должен быть номером +71234567890 "
                             "или пустым.\n" << C_RESET;
        return false;
    }
    if (!path_safe(g_cfg.signal_cli)) {
        std::cout << C_RED << "  Signal: недопустимый путь к signal-cli.\n" << C_RESET;
        return false;
    }
    if (!signal_client_available()) return false;
    // A runnable launcher is not necessarily a linked account. Do not expose
    // account lists or numbers in diagnostics; an empty store needs phone QR linking.
    std::string accounts = plat_run_capture(g_cfg.signal_cli + " -o json listAccounts " + plat_devnull_2());
    accounts.erase(std::remove_if(accounts.begin(), accounts.end(), [](unsigned char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    }), accounts.end());
    if (accounts == "[]" || (!g_cfg.signal_account.empty() &&
        accounts.find("\"number\"") != std::string::npos &&
        accounts.find("\"" + g_cfg.signal_account + "\"") == std::string::npos)) {
        std::cout << C_RED << "  Signal: настроенный аккаунт не привязан к signal-cli.\n" << C_RESET
                  << "    На телефоне: Signal -> Настройки -> Связанные устройства.\n"
                     "    Создайте QR: signal-cli link -n PubKEYS, затем отсканируйте его.\n";
        return false;
    }
    std::string cmd = signal_cmd_base() +
        " -m " + plat_quote(plat_to_native("PubKEYS: проверка связи (тест)")) +
        " " + plat_devnull_all();
    if (system(cmd.c_str()) == 0) {
        std::cout << C_GREEN << "  Signal: проверочное сообщение отправлено.\n" << C_RESET;
        return true;
    }
    std::cout << C_RED << "  Signal: сообщение НЕ отправлено.\n" << C_RESET
              << "    Проверьте привязку:  signal-cli -a " << g_cfg.signal_account
              << " receive\n";
    return false;
}

// ВНЕШНЕЕ УВЕДОМЛЕНИЕ.
//
// Зовём указанную в config.ini команду с двумя доводами: текст и путь к
// архиву (пустая строка, если архива нет). Всё, что сложнее — какой
// мессенджер, какой аккаунт, как повторять при неудаче — живёт в скрипте
// пользователя, а не здесь. Так подключается Signal, почта, ntfy, что угодно.
//
// Почему не вшит signal-cli: у него свои требования (Java, привязка устройства
// к номеру), проверить их из этой программы нельзя, а вшитая непроверенная
// команда молча не сработала бы ровно в тот момент, ради которого всё писалось.
void notify_external(const std::string &text, const std::string &file) {
    if (g_cfg.notify_cmd.empty()) return;

    // Доводы уходят в одинарных кавычках: внутри не должно быть ни одинарной
    // кавычки, ни обратной косой. Своих мы не ставим, но текст собирается из
    // адреса и подписи -- подстрахуемся.
    auto clean = [](const std::string &s) {
        std::string r;
        for (char ch : s) if (ch != '\'' && ch != '\\' && ch != '\n') r += ch;
        return r;
    };

    std::string cmd = g_cfg.notify_cmd + " '" + clean(text) + "' '" + clean(file) + "'";
    // Не в фоне: это единственная весть о находке, и знать о её провале важнее,
    // чем сэкономить секунду.
    if (system(cmd.c_str()) != 0)
        std::cerr << "  Внешнее уведомление вернуло ошибку: " << g_cfg.notify_cmd << "\n";
}

namespace {
struct FoundPostprocessJob {
    FoundInfo finding;
    std::string directory;
    std::string log_path;
    std::string external_command;
};
struct FoundPostprocessState {
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<FoundPostprocessJob> queue;
    unsigned pending=0;
    bool started=false;
#ifdef PKH_FOUND_POSTPROCESS_TEST
    std::function<void()> test_hook;
#endif
};
FoundPostprocessState& found_postprocess_state() {
    // The detached worker and every job own their data until process exit;
    // neither references stack/config objects or destructed static storage.
    static auto* const state=new FoundPostprocessState;
    return *state;
}
void found_postprocess_log(const std::string& message,
                           const std::string& path="notify.log") noexcept {
    try {
        std::ofstream log(plat_wide(path).c_str(),std::ios::app);
        if(log)log<<now_string()<<" Found: "<<message<<'\n';
    } catch(...) {}
}
void found_postprocess_worker() noexcept {
    auto& state=found_postprocess_state();
    for(;;) {
        FoundPostprocessJob job;
#ifdef PKH_FOUND_POSTPROCESS_TEST
        std::function<void()> test_hook;
#endif
        {
            std::unique_lock<std::mutex> lock(state.mutex);
            state.changed.wait(lock,[&]{return !state.queue.empty();});
            job=std::move(state.queue.front());state.queue.pop_front();
#ifdef PKH_FOUND_POSTPROCESS_TEST
            try {test_hook=state.test_hook;} catch(...) {test_hook={};}
#endif
        }
        try {
            auto log=[&](const std::string& message){found_postprocess_log(message,job.log_path);};
#ifdef PKH_FOUND_POSTPROCESS_TEST
            if(test_hook)test_hook();
#endif
            std::string why;
            const std::string card=job.directory+"/KEY_"+job.finding.addr_c+".png";
            if(plat_make_dir_private(job.directory) &&
               write_found_key_card(job.finding.priv_hex,card,why,"PubKEYS"))
                plat_make_file_private(card);
            else log("optional local key card unavailable; full key remains in Found.");
            why.clear();
            const std::string archive=make_found_archive_with(job.finding,why,nullptr,
                seven_zip_binary(),job.directory);
            if(archive.empty())
                log("optional local archive unavailable; full key remains in Found.");
            else log("password-free local archive ready: "+archive);
            if(!job.external_command.empty()) {
                const int status=run_found_process(job.external_command,30000);
                log(status==0?"external metadata notification completed":
                    "external metadata notification failed or timed out; full key remains in Found.");
            }
        } catch(...) {
            found_postprocess_log("optional background work failed; full key remains in Found.",job.log_path);
        }
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            if(state.pending)--state.pending;
        }
        state.changed.notify_all();
    }
}

bool queue_found_postprocess(const FoundInfo& finding) noexcept {
    try {
        // Build commands while the search owns its settings. Background work
        // never reads g_cfg, even if another run later changes its settings.
        const std::string notice="PubKEYS: найдено совпадение; приватный ключ сохранён локально в Found.";
        const std::string telegram=tg_text_cmd(notice);
        std::string signal;
        if(signal_enabled() && account_ok(g_cfg.signal_account) &&
           recipient_safe(g_cfg.signal_to) && path_safe(g_cfg.signal_cli))
            signal=signal_cmd_base()+" -m "+plat_quote(plat_to_native(sig_clean(notice)));
        FoundPostprocessJob job;job.finding=finding;
        job.directory=std::filesystem::absolute(std::filesystem::u8path(result_dir())).u8string();
        job.log_path=std::filesystem::absolute("notify.log").u8string();
        if(!g_cfg.notify_cmd.empty())
            job.external_command="cd "+plat_quote(std::filesystem::current_path().u8string())+
                " && "+g_cfg.notify_cmd+" "+plat_quote(notice)+" "+plat_quote("");
        // Metadata is queued before the archiver: a slow local artifact cannot
        // delay the finding notice. These existing workers own their commands.
        if(!telegram.empty())run_bg_with_retries(telegram,"Telegram/находка");
        if(!signal.empty())run_bg_with_retries(signal,"Signal/находка");

        auto& state=found_postprocess_state();
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            // Keep optional work bounded for very dense databases. The full
            // record is already durable; exhaustion never rejects that finding.
            if(state.pending>=256)return false;
            state.queue.push_back(std::move(job));++state.pending;
            if(!state.started) {
                try {std::thread(found_postprocess_worker).detach();state.started=true;}
                catch(...) {state.queue.pop_back();--state.pending;throw;}
            }
        }
        state.changed.notify_one();
        return true;
    } catch(...) {
        return false;
    }
}
} // namespace

unsigned pending_found_postprocessing() {
    auto& state=found_postprocess_state();
    std::lock_guard<std::mutex> lock(state.mutex);return state.pending;
}
bool wait_found_postprocessing(unsigned timeout_ms) {
    auto& state=found_postprocess_state();
    std::unique_lock<std::mutex> lock(state.mutex);
    const bool complete=state.changed.wait_for(lock,std::chrono::milliseconds(timeout_ms),
        [&]{return state.pending==0;});
    lock.unlock();
    if(!complete)found_postprocess_log("exit wait timed out; full keys remain saved in Found.");
    return complete;
}
#ifdef PKH_FOUND_POSTPROCESS_TEST
void set_found_postprocess_test_hook(std::function<void()> hook) {
    auto& state=found_postprocess_state();
    std::lock_guard<std::mutex> lock(state.mutex);state.test_hook=std::move(hook);
}
#endif

// Проверка связи. Делается ДО начала поиска: смысл уведомлений в том, чтобы
// уйти и не гадать, дойдёт ли сообщение. Узнать об опечатке в токене через
// сутки — то же самое, что не иметь уведомлений вовсе.
bool tg_check() {
    if (g_cfg.tg_token.empty() || g_cfg.tg_chat.empty()) return true;
    if (!tg_field_safe(g_cfg.tg_token) || !tg_field_safe(g_cfg.tg_chat)) {
        std::cout << C_RED << "  Telegram: токен или chat_id содержит недопустимые символы.\n"
                  << C_RESET;
        return false;
    }
    const std::string url = "https://api.telegram.org/bot" + g_cfg.tg_token +
                      "/sendMessage?chat_id=" + g_cfg.tg_chat +
                      "&text=PubKEYS%3A+%D0%BF%D1%80%D0%BE%D0%B2%D0%B5%D1%80%D0%BA%D0%B0+%D1%81%D0%B2%D1%8F%D0%B7%D0%B8+%28%D1%82%D0%B5%D1%81%D1%82%29";
    const std::string cmd = "curl -s -m 20 " + plat_quote(url);
    if (http_says_ok(cmd)) {
        std::cout << C_GREEN << "  Telegram: сервер принял тестовое сообщение.\n" << C_RESET;
        return true;
    }
    std::cout << C_RED << "  Telegram: сообщение НЕ доставлено — проверьте токен и chat_id.\n"
              << C_RESET;
    return false;
}

// ========================================================
// ОФОРМЛЕНИЕ НАЙДЕННОГО
//
// Всё, что здесь считается — адрес, WIF, несжатый вид — считается ровно
// один раз за находку. В горячем цикле ничего этого нет.
// ========================================================

FoundInfo make_found(const u256 key, const fe px, const fe py, int kind) {
    FoundInfo r;
    r.mirror = (kind == 2);

    u256 priv;
    fe Y;
    if (kind == 2) {
        // зеркало: искомому публичному ключу отвечает приватный n−k,
        // а его точка — (X, p−Y)
        u256_sub(priv, EC_N, key);
        fe_neg(Y, py);
    } else {
        u256_set(priv, key);
        fe_set(Y, py);
    }

    uint8_t kb[32], xb[32], yb[32];
    u256_to_be32(kb, priv);
    fe_to_be32(xb, px);
    fe_to_be32(yb, Y);

    r.priv_hex = to_hex(kb, 32);
    r.x_hex    = to_hex(xb, 32);
    r.y_hex    = to_hex(yb, 32);
    r.wif_c    = wif_from_key(kb, true);
    r.wif_u    = wif_from_key(kb, false);

    uint8_t pc[33], pu[65], h[20];
    pc[0] = (uint8_t)(0x02 | (Y[0] & 1));
    memcpy(pc + 1, xb, 32);
    pu[0] = 0x04;
    memcpy(pu + 1, xb, 32);
    memcpy(pu + 33, yb, 32);

    r.pub_c = to_hex(pc, 33);
    r.pub_u = to_hex(pu, 65);

    hash160(pc, 33, h);  r.addr_c = addr_from_hash160(h);
    hash160(pu, 65, h);  r.addr_u = addr_from_hash160(h);
    return r;
}

static void found_storage_retry() noexcept {
    std::fputs("  Результат ожидает сохранения; повтор через 5 секунд. Поиск не завершает этот участок.\n", stderr);
    std::fflush(stderr);
    try { std::this_thread::sleep_for(std::chrono::seconds(5)); } catch (...) {}
}

static bool report_found_committed(const u256 key, const fe px, const fe py, int kind) {
    std::unique_lock<std::mutex> lk(g_mtx_file);
    FoundInfo a;
    for (;;) {
        try { a = make_found(key, px, py, kind); break; }
        catch (...) { found_storage_retry(); }
    }
    if (g_quiet) {
        g_last_found = a.priv_hex;
        g_test_found.insert(a.priv_hex);
        g_found_num++;
        return true;
    }
    static std::set<std::string> seen;
    if (seen.count(a.priv_hex)) return true;
    const uint64_t num = g_found_num.load() + 1;
    std::string payload;
    std::string found_after;
    // All modes use the local plaintext record below, even with an old
    // archive_pass setting. deliver_found is now metadata-only: it sends no
    // private key, WIF, card or archive to any external channel.
    // Keep one immutable record across retries, and publish the count/seen flag
    // only after durable replacement. The scanning worker cannot complete its
    // chunk while a confirmed result is still waiting for storage recovery.
    for (;;) { try {
    found_after = found_display::elapsed(g_t0 + (std::chrono::steady_clock::now() - search_now()));
    std::ostringstream record;
    record << "=== НАЙДЕНО #" << num << " ===\n"
           << "  PUBLIC KEY (c) : " << a.pub_c << "\n"
           << "  PUBLIC KEY (u) : " << a.pub_u << "\n"
           << "  X              : " << a.x_hex << "\n"
           << "  Y              : " << a.y_hex << "\n"
           << "  PRIVATE KEY    : " << a.priv_hex << "\n"
           << "  WIF (c)        : " << a.wif_c << "\n"
           << "  WIF (u)        : " << a.wif_u << "\n"
           << "  Адрес (c)      : " << a.addr_c << "\n"
           << "  Адрес (u)      : " << a.addr_u << "\n"
           << "  Зеркало n-k    : " << (a.mirror ? "да" : "нет") << "\n"
           << "  Время          : " << now_string()
           << "\n  Найдено за     : " << found_after << "\n"
           << "===============\n\n";
    payload = record.str();
    break;
    } catch (...) { found_storage_retry(); } }
    FoundWriteState persistence;
    std::string save_error;
    bool written = persist_found_record(g_cfg.found_path, payload, persistence, save_error);
    while (!written) {
        std::fprintf(stderr, "  Находка НЕ сохранена: %s.\n", save_error.c_str());
        found_storage_retry();
        written = persist_found_record(g_cfg.found_path, payload, persistence, save_error);
    }
    for (;;) {
        try { seen.insert(a.priv_hex); g_last_found = a.priv_hex; break; }
        catch (...) { found_storage_retry(); }
    }
    g_found_num.fetch_add(1);

    // --- на экран ---
    try {
    std::unique_lock<std::mutex> lk2(g_mtx_cout);
    MinuteSpeed::LogScope found_speed_log;
    // Звонок терминала: поиск идёт часами, находку легко пропустить глазами.
    std::cout << "\a\a\a";
    std::cout << "\n" << C_BOLD << C_YELLOW
              << "  ╔══════════════════════════════════════════════════════════════════╗\n"
              << C_RESET << std::flush;
    found_display::heading("  ║  ★★★  СОВПАДЕНИЕ #" + std::to_string(num) +
                           (a.mirror ? "  (зеркало n-k)" : "") + "  ★★★", written);
    std::cout << C_BOLD << C_YELLOW
              << "  ╚══════════════════════════════════════════════════════════════════╝\n"
              << C_RESET << C_BOLD << C_GREEN
              << "  ┌──────────────────────────────────────────────────────────────────\n"
              << "  │  PUBLIC KEY : " << a.pub_c    << "\n"
              << "  │  PRIVATE KEY: " << a.priv_hex << "\n"
              << "  │  WIF(c)     : " << a.wif_c    << "\n"
              << "  │  WIF(u)     : " << a.wif_u    << "\n"
              << "  │  Адрес(c)   : " << a.addr_c   << "\n"
              << "  │  Адрес(u)   : " << a.addr_u   << "\n"
              << "  │  Найдено за : " << found_after << "\n"
              << "  └──────────────────────────────────────────────────────────────────\n"
              << C_RESET;
    // Required persistence stays synchronous. Optional rendering, archiving
    // and metadata delivery begin only after the durable commit below.
    found_speed_log.finish();
    lk2.unlock();
    lk2.lock();
    found_speed_log.resume();
    std::cout << C_RED << C_BOLD << "  >>> Сохранено в " << g_cfg.found_path
              << " <<<\n" << C_RESET
              << "  Дополнительные копии создаются в фоне.\n\n";
    std::cout.flush();
    found_speed_log.finish();lk2.unlock();
    } catch (...) {
        std::fputs("  Находка сохранена в Found; не удалось обновить экран.\n", stderr);
    }
    lk.unlock();
    try { if(!queue_found_postprocess(a)) {
        std::lock_guard<std::mutex> output(g_mtx_cout);
        MinuteSpeed::LogScope speed_log;
        std::cerr<<"  Дополнительные копии не поставлены в очередь; полный ключ уже сохранён в Found.\n";
    } } catch (...) {
        std::fputs("  Находка сохранена в Found; дополнительная обработка недоступна.\n", stderr);
    }
    return true;
}

void report_found(const u256 key, const fe px, const fe py, int kind) {
    if (search_match_allowed(key, kind))
        report_found_committed(key, px, py, kind);
}

bool report_found_in_range(const u256 key, const fe px, const fe py, int kind,
                           const u256 minimum, const u256 maximum) {
    if (kind != 1 && kind != 2) return false;
    if (!(key[0] || key[1] || key[2] || key[3]) || u256_cmp(key, EC_N) >= 0)
        return false;
    // Bounds always constrain the verified source candidate, including for
    // mirrors. The derived n-k itself is allowed outside those bounds.
    if (u256_cmp(key, minimum) < 0 || u256_cmp(key, maximum) > 0)
        return false;
    if (kind == 2 && !g_cfg.want_nk) return false;
    u256 actual;
    if (kind == 2) u256_sub(actual, EC_N, key);
    else u256_set(actual, key);
    if (!search_key_is_valid(actual)) return false;
    const Point verified = ec_mul_G(key);
    if (verified.inf || !fe_eq(verified.x, px) || !fe_eq(verified.y, py))
        return false;
    return report_found_committed(key, px, py, kind);
}

// ========================================================

// ВЫВОД ПРОГРЕССА И ПРОВЕРКИ
// ========================================================
long double u256_to_ld(const u256 a) {
    long double v = 0;
    for (int i = 3; i >= 0; i--) v = v * 18446744073709551616.0L + (long double)a[i];
    return v;
}

namespace {
#ifdef PKH_PERIODIC_REPORT_TEST
std::mutex periodic_report_test_mutex;
std::function<void(const std::string&, const std::string&)> periodic_report_test_sink;
#endif

void send_periodic_report(bool telegram, const std::string& text) {
#ifdef PKH_PERIODIC_REPORT_TEST
    std::function<void(const std::string&, const std::string&)> sink;
    {
        std::lock_guard<std::mutex> lock(periodic_report_test_mutex);
        sink = periodic_report_test_sink;
    }
    if (sink) sink(telegram ? "telegram" : "signal", text);
#else
    if (telegram) tg_send_text(text);
    else signal_send_text(text);
#endif
}
} // namespace

#ifdef PKH_PERIODIC_REPORT_TEST
void set_periodic_report_test_sink(
    std::function<void(const std::string&, const std::string&)> sink) {
    std::lock_guard<std::mutex> lock(periodic_report_test_mutex);
    periodic_report_test_sink = std::move(sink);
}
#endif

void print_progress(int progress_sec, bool console_output) {
    uint64_t total = g_count.load();
    uint64_t prev  = g_prev.exchange(total);
    uint64_t per   = (total > prev) ? total - prev : 0;

    // СКОРОСТЬ ДЕЛИТСЯ НА ИЗМЕРЕННОЕ ВРЕМЯ, А НЕ НА ЗАКАЗАННОЕ.
    //
    // Здесь стояло per / progress_sec -- деление на то число секунд, которое
    // человек ввёл в меню. Но поток вывода просыпается не ровно по нему: он
    // ждёт своё время, потом берёт блокировку вывода, и между двумя строками
    // проходит 60.3 секунды, а иногда 61.2. Деление на ровно 60 приписывало
    // эту задержку скорости -- всегда в меньшую сторону.
    //
    // Побочное следствие было заметнее самой ошибки: числа в двух подряд
    // строках выходили ОДИНАКОВЫМИ до последней цифры. Счётчик растёт
    // участками по 2^28 ключей, за минуту их проходит ровно 61 и там и там,
    // а знаменатель -- постоянная 60. Совпадение выглядело как поломка
    // («цифры за секунды одно и то же»), хотя счёт шёл. С измеренным
    // временем совпадения не будет: секундомер различает и миллисекунду.
    static std::chrono::steady_clock::time_point last = search_now();
    const auto now = search_now();
    double dt = std::chrono::duration<double>(now - last).count();
    last = now;
    if (dt < 0.001) dt = (double)progress_sec;      // первый вызов и защита от нуля
    uint64_t speed = (uint64_t)((double)per / dt + 0.5);

    // ЗНАМЕНАТЕЛЬ -- ТО, ЧТО ПРОГРАММА СОБИРАЕТСЯ ПРОВЕРИТЬ.
    //
    // При HEX-маске проверяются не все ключи интервала, а кандидаты решётки:
    // их в 16^N раз меньше. Деление на ширину интервала давало «Пройдено:
    // 0.0000000000%» навсегда -- при 40 замороженных нибблах знаменатель
    // больше числителя в 10^48 раз. Строка была бы не просто неточной, а
    // сообщала бы обратное правде: что работа стоит на месте.
    long double tot;
    if (mask_active()) {
        tot = u256_to_ld(g_mask_total);
    } else {
        u256 span; u256_sub(span, g_range_end, g_range_start);
        tot = u256_to_ld(span) + 1.0L;
    }
    if (tot < 1.0L) tot = 1.0L;
    // К работе этого запуска прибавляем закрытое прежними: «Пройдено»
    // обязано означать весь путь, иначе после продолжения оно врёт вниз.
    long double done_all = g_search.resumed_items + (long double)total;
    if (done_all > tot) done_all = tot;          // край: участки шире остатка
    long double pct = done_all / tot * 100.0L;

    // Строка собирается один раз и используется целиком и для экрана, и
    // для Telegram -- иначе форматы расходятся (в Telegram, например,
    // раньше не было "За интервал" и зоны вовсе не было).
    // «КЛЮЧЕЙ» ЗНАЧИТ РАЗНОЕ В РАЗНЫХ РЕЖИМАХ.
    //
    // Distinct shape blocks are keys even when a fixed-start wave checks only
    // one pattern. A full-range percentage belongs only to full coverage.
    const bool full = traversal_full_coverage();
    const bool distinct = full || unique_blocks_mode();

    // РЕЖИМ НАЗЫВАЕТСЯ В САМОЙ СТРОКЕ.
    //
    // Строка уходит в Telegram, где нет ни меню, ни сводки -- одни числа
    // подряд. А значат они в разных режимах разное: «Ключей» против
    // «Проверок», процент есть или его нет. Через сутки среди десятков
    // одинаковых сообщений (а машин может быть несколько, и режимы у них
    // разные) понять, к чему относится строка, было нельзя.
    //
    // Название берётся из mode_title() -- той же функции, что печатает
    // сводку и config.ini. Своя табличка здесь означала бы четвёртую копию
    // одного списка, а копии расходятся.
    std::string tag = mode_title(g_cfg.mode);
    // Маска меняет смысл счёта: проверяются кандидаты решётки, а не ключи
    // подряд. Строка обязана это показывать, иначе два разных прогона
    // выглядят одинаково.
    if (mask_active()) tag += " + маска 16^" + std::to_string(g_cfg.mask_froz);
    if (g_cfg.det_hex >= 0) tag += ", разряд " + std::to_string(g_cfg.det_hex);

    std::string line = "[" + format_elapsed() + "] " + tag
                      + (distinct ? " | Ключей: " : " | Проверок: ") + fmt_num(total)
                      + " | За интервал: " + fmt_num(per)
                      + " | keys/s: "     + fmt_num(speed);
#ifdef PKH_HAVE_CUDA
    // САМА КАРТА -- ОТДЕЛЬНЫМ ЧИСЛОМ, С ЧАСТОТОЙ И МОЩНОСТЬЮ.
    //
    // keys/s выше -- это всё вместе: подготовка, паузы мощности, подтверждение.
    // Когда оно ниже ожидаемого, первый вопрос -- "карта стала медленнее или
    // что-то вокруг неё?", и ответить на него по одному числу нельзя. Здесь
    // чистая скорость ядра за интервал и то, на какой частоте и мощности карта
    // при этом работала. Ноутбучная 5090 под лимитом мощности сбрасывает
    // частоту -- и это видно сразу, а не угадывается.
    if (g_gpu_active) {
        uint64_t kk = 0; double ks = 0;
        gpu_interval_rate(&kk, &ks);
        if (ks > 0.01) {
            int mhz = 0, tc = 0, pw = 0;
            gpu_now(&mhz, &tc, &pw);
            line += " | карта: " + fmt_num((uint64_t)((double)kk / ks + 0.5)) + "/с";
            if (mhz) line += ", " + std::to_string(mhz) + " МГц";
            if (pw)  line += ", " + std::to_string(pw)  + " Вт";
            if (tc)  line += ", " + std::to_string(tc)  + " C";
        }
    }
#endif
    char pb[48];
    snprintf(pb, sizeof(pb), "%.10Lf", pct);
    // Only complete-range modes have a meaningful full-range percentage.
    if (full)
        line += " | Пройдено: " + std::string(pb) + "%";
    else if (g_cfg.mode == TRAV_HYBRID) {
        const auto state = hybrid_mode_status();
        line += " | A: блок " + std::to_string(state.sequence)
             + (state.exhausted ? ", B завершена"
                                : ", проверено в B: " + std::to_string(state.offset))
             + (state.single_prefix ? " | единственная A, один блок B"
                                    : " | A случайная, повторы возможны; полного покрытия нет");
    }
    else if (g_cfg.mode == TRAV_WAVE)
        line += g_cfg.wave_start.empty()
            ? " | целые волны по очереди битовых интервалов, без повторов"
            : " | заданный узор и остаток диапазона, без повторов";
    else if (g_cfg.mode == TRAV_ZONE)
        line += " | целые зоны по очереди битовых интервалов, без повторов";
    else if (g_zone_idx.load() >= 1)
        line += " | ЗОНА " + std::to_string(g_zone_idx.load()) + " | выборка, покрытия нет";
    else
        line += " | выборка, покрытия нет";
    line += " | Найдено: " + std::to_string(g_found_num.load());

    if (console_output) {
        std::lock_guard<std::mutex> lk(g_mtx_cout);
        MinuteSpeed::LogScope speed_log;
        std::cout << "  " << line << "\n";
        std::cout.flush();
    }

    if (g_cfg.tg_progress) send_periodic_report(true, line);
    if (g_cfg.signal_progress) send_periodic_report(false, line);
}

void print_check(bool send_notifications, bool console_output) {
    const bool telegram = send_notifications && g_cfg.tg_progress;
    const bool signal = send_notifications && g_cfg.signal_progress;
    if (!console_output && !telegram && !signal) return;

    u256 key;
    bool sample_available = false;
    {
        std::lock_guard<std::mutex> lk(g_mtx_sample);
        if (g_sample_valid) {
            u256_set(key, g_sample_key);
            sample_available = true;
        }
    }
    if (!sample_available && !telegram && !signal) return;
    Point p{};
    if (sample_available) p = ec_mul_G(key);

    // The user explicitly confirmed the full trial block in Telegram:
    // PRIVATE KEY, X/Y and both public-key formats. Use one sample snapshot;
    // Signal and finding notifications retain their separate status summaries.
    // ОТКУДА ВЗЯЛСЯ ЭТОТ КЛЮЧ -- НАПИСАНО ПРЯМО ЗДЕСЬ.
    //
    // В журнале участков стоят НАЧАЛА блоков, а они выровнены: у зонального
    // режима по 4096, то есть всегда оканчиваются на три нуля. Здесь же ключ
    // берётся ИЗНУТРИ блока, со случайным смещением -- нарочно, потому что
    // круглый ключ ничего не доказывает про остальные 4095.
    //
    // Из-за этой разницы экран и журнал выглядят несхоже, и вопрос «почему
    // по-разному» задавался не раз. Отвечаем в самой строке: три последних
    // разряда -- это смещение внутри блока, отбросьте их и получите ту самую
    // строку, что стоит в журнале.
    // Режим включён и в полный пробный блок, и в краткую сводку проверки.
    std::string vtag = mode_title(g_cfg.mode);
    if (mask_active()) vtag += " + маска 16^" + std::to_string(g_cfg.mask_froz);
    if (g_cfg.det_hex >= 0) vtag += ", разряд " + std::to_string(g_cfg.det_hex);

    const std::string header = "ПРОВЕРКА | " + format_elapsed() + " | " + vtag
                      + " | Ключей: " + fmt_num(g_count.load())
                      + " | Найдено: " + std::to_string(g_found_num.load());

    std::string body;
    if (sample_available && (console_output || (telegram && !p.inf))) {
        uint8_t kb[32], xb[32], yb[32], pc[33], pu[65];
        u256_to_be32(kb, key);
        fe_to_be32(xb, p.x);
        fe_to_be32(yb, p.y);
        pc[0] = (uint8_t)(0x02 | (p.y[0] & 1));
        memcpy(pc + 1, xb, 32);
        pu[0] = 0x04;
        memcpy(pu + 1, xb, 32);
        memcpy(pu + 33, yb, 32);

        body = header + "\n"
                      + std::string(74, '-') + "\n"
                      + (g_cfg.mode == TRAV_WAVE || g_cfg.mode == TRAV_ZONE
                         ? std::string(g_cfg.mode == TRAV_WAVE ? "Блок волны: 16^" : "Зонный блок: 16^")
                           + std::to_string(traversal_block_bits() / 4) + " | "
                           + fmt_num(UINT64_C(1) << traversal_block_bits())
                           + " ключей в полном блоке; каждый блок один раз.\n"
                         : std::string())
                      + "Пробный образец: это НЕ совпадение с базой и не запись в Found.\n"
                      + "Ключ взят изнутри проверяемого сейчас участка, не с его начала.\n"
                      + "PRIVATE KEY   : " + to_hex(kb, 32)
                      + "   (Приватный ключ)\n"
                      + "X             : " + to_hex(xb, 32) + "\n"
                      + "Y             : " + to_hex(yb, 32) + "\n"
                      + "PUBLIC KEY(c) : " + to_hex(pc, 33) + "\n"
                      + "PUBLIC KEY(u) : " + to_hex(pu, 65);

    }

    if (console_output && sample_available) {
        std::lock_guard<std::mutex> lk(g_mtx_cout);
        MinuteSpeed::LogScope speed_log;
        std::cout << "\n" << C_CYAN << std::string(74, '=') << "\n  " << body << "\n"
                  << std::string(74, '=') << C_RESET << "\n\n";
        std::cout.flush();
    }

    if (telegram || signal) {
        const std::string status = !sample_available
            ? "Образец пока недоступен; пробная точка не вычислялась."
            : p.inf ? "Образец доступен; конечная пробная точка не получена."
                    : "Образец доступен; пробная публичная точка вычислена.";
        const std::string notice = header + "\n" + status;
        if (telegram) send_periodic_report(true,
            sample_available && !p.inf && !body.empty() ? body : notice);
        if (signal) send_periodic_report(false, notice);
    }
}

// ========================================================
