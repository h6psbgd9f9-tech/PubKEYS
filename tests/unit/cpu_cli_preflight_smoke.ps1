$ErrorActionPreference = 'Stop'
$taskBase = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$taskBuild = Join-Path $taskBase 'build\audit_fixes_20261026\cpu_cli_smoke'
$taskExe = Join-Path $taskBuild 'cpu_cli_smoke.exe'
if (-not (Test-Path -LiteralPath $taskExe)) { throw 'CPU-only application has not been built.' }
$taskDump = (Get-Command dumpbin.exe -ErrorAction Stop).Source
$taskImports = (& $taskDump /imports $taskExe | Out-String)
if ($LASTEXITCODE -ne 0) { throw 'Could not inspect CPU application imports.' }
if ($taskImports -match '(?i)(?:nvcuda|cudart|nvrtc|__cudaRegister|cudaLaunch|cuLaunchKernel)') {
    throw 'CPU smoke binary unexpectedly imports CUDA.'
}
if ($taskImports -match '(?i)\b(?:CreateMutex[AW]|OpenMutex[AW]|CreateEvent[AW]|OpenEvent[AW]|SetEvent|ResetEvent|WinHttpOpen|WinHttpConnect|CreateProcess[AW])\b') {
    throw 'Forbidden named IPC/process/network entry point bypassed the traps.'
}
$taskCases = @(
    @{args='-h'; expected=0},
    @{args='--help'; expected=0},
    @{args='--kangaroo'; expected=2},
    @{args='--kangaroo-gpu'; expected=2},
    @{args='--kangaroo-gpu-db'; expected=2},
    @{args='--kangaroo-test'; expected=2},
    @{args='--kangaroo-unknown'; expected=2},
    @{args='--help --kangaroo'; expected=2},
    @{args='--kangaroo-test --help'; expected=2},
    @{args='--check-notifications extra'; expected=2}
)
$taskScratch = Join-Path $taskBuild ('scratch_' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $taskScratch | Out-Null
$taskResults = @()
foreach ($taskCase in $taskCases) {
    $taskStart = New-Object Diagnostics.ProcessStartInfo
    $taskStart.FileName = $taskExe
    $taskStart.Arguments = $taskCase.args
    $taskStart.WorkingDirectory = $taskScratch
    $taskStart.UseShellExecute = $false
    $taskStart.CreateNoWindow = $true
    $taskStart.RedirectStandardOutput = $true
    $taskStart.RedirectStandardError = $true
    $taskStart.StandardOutputEncoding = [Text.Encoding]::UTF8
    $taskStart.StandardErrorEncoding = [Text.Encoding]::UTF8
    $taskStart.EnvironmentVariables['PUBKEYHUNT_TG_TOKEN'] = ''
    $taskStart.EnvironmentVariables['PUBKEYHUNT_TG_CHAT'] = ''
    $taskStart.EnvironmentVariables['PKH_KXE_SASS_DIR'] = ''
    $taskStart.EnvironmentVariables['PKH_RC_ENGINE'] = ''
    $taskProcess = [Diagnostics.Process]::Start($taskStart)
    $taskOutput = $taskProcess.StandardOutput.ReadToEnd()
    $taskError = $taskProcess.StandardError.ReadToEnd()
    $taskProcess.WaitForExit()
    if ($taskProcess.ExitCode -ne $taskCase.expected) {
        throw ('Unexpected exit for {0}: {1}; {2}' -f $taskCase.args, $taskProcess.ExitCode, $taskError)
    }
    if (($taskOutput + $taskError).Contains('PREFLIGHT_FORBIDDEN:')) {
        throw ('Forbidden OS operation attempted by ' + $taskCase.args)
    }
    if ($taskCase.expected -eq 0) {
        if (-not $taskOutput.Contains('PubkeyHunt') -or $taskOutput.Contains('--kangaroo')) {
            throw 'Help output does not match the retired-feature build.'
        }
        if ($taskError.Length -ne 0) { throw 'Help unexpectedly wrote stderr.' }
    } elseif ($taskOutput.Length -ne 0 -or $taskError.Length -eq 0) {
        throw 'Rejected command should produce only a clear stderr error.'
    }
    if (@(Get-ChildItem -LiteralPath $taskScratch -Force).Count -ne 0) {
        throw 'CLI preflight unexpectedly wrote files or directories.'
    }
    $taskResults += [pscustomobject]@{
        arguments=$taskCase.args; exit_code=$taskProcess.ExitCode;
        expected_output=$true; forbidden_os_operation=$false; scratch_unchanged=$true
    }
    $taskProcess.Dispose()
}
$taskSummary = [pscustomobject]@{
    scope='Actual CPU-linked application CLI preflight only; test-only forbidden-API traps enabled';
    source_main_sha256=(Get-FileHash -LiteralPath (Join-Path $taskBase 'main.cpp') -Algorithm SHA256).Hash;
    executable_sha256=(Get-FileHash -LiteralPath $taskExe -Algorithm SHA256).Hash;
    cuda_imports=$false; named_ipc_process_network_entrypoint_imports=$false;
    tests=$taskResults; scratch_unchanged=$true;
    not_executed=@('ordinary search','--test','notification checks','production executable','GPU or SASS tests')
}
$taskSummary | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $taskBuild 'cpu_cli_preflight_results.json') -Encoding UTF8
Write-Output ('Actual CPU application preflight smoke: {0}/{0} passed; no GPU imports, named IPC, process/network launch, or scratch writes.' -f $taskCases.Count)
