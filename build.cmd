@echo off
setlocal
rem Compile only. This command never starts a search or a GPU test.
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\Build-Windows.ps1" %*
exit /b %errorlevel%
