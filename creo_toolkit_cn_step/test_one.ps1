# test_one.ps1 - launch one Creo version headless, see whether the plugin loads
param(
    [Parameter(Mandatory=$true)][string]$Version,   # e.g. 8
    [Parameter(Mandatory=$true)][string]$Install,   # e.g. 8.0.12.0
    [int]$WaitSeconds = 200,
    [string]$DllVersion = "",   # empty = same as $Version; set to test a cross build
    [switch]$Headless
)

$ErrorActionPreference = "Continue"
$here = "C:\Users\Administrator\WorkBuddy\2026-10-20-14-05-22\creo_toolkit_cn_step"
$dist = Join-Path $here ("dist_c" + $Version)
$exe  = "C:\Program Files\PTC\Creo $Install\Parametric\bin\parametric.exe"
$log  = Join-Path $dist "cn_step_load.log"

# Not Remove-Item: a safe-delete hook in this environment blocks it.
# Also avoid [IO.File]::Move(src,dst,$true) - that 3-arg overload is .NET
# Core only, so it throws (silently, under try/catch) on Windows PowerShell.
$stamp = Get-Date -Format "yyyyMMdd_HHmmss"
if (Test-Path $log) {
    try { [System.IO.File]::Move($log, "$log.$stamp.old") }
    catch { Write-Output "WARN could not archive $log : $_" }
}
$startTime = Get-Date

$protk = Join-Path $dist "protk.dat"
if (-not (Test-Path $protk)) { Write-Output "MISSING $protk"; exit 2 }

# --- optional: swap in a DLL built against a different SDK -----------------
$dllTarget = Join-Path $dist ("cn_step_c" + $Version + ".dll")
$swapped = $false
if ($DllVersion -ne "" -and $DllVersion -ne $Version) {
    $src = Join-Path $here ("build\cn_step_c" + $DllVersion + ".dll")
    if (-not (Test-Path $src)) { Write-Output "MISSING $src"; exit 2 }
    Copy-Item $dllTarget ($dllTarget + ".orig") -Force
    Copy-Item $src $dllTarget -Force
    $swapped = $true
    Write-Output "DLL     = built with Creo $DllVersion SDK (swapped in)"
} else {
    Write-Output "DLL     = built with Creo $Version SDK"
}

# Creo already auto-reads <cwd>\protk.dat, so do NOT also add a config.pro
# line for it - doing both makes Creo warn "already registered by config.pro"
# and skip the load.
#
# Creo 13 introduced a signature check: an unsigned TOOLKIT DLL triggers a
# modal dialog "This application is from an unknown source". Nobody is at
# the keyboard during an automated run, so it would hang forever. The switch
# is open_protk_unsigned_apps (Prompt | Never | Always).
$cfg = Join-Path $dist "config.pro"
if (Test-Path $cfg) { try { [System.IO.File]::Move($cfg, "$cfg.$stamp.old") } catch {} }
if ([int]$Version -ge 13) {
    Set-Content -Path $cfg -Encoding ASCII -Value "open_protk_unsigned_apps always"
    Write-Output "config.pro: open_protk_unsigned_apps always"
}

Write-Output "=== Creo $Install ==="
Write-Output "exe  = $exe"
Write-Output "cwd  = $dist"

if ($Headless) {
    $p = Start-Process -FilePath $exe -ArgumentList "-g:no_graphics" -WorkingDirectory $dist -PassThru
} else {
    $p = Start-Process -FilePath $exe -WorkingDirectory $dist -PassThru
}
Write-Output "launched pid=$($p.Id)"

$deadline = (Get-Date).AddSeconds($WaitSeconds)
$found = $false
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 3
    if (Test-Path $log) {
        # only trust a log written by THIS launch
        if ((Get-Item $log).LastWriteTime -gt $startTime) { $found = $true; break }
    }
    # parametric.exe is only a launcher; it exits while xtop.exe keeps running,
    # so do not break on HasExited - keep waiting for the log.
}

# give the plugin a couple of seconds to finish writing
if ($found) { Start-Sleep -Seconds 2 }

Write-Output "--- cn_step_load.log ---"
if (Test-Path $log) { Get-Content $log -Encoding UTF8 } else { Write-Output "(not created)" }

# cleanup: kill whatever this launch spawned
Get-Process xtop,parametric -ErrorAction SilentlyContinue | ForEach-Object {
    try { $_.Kill() } catch {}
}
Start-Sleep -Seconds 6
Get-Process xtop,parametric -ErrorAction SilentlyContinue | ForEach-Object {
    try { $_.Kill() } catch {}
}

if ($swapped) {
    Copy-Item ($dllTarget + ".orig") $dllTarget -Force
    try { [System.IO.File]::Move(($dllTarget + ".orig"), ($dllTarget + ".orig.done")) } catch {}
}

if ($found) { exit 0 } else { exit 1 }
