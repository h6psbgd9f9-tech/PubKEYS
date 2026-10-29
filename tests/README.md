# Tests

Tests use known synthetic values and small intervals. No production target
database, found-key file, delivery configuration or saved session is included.

## CPU application checks

Build a CPU executable from this checkout:

```bat
build.cmd cpu -SkipGui
pubkeyhunt.exe --test
pubkeyhunt.exe --range-test
pubkeyhunt.exe --strict-range-test
```

These commands select self-checks rather than an interactive search. Use a
separate checkout without a real configuration or saved progress. Notification
defaults are blank. `--bench` is a benchmark and is not part of these checks.

Standalone host checks are also available in the Windows Makefile. From an
x64 MSVC developer prompt:

```bat
nmake /f Makefile.win filter-test
nmake /f Makefile.win field-test
nmake /f Makefile.win host-test
nmake /f Makefile.win gpu-plan-test
nmake /f Makefile.win targets-test
nmake /f Makefile.win hybrid-test
nmake /f Makefile.win prefetch-test
nmake /f Makefile.win rotation-test
```

The `gpu-plan-test` target checks host-side planning; it does not run CUDA
kernels. More specialized unit sources are supplied for review and targeted
harnesses; the list above is not a claim that every historical audit source
has one identical standalone build command.

## Explicit GPU checks

The `cuda-test` and other CUDA fixture targets execute GPU work. They are
opt-in and require a compatible NVIDIA GPU and Toolkit. Run them only when
the GPU is available for testing; they are not invoked by `build.cmd` or the
CPU CI job. Successful compilation alone does not establish GPU correctness.

Historical scripts that pause or restart another running search, or depend on
private audit folders, are deliberately excluded from this public package.
