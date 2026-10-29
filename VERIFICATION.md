# Source-publication verification

Prepared on **2026-10-29** for Albert Vainshtein (Вайнштайн Альберт).

## Builds performed

| Check | Result |
| --- | --- |
| Clean Windows x64 CPU build from source | PASS |
| Windows WPF panel build from source | PASS |
| Clean Windows CUDA build, architecture 120 | PASS |
| Public launcher forwarding of `--help` | PASS |
| Public console launcher forwarding of `--pult-protocol-version` | PASS |

Build toolchain: Visual Studio 2022 MSVC 14.44 and CUDA Toolkit 13.3.
All required objects were compiled locally from the exported sources; no
prebuilt application objects are part of the source distribution.

CUDA compilation is **not** a GPU runtime correctness test. No GPU kernel,
search, notification diagnostic or benchmark was run for this publication.
Other CUDA architectures and operating systems require validation on their
corresponding toolchains and hardware.

## CPU self-checks performed

The actual exported `self_test()`, `range_self_test()` and
`strict_range_self_test()` functions were linked into a separate CPU driver
without application `main.cpp` or CUDA. The driver had forced traps for
network delivery, process creation and named inter-process control. Each
suite ran in its own initially empty fixture directory.

- General self-checks: PASS, exit code 0.
- Range audit: 32,896 intervals, zero failures.
- Strict range audit: 234 checks, zero failures.
- GPU operations and notifications: zero.
- Fixture directories after these suites: empty.

This allowed validation without acquiring a running search's application
mutex, loading its database or modifying its results/checkpoints.

## Publication contents

The staged source tree was checked for private delivery settings and common
credential formats. Telegram token/chat defaults and the Signal recipient
default are blank. Personal paths and local profiles were removed.

The Git export excludes target databases, runtime INI files, found-key files,
checkpoints, logs, GUI sessions, object files, executables and DLLs. Only
`config.example.ini`, containing blank notification settings, is included.
Original third-party copyright and license notices are retained.
