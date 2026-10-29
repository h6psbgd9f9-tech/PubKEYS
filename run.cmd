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
if not "%~1"=="" goto console
if not exist "%~dp0PubKEYS-Pult.exe" goto console
start "PubKEYS" "%~dp0PubKEYS-Pult.exe" --engine "%PKH_ENGINE%"
exit /b %errorlevel%
:console
"%PKH_ENGINE%" %*
exit /b %errorlevel%
