# Building PubKEYS

Author: Albert Vainshtein (Вайнштайн Альберт).

## Windows prerequisites

- A 64-bit Windows installation.
- Visual Studio 2022 or Visual Studio 2022 Build Tools with the MSVC C++
  toolchain and a Windows SDK. Select **Desktop development with C++** in
  the installer. The wrapper uses `vswhere.exe` to find the installation;
  it also accepts an already configured x64 developer environment.
- For the optional GUI, Windows .NET Framework 4.x with WPF and its C# compiler.
  The program uses the framework compiler shipped with Windows rather than
  requiring a separate NuGet dependency restore.
- For CUDA builds only: NVIDIA CUDA Toolkit and a compatible NVIDIA driver.
  The Toolkit must support the installed MSVC version and selected GPU targets.

Official downloads: [Visual Studio / Build Tools](https://visualstudio.microsoft.com/downloads/),
[older Visual Studio versions](https://visualstudio.microsoft.com/vs/older-downloads/),
and [NVIDIA CUDA Toolkit](https://developer.nvidia.com/cuda-downloads).

The source package includes libsecp256k1 implementation headers, the QR
generator and imported CUDA arithmetic. It contains no compiled dependency,
binary object, executable, database or local configuration.

## Build commands

Run these commands from an ordinary Command Prompt or PowerShell in the
repository root. No administrator privileges are needed for compilation.

```bat
build.cmd cpu
build.cmd cuda
build.cmd gui
build.cmd all
```

CPU is the default. CPU/CUDA builds also compile the GUI unless `-SkipGui`
is supplied. Neither building nor displaying help starts a search.

```bat
build.cmd cpu -SkipGui
build.cmd cuda -CudaArchitectures 120
```

The CUDA wrapper reads supported architecture names from `nvcc`, not from a
GPU benchmark. Its default multi-architecture build can take substantially
longer than targeting one architecture. An NVIDIA GPU is not required merely
to compile CUDA sources, but one is needed to run the CUDA backend.

Outputs:

| Build | Executable |
| --- | --- |
| CPU | `pubkeyhunt.exe` |
| CUDA | `pubkeyhunt_cuda_verified.exe` |
| Windows GUI | `PubKEYS-Pult.exe` |

The CUDA executable name is retained for compatibility with the desktop panel;
the word `verified` in its filename is not proof that a new GPU or driver has
passed runtime correctness tests.

Build logs and temporary objects are local build outputs and are ignored by Git.
Use a fresh folder when reproducing a release rather than copying old objects.

## Start the application

```bat
run.cmd
run-console.cmd
run.cmd --help
```

The launcher prefers the locally built CUDA executable when it exists;
otherwise it selects the CPU executable. With the panel available, `run.cmd`
opens the panel with the selected engine. `run-console.cmd` always opens the
console. Supplying command-line arguments to `run.cmd` forwards them directly
to the selected engine.

Choose your public-key text file and search parameters. There is no bundled
database. `config.example.ini` demonstrates blank notification settings and may
be copied to `config.ini`; normal interactive setup also writes local settings.

The panel can remember an accepted run and resume a compatible saved session.
An unused source checkout contains no such profile. Keep test folders separate
from folders containing real search progress.

## Manual Windows build

From **x64 Native Tools Command Prompt for VS 2022**:

```bat
nmake /f Makefile.win
gui\build.cmd "%CD%"
```

For a CUDA build, ensure `CUDA_PATH` points to the Toolkit and `nvcc` is on PATH:

```bat
gen_gencode.bat
nmake /f Makefile.win cuda CUDA_EXE=pubkeyhunt_cuda_verified.exe
gui\build.cmd "%CD%"
```

The public wrapper is preferred because it performs dependency discovery and
architecture checks automatically. Architecture flags and `CUDA_HALF` must be
consistent between CUDA and host objects; do not mix objects from different
build configurations.

## Linux and macOS

The GNU Makefile provides a console CPU build with a C++17 compiler, C compiler
and POSIX threads:

```sh
make
./pubkeyhunt --help
./pubkeyhunt
```

`make own` selects the independent arithmetic backend. libsecp256k1 sources
are bundled for the default backend. The WPF panel is Windows-only.

The current Windows release preparation validates Windows builds. Unix builds
must be checked on their target platform. The legacy GNU Makefile CUDA target
only compiles CUDA objects; it is not a complete supported CUDA application
build. Use the Windows CUDA build instructions above for this source release.

## Troubleshooting

| Symptom | Action |
| --- | --- |
| MSVC or Windows SDK not found | Install the C++ workload and SDK, then rerun `build.cmd`. |
| `vswhere` cannot locate a toolchain | Open an x64 VS developer prompt and rerun, or repair the Build Tools installation. |
| `nvcc` missing | Install CUDA Toolkit and reopen the terminal; CPU builds do not need CUDA. |
| Unsupported GPU architecture | Choose a target reported by your Toolkit or install a Toolkit supporting the GPU. |
| Unsupported MSVC version reported by CUDA | Use an NVIDIA-supported MSVC/Toolkit combination. |
| GUI compiler or WPF libraries missing | Enable/install the .NET Framework desktop components, or build with `-SkipGui`. |
| Required VC runtime DLL missing on another machine | Install the Microsoft Visual C++ x64 Redistributable matching the toolchain. |
| Public-key file missing | Supply your own file; the repository intentionally contains no target database. |

See [NOTIFICATIONS.md](NOTIFICATIONS.md) for optional delivery setup and
[tests/README.md](tests/README.md) for CPU checks and explicit GPU tests.
The checks actually performed for this source publication are listed in
[VERIFICATION.md](VERIFICATION.md).
