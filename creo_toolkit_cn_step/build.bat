@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"

rem ============================================================
rem  Build cn_step.dll against a chosen Creo Pro/TOOLKIT SDK.
rem
rem    build.bat        -> newest Creo installed, dist\
rem    build.bat 8      -> Creo 8.x only,  dist_c8\cn_step_c8.dll
rem    build.bat 12     -> Creo 12.x only, dist_c12\cn_step_c12.dll
rem    build.bat 13     -> Creo 13.x only, dist_c13\cn_step_c13.dll
rem
rem  VERSION RULE (measured on Creo 8/9/10/11/12/13):
rem    a DLL loads only in a Creo whose version is >= the SDK it was
rem    built with.  A Creo 8 build therefore runs on 8..13; a Creo 13
rem    build runs on 13 only, and Creo rejects the mismatch with
rem    user_initialize status -98 ("requires Creo 13; current is 12").
rem    => build.bat 8 produces the one-DLL-for-everything build.
rem
rem  Flags mirror PTC's official makefile (x86e_win64/obj/make_basic):
rem     -DPRO_MACHINE=36 -DPRO_OS=4 -D_WSTDIO_DEFINED
rem     link: protk_dll_NU.lib + ucore.lib + udata.lib
rem  INCLUDE/LIB are assembled by hand (vcvars64.bat calls reg.exe,
rem  which some restricted shells block).
rem  Inside ( ) blocks always use !VAR!, never %VAR%.
rem ============================================================

set "VER=%~1"

rem ---- locate Creo ----
set "CREO="
if defined CN_STEP_CREO (
  call :try_creo "!CN_STEP_CREO!"
)
rem  Wildcard-in-the-set forms ("Creo !VER!*") blow up in cmd when the
rem  pattern contains a space, so enumerate the plain parent folder and
rem  match the leaf name ourselves.  Leaf looks like "Creo 9.0.10.0" or
rem  "Creo 13.4.1.0" - VER is 1 or 2 digits - so compare "N." at both
rem  widths instead of a fixed-length substring.
if not defined CREO (
  for /d %%D in ("C:\Program Files\PTC\*") do (
    set "LEAF=%%~nxD"
    if "!LEAF:~0,5!"=="Creo " (
      if not defined VER (
        call :try_creo "%%~fD"
      ) else (
        if "!LEAF:~5,2!"=="!VER!." call :try_creo "%%~fD"
        if "!LEAF:~5,3!"=="!VER!." call :try_creo "%%~fD"
      )
    )
  )
)
if not defined CREO (
  echo [ERROR] no Creo installation with Pro/TOOLKIT found.
  echo         Try: build.bat 12   ^(or set CN_STEP_CREO=D:/PTC/Creo 12.4.1.0^)
  exit /b 1
)

rem ---- output names ----
set "SUF="
set "DIST=dist"
if defined VER (
  set "SUF=_c!VER!"
  set "DIST=dist_c!VER!"
)

rem ---- locate MSVC: BuildTools / Community / Professional all work ----
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

set "MACHINE=x86e_win64"
set "TKREV=18"

set "PTK="
rem  Older Creo (2.0) keeps the SDK under "Common Files\M250\protoolkit",
rem  newer ones under "Common Files\protoolkit" - probe every subdir.
for /d %%M in ("!CREO!\Common Files\*") do (
  if not defined PTK if exist "%%~fM\protoolkit\includes\ProToolkit.h" set "PTK=%%~fM\protoolkit"
)
if not defined PTK set "PTK=!CREO!\Common Files\protoolkit"
set "INC=!PTK!\includes"
set "OBJ=!PTK!\!MACHINE!\obj"

set "MSVC="
for /d %%D in ("!VSBASE!\VC\Tools\MSVC\*") do set "MSVC=%%~fD"
set "SDKI="
for /d %%D in ("!SDKBASE!\Include\*") do set "SDKI=%%~fD"
set "SDKL="
for /d %%D in ("!SDKBASE!\Lib\*") do set "SDKL=%%~fD"

set "PATH=!MSVC!\bin\Hostx64\x64;!PATH!"
set "INCLUDE=!MSVC!\include;!SDKI!\ucrt;!SDKI!\um;!SDKI!\shared;!SDKI!\winrt;!SDKI!\cppwinrt"
set "LIB=!MSVC!\lib\x64;!SDKL!\ucrt\x64;!SDKL!\um\x64"

