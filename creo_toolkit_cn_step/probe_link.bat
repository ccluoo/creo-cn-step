@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"

rem  Probe link for the Creo 2 SDK: same objects, but
rem    - msvcp100 import lib MOVED AFTER protk_dll.lib  (so the VS2010 STL
rem      symbols resolve from msvcp100.dll instead of the VS2022 libcpmt)
rem    - /NODEFAULTLIB:libcpmt so it is not auto-pulled at all
rem  Prints the remaining unresolved symbols so we know what to stub.

set "CREO=C:\Program Files\PTC\Creo 2.0"
set "VSBASE=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
set "SDKBASE=C:\Program Files (x86)\Windows Kits\10"

set "PTK="
for /d %%M in ("!CREO!\Common Files\*") do (
  if not defined PTK if exist "%%~fM\protoolkit\includes\ProToolkit.h" set "PTK=%%~fM\protoolkit"
)
if not defined PTK set "PTK=!CREO!\Common Files\protoolkit"
set "OBJ=!PTK!\x86e_win64\obj"

set "MSVC="
for /d %%D in ("!VSBASE!\VC\Tools\MSVC\*") do set "MSVC=%%~fD"
set "SDKI="
for /d %%D in ("!SDKBASE!\Include\*") do set "SDKI=%%~fD"
set "SDKL="
for /d %%D in ("!SDKBASE!\Lib\*") do set "SDKL=%%~fD"

set "PATH=!MSVC!\bin\Hostx64\x64;!PATH!"
set "INCLUDE=!MSVC!\include;!SDKI!\ucrt;!SDKI!\um;!SDKI!\shared"
set "LIB=!CD!\build\c2_crt;!MSVC!\lib\x64;!SDKL!\ucrt\x64;!SDKL!\um\x64"

echo PTK = !PTK!
echo OBJ = !OBJ!

link /nologo /dll /machine:amd64 /debug:none ^
   /NODEFAULTLIB:libcpmt.lib ^
   /alternatename:?_open@@YAHPEBDHH@Z=_open ^
   /alternatename:?_sopen@@YAHPEBDHHH@Z=_sopen ^
   /alternatename:??0exception@std@@QEAA@AEBQEBD@Z=c2_exc_ctor_ref ^
   /alternatename:?_Facet_Register@facet@locale@std@@CAXPEAV123@@Z=c2_facet_register ^
   /alternatename:??0bad_cast@std@@QEAA@PEBD@Z=c2_badcast_ctor ^
   /alternatename:??9type_info@@QEBA_NAEBV0@@Z=c2_tieq_ne ^
   /map:build\probe.map ^
   /out:build\probe_c2.dll ^
   /def:src\cn_step.def ^
   build\cn_step_c2.obj ^
   "!OBJ!\protk_dll.lib" ^
   build\c2_crt\msvcp100.lib ^
   legacy_stdio_definitions.lib ^
   build\c2_shim.obj ^
   kernel32.lib user32.lib wsock32.lib advapi32.lib mpr.lib ^
   winspool.lib netapi32.lib psapi.lib gdi32.lib shell32.lib ^
   comdlg32.lib ole32.lib ws2_32.lib > build\probe_link.log 2>&1
echo EXIT=%ERRORLEVEL%
exit /b %ERRORLEVEL%
