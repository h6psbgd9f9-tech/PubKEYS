@echo off
rem ===========================================================================
rem  gen_gencode.bat -- список архитектур видеокарт СПРАШИВАЕТСЯ У nvcc.
rem
rem  То же, что делает GNU Makefile целью gpu-arch, но для nmake: он не умеет
rem  ни $(shell ...), ни $(foreach ...), поэтому список берётся здесь и
rem  кладётся в gencode.inc, который Makefile.win подключает.
rem
rem  Смысл: под каждую известную этому CUDA Toolkit архитектуру собирается
rem  машинный код, плюс PTX самой ранней -- промежуточное представление, из
rem  которого драйвер сам соберёт код под карту, вышедшую ПОСЛЕ этой сборки.
rem  Обновили CUDA -- набор расширился, править ничего не надо.
rem
rem  Запускать перед  nmake /f Makefile.win cuda
rem ===========================================================================
setlocal enabledelayedexpansion

where nvcc >nul 2>nul
if errorlevel 1 (
    echo nvcc не найден. Откройте окно с окружением Visual Studio,
    echo либо проверьте, что CUDA Toolkit установлен.
    exit /b 1
)

set "FLAGS="
set "PTX_ARCH="
for /f "usebackq tokens=*" %%a in (`nvcc --list-gpu-arch`) do (
    set "A=%%a"
    set "A=!A:compute_=!"
    set "FLAGS=!FLAGS! -gencode arch=compute_!A!,code=sm_!A!"
    if not defined PTX_ARCH set "PTX_ARCH=!A!"
)

if "!PTX_ARCH!"=="" (
    echo nvcc не сообщил ни одной архитектуры.
    exit /b 1
)

rem PTX нижней архитектуры подходит и картам без своего native cubin.
rem Драйвер всё равно должен понимать версию PTX установленного Toolkit.
set "FLAGS=!FLAGS! -gencode arch=compute_!PTX_ARCH!,code=compute_!PTX_ARCH!"

> gencode.inc echo GENCODE =!FLAGS!
echo Архитектуры от nvcc записаны в gencode.inc:
type gencode.inc
endlocal
