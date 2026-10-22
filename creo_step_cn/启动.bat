@echo off
chcp 65001 >nul
cd /d "%~dp0"

set "PYW="
if exist "C:\Python314\pythonw.exe" set "PYW=C:\Python314\pythonw.exe"
if not defined PYW (
  for /f "delims=" %%i in ('where pythonw 2^>nul') do (
    set "PYW=%%i"
    goto :run
  )
)
:run
if defined PYW (
  start "" "%PYW%" "creo_step_cn.py"
) else (
  echo 未找到 pythonw.exe，改用控制台模式启动...
  python "creo_step_cn.py"
  pause
)