if not exist "!INC!\ProToolkit.h" (
  echo [ERROR] Pro/TOOLKIT includes not found: !INC!
  exit /b 1
)
rem  Creo 3 and newer ship the split libs (protk_dll_NU + ucore + udata);
rem  Creo 2 ships a single monolithic protk_dll.lib - pick what exists.
set "PTCLIBS="
set "OLDSDK="
if exist "!OBJ!\protk_dll_NU.lib" (
  rem  Creo 3+: split libs; embed the quotes so the link line survives
  rem  paths with spaces ("Program Files").
  set "PTCLIBS=^"!OBJ!\protk_dll_NU.lib^" ^"!OBJ!\ucore.lib^" ^"!OBJ!\udata.lib^""
) else if exist "!OBJ!\protk_dll.lib" (
  rem  Creo 2: one monolithic unicode lib, plus the old-API compile flag
  set "PTCLIBS=^"!OBJ!\protk_dll.lib^""
  set "OLDSDK=/DCN_STEP_OLD_SDK"
)
if not defined PTCLIBS (
  echo [ERROR] Pro/TOOLKIT lib not found in !OBJ!
  exit /b 1
)

rem  ------------------------------------------------------------------
rem  Creo 2 (VS2010 SDK) linking strategy - the whole trick is HERE.
rem
rem  protk_dll.lib carries  /DEFAULTLIB:"libcpmt"  (it was built /MT), so
rem  every C++ STL symbol it needs used to be satisfied by the VS2022 copy
rem  of libcpmt.  That pulled ~530 VS2022 STL objects into the DLL, and
rem  their dynamic initialisers (??__Eclassic_locale / ??__Ecout /
rem  ??__Einitlocks ...) ran against VS2010 STL data at load time ->
rem  DllMain returned FALSE -> ERROR_DLL_INIT_FAILED (1114).
rem
rem  Fix: don't use libcpmt at all.
rem    /NODEFAULTLIB:libcpmt.lib      kill the auto-pull
rem    msvcp100.lib AFTER protk_dll   so the 136 STL symbols protk needs
rem                                    are imported from msvcp100.dll (the
rem                                    VS2010 C++ runtime Creo 2 itself
rem                                    loads), matching its ABI exactly
rem  That leaves only 5 symbols, all handled by src\c2_shim.cpp.
rem  ------------------------------------------------------------------
set "OLDLINK="
if defined OLDSDK set "OLDLINK=/NODEFAULTLIB:libcpmt.lib /alternatename:?_open@@YAHPEBDHH@Z=_open /alternatename:?_sopen@@YAHPEBDHHH@Z=_sopen /alternatename:??0exception@std@@QEAA@AEBQEBD@Z=c2_exc_ctor_ref /alternatename:?_Facet_Register@facet@locale@std@@CAXPEAV123@@Z=c2_facet_register /alternatename:??0bad_cast@std@@QEAA@PEBD@Z=c2_badcast_ctor /alternatename:??9type_info@@QEBA_NAEBV0@@Z=c2_tieq_ne"
rem  lib.exe turns the .def (all 1676 exports of msvcp100.dll) into a real
rem  import library.  It MUST come after !PTCLIBS! on the link line: LINK
rem  resolves libraries left to right, so only protk_dll.lib introducing
rem  the undefined STL symbols can make msvcp100 win over libcpmt.
set "OLDLINKDLLS="
if defined OLDSDK (
  if not exist "build\c2_crt" mkdir "build\c2_crt"
  lib /nologo /def:src\c2_msvcp100.def /machine:x64 /out:build\c2_crt\msvcp100.lib
  if errorlevel 1 (
    echo [ERROR] could not build the msvcp100 import library
    exit /b 1
  )
  set "OLDLINKDLLS=build\c2_crt\msvcp100.lib"
)
set "SHIMOBJ="
if defined OLDSDK set "SHIMOBJ=build\c2_shim.obj"

echo [1/3] toolset
echo        CREO    = !CREO!
echo        MSVC    = !MSVC!
echo        TOOLKIT = !INC!

if not exist "build" mkdir build

echo [2/3] compile cn_step.c
cl /nologo /c /O2 /W3 /GS /fp:precise /utf-8 ^
   /D_WSTDIO_DEFINED /DPRO_MACHINE=36 /DPRO_OS=4 ^
   /D_CRT_SECURE_NO_WARNINGS /DPRO_USE_VAR_ARGS !OLDSDK! ^
   /I"!INC!" ^
   /Fo:build\cn_step!SUF!.obj ^
   src\cn_step.c
if errorlevel 1 (
  echo.
  echo [FAILED] compile stage.
  exit /b 1
)

