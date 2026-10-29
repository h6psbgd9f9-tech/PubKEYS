#pragma once

#include <string>

// Creates a local PNG containing the 64-digit key in Consolas Bold, 16 points,
// and an offline QR code whose payload is exactly hex64. No network is used.
// Errors never include the private key. utf8path may contain Unicode characters.
bool write_found_key_card(const std::string& hex64,
                          const std::string& utf8path,
                          std::string& why,
                          const std::string& app_name = "PubKEYS");
