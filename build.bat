@echo off
rem Build Pi-CMD on Windows.
rem
rem   build.bat            incremental build for a Pi 3 (RASPPI=3)
rem   build.bat 2          incremental build for a Pi 2
rem   build.bat 3 clean    full rebuild
rem
rem This sets up the environment itself rather than relying on PATH, because
rem two things need to be right and neither is obvious:
rem
rem  * make must be MSYS2's. A GnuWin32 make will get part way and then fail
rem    on the recursive sub-make for uspi, because its own path contains
rem    spaces and brackets ("C:\Program Files (x86)\...") which the shell it
rem    invokes cannot parse. The error mentions a syntax error near '(' and
rem    looks nothing like a toolchain problem.
rem
rem  * arm-none-eabi-gcc has to be findable. It is usually not on PATH.

setlocal enabledelayedexpansion

set RASPPI=%1
if "%RASPPI%"=="" set RASPPI=3

rem ---------------------------------------------------------------------------
rem Locate the ARM cross compiler. ARM_TOOLCHAIN_BIN overrides the search.
rem ---------------------------------------------------------------------------
set ARMBIN=
if defined ARM_TOOLCHAIN_BIN if exist "%ARM_TOOLCHAIN_BIN%\arm-none-eabi-gcc.exe" set ARMBIN=%ARM_TOOLCHAIN_BIN%

if not defined ARMBIN (
  for %%D in (
    "C:\tmp\armtc\bin"
    "C:\Tools\arm-gnu-toolchain\bin"
    "C:\arm-gnu-toolchain\bin"
    "C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\13.3 rel1\bin"
    "C:\SysGCC\arm-eabi\bin"
  ) do (
    if not defined ARMBIN if exist "%%~D\arm-none-eabi-gcc.exe" set ARMBIN=%%~D
  )
)

if not defined ARMBIN (
  where arm-none-eabi-gcc >nul 2>nul && set ARMBIN=__ON_PATH__
)

if not defined ARMBIN (
  echo.
  echo ERROR: arm-none-eabi-gcc not found.
  echo.
  echo Install the Arm GNU Toolchain for arm-none-eabi ^(10.2 or newer^), from
  echo   https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads
  echo pick the "mingw-w64-i686" Windows host build for arm-none-eabi.
  echo.
  echo Then either add its bin directory to PATH, or set ARM_TOOLCHAIN_BIN:
  echo   set ARM_TOOLCHAIN_BIN=C:\Tools\arm-gnu-toolchain\bin
  echo.
  echo Note: unpack it somewhere permanent. Long paths have caused trouble
  echo here - a deeply nested location made the compiler fail to find its own
  echo sys/errno.h.
  exit /b 1
)

rem ---------------------------------------------------------------------------
rem Locate MSYS2's make
rem ---------------------------------------------------------------------------
set MAKEEXE=
for %%M in (
  "C:\msys64\usr\bin\make.exe"
  "C:\msys32\usr\bin\make.exe"
) do (
  if not defined MAKEEXE if exist %%M set MAKEEXE=%%~M
)

if not defined MAKEEXE (
  echo.
  echo ERROR: MSYS2's make not found ^(expected C:\msys64\usr\bin\make.exe^).
  echo Install MSYS2 from https://www.msys2.org/ then: pacman -S make
  echo.
  echo A GnuWin32 make will not work: the recursive build of uspi fails
  echo because its own install path contains spaces and brackets.
  exit /b 1
)

if not "%ARMBIN%"=="__ON_PATH__" set PATH=%ARMBIN%;%PATH%
set PATH=C:\msys64\usr\bin;%PATH%

echo make        : %MAKEEXE%
for /f "delims=" %%V in ('arm-none-eabi-gcc -dumpversion') do echo arm gcc     : %%V
echo target      : RASPPI=%RASPPI%

rem ---------------------------------------------------------------------------
rem Build. Incremental by default.
rem
rem The Makefile tracks header dependencies now (-MMD -MP), so editing a header
rem rebuilds everything that includes it and nothing else. This used to be a
rem clean build every time because it could not: a lot of this emulator lives in
rem headers - iec_bus.h is inlined into main.o - and an incremental build after
rem editing one produced a kernel with some of the change in it and not the
rem rest, which on real hardware looks exactly like a bug in the change.
rem
rem   build.bat 3          incremental
rem   build.bat 3 clean    full rebuild
rem ---------------------------------------------------------------------------
if /i "%2"=="clean" (
  echo mode        : clean
  "%MAKEEXE%" clean >nul 2>nul
) else (
  echo mode        : incremental
)
echo.

"%MAKEEXE%" RASPPI=%RASPPI%
if errorlevel 1 (
  echo.
  echo BUILD FAILED
  exit /b 1
)

if not exist target\kernel.img (
  echo.
  echo BUILD FAILED - no target\kernel.img produced
  exit /b 1
)

echo.
for %%F in (target\kernel.img) do echo Built target\kernel.img  %%~zF bytes
echo.
echo Copy target\kernel.img to the root of the SD card. The other files it
echo needs are in firmware\3b\ - see firmware\3b\README.md.
endlocal
