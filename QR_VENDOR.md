# Embedded QR generator

PubKEYS embeds the C++ QR Code generator by Project Nayuki under its MIT license.
The generator runs inside the executable; it has no external service or runtime
dependency. The QR payload is the exact 64-character hexadecimal key, including
leading zeros and the supplied letter case. It is not a URL or WIF string.

Upstream repository: https://github.com/nayuki/QR-Code-generator

Pinned commit: `3c6d0b3cefb4e049dc337e82237c9644399716a8`

Files copied without changes:

- `cpp/qrcodegen.hpp`, SHA-256 `b779c3b156cf7a57ce789d6fee4fc991ccc2913774d26c909d22bb8f26b2a793`
- `cpp/qrcodegen.cpp`, SHA-256 `8948b57053deb5d132bfc675ca2688b7abef9f03ec633c0de59770c945a66fc9`

Their full license notices remain in both source files and are also supplied in
`QR_LICENSE.txt`. `key_qr.cpp` is the PubKEYS validation/rendering wrapper. It uses
upstream `QrCode::encodeText` with MEDIUM error correction, automatic mask and
version selection, and the library's same-size error-correction boost. The caller
adds the four-module quiet zone.

Add `key_qr.cpp` and `qrcodegen.cpp` to the C++ build. C++11 or later is sufficient.
