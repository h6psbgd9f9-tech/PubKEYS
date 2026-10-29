#include "telegram_found.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <functional>
#include <mutex>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#ifdef _MSC_VER
#pragma comment(lib, "winhttp.lib")
#endif
#endif

namespace {
constexpr size_t MAX_PNG = 10u * 1024u * 1024u;
constexpr size_t MAX_RESPONSE = 1024u * 1024u;
constexpr int RETRY_SECONDS[] = {0, 30, 120, 600, 1800, 3600, 3600, 3600};
#ifdef TELEGRAM_FOUND_TEST
using Request = telegram_found_test::Request;
using Response = telegram_found_test::Response;
using Result = telegram_found_test::Result;
#else
struct Request { std::string method, content_type, body; };
struct Response { unsigned status = 0, error = 0; std::string body; };
struct Result { bool text_ok = false, photo_ok = false; unsigned text_tries = 0, photo_tries = 0; };
#endif
using Transport = std::function<Response(const Request&)>;

struct PendingWorkers {
    std::mutex mutex;
    std::condition_variable changed;
    unsigned count = 0;
};
PendingWorkers& pending_workers() {
    // Detached workers may still be stopping during process teardown.
    static PendingWorkers* const value = new PendingWorkers;
    return *value;
}
void begin_pending_impl() {
    auto& pending = pending_workers();
    std::lock_guard<std::mutex> lock(pending.mutex);
    if (pending.count == (std::numeric_limits<unsigned>::max)()) throw std::runtime_error("worker limit");
    ++pending.count;
}
void finish_pending_impl() noexcept {
    try {
        auto& pending = pending_workers();
        std::lock_guard<std::mutex> lock(pending.mutex);
        if (pending.count) --pending.count;
        pending.changed.notify_all();
    } catch (...) {}
}
struct FinishPending {
    ~FinishPending() { finish_pending_impl(); }
};
bool ensure_exit_drain() {
    // Register after allocating the process-lifetime worker state. The callback
    // may run before or after unrelated application static destructors; it
    // accesses only this module's heap-retained state and generic file log.
    static const bool registered = [] {
        (void)pending_workers();
        return std::atexit([] { (void)wait_found_telegram(45000); }) == 0;
    }();
    return registered;
}

bool ascii_digit(char c) { return c >= '0' && c <= '9'; }
bool ascii_letter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool valid_input_impl(const std::string& token, const std::string& chat,
                      const std::string& hex, const std::string& address,
                      const std::string& app) {
    const size_t colon = token.find(':');
    if (colon == std::string::npos || colon == 0 || colon > 20 ||
        colon + 1 >= token.size() || token.size() > 256) return false;
    for (size_t i = 0; i < token.size(); ++i) {
        if (i == colon) continue;
        const char c = token[i];
        if (i < colon ? !ascii_digit(c) :
            !(ascii_digit(c) || ascii_letter(c) || c == '_' || c == '-')) return false;
    }
    if (chat.empty() || chat.size() > 64) return false;
    size_t start = 0;
    if (chat[0] == '@') {
        if (chat.size() < 2) return false;
        for (size_t i = 1; i < chat.size(); ++i)
            if (!ascii_letter(chat[i]) && !ascii_digit(chat[i]) && chat[i] != '_') return false;
    } else {
        if (chat[0] == '-') start = 1;
        if (start == chat.size()) return false;
        for (size_t i = start; i < chat.size(); ++i) if (!ascii_digit(chat[i])) return false;
    }
    if (hex.size() != 64) return false;
    for (const char c : hex)
        if (!ascii_digit(c) && !(c >= 'a' && c <= 'f') && !(c >= 'A' && c <= 'F')) return false;
    if (app.empty() || app.size() > 64 || address.size() > 512) return false;
    for (const char c : app)
        if (!ascii_letter(c) && !ascii_digit(c) && c != ' ' && c != '_' && c != '-') return false;
    // Addresses are ASCII. Reject controls; HTML metacharacters are escaped.
    for (const unsigned char c : address) if (c < 32 || c > 126) return false;
    return true;
}

std::string html_escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '&') out += "&amp;";
        else if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else if (c == '"') out += "&quot;";
        else out += c;
    }
    return out;
}

std::string json_string(const std::string& s) {
    static const char HEX[] = "0123456789abcdef";
    std::string out = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
        else if (c < 32) { out += "\\u00"; out += HEX[c >> 4]; out += HEX[c & 15]; }
        else out += static_cast<char>(c);
    }
    return out + '"';
}

