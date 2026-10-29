# PubKEYS / PubkeyHunt

**Author: Albert Vainshtein (Вайнштайн Альберт).**

PubKEYS is a C++17 secp256k1 public-key range scanner with a CPU backend, an
optional NVIDIA CUDA backend, and a Windows desktop control panel. It generates
candidate public keys from known candidate private scalars, compares them with
a user-supplied public-key list, independently confirms matches on the CPU,
and saves confirmed private keys and Wallet Import Format (WIF) values.

The project is intended for reproducible range-search experiments, puzzles and
recovery tasks where the candidate interval is known. It does not provide an
algorithm for reversing an arbitrary Bitcoin public key. Exhaustive search of
the entire 256-bit space is computationally impractical.

This repository contains source code, bundled third-party source dependencies,
build scripts and tests. **No target database, personal configuration, saved
results, checkpoints, Telegram credentials or Signal recipients are included.**
The current application interface is primarily in Russian; this documentation
and the public build interface are in English.

## Quick start on Windows

1. Install **Visual Studio 2022** or its **Build Tools**, selecting **Desktop
   development with C++** and a Windows SDK. The optional control panel uses
   the Windows .NET Framework 4.x / WPF compiler and libraries.
2. Download this repository as a ZIP and extract it, or clone it with Git.
   The required secp256k1 and QR sources are already included; no submodule
   initialization or separately compiled secp256k1 library is required.
3. In the extracted folder, run:

   ```bat
   build.cmd cpu
   ```

4. Run the desktop control panel:

   ```bat
   run.cmd
   ```

   Or use the interactive console:

   ```bat
   run-console.cmd
   ```

5. Select your own public-key file, interval, traversal mode and power setting
   before starting a search. The repository does not contain that file.

The build script locates Visual Studio automatically and builds from source.
It does not start a search. See [BUILDING.md](BUILDING.md) for CUDA, GUI-only
and Unix build instructions, prerequisites and troubleshooting.

### NVIDIA GPU build

Install a supported NVIDIA driver and a CUDA Toolkit compatible with your
GPU and MSVC installation, then run:

```bat
build.cmd cuda
run.cmd
```

To reduce compilation time when targeting one GPU architecture:

```bat
build.cmd cuda -CudaArchitectures 120
```

`120` is an example for an appropriate Blackwell GPU, not a setting for every
NVIDIA card. Select the compute capability of your GPU. The script checks
supported compiler targets without running GPU computations.

## Traversal modes

| Mode | How it visits candidate scalars |
| --- | --- |
| Deterministic (`seq`) | Consecutive blocks, from the lower bound upward. |
| KXE (`kxe`) | A permutation of the deterministic blocks, without repeating a primary block within one complete traversal. |
| Wave (`wave`) | Original wave patterns change groups of hexadecimal positions together. Complete patterns are scheduled across bit-length buckets; an exact complementary traversal covers the remaining windows. |
| Zonal (`zone`) | Original zone patterns vary one hexadecimal position above a consecutive-key block. Complete patterns are balanced across bit-length buckets, with the remaining windows covered by a complementary traversal. |
| Hybrid (`hybrid`) | A random upper part A remains fixed while the lower part B is scanned consecutively. |
| Random (`random`) | Randomly selected blocks; repeated work is possible. |

Wave and zonal traversal preserve their distinct pattern rules. Balancing
means turns through whole patterns, not identical time spent at each bit
length. An explicitly fixed wave start is an intentional exception to automatic
bit-length balancing. A new independent search can revisit prior work; use a
compatible checkpoint when continuing a saved traversal.

### Ranges and the mirror option

The interval entered as `a` to `b` means **[2^a, 2^b)**, clipped to valid
secp256k1 private scalars **1 <= k < n**. For example, `54` to `55` includes
2^54 and excludes 2^55. `0` to `256` covers the valid scalar space; zero is
never a private key.

When the `n-k` mirror is enabled, the opposite public point is also checked
against the target list, including when its private scalar lies outside the
primary interval. Mirror checks do not imply that a second complete primary
interval has been scanned.

