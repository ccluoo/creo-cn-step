@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"

rem Build and run the Creo-independent algorithm self test.
rem Same toolchain discovery as build.bat, no Pro/TOOLKIT libs needed.

set "VSBASE=C:/Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
set "SDKBASE=C:/Program Files (x86)\Windows Kits\10"

set "MSVC="
for /d %%D in ("!VSBASE!\VC\Tools\MSVC\*") do set "MSVC=%%~fD"
set "SDKI="
for /d %%D in ("!SDKBASE!\Include\*") do set "SDKI=%%~fD"
set "SDKL="
for /d %%D in ("!SDKBASE!\Lib\*") do set "SDKL=%%~fD"

set "PATH=!MSVC!\bin\Hostx64\x64;!PATH!"
set "INCLUDE=!MSVC!\include;!SDKI!\ucrt;!SDKI!\um;!SDKI!\shared"
set "LIB=!MSVC!\lib\x64;!SDKL!\ucrt\x64;!SDKL!\um\x64"

if not exist "build" mkdir build

rem Grabbing a real Creo-exported assembly makes section [4] meaningful.
rem Only a COPY is touched; the original file is never modified.
set "REALSRC=%USERPROFILE%\Desktop\11.stp"
if exist "!REALSRC!" (
  copy /y "!REALSRC!" "build\selftest_real.stp" >nul
  echo [info] real fixture: copied from !REALSRC!
) else (
  echo [info] real fixture not found, section [4] will skip
)

echo [1/2] build selftest
cl /nologo /O2 /W3 /utf-8 /DCN_STEP_SELFTEST ^
   /Fo:build\selftest.obj ^
   /Fe:build\cn_step_selftest.exe ^
   src\cn_step.c
if errorlevel 1 (
  echo [FAILED] selftest build
  exit /b 1
)

echo.
echo [2/2] run selftest
echo ----------------------------------------
build\cn_step_selftest.exe
set "RC=!ERRORLEVEL!"
echo ----------------------------------------
echo exit code = !RC!
exit /b !RC!
