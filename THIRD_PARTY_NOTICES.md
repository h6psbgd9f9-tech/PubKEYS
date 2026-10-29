# Copyright and third-party notices

Original PubKEYS project material: Copyright (c) 2026 Albert Vainshtein
(Вайнштайн Альберт), released under the GNU Affero General Public License,
version 3 only, unless a file states otherwise. The full text is in `LICENSE`.
Third-party files keep their original authors, copyright notices and licenses.

## CUDA field arithmetic

`vanity_vendor/GPUMath_corrected.h` derives from Jean Luc PONS's VanitySearch
arithmetic through the documented VanitySearch-Bitcrack / BitPazl import.
The header states **GNU GPL version 3** and Copyright (c) 2019 Jean Luc PONS.
The imported upstream repository also supplied an **AGPL version 3**
`LICENSE.txt`. Both notices are retained, rather than replacing one with the
other or describing this component as MIT licensed.

- Original header and changes: `vanity_vendor/GPUMath_corrected.h`.
- Exact retained import history: `vanity_vendor/ORIGIN.md`.
- Imported AGPL text: `vanity_vendor/LICENSE.txt` and `LICENSE`.
- Additional GPL version 3 text: `licenses/GPL-3.0.txt`.
- Upstream: <https://github.com/JeanLucPons/VanitySearch> and
  <https://github.com/FixedPaul/VanitySearch-Bitcrack>.

The exported program retains these copyleft notices. The original project
portions use AGPL version 3; this does not erase a third-party file's separate
copyright or license. The retained import history describes some historical
experiments whose local output folders are not part of this repository.

## libsecp256k1

Bundled implementation headers and source: `secp256k1/`.
License: MIT, copyright the libsecp256k1 contributors. See
`secp256k1/COPYING`. Upstream: <https://github.com/bitcoin-core/secp256k1>.
The default build uses the included implementation headers; users do not
need to compile a separate library or fetch a submodule.

## QR Code generator

`qrcodegen.cpp` and `qrcodegen.hpp` are Project Nayuki's C++ QR Code generator,
under the MIT license. Full notices are preserved in those files and
`QR_LICENSE.txt`; provenance is recorded in `QR_VENDOR.md`.
Upstream: <https://github.com/nayuki/QR-Code-generator>.

## Source publication changes

The source publication prepared on 2026-10-29 removes private delivery defaults
and personal paths, adds a portable Windows build wrapper and public launch
scripts, and replaces local installation notes with public English
documentation. Search algorithms are not changed by these packaging edits.