Request make_text(const std::string& chat, const std::string& hex,
                  const std::string& address, const std::string& app) {
    const std::string text = "<b>" + html_escape(app) + ": совпадение</b>\n"
        "Приватный ключ HEX64:\n<code>" + hex + "</code>" +
        (address.empty() ? "" : "\nАдрес: <code>" + html_escape(address) + "</code>");
    return {"sendMessage", "application/json; charset=utf-8",
        "{\"chat_id\":" + json_string(chat) + ",\"text\":" + json_string(text) +
        ",\"parse_mode\":\"HTML\",\"link_preview_options\":{\"is_disabled\":true}}"};
}

Request make_photo(const std::string& chat, const std::string& hex,
                   const std::vector<unsigned char>& png, const std::string& app) {
    if (png.empty() || png.size() > MAX_PNG) return {};
    const std::string binary(reinterpret_cast<const char*>(png.data()), png.size());
    // The chosen boundary must not occur in the binary payload or text fields.
    std::string boundary;
    for (unsigned i = 0; ; ++i) {
        boundary = "FoundCardBoundary_9D87508EA8BC_" + std::to_string(i);
        if (binary.find(boundary) == std::string::npos && chat.find(boundary) == std::string::npos &&
            app.find(boundary) == std::string::npos) break;
    }
    std::string body;
    auto field = [&](const char* name, const std::string& value) {
        body += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + name +
            "\"\r\n\r\n" + value + "\r\n";
    };
    field("chat_id", chat);
    field("caption", app + ": совпадение\nHEX64: " + hex);
    body += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"photo\"; "
        "filename=\"found.png\"\r\nContent-Type: image/png\r\n\r\n";
    body.append(binary.data(), binary.size());
    body += "\r\n--" + boundary + "--\r\n";
    return {"sendPhoto", "multipart/form-data; boundary=" + boundary, std::move(body)};
}

Request make_public_text(const std::string& chat,const std::string& public_key,
                         const std::string& address,unsigned long long index,bool x_only){
    const std::string text="SeedPub: подтверждённое совпадение\nПубличный ключ: "+public_key+
        "\nTRON: "+address+"\nНомер фразы: "+std::to_string(index)+
        "\nСравнение: "+(x_only?"только X":"полный публичный ключ");
    return {"sendMessage","application/json; charset=utf-8",
        "{\"chat_id\":"+json_string(chat)+",\"text\":"+json_string(text)+
        ",\"link_preview_options\":{\"is_disabled\":true}}"};
}

