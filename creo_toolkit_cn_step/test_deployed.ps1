# test_deployed.ps1 - launch a Creo that has been deployed via deploy.ps1
# and check that the plugin registered through the GLOBAL config.pro loads.
#
# Working directory is a throw-away empty folder, so no <cwd>\protk.dat can
# interfere: this reproduces exactly what a user gets after a double-click of
# 一键部署.bat.
param(
    [Parameter(Mandatory=$true)][string]$Install,   # e.g. "8.0.12.0"
    [int]$WaitSeconds = 200
)

$ErrorActionPreference = "Continue"
$root = "C:\Program Files\PTC\Creo $Install"
$exe  = Join-Path $root "Parametric\bin\parametric.exe"
$log  = Join-Path $root "cc_luoluoo\cn_step_load.log"

if (-not (Test-Path $exe)) { Write-Output "MISSING $exe"; exit 2 }
if (-not (Test-Path (Join-Path $root "cc_luoluoo\protk.dat"))) {
    Write-Output "NOT DEPLOYED: $root\cc_luoluoo"; exit 2
}

$stamp = Get-Date -Format "yyyyMMdd_HHmmss"
if (Test-Path $log) {
    try { [System.IO.File]::Move($log, "$log.$stamp.old") } catch {}
}
$startTime = Get-Date

# neutral cwd - no protk.dat / config.pro here
$work = Join-Path $env:TEMP ("luoluo_test_" + ($Install -replace '[^\d]',''))
New-Item -ItemType Directory -Force -Path $work | Out-Null
Get-ChildItem -LiteralPath $work -Force -ErrorAction SilentlyContinue | ForEach-Object {
    try { [System.IO.File]::Move($_.FullName, "$($_.FullName).$stamp.old") } catch {}
}

Write-Output "=== Creo $Install ==="
Write-Output "cwd = $work"
$p = Start-Process -FilePath $exe -WorkingDirectory $work -PassThru
Write-Output "launched pid=$($p.Id)"

$deadline = (Get-Date).AddSeconds($WaitSeconds)
$found = $false
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 3
    if (Test-Path $log) {
        if ((Get-Item $log).LastWriteTime -gt $startTime) { $found = $true; break }
    }
}

if ($found) { Start-Sleep -Seconds 2 }
Write-Output "--- cc_luoluoo\cn_step_load.log ---"
if ($found) { Get-Content $log -Encoding UTF8 } else { Write-Output "(not created)" }

Get-Process xtop,parametric -ErrorAction SilentlyContinue | ForEach-Object { try { $_.Kill() } catch {} }
Start-Sleep -Seconds 6
Get-Process xtop,parametric -ErrorAction SilentlyContinue | ForEach-Object { try { $_.Kill() } catch {} }

if ($found) { exit 0 } else { exit 1 }
