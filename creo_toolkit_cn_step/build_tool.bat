@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"

rem ============================================================
rem  Build the STANDALONE tool: step_cn.exe
rem
rem  No Creo, no Pro/TOOLKIT, no VC runtime on the target PC:
rem    - only needs MSVC (v143) + Windows SDK to BUILD
rem    - links the C runtime statically (/MT) so the .exe is
rem      self-contained and runs on any Windows 10/11 x64
rem
rem  Unlike cn_step.dll (which is bound to one Creo major version
rem  because it links protk_dll_NU.lib), this tool has no Creo
rem  dependency at all.
rem ============================================================

rem ---- locate MSVC ----
set "VSBASE="
for %%P in (
  "C:/Program Files/Microsoft Visual Studio/2022/BuildTools"
  "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools"
  "C:/Program Files/Microsoft Visual Studio/2022/Community"
  "C:/Program Files/Microsoft Visual Studio/2022/Professional"
  "C:/Program Files/Microsoft Visual Studio/2022/Enterprise"
  "C:/Program Files (x86)/Microsoft Visual Studio/2022/Community"
) do (
  if exist "%%~fP\VC\Tools\MSVC" set "VSBASE=%%~fP"
)
if not defined VSBASE (
  echo [ERROR] Visual Studio 2022 C++ tools not found.
  exit /b 1
)

rem ---- locate Windows SDK ----
set "SDKBASE="
for %%P in (
  "C:/Program Files (x86)/Windows Kits/10"
  "C:/Program Files/Windows Kits/10"
) do (
  if exist "%%~fP\Include" set "SDKBASE=%%~fP"
)
if not defined SDKBASE (
  echo [ERROR] Windows SDK not found.
  exit /b 1
)

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

echo [1/2] compile step_cn.exe (static CRT)
cl /nologo /O2 /W3 /GS /fp:precise /utf-8 /MT ^
   /DCN_STEP_TOOL /D_CRT_SECURE_NO_WARNINGS ^
   /Fo:build\step_cn.obj ^
   /Fe:build\step_cn.exe ^
   src\cn_step.c ^
   /link kernel32.lib user32.lib
if errorlevel 1 (
  echo.
  echo [FAILED] compile stage.
  exit /b 1
)

echo [2/2] smoke test
build\step_cn.exe --help >nul
if errorlevel 2 (
  echo [WARN] --help returned unexpected code
)

echo.
echo [OK] build\step_cn.exe
dir /b build\step_cn.exe
exit /b 0
