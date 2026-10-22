@echo off
chcp 936 >nul 2>&1
setlocal
cd /d "%~dp0"

rem ============================================================
rem  落落工具（cn_step）一键部署
rem
rem  双击本文件即可。做的事：
rem    1. 自动扫描本机所有 Creo 版本（Creo 2.0 ~ Creo 13.x）
rem    2. 在每个 Creo 根目录下新建 cc_luoluoo\，把插件文件复制进去
rem    3. 为该版本注入最高优先级的 config.pro，指向 cc_luoluoo
rem
rem  写 Program Files 需要管理员权限，脚本会自己请求提权。
rem  重复运行安全：旧注册行会被替换，不会重复注入。
rem ============================================================

rem ---- 需要管理员：写 Program Files 下的 config.pro ----
fltmc >nul 2>&1
if errorlevel 1 (
  echo.
  echo 需要管理员权限来写入 Creo 安装目录，正在请求提权...
  powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
  exit /b 0
)

echo.
echo ============================================================
echo   落落工具 · 一键部署
echo ============================================================

where powershell >nul 2>&1
if errorlevel 1 (
  echo [ERROR] 找不到 powershell，无法继续。
  pause
  exit /b 1
)

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0deploy.ps1" %*
set "RC=%ERRORLEVEL%"

echo.
if not "%RC%"=="0" (
  echo [失败] 部署没有完成，返回码 %RC%。
  echo        上面的输出里有具体原因。
) else (
  echo [成功] 部署完成。打开 Creo 就能在「工具」菜单里看到「落落工具」。
)
echo.
pause
exit /b %RC%
