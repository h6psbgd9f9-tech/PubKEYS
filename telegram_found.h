#pragma once
#include <string>

// Call after attempting the local save, including when that save failed.
// The two notifications are
// acknowledged and retried independently. Background retries stop at app exit.
// Windows uses WinHTTP in this executable; no shell, curl or helper is launched.
// First use registers a normal-exit drain (up to 45 seconds). Forced termination
// and crashes cannot be drained; they do not undo the separate local key file.
void queue_found_telegram(const std::string& token, const std::string& chat,
                          const std::string& hex64, const std::string& address,
                          const std::string& png_path,
                          const std::string& app_name = "PubKEYS");

// Call from normal exit/stop handling, never from a signal handler or GPU hot
// loop. Waits up to timeout_ms for queued notifications; remaining retries
// cannot survive process exit. Returns false if any work is still pending.
bool wait_found_telegram(unsigned timeout_ms = 45000);
unsigned pending_found_telegram();

// Public-only SeedPub match notification. Uses the same WinHTTP delivery,
// retry tracking and normal-exit drain as the existing PubKEYS notifier.
void queue_public_found_telegram(const std::string& token,const std::string& chat,
                                const std::string& public_key,const std::string& address,
                                unsigned long long phrase_index,bool x_only);

#ifdef TELEGRAM_FOUND_TEST
#include <functional>
#include <vector>
namespace telegram_found_test {
struct Request { std::string method, content_type, body; };
struct Response { unsigned status = 0, error = 0; std::string body; };
struct Result { bool text_ok = false, photo_ok = false; unsigned text_tries = 0, photo_tries = 0; };
using Transport = std::function<Response(const Request&)>;
bool valid_input(const std::string& token, const std::string& chat,
                 const std::string& hex64, const std::string& address,
                 const std::string& app_name);
Request text_request(const std::string& chat, const std::string& hex64,
                     const std::string& address, const std::string& app_name);
Request public_text_request(const std::string& chat,const std::string& public_key,
                            const std::string& address,unsigned long long index,bool x_only);
Request photo_request(const std::string& chat, const std::string& hex64,
                      const std::vector<unsigned char>& png, const std::string& app_name);
bool response_ok(const Response& response);
Result run_attempts(const Request& text, const Request& photo,
                    const Transport& transport, unsigned rounds);
bool load_png(const std::string& path, std::vector<unsigned char>& png);
void begin_pending();
void finish_pending();
}
#endif
