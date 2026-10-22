# shot_creo.ps1 - launch a deployed Creo, wait for the plugin log, then grab a
# screenshot so the ribbon tab can be checked visually.
param(
    [Parameter(Mandatory=$true)][string]$Install,   # e.g. "8.0.12.0"
    [int]$WaitSeconds = 200,
    [string]$Out = ""
)

$ErrorActionPreference = "Continue"
$root = "C:\Program Files\PTC\Creo $Install"
$exe  = Join-Path $root "Parametric\bin\parametric.exe"
$log  = Join-Path $root "cc_luoluoo\cn_step_load.log"

if (-not (Test-Path $exe)) { Write-Output "MISSING $exe"; exit 2 }

$stamp = Get-Date -Format "yyyyMMdd_HHmmss"
if (Test-Path $log) { try { [System.IO.File]::Move($log, "$log.$stamp.old") } catch {} }
$startTime = Get-Date

$work = Join-Path $env:TEMP ("luoluo_shot_" + ($Install -replace '[^\d]',''))
New-Item -ItemType Directory -Force -Path $work | Out-Null
Get-ChildItem -LiteralPath $work -Force -ErrorAction SilentlyContinue | ForEach-Object {
    try { [System.IO.File]::Move($_.FullName, "$($_.FullName).$stamp.old") } catch {}
}

if ($Out -eq "") {
    $Out = Join-Path "C:\Users\Administrator\WorkBuddy\2026-10-20-14-05-22\creo_toolkit_cn_step\build" ("shot_creo" + ($Install -replace '[^\d]','') + ".png")
}

Write-Output "launching $exe"
$p = Start-Process -FilePath $exe -WorkingDirectory $work -PassThru

$deadline = (Get-Date).AddSeconds($WaitSeconds)
$found = $false
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 3
    if (Test-Path $log) {
        if ((Get-Item $log).LastWriteTime -gt $startTime) { $found = $true; break }
    }
}
Write-Output ("plugin log: " + $(if ($found) { "yes" } else { "NO" }))
if ($found) { Get-Content $log -Encoding UTF8 }

# let the UI finish painting
Start-Sleep -Seconds 20

# bring Creo's main window forward (the launcher pid is not xtop, so search by title)
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class W {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hwnd, IntPtr hdc, uint flags);
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int L, T, R, B; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
}
"@
$wins = $null
for ($i = 0; $i -lt 20; $i++) {
    $wins = Get-Process xtop -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 }
    if ($wins) { break }
    Start-Sleep -Seconds 3
}
if (-not $wins) { Write-Output "no Creo window found after waiting" }

Add-Type -AssemblyName System.Drawing
foreach ($w in $wins) {
    $h = $w.MainWindowHandle
    [W]::ShowWindow($h, 3) | Out-Null
    Start-Sleep -Seconds 4

    $r = New-Object W+RECT
    [W]::GetWindowRect($h, [ref]$r) | Out-Null
    $wd = $r.R - $r.L; $ht = $r.B - $r.T
    if ($wd -le 0 -or $ht -le 0) { Write-Output "bad window rect"; continue }

    $bmp = New-Object System.Drawing.Bitmap $wd, $ht
    $gfx = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $gfx.GetHdc()
    # PW_RENDERFULLCONTENT (2): also captures DirectX-rendered content,
    # which is how the Creo ribbon is drawn
    [W]::PrintWindow($h, $hdc, 2) | Out-Null
    $gfx.ReleaseHdc($hdc); $gfx.Dispose()
    $bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    Write-Output ("screenshot: " + $Out + "  (" + $wd + "x" + $ht + ")")
}

Get-Process xtop,parametric -ErrorAction SilentlyContinue | ForEach-Object { try { $_.Kill() } catch {} }
Start-Sleep -Seconds 8
Get-Process xtop,parametric -ErrorAction SilentlyContinue | ForEach-Object { try { $_.Kill() } catch {} }

if ($found) { exit 0 } else { exit 1 }
