// ============================================================================
//  platform.cpp — реализация под три системы.
//
//  Файл нарочно устроен так: сначала общее, затем два блока — Windows и
//  POSIX, — и каждая функция написана в обоих. Не «в одном, а во втором
//  как-нибудь»: пустая заглушка здесь равносильна тихой поломке, и один
//  такой случай в этом проекте уже был.
//
//  ЧТО ПРОВЕРЕНО, А ЧТО НЕТ. Путь POSIX собран и прогнан. Путь Windows
//  написан, но НЕ СОБРАН НИ РАЗУ: ни на маке, ни в среде, где это писалось,
//  нет ни MSVC, ни mingw. Первая сборка под Windows — у вас, и первым делом
//  надо запустить plat_selftest: он проверяет каждую функцию этого файла на
//  той системе, где выполняется.
// ============================================================================
#include "platform.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <string>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <direct.h>
  #include <io.h>
#else
  #include <fcntl.h>
  #include <unistd.h>
  #include <sys/stat.h>
  #if defined(__APPLE__)
    #include <mach-o/dyld.h>
  #endif
#endif

const char *plat_name() {
#if defined(_WIN32)
    return "Windows";
#elif defined(__APPLE__)
    return "macOS";
#else
    return "Linux";
#endif
}

const char *plat_devnull_2() {
#ifdef _WIN32
    return "2>NUL";
#else
    return "2>/dev/null";
#endif
}

const char *plat_devnull_all() {
#ifdef _WIN32
    return ">NUL 2>&1";
#else
    return ">/dev/null 2>&1";
#endif
}

std::string plat_quote(const std::string &s) {
#ifdef _WIN32
    // cmd.exe одинарных кавычек не знает вовсе, зато понимает двойные.
    // Внутренние двойные кавычки не экранируем, а выбрасываем: в этой
    // программе в кавычки попадают только URL и текст сообщений, где их
    // быть не должно, а половинчатое экранирование в cmd -- известный
    // источник дыр.
    std::string r = "\"";
    for (char c : s) if (c != '"') r += c;
    r += '"';
    return r;
#else
    std::string r = "'";
    for (char c : s) if (c != '\'') r += c;
    r += '\'';
    return r;
#endif
}

// КИРИЛЛИЦА В АРГУМЕНТЕ ВНЕШНЕЙ ПРОГРАММЫ.
//
// Симптом: в Signal сообщения приходят, но русский текст нечитаем, а в
// Telegram тот же самый текст правильный.
//
// Отчего так выходит. Программа держит строки в UTF-8. system() на Windows --
// это CreateProcessA, и он переводит командную строку из ОДНОБАЙТОВОЙ
// кодировки системы (у русской Windows это CP1251) в UTF-16, а уже её
// получает запускаемая программа. Наши байты UTF-8 при этом читаются как
// CP1251 -- каждый байт превращается в свою букву, и текст рассыпается.
//
// Почему Telegram при этом цел. curl собран как обычная программа на C и
// читает аргументы однобайтовыми. Перевод CP1251 -> UTF-16 -> CP1251
// возвращает ровно те же байты, curl получает исходный UTF-8 и отправляет
// его как есть. А signal-cli -- это Java, и она берёт аргументы уже в
// UTF-16, то есть в испорченном виде. Одна и та же командная строка, разный
// итог; отсюда и впечатление, что "виноват Signal".
//
// Что делаем. Переводим текст из UTF-8 в кодировку системы САМИ, до того как
// он попадёт в командную строку. Тогда CreateProcessA переводит его в UTF-16
// верно, и Java получает настоящие буквы.
//
// ЧЕГО ЭТО НЕ УМЕЕТ, ЧЕСТНО: символ, которого в кодировке системы нет
// (эмодзи, длинное тире, греческая буква), станет '?'. Для русского и
// латиницы на русской Windows потерь нет. Применять эту функцию нужно ТОЛЬКО
// к тексту для программ вроде Java; для curl она бы всё испортила -- он и
// так получает верные байты.
std::string plat_to_native(const std::string &utf8) {
#ifdef _WIN32
    if (utf8.empty()) return utf8;
    // Если системная кодировка и так UTF-8 (в Windows есть такая настройка,
    // «бета: использовать Юникод UTF-8»), переводить нечего -- а вызов
    // WideCharToMultiByte с lpDefaultChar для UTF-8 к тому же ЗАПРЕЩЁН и
    // вернул бы ноль: строка стала бы пустой, и сообщение пропало бы.
    if (GetACP() == CP_UTF8) return utf8;

    const int wn = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(),
                                       nullptr, 0);
    if (wn <= 0) return utf8;                      // не UTF-8 -- отдаём как есть
    std::wstring w((size_t)wn, L'\0');
    if (MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), &w[0], wn) <= 0)
        return utf8;

    const int an = WideCharToMultiByte(CP_ACP, 0, w.c_str(), wn,
                                       nullptr, 0, nullptr, nullptr);
    if (an <= 0) return utf8;
    std::string a((size_t)an, '\0');
    const char def = '?';
    BOOL lost = FALSE;
    if (WideCharToMultiByte(CP_ACP, 0, w.c_str(), wn, &a[0], an, &def, &lost) <= 0)
        return utf8;                               // не вышло -- лучше как есть, чем пусто
    return a;