## Target-file format

Provide a text file with one public key per line, in hexadecimal:

- SEC1 compressed: `02` or `03`, followed by 64 hexadecimal digits;
- SEC1 uncompressed: `04`, followed by 128 hexadecimal digits;
- an X-only coordinate: 64 hexadecimal digits, interpreted according to the
  application's X-only matching rules.

A Bitcoin address such as `1...` or `bc1...` is not a public key and is not
an accepted substitute for this input format. Public keys and encodings are
validated before use. The Bloom filter is a prefilter; candidate matches are
confirmed before being recorded.

## Results, progress and control

- Confirmed matches are written to the configured Found file with the private
  scalar and compressed/uncompressed WIF information. Finding a match does
  not itself terminate an unfinished search interval.
- Status and a current candidate sample are updated periodically. A sample
  headed `ПРОВЕРКА` is a demonstration of work, not a confirmed finding.
- The Windows panel displays status, throughput, power and supported GPU
  temperature information, and provides search controls.
- The console supports `+` / `-` for power adjustment and protects against
  accidental Ctrl+C. Explicit stop uses `Q`, then confirmation with `Y`.
- On supported Windows laptops, loss of AC power pauses work; AC restoration
  resumes at the previously selected power setting.
- Progress measurements are written to `RESULT/performance.csv`.

The power percentage controls computation duty cycle. It is not an electrical
wattage limit or a substitute for GPU firmware power management.

### Checkpoints

Deterministic, KXE, wave and zonal modes retain confirmed traversal progress;
hybrid traversal has its own state format. Random traversal does not promise
exact continuation or complete coverage.

Wave/zonal checkpoints are saved initially, every five minutes of active work,
on acknowledged pause, at orderly stop, and before a HEX-part transition. They
bind the traversal parameters and target-set identity, and store the traversal
seed, confirmed position and exact confirmed count with an integrity checksum.
Only a fully completed contiguous prefix is committed. After a crash, recent
work and an unfinished logical block may be repeated. A checksum detects
accidental corruption; it does not authenticate files against a malicious writer.

## Optional notifications

Telegram and Signal are **disabled by empty delivery settings in public builds**.
Users configure their own credentials and recipients locally. Signal uses a
separately installed and configured `signal-cli`; it does not use the Telegram
bot-token mechanism. See [NOTIFICATIONS.md](NOTIFICATIONS.md).

## Tests and verification

Build the CPU executable first. CPU-only self-check commands are:

```bat
pubkeyhunt.exe --test
pubkeyhunt.exe --range-test
pubkeyhunt.exe --strict-range-test
```

Use a separate checkout for tests. Explicit CUDA tests require a free GPU and
are not part of the default build. See [tests/README.md](tests/README.md).

## Source map

| Files | Responsibility |
| --- | --- |
| `main.cpp`, `ui.cpp`, `config.cpp` | Application lifecycle, console UI and configuration. |
| `traverse.cpp`, `traverse_modes.cpp` | Traversal mapping, confirmed progress and checkpoint persistence. |
| `balanced_shape_traversal.h`, `whole_shape_traversal.h`, `shaped_traversal.h` | Wave/zonal patterns, balancing and complete interval coverage. |
| `engine.cpp`, `scan_secp.c` | CPU scanning and candidate processing. |
| `cuda_scan.cu`, `cuda_host.cpp`, `cuda_*.cuh` | CUDA kernels and host integration. |
| `targets.cpp`, `targets.h`, `bloom.h` | Target parsing and lookup. |
| `notify.cpp`, `telegram_found.cpp`, `found_*.h` | Confirmed results and optional delivery. |
| `gui/`, `pult_control.cpp` | Windows WPF control panel and worker protocol. |

## License and credits

Original PubKEYS project material is published under **GNU AGPL version 3**;
third-party files retain their own notices and applicable licenses. See
[LICENSE](LICENSE) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md), including
the retained GPLv3/AGPLv3 notices for the imported CUDA arithmetic. The code is
provided without warranty.
