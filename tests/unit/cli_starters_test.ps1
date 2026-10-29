$ErrorActionPreference = 'Stop'
$taskBase = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$taskRoot = Join-Path $taskBase ('build\audit_fixes_20261026\starter_stub_' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $taskRoot | Out-Null
$taskPassed = 0
function Assert-Task([bool] $ok, [string] $label) {
    if (-not $ok) { throw ('FAIL ' + $label) }
    $script:taskPassed++
}
$taskStub = @'
@echo off
if not "%~1"=="%CLI_STUB_EXPECT_FIRST%" exit /b 93
if "%~1"=="--stub-sass" if defined PKH_KXE_SASS_DIR exit /b 95
:next_arg
if "%~1"=="" goto args_done
set "CLI_STUB_LAST=%~1"
shift
goto next_arg
:args_done
if not "%CLI_STUB_LAST%"=="sentinel with space" exit /b 94
echo SAFE_STUB_ONLY>"%CLI_STUB_MARKER%"
exit /b 7
'@
[IO.File]::WriteAllText((Join-Path $taskRoot 'Launch-PubKEYS.cmd'), $taskStub, [Text.Encoding]::ASCII)
$taskCases = @(
    @{name='Start_KXE_SASS.cmd'; first='--stub-sass'; args='--stub-sass "sentinel with space"'},
    @{name='start_wide_database.cmd'; disabled=$true; args='"sentinel with space"'},
    @{name='CHECK_NOTIFICATIONS.cmd'; first='--check-notifications'; args='"sentinel with space"'}
)
foreach ($taskCase in $taskCases) {
    $taskSource = Join-Path $taskBase $taskCase.name
    $taskText = [IO.File]::ReadAllText($taskSource)
    Assert-Task (-not $taskText.Contains('pubkeyhunt_cuda.exe')) ($taskCase.name + ' has no obsolete executable')
    if ($taskCase.disabled) {
        Assert-Task (-not ($taskText -match 'Launch-PubKEYS|\.exe')) ($taskCase.name + ' cannot launch removed feature or another mode')
        Assert-Task ($taskText.Contains('Kangaroo was removed.')) ($taskCase.name + ' explains removal')
    } else {
        Assert-Task ($taskText.Contains('call "%~dp0Launch-PubKEYS.cmd"')) ($taskCase.name + ' uses managed launcher')
        Assert-Task ($taskText.Contains('%*')) ($taskCase.name + ' forwards arguments')
    }
    $taskCopy = Join-Path $taskRoot $taskCase.name
    Copy-Item -LiteralPath $taskSource -Destination $taskCopy
    $taskMarker = Join-Path $taskRoot ($taskCase.name + '.stub-called')
    $taskStart = New-Object Diagnostics.ProcessStartInfo
    $taskStart.FileName = $env:ComSpec
    $taskStart.Arguments = '/d /c ""' + $taskCopy + '" ' + $taskCase.args + ' <nul"'
    $taskStart.WorkingDirectory = $taskRoot
    $taskStart.UseShellExecute = $false
    $taskStart.CreateNoWindow = $true
    $taskStart.RedirectStandardOutput = $true
    $taskStart.RedirectStandardError = $true
    $taskStart.EnvironmentVariables['CLI_STUB_EXPECT_FIRST'] = [string]$taskCase.first
    $taskStart.EnvironmentVariables['CLI_STUB_MARKER'] = $taskMarker
    $taskStart.EnvironmentVariables['PKH_KXE_SASS_DIR'] = 'ISOLATED_TEST_MUST_BE_CLEARED_BY_SASS_STARTER'
    $taskProcess = [Diagnostics.Process]::Start($taskStart)
    $taskOutput = $taskProcess.StandardOutput.ReadToEnd()
    $taskError = $taskProcess.StandardError.ReadToEnd()
    $taskProcess.WaitForExit()
    if ($taskCase.disabled) {
        Assert-Task ($taskProcess.ExitCode -eq 2) ($taskCase.name + ' returns removed feature status')
        Assert-Task (-not (Test-Path -LiteralPath $taskMarker)) ($taskCase.name + ' never invokes a launcher')
    } else {
        Assert-Task ($taskProcess.ExitCode -eq 7) ($taskCase.name + ' preserves stub launcher exit code and quoted arguments')
        Assert-Task ((Test-Path -LiteralPath $taskMarker) -and ([IO.File]::ReadAllText($taskMarker).Trim() -eq 'SAFE_STUB_ONLY')) ($taskCase.name + ' invokes only isolated launcher stub')
    }
    Assert-Task ($taskError.Length -eq 0) ($taskCase.name + ' has no command errors')
    $taskProcess.Dispose()
}
$taskMain = [IO.File]::ReadAllText((Join-Path $taskBase 'main.cpp'))
$taskEntry = $taskMain.Substring($taskMain.IndexOf('int main(int argc, char **argv)'))
Assert-Task ($taskEntry.IndexOf('if (cli_help_requested(argc, argv)) return print_command_line_help();') -lt $taskEntry.IndexOf('SingleInstanceGuard instance')) 'help returns before named IPC guard'
Assert-Task ($taskEntry.IndexOf('if (cli_help_requested(argc, argv)) return print_command_line_help();') -lt $taskEntry.IndexOf('FoundWorkExitDrain')) 'help bypasses exit-drain work'
Assert-Task ($taskEntry.IndexOf('--check-notifications') -lt $taskEntry.IndexOf('SingleInstanceGuard instance')) 'notification diagnostic precedes run guard'
Assert-Task ($taskEntry.Contains('if (argc != 2)')) 'extra notification parameters fail before application startup'
Assert-Task ($taskEntry.IndexOf('if (cli_removed_kangaroo_requested(argc, argv))') -lt $taskEntry.IndexOf('if (cli_help_requested')) 'removed commands reject before help or initialization'
Assert-Task (-not ($taskMain -match '#include "kangaroo.h"|kangaroo_cli\(|kangaroo_menu\(')) 'main has no removed feature entry point or implementation include'
$taskHelpStart = $taskMain.IndexOf('static int print_command_line_help()')
$taskHelpEnd = $taskMain.IndexOf('static int application_main', $taskHelpStart)
$taskHelp = $taskMain.Substring($taskHelpStart, $taskHelpEnd - $taskHelpStart)
Assert-Task (-not ($taskHelp -match 'gpu_backend|build_table|load_config_ini|SingleInstance|tg_send|signal_check|chdir_to_program')) 'help body has no compute, IPC, config, network or working-directory setup'
Assert-Task (-not ($taskHelp -match 'kangaroo|кенгуру')) 'help does not offer removed commands'
$taskUi = [IO.File]::ReadAllText((Join-Path $taskBase 'ui.cpp'))
Assert-Task (-not ($taskUi -match 'g_want_kangaroo|startup_mode|Кенгуру')) 'startup menu offers only the database search'
$taskPreviousUi = [IO.File]::ReadAllText((Join-Path $taskBase 'build\audit_fixes_20261026\clibackup\before_kangaroo_removal_ui.cpp'))
$taskValidationLine = [regex]::Match($taskPreviousUi, '(?m)^.*CPU-.*GPU-.*$').Value
Assert-Task ($taskValidationLine.Length -gt 0 -and $taskUi.Contains($taskValidationLine)) 'honest build validation label is retained byte-for-byte'
$taskGuard = [IO.File]::ReadAllText((Join-Path $taskBase 'single_instance.h'))
Assert-Task (-not $taskGuard.Contains('SetEvent(request_)')) 'new guard cannot signal another instance stop'
Assert-Task ($taskGuard.IndexOf('error_ = ERROR_ALREADY_EXISTS') -lt $taskGuard.IndexOf('request_ = CreateEventW')) 'busy guard does not open or modify legacy stop event'
Write-Output ('Isolated starter/source checks: {0} passed, 0 failed; no production executable or real notification transport launched.' -f $taskPassed)
