#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Encode exactly 64 hexadecimal characters, preserving case and leading zeros.
// Output is row-major, 0 = white and 1 = black. The renderer must add a white
// quiet zone of at least four modules on every side and use integer pixel scale.
// No URL, WIF conversion, file, subprocess, or network operation is involved.
bool make_key_qr(const std::string& hex64, std::vector<std::uint8_t>& modules,
                 int& size, std::string& why);

