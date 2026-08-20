@echo off
rem regtable desktop build for Windows cmd, no make needed.
rem
rem   .\build            build example.exe and regtest.exe
rem   .\build run        build and start the interactive example
rem   .\build test       build and run the regression test
rem   .\build strict     same, with -Werror
rem   .\build clean
rem
rem Needs gcc (or clang: set CC=clang) on PATH.

setlocal
if "%CC%"=="" set CC=gcc
set CFLAGS=-std=c99 -Wall -Wextra -Wpedantic -O2 -Isrc
set LIB=src/regtable_core.c src/regtable_cli.c src/regtable_modbus.c
if "%1"=="strict" set CFLAGS=%CFLAGS% -Werror

if "%1"=="clean" (
    del /q example.exe regtest.exe mbtest.exe 2>nul
    exit /b 0
)

%CC% %CFLAGS% -o example.exe example_desktop.c %LIB% || exit /b 1
%CC% %CFLAGS% -o regtest.exe    regtable_test.c   %LIB% || exit /b 1
%CC% %CFLAGS% -o mbtest.exe     regtable_modbus_test.c %LIB% || exit /b 1

if "%1"=="run"  .\example.exe
if "%1"=="test"   .\regtest.exe && .\mbtest.exe
if "%1"=="strict" .\regtest.exe && .\mbtest.exe
exit /b %ERRORLEVEL%