// Parse a bounded JSON document rather than looking for an `ok:true` substring
// that could occur in an error description or a nested result. Only a unique
// top-level boolean `ok` and a fully valid response are accepted.
class JsonReply {
    const std::string& s;
    size_t p = 0;
    void ws() { while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r' || s[p] == '\n')) ++p; }
    bool take(char c) { ws(); if (p >= s.size() || s[p] != c) return false; ++p; return true; }
    static int digit(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }
    bool string(std::string* key = nullptr) {
        if (!take('"')) return false;
        while (p < s.size()) {
            unsigned char c = static_cast<unsigned char>(s[p++]);
            if (c == '"') return true;
            if (c < 32) return false;
            if (c != '\\') { if (key) *key += static_cast<char>(c); continue; }
            if (p == s.size()) return false;
            c = static_cast<unsigned char>(s[p++]);
            if (c == 'u') {
                if (s.size() - p < 4) return false;
                unsigned v = 0;
                for (int i = 0; i < 4; ++i) { int d = digit(s[p++]); if (d < 0) return false; v = v * 16 + d; }
                if (key) *key += v < 128 ? static_cast<char>(v) : '?';
            } else if (c == '"' || c == '\\' || c == '/' || c == 'b' || c == 'f' || c == 'n' || c == 'r' || c == 't') {
                if (key) *key += (c == '"' || c == '\\' || c == '/') ? static_cast<char>(c) : '?';
            } else return false;
        }
        return false;
    }
    bool literal(const char* text) {
        const size_t n = std::strlen(text);
        if (s.compare(p, n, text) != 0) return false;
        p += n; return true;
    }
    bool number() {
        if (p < s.size() && s[p] == '-') ++p;
        if (p == s.size()) return false;
        if (s[p] == '0') ++p;
        else {
            if (s[p] < '1' || s[p] > '9') return false;
            while (p < s.size() && ascii_digit(s[p])) ++p;
        }
        if (p < s.size() && s[p] == '.') {
            ++p; const size_t first = p;
            while (p < s.size() && ascii_digit(s[p])) ++p;
            if (p == first) return false;
        }
        if (p < s.size() && (s[p] == 'e' || s[p] == 'E')) {
            ++p; if (p < s.size() && (s[p] == '+' || s[p] == '-')) ++p;
            const size_t first = p;
            while (p < s.size() && ascii_digit(s[p])) ++p;
            if (p == first) return false;
        }
        return true;
    }
    bool value(unsigned depth) {
        ws(); if (depth > 32 || p == s.size()) return false;
        if (s[p] == '"') return string();
        if (s[p] == '{' || s[p] == '[') {
            const bool obj = s[p++] == '{'; const char end = obj ? '}' : ']';
            if (take(end)) return true;
            do {
                if (obj && (!string() || !take(':'))) return false;
                if (!value(depth + 1)) return false;
                if (take(end)) return true;
            } while (take(','));
            return false;
        }
        if (s[p] == 't') return literal("true");
        if (s[p] == 'f') return literal("false");
        if (s[p] == 'n') return literal("null");
        return number();
    }
public:
    explicit JsonReply(const std::string& text) : s(text) {}
    bool ok() {
        if (!take('{') || take('}')) return false;
        bool seen = false, success = false;
        do {
            std::string key;
            if (!string(&key) || !take(':')) return false;
            ws();
            if (key == "ok") {
                if (seen) return false;
                seen = true;
                if (literal("true")) success = true;
                else if (!literal("false")) return false;
            } else if (!value(1)) return false;
            if (take('}')) { ws(); return p == s.size() && seen && success; }
        } while (take(','));
        return false;
    }
};

bool accepted(const Response& r) {
    return r.error == 0 && r.status == 200 && r.body.size() <= MAX_RESPONSE && JsonReply(r.body).ok();
}

// This log deliberately has no key, address, chat, token, URL or server body.
void status_log(const char* channel, const char* state, unsigned attempt = 0,
                unsigned status = 0, unsigned error = 0) noexcept {
    try {
        // Intentionally process-lifetime: detached workers must not race a
        // function-static mutex destructor during normal application exit.
        static std::mutex* const guard = new std::mutex;
        std::lock_guard<std::mutex> lock(*guard);
        std::ofstream log("notify.log", std::ios::app);
        if (log) log << static_cast<long long>(std::time(nullptr)) << " Telegram/found/" << channel
            << " " << state << " attempt=" << attempt << " http=" << status << " error=" << error << '\n';
    } catch (...) {}
}

void attempt(const Request& text, const Request& photo, const Transport& transport,
              Result& result, bool log_status) {
    auto send = [&](const Request& request, bool& done, unsigned& count, const char* name) {
        if (done || request.method.empty()) return;
        ++count;
        Response response;
        try { response = transport(request); } catch (...) { response.error = 1; }
        done = accepted(response);
        if (log_status) status_log(name, done ? "delivered" : "retry_pending", count, response.status, response.error);
    };
    send(text, result.text_ok, result.text_tries, "text");
    send(photo, result.photo_ok, result.photo_tries, "photo");
}

#ifdef _WIN32
std::wstring widen(const std::string& utf8) {
    if (utf8.empty() || utf8.size() > 32767) return {};
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), &out[0], n) != n) return {};
    return out;
}
struct HttpHandle {
    HINTERNET value = nullptr;
    explicit HttpHandle(HINTERNET v) : value(v) {}
    ~HttpHandle() { if (value) WinHttpCloseHandle(value); }
    HttpHandle(const HttpHandle&) = delete;
    HttpHandle& operator=(const HttpHandle&) = delete;
    explicit operator bool() const { return value != nullptr; }
};