rem  CRT compatibility shims for the VS2010-built Creo 2 SDK
if defined OLDSDK (
  cl /nologo /c /O2 /W3 /EHsc /utf-8 ^
     /Fo:build\c2_shim.obj ^
     src\c2_shim.cpp
  if errorlevel 1 (
    echo.
    echo [FAILED] c2 shim compile.
    exit /b 1
  )
  rem  stamp the shim's _MSC_VER directive down to 1600 so it agrees
  rem  with the VS2010 objects inside protk_dll.lib
  powershell -NoProfile -Command "$e=[Text.Encoding]::GetEncoding(28591); $p='!CD!\build\c2_shim.obj'; $s=$e.GetString([IO.File]::ReadAllBytes($p)); $s=[regex]::Replace($s,'_MSC_VER=\d{4}','_MSC_VER=1600'); [IO.File]::WriteAllBytes($p,$e.GetBytes($s))"
  if errorlevel 1 (
    echo [ERROR] could not patch c2_shim.obj
    exit /b 1
  )
)

echo [3/3] link cn_step!SUF!.dll
rem  ORDER MATTERS: obj -> protk_dll -> msvcp100 (creo 2 only) -> the rest.
link /nologo /dll /machine:amd64 /debug:none /map:build\cn_step!SUF!.map !OLDLINK! ^
   /out:build\cn_step!SUF!.dll ^
   /def:src\cn_step.def ^
   build\cn_step!SUF!.obj ^
   !PTCLIBS! ^
   !OLDLINKDLLS! ^
   legacy_stdio_definitions.lib ^
   !SHIMOBJ! ^
   kernel32.lib user32.lib wsock32.lib advapi32.lib mpr.lib ^
   winspool.lib netapi32.lib psapi.lib gdi32.lib shell32.lib ^
   comdlg32.lib ole32.lib ws2_32.lib
if errorlevel 1 (
  echo.
  echo [FAILED] link stage.
  exit /b 1
)

echo.
echo === staging !DIST! ===
set "DSTDIR=%~dp0!DIST!"
if not exist "!DIST!" mkdir "!DIST!"
if not exist "!DIST!\text\chinese_cn" mkdir "!DIST!\text\chinese_cn"
if not exist "!DIST!\text\usascii"   mkdir "!DIST!\text\usascii"

copy /y "build\cn_step!SUF!.dll" "!DIST!\" >nul
fc /b "build\cn_step!SUF!.dll" "!DIST!\cn_step!SUF!.dll" >nul 2>&1
if errorlevel 1 (
  echo.
  echo [WARN] !DIST!\cn_step!SUF!.dll could not be updated - it is locked.
  echo        Creo is probably running. Close Creo, then run this again.
  echo        The fresh binary is already built at build\cn_step!SUF!.dll
)
copy /y "text\chinese_cn\msg_cn_step.txt" "!DIST!\text\chinese_cn\" >nul
copy /y "text\usascii\msg_cn_step.txt"   "!DIST!\text\usascii\" >nul
copy /y "cn_step.cfg" "!DIST!\" >nul
rem stage the name-mapping sample; wildcard avoids non-ASCII in this .bat
copy /y *.csv "!DIST!\" >nul

rem ---- generate protk.dat with real absolute paths ----
> "!DIST!\protk.dat" echo NAME       cn_step
>>"!DIST!\protk.dat" echo EXEC_FILE  !DSTDIR!\cn_step!SUF!.dll
>>"!DIST!\protk.dat" echo TEXT_DIR   !DSTDIR!\text
>>"!DIST!\protk.dat" echo STARTUP    dll
>>"!DIST!\protk.dat" echo REVISION   !TKREV!
>>"!DIST!\protk.dat" echo ALLOW_STOP TRUE
>>"!DIST!\protk.dat" echo END

echo.
echo [OK] build\cn_step!SUF!.dll
echo.
echo --- !DIST!\protk.dat ---
type "!DIST!\protk.dat"
exit /b 0

:try_creo
rem  %1 = candidate Creo root; accept it only if Pro/TOOLKIT headers are
rem  there.  The SDK sits in "Common Files\protoolkit" on newer versions
rem  and "Common Files\M250\protoolkit" (datecode subdir) on Creo 2.
if exist "%~1\Common Files\protoolkit\includes\ProToolkit.h" set "CREO=%~1"
if defined CREO goto :eof
for /d %%M in ("%~1\Common Files\*") do (
  if exist "%%~fM\protoolkit\includes\ProToolkit.h" set "CREO=%~1"
)
goto :eof
