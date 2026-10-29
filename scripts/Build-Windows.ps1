[CmdletBinding()]
param(
    [Parameter(Position=0)]
    [ValidateSet('cpu','cuda','gui','all')]
    [string]$Target = 'cpu',
    [switch]$SkipGui,
    [string]$CudaArchitectures = ''
)

$ErrorActionPreference = 'Stop'
$sourceRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$logDirectory = Join-Path $sourceRoot 'build\logs'
[IO.Directory]::CreateDirectory($logDirectory) | Out-Null
$logFile = Join-Path $logDirectory ('windows-{0}-{1}.log' -f $Target,(Get-Date -Format 'yyyyMMdd-HHmmss'))
$transcriptStarted = $false

function Invoke-CheckedNative {
    param([string]$Program,[string[]]$Arguments)
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw ('Build command failed with exit code {0}: {1}' -f $LASTEXITCODE,$Program)
    }
}

function Import-MsvcEnvironment {
    if ((Get-Command cl.exe -CommandType Application -ErrorAction SilentlyContinue) -and
        (Get-Command nmake.exe -CommandType Application -ErrorAction SilentlyContinue) -and
        $env:VSCMD_ARG_TGT_ARCH -eq 'x64') { return }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
        throw 'Visual Studio 2022 Build Tools were not found. Install Desktop development with C++, MSVC x64/x86 tools and a Windows SDK.'
    }
    $installation = @(& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath) | Select-Object -First 1
    if (-not $installation) { throw 'No Visual Studio installation with the MSVC C++ tools was found.' }
    $vcvars = Join-Path $installation 'VC\Auxiliary\Build\vcvars64.bat'
    if (-not (Test-Path -LiteralPath $vcvars -PathType Leaf)) { throw 'The x64 Visual Studio environment script was not found.' }
    if ($vcvars -match '["\r\n&|<>]') { throw 'Unsupported characters in the Visual Studio installation path.' }
    # Pass cmd.exe its command line verbatim. PowerShell's native argument
    # conversion can otherwise remove the quotes around an installation path
    # containing spaces (for example, the Visual Studio GitHub runner path).
    $environmentProcess = New-Object System.Diagnostics.Process
    $environmentProcess.StartInfo.FileName = $env:ComSpec
    $environmentProcess.StartInfo.Arguments = '/d /s /c (call "' + $vcvars + '" >nul) && set'
    $environmentProcess.StartInfo.UseShellExecute = $false
    $environmentProcess.StartInfo.CreateNoWindow = $true
    $environmentProcess.StartInfo.RedirectStandardOutput = $true
    $environmentProcess.StartInfo.RedirectStandardError = $true
    try {
        [void]$environmentProcess.Start()
        $environmentError = $environmentProcess.StandardError.ReadToEndAsync()
        $environmentOutput = $environmentProcess.StandardOutput.ReadToEnd()
        $environmentProcess.WaitForExit()
        if ($environmentProcess.ExitCode -ne 0) { throw 'Visual Studio could not initialize its x64 compilation environment.' }
        [void]$environmentError.GetAwaiter().GetResult()
        $environmentLines = @($environmentOutput -split '\r?\n')
    } finally { $environmentProcess.Dispose() }
    foreach ($line in $environmentLines) {
        if ($line -match '^([^=]+)=(.*)$') {
            [Environment]::SetEnvironmentVariable($matches[1],$matches[2],'Process')
        }
    }
    if (-not (Get-Command cl.exe -CommandType Application -ErrorAction SilentlyContinue) -or
        -not (Get-Command nmake.exe -CommandType Application -ErrorAction SilentlyContinue)) {
        throw 'The x64 MSVC compiler and nmake are not available after initialization.'
    }
}