Response post_winhttp(const std::string& token, const Request& message) {
    Response result;
    auto fail = [&]() { result.error = GetLastError(); if (!result.error) result.error = ERROR_INVALID_DATA; return result; };
    if (message.body.size() > MAX_PNG + 16384 ||
        (message.method != "sendMessage" && message.method != "sendPhoto")) {
        result.error = ERROR_INVALID_PARAMETER; return result;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
    HttpHandle session(WinHttpOpen(L"FoundNotifier/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) return fail();
    if (!WinHttpSetTimeouts(session.value, 10000, 10000, 20000, 20000)) return fail();
    HttpHandle connection(WinHttpConnect(session.value, L"api.telegram.org", INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (!connection) return fail();
    const std::wstring path = widen("/bot" + token + "/" + message.method);
    HttpHandle request(WinHttpOpenRequest(connection.value, L"POST", path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (!request) return fail();
    DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if (!WinHttpSetOption(request.value, WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof(redirects))) return fail();
    DWORD autologon = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
    if (!WinHttpSetOption(request.value, WINHTTP_OPTION_AUTOLOGON_POLICY, &autologon, sizeof(autologon))) return fail();
    DWORD disabled = WINHTTP_DISABLE_COOKIES;
    if (!WinHttpSetOption(request.value, WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled))) return fail();
    // Certificate validation and TLS policy remain at secure OS defaults.
    const std::wstring headers = widen("Content-Type: " + message.content_type + "\r\n");
    if (!WinHttpSendRequest(request.value, headers.c_str(), static_cast<DWORD>(headers.size()),
        WINHTTP_NO_REQUEST_DATA, 0, static_cast<DWORD>(message.body.size()), 0)) return fail();
    size_t offset = 0;
    while (offset < message.body.size()) {
        if (std::chrono::steady_clock::now() > deadline) { result.error = ERROR_TIMEOUT; return result; }
        const DWORD chunk = static_cast<DWORD>((std::min)(size_t(65536), message.body.size() - offset));
        DWORD written = 0;
        if (!WinHttpWriteData(request.value, message.body.data() + offset, chunk, &written)) return fail();
        if (!written) { result.error = ERROR_WRITE_FAULT; return result; }
        offset += written;
    }
    if (!WinHttpReceiveResponse(request.value, nullptr)) return fail();
    DWORD status = 0, size = sizeof(status);
    if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX)) return fail();
    result.status = status;
    // Do not follow redirects or retain potentially sensitive error bodies.
    if (status != 200) return result;
    for (;;) {
        if (std::chrono::steady_clock::now() > deadline) { result.error = ERROR_TIMEOUT; return result; }
        char buffer[16384]; DWORD read = 0;
        if (!WinHttpReadData(request.value, buffer, sizeof(buffer), &read)) return fail();
        if (!read) break;
        if (read > MAX_RESPONSE - result.body.size()) { result.error = ERROR_INSUFFICIENT_BUFFER; return result; }
        result.body.append(buffer, read);
    }
    return result;
}
#endif

bool read_png(const std::string& path, std::vector<unsigned char>& png) {
    png.clear();
#ifdef _WIN32
    const std::wstring filename = widen(path);
    if (filename.empty() || path.find('\0') != std::string::npos) return false;
    HANDLE file = CreateFileW(filename.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    struct CloseFile { HANDLE value; ~CloseFile() { CloseHandle(value); } } closer{file};
    LARGE_INTEGER size;
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 8 || size.QuadPart > static_cast<LONGLONG>(MAX_PNG)) return false;
    png.resize(static_cast<size_t>(size.QuadPart));
    size_t offset = 0;
    while (offset < png.size()) {
        DWORD read = 0;
        const DWORD chunk = static_cast<DWORD>((std::min)(size_t(65536), png.size() - offset));
        if (!ReadFile(file, png.data() + offset, chunk, &read, nullptr) || read == 0) { png.clear(); return false; }
        offset += read;
    }
#else
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return false;
    const std::streamoff size = file.tellg();
    if (size < 8 || size > static_cast<std::streamoff>(MAX_PNG)) return false;
    png.resize(static_cast<size_t>(size)); file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(png.data()), size)) { png.clear(); return false; }
#endif
    static const unsigned char magic[] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (!std::equal(magic, magic + sizeof(magic), png.begin())) { png.clear(); return false; }
    return true;
}
} // namespace

void queue_found_telegram(const std::string& token, const std::string& chat,
                          const std::string& hex64, const std::string& address,
                          const std::string& png_path, const std::string& app_name) {
    if (token.empty() || chat.empty()) return;
    if (!valid_input_impl(token, chat, hex64, address, app_name)) {
        status_log("queue", "invalid_input"); return;
    }
#if defined(_WIN32) && !defined(TELEGRAM_FOUND_TEST)
    bool registered = false;
    try {
        if (!ensure_exit_drain()) status_log("queue", "automatic_exit_wait_unavailable");
        begin_pending_impl();
        registered = true;
        std::thread([token, chat, hex64, address, png_path, app_name]() {
            FinishPending finish;
            try {
                const Request text = make_text(chat, hex64, address, app_name);
                std::vector<unsigned char> png;
                Request photo;
                if (!png_path.empty() && read_png(png_path, png)) photo = make_photo(chat, hex64, png, app_name);
                if (photo.method.empty()) status_log("photo", "unavailable_text_continues");
                const Transport transport = [&](const Request& request) { return post_winhttp(token, request); };
                Result result;
                for (int seconds : RETRY_SECONDS) {
                    if (seconds) std::this_thread::sleep_for(std::chrono::seconds(seconds));
                    attempt(text, photo, transport, result, true);
                    if (result.text_ok && (photo.method.empty() || result.photo_ok)) return;
                }
                if (!result.text_ok) status_log("text", "attempts_exhausted", result.text_tries);
                if (!photo.method.empty() && !result.photo_ok) status_log("photo", "attempts_exhausted", result.photo_tries);
            } catch (...) { status_log("queue", "worker_failed"); }
        }).detach();
    } catch (...) {
        if (registered) finish_pending_impl();
        status_log("queue", "worker_start_failed");
    }
#else
    (void)png_path;
    // This module adds no POSIX transport or external dependency. The caller
    // can retain its existing POSIX notifier. Test builds cannot send at all.
    status_log("queue", "transport_not_enabled");
#endif
}

unsigned pending_found_telegram() {
    try {
        auto& pending = pending_workers();
        std::lock_guard<std::mutex> lock(pending.mutex);
        return pending.count;
    } catch (...) { return (std::numeric_limits<unsigned>::max)(); }
}

void queue_public_found_telegram(const std::string& token,const std::string& chat,
                                const std::string& public_key,const std::string& address,
                                unsigned long long phrase_index,bool x_only){
    if(token.empty()||chat.empty())return;
    bool key_ok=public_key.size()==66&&(public_key.substr(0,2)=="02"||public_key.substr(0,2)=="03");
    for(char ch:public_key)if(!ascii_digit(ch)&&!(ch>='a'&&ch<='f')&&!(ch>='A'&&ch<='F'))key_ok=false;
    if(!key_ok||!valid_input_impl(token,chat,std::string(64,'0'),address,"SeedPub")){status_log("queue","invalid_public_match");return;}
#if defined(_WIN32) && !defined(TELEGRAM_FOUND_TEST)
    bool registered=false;
    try{
        if(!ensure_exit_drain())status_log("queue","automatic_exit_wait_unavailable");
        begin_pending_impl();registered=true;
        std::thread([token,chat,public_key,address,phrase_index,x_only]{
            FinishPending finish;
            try{
                auto text=make_public_text(chat,public_key,address,phrase_index,x_only);
                const Transport transport=[&](const Request& request){return post_winhttp(token,request);};
                Result result;
                for(int seconds:RETRY_SECONDS){
                    if(seconds)std::this_thread::sleep_for(std::chrono::seconds(seconds));
                    attempt(text,{},transport,result,true);if(result.text_ok)return;
                }
                status_log("public_text","attempts_exhausted",result.text_tries);
            }catch(...){status_log("queue","public_worker_failed");}
        }).detach();
    }catch(...){if(registered)finish_pending_impl();status_log("queue","public_worker_start_failed");}
#endif
}

bool wait_found_telegram(unsigned timeout_ms) {
    try {
        auto& pending = pending_workers();
        std::unique_lock<std::mutex> lock(pending.mutex);
        const bool done = pending.changed.wait_for(lock, std::chrono::milliseconds(timeout_ms),
            [&pending] { return pending.count == 0; });
        lock.unlock();
        if (!done) status_log("queue", "exit_wait_timeout_pending");
        return done;
    } catch (...) { status_log("queue", "exit_wait_failed"); return false; }
}

#ifdef TELEGRAM_FOUND_TEST
namespace telegram_found_test {
bool valid_input(const std::string& token, const std::string& chat, const std::string& hex,
                 const std::string& address, const std::string& app) {
    return valid_input_impl(token, chat, hex, address, app);
}
Request text_request(const std::string& chat, const std::string& hex,
                     const std::string& address, const std::string& app) { return make_text(chat, hex, address, app); }
Request public_text_request(const std::string& chat,const std::string& public_key,
                            const std::string& address,unsigned long long index,bool x_only){return make_public_text(chat,public_key,address,index,x_only);}
Request photo_request(const std::string& chat, const std::string& hex,
                      const std::vector<unsigned char>& png, const std::string& app) { return make_photo(chat, hex, png, app); }
bool response_ok(const Response& response) { return accepted(response); }
Result run_attempts(const Request& text, const Request& photo, const Transport& transport, unsigned rounds) {
    Result result;
    for (unsigned i = 0; i < rounds; ++i) {
        attempt(text, photo, transport, result, false);
        if (result.text_ok && (photo.method.empty() || result.photo_ok)) break;
    }
    return result;
}
bool load_png(const std::string& path, std::vector<unsigned char>& png) { return read_png(path, png); }
void begin_pending() { begin_pending_impl(); }
void finish_pending() { finish_pending_impl(); }
}
#endif
