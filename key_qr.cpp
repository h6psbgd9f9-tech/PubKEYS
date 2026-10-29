#include "key_qr.h"
#include "qrcodegen.hpp"

#include <exception>

bool make_key_qr(const std::string& hex64, std::vector<std::uint8_t>& modules,
                 int& size, std::string& why) {
    modules.clear();
    size = 0;
    why.clear();
    if (hex64.size() != 64) {
        why = "Private-key QR requires exactly 64 hexadecimal characters";
        return false;
    }
    for (const unsigned char c : hex64) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) {
            why = "Private-key QR input contains a non-hexadecimal character";
            return false;
        }
    }
    try {
        // Upstream chooses the smallest version and the lowest-penalty mask.
        // Its automatic ECC boost may improve MEDIUM without enlarging the QR.
        const auto qr = qrcodegen::QrCode::encodeText(
            hex64.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
        const int qrSize = qr.getSize();
        std::vector<std::uint8_t> encoded(static_cast<std::size_t>(qrSize) * qrSize);
        for (int y = 0; y < qrSize; ++y) {
            for (int x = 0; x < qrSize; ++x) {
                encoded[static_cast<std::size_t>(y) * qrSize + x] =
                    qr.getModule(x, y) ? 1 : 0;
            }
        }
        modules.swap(encoded);
        size = qrSize;
        return true;
    } catch (const std::exception&) {
        // Do not include the private input in diagnostics.
        why = "Private-key QR encoding failed";
        return false;
    }
}