#else
    // POSIX: командная строка -- байты, и они доходят до программы такими,
    // какими их дали. Переводить нечего.
    return utf8;
#endif
}

bool plat_replace_file(const std::string &tmp, const std::string &path) {
#ifdef _WIN32
    // MOVEFILE_REPLACE_EXISTING -- заменить поверх; MOVEFILE_WRITE_THROUGH --
    // не возвращаться, пока переименование не дошло до диска: это заменяет
    // fsync каталога, которого на Windows нет.
    return MoveFileExW(plat_wide(tmp).c_str(), plat_wide(path).c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return std::rename(tmp.c_str(), path.c_str()) == 0;
#endif
}

PowerSource plat_power_source() {
#ifdef _WIN32
    SYSTEM_POWER_STATUS status{};
    if (GetSystemPowerStatus(&status)) {
        if (status.ACLineStatus == 0) return PowerSource::battery;
        if (status.ACLineStatus == 1) return PowerSource::mains;
    }
#endif
    return PowerSource::unknown;
}

bool plat_keep_awake(bool on) {
#ifdef _WIN32
    // SystemRequired alone does not protect this process from Modern Standby
    // lifetime management. ExecutionRequired keeps the search itself running;
    // deliberately do not request DisplayRequired. Main search thread only.
    static HANDLE request = INVALID_HANDLE_VALUE;
    if (!on) {
        if (request != INVALID_HANDLE_VALUE) {
            PowerClearRequest(request, PowerRequestExecutionRequired);
            PowerClearRequest(request, PowerRequestSystemRequired);
            CloseHandle(request);
            request = INVALID_HANDLE_VALUE;
        }
        return true;
    }
    // Recreate on periodic calls as explicit sleep can invalidate requests.
    // Acquire the replacement before releasing the old object, so ordinary
    // refreshes never leave a gap and repeated calls do not grow the count.
    REASON_CONTEXT reason{};
    reason.Version = POWER_REQUEST_CONTEXT_VERSION;
    reason.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
    wchar_t description[] = L"PubKEYS: поиск продолжается с выключенным экраном";
    reason.Reason.SimpleReasonString = description;
    const HANDLE created = PowerCreateRequest(&reason);
    if (created == INVALID_HANDLE_VALUE) return false;
    if (!PowerSetRequest(created, PowerRequestSystemRequired)) {
        CloseHandle(created); return false;
    }
    if (!PowerSetRequest(created, PowerRequestExecutionRequired)) {
        PowerClearRequest(created, PowerRequestSystemRequired);
        CloseHandle(created); return false;
    }
    if (request != INVALID_HANDLE_VALUE) {
        PowerClearRequest(request, PowerRequestExecutionRequired);
        PowerClearRequest(request, PowerRequestSystemRequired);
        CloseHandle(request);
    }
    request = created;
    return true;
#else
    // macOS/Linux: у настольных систем экран на лимит карты не влияет, а
    // «не засыпать» там делает caffeinate/systemd-inhibit снаружи. Ничего.
    (void)on;
    return true;
#endif
}

int plat_run_status(const std::string &cmd) {
    return system(cmd.c_str());
}

bool plat_file_exists(const std::string &path) {
#ifdef _WIN32
    const DWORD a = GetFileAttributesW(plat_wide(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
#endif
}

std::string plat_envref(const std::string &name) {
#ifdef _WIN32
    return "%" + name + "%";
#else
    return "\"$" + name + "\"";
#endif
}

// Отрезать имя файла, оставив каталог. Разделитель на Windows бывает любой:
// система принимает и '\\', и '/', а пути приходят из обоих источников.
static std::string dir_of(const std::string &path) {
    size_t p = path.find_last_of("/\\");
    return (p == std::string::npos) ? std::string() : path.substr(0, p);
}

// ============================================================================
#ifdef _WIN32
// ============================================================================

std::string plat_exe_dir() {
    // Буфер растим, пока не хватит: путь может быть длиннее MAX_PATH, если
    // включены длинные пути. Обрезанный путь GetModuleFileName возвращает
    // БЕЗ признака ошибки в старых версиях, поэтому смотрим на GetLastError.
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        SetLastError(0);
        DWORD n = GetModuleFileNameW(nullptr, &buf[0], (DWORD)buf.size());
        if (n == 0) return std::string();
        if (n < buf.size() && GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
            buf.resize(n);
            const int size = WideCharToMultiByte(CP_UTF8, 0, buf.data(), (int)n, nullptr, 0, nullptr, nullptr);
            if (size <= 0) return std::string();
            std::string utf8((size_t)size, '\0');
            if (!WideCharToMultiByte(CP_UTF8, 0, buf.data(), (int)n, &utf8[0], size, nullptr, nullptr))
                return std::string();
            return dir_of(utf8);
        }
        if (buf.size() > 32768) return std::string();
        buf.resize(buf.size() * 2);
    }
}

bool plat_chdir(const std::string &dir) {
    return dir.empty() ? false : (_wchdir(plat_wide(dir).c_str()) == 0);
}

bool plat_make_dir_private(const std::string &path) {
    if (_wmkdir(plat_wide(path).c_str()) == 0) return true;
    // EEXIST означает «что-то уже есть», а не «есть каталог». Обычный файл с
    // именем RESULT сделал бы Found.txt незаписываемым при находке, поэтому
    // годится только тогда, когда под этим именем действительно каталог.
    if (errno != EEXIST) return false;
    const DWORD a = GetFileAttributesW(plat_wide(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

void plat_make_file_private(const std::string &path) {
    // На Windows права наследуются от каталога, и сузить их так же просто,
    // как chmod 0600, нельзя. Делаем то, что доступно без возни со списками
    // доступа: снимаем «только для чтения», если он был, и оставляем файл в
    // каталоге, созданном программой.
    //
    // Это СЛАБЕЕ, чем 0600 на POSIX, и программа об этом говорит --
    // см. plat_privacy_note(). Написать здесь пустое тело и промолчать было
    // бы обманом: человек решает, где хранить ключи, исходя из того, что ему
    // сказали.
    DWORD a = GetFileAttributesW(plat_wide(path).c_str());
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY))
        SetFileAttributesW(plat_wide(path).c_str(), a & ~FILE_ATTRIBUTE_READONLY);
}

const char *plat_privacy_note() {
    return "Windows: права наследуются от папки — файл защищён не так строго, "
           "как на macOS/Linux (там 0600). Держите папку RESULT там, куда нет "
           "доступа другим пользователям машины.";
}

bool plat_fsync_file(const std::string &path) {
    HANDLE h = CreateFileW(plat_wide(path).c_str(), GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const BOOL ok = FlushFileBuffers(h);
    CloseHandle(h);
    return ok != 0;
}

bool plat_fsync_dir(const std::string &) {
    // У Windows нет понятия «сбросить каталог»: переименование становится
    // долговечным вместе с самим файлом. Возвращаем true не потому, что
    // сделали, а потому что делать нечего.
    return true;
}

namespace {
constexpr DWORD search_console_input_mode(DWORD original) noexcept {
    // Extended flags are required for Windows to accept clearing QuickEdit.
    // Leave processed input, line/echo input and all keyboard flags unchanged.
    return (original | ENABLE_EXTENDED_FLAGS) & ~ENABLE_QUICK_EDIT_MODE;
}

class SearchConsoleInputGuard {
    HANDLE input_ = INVALID_HANDLE_VALUE;
    DWORD original_ = 0;
    bool changed_ = false;
public:
    SearchConsoleInputGuard() noexcept {
        const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
        DWORD original = 0;
        // Redirected stdin is not a console and must not be changed or opened.
        if (!input || input == INVALID_HANDLE_VALUE ||
            !GetConsoleMode(input, &original)) return;
        if (!SetConsoleMode(input, search_console_input_mode(original))) return;
        input_ = input;
        original_ = original;
        changed_ = true;
    }
    ~SearchConsoleInputGuard() noexcept {
        if (changed_) SetConsoleMode(input_, original_);
    }
    SearchConsoleInputGuard(const SearchConsoleInputGuard&) = delete;
    SearchConsoleInputGuard& operator=(const SearchConsoleInputGuard&) = delete;
};
} // namespace

void plat_console_utf8() {
    // Mouse selection in a legacy Windows console can suspend writes, and a
    // finding prints enough text to expose that as a stopped search. Keep this
    // process's console out of QuickEdit; restore its input mode only at exit.
    static SearchConsoleInputGuard input_guard;
    // Без этого весь кириллический вывод — мусор: консоль по умолчанию в 866
    // или 1251. Ставим и вывод, и ввод: пути к файлам пользователь набирает
    // руками, и они тоже бывают кириллическими.
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
}

void plat_run_detached(const std::string &cmd) {
    // start "" /B — запустить и не ждать. Пустые кавычки обязательны: первый
    // параметр start это заголовок окна, и без них он съест саму команду.
    std::string full = "cmd /C start \"\" /B " + cmd;
    STARTUPINFOA si{};  si.cb = sizeof si;
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;              // не мигать окном консоли
    PROCESS_INFORMATION pi{};
    std::string mutable_cmd = full;        // CreateProcess правит строку на месте
    if (CreateProcessA(nullptr, &mutable_cmd[0], nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);          // не ждём: в этом весь смысл
    }
}

std::string plat_run_capture(const std::string &cmd) {
    std::string out;
    FILE *f = _popen(cmd.c_str(), "r");
    if (!f) return out;
    char buf[512];
    while (fgets(buf, sizeof buf, f)) out += buf;
    _pclose(f);
    return out;
}

bool plat_have_command(const std::string &name) {
    return system(("where " + name + " >NUL 2>NUL").c_str()) == 0;
}

// UTF-8 -> UTF-16. Тот же приём, что и в plat_to_native, но без потерь: здесь
// цель — путь для широкого системного вызова, а не аргумент чужой программы.
static std::wstring win_utf8_to_wide(const std::string &s) {
    if (s.empty()) return std::wstring();
    const int wn = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(),
                                       nullptr, 0);
    if (wn <= 0) return std::wstring();
    std::wstring w((size_t)wn, L'\0');
    if (MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], wn) <= 0)
        return std::wstring();
    return w;
}

plat_path_t plat_wide(const std::string &utf8) {
    return win_utf8_to_wide(utf8);
}

FILE *plat_fopen(const std::string &path, const char *mode) {
    // Через _wfopen: узкий fopen прочитал бы байты UTF-8 как CP1251, и путь с
    // кириллицей не открылся бы. Режим — только латиница ("rb","wb",...),
    // поэтому расширяем его до широкого посимвольно.
    const std::wstring wpath = win_utf8_to_wide(path);
    if (wpath.empty()) return nullptr;
    wchar_t wmode[8] = {0};
    for (int i = 0; i < 7 && mode && mode[i]; i++) wmode[i] = (wchar_t)mode[i];
    return _wfopen(wpath.c_str(), wmode);
}

bool plat_remove_file(const std::string &path) {
    const std::wstring wpath = win_utf8_to_wide(path);
    if (wpath.empty()) return false;
    return _wremove(wpath.c_str()) == 0;
}

// ============================================================================
#else   // POSIX: macOS и Linux
// ============================================================================

std::string plat_exe_dir() {
#if defined(__APPLE__)
    uint32_t n = 0;
    _NSGetExecutablePath(nullptr, &n);          // сначала спрашиваем длину
    std::string buf(n ? n : 4096, '\0');
    if (_NSGetExecutablePath(&buf[0], &n) != 0) return std::string();
    buf.resize(strlen(buf.c_str()));
    return dir_of(buf);
#else
    std::string buf(4096, '\0');
    ssize_t n = readlink("/proc/self/exe", &buf[0], buf.size() - 1);
    if (n <= 0) return std::string();
    buf.resize((size_t)n);
    return dir_of(buf);
#endif
}

bool plat_chdir(const std::string &dir) {
    return dir.empty() ? false : (chdir(dir.c_str()) == 0);
}

bool plat_make_dir_private(const std::string &path) {
    if (::mkdir(path.c_str(), 0700) == 0) return true;
    // EEXIST — «что-то уже есть». Принять можно только настоящий каталог:
    // обычный файл под этим именем означал бы, что находка не запишется.
    if (errno != EEXIST) return false;
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

void plat_make_file_private(const std::string &path) {
    // Создаём, если нет, и сразу закрываем права. Именно в таком порядке:
    // chmod уже существующего файла оставил бы окно, в котором он открыт.
    int fd = ::open(path.c_str(), O_CREAT | O_WRONLY | O_APPEND, 0600);
    if (fd >= 0) { ::fchmod(fd, 0600); ::close(fd); }
}

const char *plat_privacy_note() {
    return "Права 0600: файл доступен только владельцу.";
}

bool plat_fsync_file(const std::string &path) {
    int fd = ::open(path.c_str(), O_WRONLY);
    if (fd < 0) return false;
    const int rc = ::fsync(fd);
    ::close(fd);
    return rc == 0;
}

bool plat_fsync_dir(const std::string &path) {
    // Переименование файла становится долговечным только после сброса
    // КАТАЛОГА. Без этого атомарная замена чекпоинта (tmp -> rename) при
    // внезапном выключении может не состояться, и файла не окажется вовсе.
    int fd = ::open(path.empty() ? "." : path.c_str(), O_RDONLY);
    if (fd < 0) return false;
    const int rc = ::fsync(fd);
    ::close(fd);
    return rc == 0;
}

void plat_console_utf8() {
    // Терминалы macOS и Linux и так в UTF-8.
}

void plat_run_detached(const std::string &cmd) {
    // Хвостовой «&» уводит оболочку в фон. Возврат system() при этом не
    // значит ничего, кроме «оболочку удалось запустить»: она вернётся сразу,
    // ещё до первой попытки. Судить по нему об успехе нельзя в принципе.
    const int rc = system((cmd + " > /dev/null 2>&1 &").c_str());
    (void)rc;
}

std::string plat_run_capture(const std::string &cmd) {
    std::string out;
    FILE *f = popen(cmd.c_str(), "r");
    if (!f) return out;
    char buf[512];
    while (fgets(buf, sizeof buf, f)) out += buf;
    pclose(f);
    return out;
}

bool plat_have_command(const std::string &name) {
    return system(("command -v " + name + " > /dev/null 2>&1").c_str()) == 0;
}

plat_path_t plat_wide(const std::string &utf8) {
    return utf8;                 // на POSIX преобразовывать нечего
}

FILE *plat_fopen(const std::string &path, const char *mode) {
    // На POSIX командная строка и пути — байты UTF-8, доходят как есть.
    return std::fopen(path.c_str(), mode);
}

bool plat_remove_file(const std::string &path) {
    return std::remove(path.c_str()) == 0;
}

#endif