function Build-Gui {
    $framework = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319'
    $compiler = Join-Path $framework 'csc.exe'
    if (-not (Test-Path -LiteralPath $compiler -PathType Leaf)) {
        throw 'The .NET Framework 4.x x64 C# compiler is missing. Install .NET Framework 4.8 on Windows.'
    }
    $arguments = @('/nologo','/target:winexe','/platform:x64','/optimize+','/utf8output',
        ('/out:' + (Join-Path $sourceRoot 'PubKEYS-Pult.exe')),
        ('/resource:' + (Join-Path $sourceRoot 'gui\Pult.xaml') + ',PubKEYS.Pult.xaml'))
    foreach ($assembly in @('PresentationFramework','PresentationCore','WindowsBase','UIAutomationTypes')) {
        $assemblyPath = Join-Path $framework ('WPF\' + $assembly + '.dll')
        if (-not (Test-Path -LiteralPath $assemblyPath -PathType Leaf)) { throw ('Missing .NET Framework assembly: ' + $assembly) }
        $arguments += '/reference:' + $assemblyPath
    }
    $arguments += '/reference:' + (Join-Path $framework 'System.Xaml.dll')
    foreach ($source in @('Pult.cs','PultModel.cs','GpuTemperature.cs','MinuteMetrics.cs','SessionElapsed.cs','Profile.cs')) {
        $arguments += Join-Path $sourceRoot ('gui\' + $source)
    }
    Write-Host 'Compiling the Windows control panel...'
    Invoke-CheckedNative $compiler $arguments
}

function Build-Cuda {
    $nvcc = Get-Command nvcc.exe -CommandType Application -ErrorAction SilentlyContinue
    if (-not $nvcc -and $env:CUDA_PATH) {
        $candidate = Join-Path $env:CUDA_PATH 'bin\nvcc.exe'
        if (Test-Path -LiteralPath $candidate -PathType Leaf) { $nvcc = Get-Item -LiteralPath $candidate }
    }
    if (-not $nvcc) { throw 'CUDA Toolkit was not found. Install an NVIDIA CUDA Toolkit compatible with your GPU, driver and MSVC version.' }
    $nvccPath = $nvcc.Source
    if (-not $nvccPath) { $nvccPath = $nvcc.FullName }
    $toolkitRoot = Split-Path (Split-Path $nvccPath -Parent) -Parent
    if (-not (Test-Path -LiteralPath (Join-Path $toolkitRoot 'include\cuda_runtime_api.h') -PathType Leaf) -or
        -not (Test-Path -LiteralPath (Join-Path $toolkitRoot 'lib\x64\cudart.lib') -PathType Leaf)) {
        throw 'The selected nvcc is not inside a complete Windows x64 CUDA Toolkit.'
    }
    $env:CUDA_PATH = $toolkitRoot
    $env:Path = (Join-Path $toolkitRoot 'bin') + ';' + $env:Path
    # Compiler metadata only: never query a GPU or create a CUDA context.
    $virtual = @(& $nvccPath --list-gpu-arch | ForEach-Object { if ($_ -match '^\s*compute_([0-9]+[af]?)\s*$') { $matches[1] } })
    if ($LASTEXITCODE -ne 0 -or $virtual.Count -eq 0) { throw 'nvcc did not report supported virtual architectures.' }
    $machine = @(& $nvccPath --list-gpu-code | ForEach-Object { if ($_ -match '^\s*sm_([0-9]+[af]?)\s*$') { $matches[1] } })
    if ($LASTEXITCODE -ne 0 -or $machine.Count -eq 0) { throw 'nvcc did not report supported machine-code architectures.' }
    if ($CudaArchitectures) {
        $selected = @($CudaArchitectures -split '[,; ]+' | Where-Object { $_ })
        foreach ($architecture in $selected) {
            if ($architecture -notmatch '^[0-9]+[af]?$' -or $virtual -notcontains $architecture -or $machine -notcontains $architecture) {
                throw ('Architecture ' + $architecture + ' is not supported by the selected CUDA Toolkit. Use values printed by nvcc --list-gpu-arch.')
            }
        }
    } else { $selected = @($virtual | Where-Object { $machine -contains $_ }) }
    if ($selected.Count -eq 0) { throw 'No matching virtual/machine CUDA architectures were selected.' }
    $selected = @($selected | Sort-Object { [int]($_ -replace '[^0-9]','') },{ $_ } -Unique)
    $flags = @($selected | ForEach-Object { '-gencode arch=compute_{0},code=sm_{0}' -f $_ })
    $ptx = @($selected | Where-Object { $_ -match '^[0-9]+$' } | Select-Object -First 1)
    if ($ptx.Count) { $flags += '-gencode arch=compute_{0},code=compute_{0}' -f $ptx[0] }
    Write-Host ('Compiling CUDA architectures: ' + ($selected -join ', '))
    Invoke-CheckedNative 'nmake.exe' @('/NOLOGO','/A','/f','Makefile.win','cuda',
        'CUDA_EXE=pubkeyhunt_cuda_verified.exe',('GENCODE=' + ($flags -join ' ')))
}

try {
    Start-Transcript -LiteralPath $logFile | Out-Null
    $transcriptStarted = $true
    Push-Location -LiteralPath $sourceRoot
    try {
        if ($env:OS -ne 'Windows_NT') { throw 'This script requires Windows x64. Use the GNU Makefile for a Unix CPU build.' }
        if ($Target -ne 'gui') { Import-MsvcEnvironment }
        if ($Target -eq 'cpu' -or $Target -eq 'all') {
            Write-Host 'Compiling the CPU engine...'
            Invoke-CheckedNative 'nmake.exe' @('/NOLOGO','/A','/f','Makefile.win','pubkeyhunt.exe')
        }
        if ($Target -eq 'cuda' -or $Target -eq 'all') { Build-Cuda }
        if (-not $SkipGui -or $Target -eq 'gui') { Build-Gui }
        Write-Host 'Build completed. No search or GPU test was started.'
        Write-Host ('Build log: ' + $logFile)
    } finally { Pop-Location }
} catch {
    Write-Host ('BUILD FAILED: ' + $_.Exception.Message) -ForegroundColor Red
    Write-Host ('Build log: ' + $logFile)
    exit 1
} finally {
    if ($transcriptStarted) { Stop-Transcript | Out-Null }
}
