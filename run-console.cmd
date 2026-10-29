@echo off
setlocal
cd /d "%~dp0"
set "PKH_ENGINE="
if exist "%~dp0pubkeyhunt.exe" set "PKH_ENGINE=%~dp0pubkeyhunt.exe"
if exist "%~dp0pubkeyhunt_cuda_verified.exe" set "PKH_ENGINE=%~dp0pubkeyhunt_cuda_verified.exe"
if not defined PKH_ENGINE (
  echo PubKEYS is not built. Run build.cmd cpu or build.cmd cuda first.
  exit /b 2
)
"%PKH_ENGINE%" %*
exit /b %errorlevel%
