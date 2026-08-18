@echo off
rem regtable desktop build for Windows cmd, no make needed.
rem
rem   .\build            build example.exe and test.exe
rem   .\build run        build and start the interactive example
rem   .\build test       build and run the regression test
rem   .\build clean
rem
rem Needs gcc (or clang: set CC=clang) on PATH.

setlocal
if "%CC%"=="" set CC=gcc
set CFLAGS=-std=c99 -Wall -Wextra -Wpedantic -O2
set LIB=regtable_core.c regtable_cli.c

if "%1"=="clean" (
    del /q example.exe test.exe 2>nul
    exit /b 0
)

%CC% %CFLAGS% -o example.exe example_desktop.c %LIB% || exit /b 1
%CC% %CFLAGS% -o test.exe    regtable_test.c   %LIB% || exit /b 1

if "%1"=="run"  .\example.exe
if "%1"=="test" .\test.exe
exit /b %ERRORLEVEL%
