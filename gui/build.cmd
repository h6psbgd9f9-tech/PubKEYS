@echo off
setlocal
rem Portable source checkout: put the panel beside the engines.
call "%~dp0..\build.cmd" gui
exit /b %errorlevel%
