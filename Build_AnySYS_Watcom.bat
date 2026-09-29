@echo off
rem SPDX-License-Identifier: GPL-2.0-or-later
rem Build AnySYS as a 16-bit DOS EXE with Open Watcom / Watcom C.

set OWCL=wcl
where wcl >nul 2>nul
if not errorlevel 1 goto have_wcl

if not "%WATCOM%"=="" if exist "%WATCOM%\binnt64\wcl.exe" set OWCL=%WATCOM%\binnt64\wcl.exe
if not "%WATCOM%"=="" if exist "%WATCOM%\binnt\wcl.exe" set OWCL=%WATCOM%\binnt\wcl.exe
if not "%WATCOM%"=="" if exist "%WATCOM%\binw\wcl.exe" set OWCL=%WATCOM%\binw\wcl.exe

if "%OWCL%"=="wcl" goto no_wcl

:have_wcl
if not "%WATCOM%"=="" if "%INCLUDE%"=="" set INCLUDE=%WATCOM%\h
if not "%WATCOM%"=="" if "%LIB%"=="" set LIB=%WATCOM%\lib286\dos;%WATCOM%\lib286

if "%1"=="make" goto use_wmake

"%OWCL%" -bt=dos -ms -os -s -w4 -zq -fe=AnySYS.exe src\anysys.c
goto end

:use_wmake
wmake -f makefile.wat
goto end

:no_wcl
echo Open Watcom wcl.exe was not found.
echo Add Open Watcom to PATH or set WATCOM, for example: set WATCOM=C:\WATCOM
exit /b 1

:end
