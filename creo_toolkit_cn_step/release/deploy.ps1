<#
    deploy.ps1 - cn_step（落落工具）一键部署核心脚本

    做四件事：
      1. 检查发布包的载荷是否完整
      2. 扫描本机所有 Creo 安装（支持 Creo 8.0 ~ 13.0）
      3. 在每个 Creo 根目录下新建 cc_luoluoo\ 文件夹，把插件必要文件复制进去
         （DLL 按版本挑选：能用哪个就用能跑的最高 SDK 版本）
      4. 在该 Creo 的「最高优先级 config.pro」里注入注册行，指向 cc_luoluoo

    重复运行是安全的：旧的本插件注册行会被清掉再重写，不会重复注入。

    用法：
        powershell -NoProfile -ExecutionPolicy Bypass -File deploy.ps1
        powershell ... -File deploy.ps1 -Root "D:\PTC"     # 装在其他盘
        powershell ... -File deploy.ps1 -WhatIfOnly        # 只扫描不落地
#>

param(
    [string[]]$Root = @(),
    [switch]$WhatIfOnly
)

$ErrorActionPreference = "Stop"

# ---------------------------------------------------------------- 常量
$PLUGIN_DIR = "cc_luoluoo"
$MARK_BEGIN = "! >>> luoluo_tools (managed by deploy.ps1, do not edit) >>>"
$MARK_END   = "! <<< luoluo_tools <<<"
$QR_NAME    = "luoluo_qr.bmp"
$CFG_NAME   = "cn_step.cfg"
$CSV_NAME   = "名称对照.csv"

$HERE = Split-Path -Parent $MyInvocation.MyCommand.Path
$BIN  = Join-Path $HERE "bin"
$script:locked = $false

# ---------------------------------------------------------------- 小工具
function Write-Head($text) {
    Write-Host ""
    Write-Host ("=" * 62)
    Write-Host "  $text"
    Write-Host ("=" * 62)
}

function Get-TextEncoding([string]$path) {
    # 保持原文件编码：UTF-8 BOM 的仍写回 UTF-8 BOM，其余按系统 ANSI 处理
    if (Test-Path $path) {
        $bytes = [IO.File]::ReadAllBytes($path)
        if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) {
            return New-Object Text.UTF8Encoding($true)
        }
    }
    return [Text.Encoding]::Default
}

function Read-Lines([string]$path, $enc) {
    if (-not (Test-Path $path)) { return @() }
    $text = [IO.File]::ReadAllText($path, $enc)
    return [string[]]($text -split "`r`n|`n")
}

# 本插件曾经写进去的行：注释、注册行、无签名放行
function Test-ManagedLine([string]$line) {
    if ($line -match '^\s*!\s*(>>>|<<<)\s*luoluo_tools') { return $true }
    if ($line -match '^\s*!\s*cn_step')     { return $true }
    if ($line -match '^\s*toolkit_registry_file\s+.*cn_step') { return $true }
    if ($line -match '^\s*toolkit_registry_file\s+.*cc_luoluoo') { return $true }
    if ($line -match '^\s*open_protk_unsigned_apps\s') { return $true }
    return $false
}

function Copy-FileSafe([string]$src, [string]$dstDir) {
    # Creo 正在运行时 DLL 会被占用，直接 Copy-Item 会抛异常中断整个部署。
    # 这种情况给出人能看懂的提示，然后继续处理下一个文件/版本。
    try {
        Copy-Item -LiteralPath $src -Destination $dstDir -Force -ErrorAction Stop
        return $true
    } catch {
        Write-Host ("    [占用] " + (Split-Path -Leaf $src) + " 复制失败——请先关闭正在运行的 Creo 再重试") -ForegroundColor Yellow
        return $false
    }
}

# ---------------------------------------------------------------- 载荷检查
Write-Head "1/4  检查发布包"

$payload = @()
foreach ($v in 8,9,10,11,12,13) {
    $payload += [pscustomobject]@{ Kind="dll"; Path=(Join-Path $BIN "cn_step_c$v.dll"); Ver=$v }
}
foreach ($lang in "chinese_cn","usascii") {
    $payload += [pscustomobject]@{ Kind="txt"; Path=(Join-Path $HERE "text\$lang\msg_cn_step.txt") }
}
$payload += [pscustomobject]@{ Kind="cfg"; Path=(Join-Path $HERE $CFG_NAME) }
$payload += [pscustomobject]@{ Kind="bmp"; Path=(Join-Path $HERE $QR_NAME) }
$payload += [pscustomobject]@{ Kind="csv"; Path=(Join-Path $HERE $CSV_NAME) }

$missing = @()
foreach ($p in $payload) {
    if (Test-Path $p.Path) {
        if ($p.Kind -eq "dll") { Write-Host ("  [ok]   bin\cn_step_c{0}.dll" -f $p.Ver) }
        else { Write-Host ("  [ok]   " + (Split-Path -Leaf $p.Path)) }
    } else {
        if ($p.Kind -eq "dll") { Write-Host ("  [MISS] bin\cn_step_c{0}.dll" -f $p.Ver) }
        else { Write-Host ("  [MISS] " + (Split-Path -Leaf $p.Path)) }
        $missing += $p.Path
    }
}

# 缺了某个版本号的 DLL 不算致命——按"能用最高 ≤ 目标版本"降级即可；
# 但一个都没有、或者文本/配置缺失就跑不起来。
$haveDll = @(8,9,10,11,12,13 | Where-Object { Test-Path (Join-Path $BIN "cn_step_c$_.dll") })
if ($haveDll.Count -eq 0) {
    Write-Host ""
    Write-Host "[ERROR] bin 目录里一个 DLL 都没有，发布包不完整。" -ForegroundColor Red
    Write-Host "        请把整个文件夹复制过来，不要只拷 .bat。"
    exit 1
}
$hardMissing = $missing | Where-Object { $_ -notmatch 'cn_step_c\d+\.dll$' }
if ($hardMissing.Count -gt 0) {
    Write-Host ""
    Write-Host "[ERROR] 缺少必需文件，发布包不完整：" -ForegroundColor Red
    $hardMissing | ForEach-Object { Write-Host "        $_" }
    exit 1
}
Write-Host ("  可用 DLL 版本：" + ($haveDll -join ", "))

# ---------------------------------------------------------------- 找 Creo
Write-Head "2/4  扫描本机 Creo 安装"

if ($Root.Count -eq 0) {
    $Root = @(
        (Join-Path $env:ProgramFiles "PTC"),
        (Join-Path ${env:ProgramFiles(x86)} "PTC")
    ) | Where-Object { $_ -and (Test-Path $_) }
}

$installs = @()
foreach ($r in $Root) {
    if (-not (Test-Path $r)) { continue }
    Get-ChildItem -LiteralPath $r -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '^Creo\s+\d+' } |
        ForEach-Object { $installs += $_.FullName }
}
$installs = $installs | Sort-Object -Unique

if ($installs.Count -eq 0) {
    Write-Host "[ERROR] 没找到任何 Creo 安装目录。" -ForegroundColor Red
    Write-Host "        扫描过：$($Root -join '  |  ')"
    Write-Host "        装在别的盘可以这样跑：deploy.ps1 -Root D:\PTC"
    exit 1
}

$targets = @()
foreach ($dir in $installs) {
    $leaf = Split-Path -Leaf $dir
    $major = 0
    if ($leaf -match '^Creo\s+(\d+)') { $major = [int]$Matches[1] }

    $exe = Join-Path $dir "Parametric\bin\parametric.exe"
    if (-not (Test-Path $exe)) {
        Write-Host ("  [skip] {0}  （没有 Parametric，不是主程序安装）" -f $leaf)
        continue
    }

    # Pro/TOOLKIT 头文件在 Common Files\protoolkit 或 Common Files\M250\protoolkit
    if ($major -lt 8) {
        Write-Host ("  [skip] {0}  （仅支持 Creo 8.0 ~ 13.0，老版本未适配）" -f $leaf)
        continue
    }
    $sdk = $null
    $plain = Join-Path $dir "Common Files\protoolkit\includes\ProToolkit.h"
    if (Test-Path $plain) {
        $sdk = Join-Path $dir "Common Files\protoolkit"
    } else {
        Get-ChildItem -LiteralPath (Join-Path $dir "Common Files") -Directory -ErrorAction SilentlyContinue |
            ForEach-Object {
                $cand = Join-Path $_.FullName "protoolkit\includes\ProToolkit.h"
                if (-not $sdk -and (Test-Path $cand)) { $sdk = Join-Path $_.FullName "protoolkit" }
            }
    }

    # 挑 DLL：优先同版本，否则退到不高于它的最高版本
    $pick = ($haveDll | Where-Object { $_ -le $major } | Sort-Object -Descending | Select-Object -First 1)
    if (-not $pick) {
        Write-Host ("  [skip] {0}  （Creo 版本太老，没有适配的 DLL）" -f $leaf)
        continue
    }

    $targets += [pscustomobject]@{
        Root   = $dir
        Leaf   = $leaf
        Major  = $major
        Sdk    = $sdk
        DllVer = $pick
        Plugin = (Join-Path $dir $PLUGIN_DIR)
    }
    $sdkNote = if ($sdk) { "SDK 在 " + (Split-Path -Leaf $sdk) } else { "未装 Pro/TOOLKIT（菜单可能不出现）" }
    Write-Host ("  [found] {0,-14} -> cn_step_c{1}.dll   {2}" -f $leaf, $pick, $sdkNote)
}

if ($targets.Count -eq 0) { Write-Host "[ERROR] 没有可部署的 Creo。" -ForegroundColor Red; exit 1 }
if ($WhatIfOnly) { Write-Host ""; Write-Host "（-WhatIfOnly：只扫描，不落地）"; exit 0 }

# ---------------------------------------------------------------- 复制
Write-Head "3/4  复制插件到各版本的 cc_luoluoo\"

foreach ($t in $targets) {
    Write-Host ""
    Write-Host ("--- {0}" -f $t.Leaf) -ForegroundColor Cyan
    $dst = $t.Plugin
    New-Item -ItemType Directory -Force -Path $dst                  | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $dst "text\chinese_cn\ribbon") | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $dst "text\usascii\ribbon")     | Out-Null

    $dll = Join-Path $BIN ("cn_step_c{0}.dll" -f $t.DllVer)
    if (Copy-FileSafe $dll (Join-Path $dst "cn_step.dll")) {
        Write-Host ("    cn_step.dll                    <- cn_step_c{0}.dll" -f $t.DllVer)
    } else {
        $script:locked = $true
    }

    foreach ($lang in "chinese_cn","usascii") {
        $src = Join-Path $HERE "text\$lang"
        Copy-Item -LiteralPath (Join-Path $src "msg_cn_step.txt") -Destination (Join-Path $dst "text\$lang") -Force
        $rbn = Join-Path (Join-Path $src "ribbon") "toolkitribbonui.rbn"
        if (Test-Path $rbn) {
            Copy-Item -LiteralPath $rbn -Destination (Join-Path $dst "text\$lang\ribbon") -Force
        }
    }
    Write-Host "    text\chinese_cn, text\usascii   <- 消息文件 / 功能区定义"

    Copy-Item -LiteralPath (Join-Path $HERE $CFG_NAME) -Destination $dst -Force
    Copy-Item -LiteralPath (Join-Path $HERE $QR_NAME)  -Destination $dst -Force
    Copy-Item -LiteralPath (Join-Path $HERE $CSV_NAME) -Destination $dst -Force
    Write-Host "    cn_step.cfg / luoluo_qr.bmp / 名称对照.csv"

    # protk.dat 用绝对路径，Creo 才找得到 TEXT_DIR
    $protk = @(
        "NAME       cn_step",
        ("EXEC_FILE  " + (Join-Path $dst "cn_step.dll")),
        ("TEXT_DIR   " + (Join-Path $dst "text")),
        "STARTUP    dll",
        "REVISION   18",
        "ALLOW_STOP TRUE",
        "END"
    )
    [IO.File]::WriteAllText((Join-Path $dst "protk.dat"), ($protk -join "`r`n") + "`r`n", [Text.Encoding]::Default)
    Write-Host "    protk.dat                      -> 指向本目录"
}

# ---------------------------------------------------------------- config.pro
Write-Head "4/4  写入各版本最高优先级 config.pro"

foreach ($t in $targets) {
    Write-Host ""
    Write-Host ("--- {0}" -f $t.Leaf) -ForegroundColor Cyan

    # 全局 config.pro：Creo 3+ 在 Common Files\text，Creo 2 在 Common Files\<M码>\text
    $cfgTextDir = Join-Path $t.Root "Common Files\text"
    if (-not (Test-Path (Join-Path $cfgTextDir "config.pro"))) {
        $cfgTextDir = $null
        Get-ChildItem -LiteralPath (Join-Path $t.Root "Common Files") -Directory -ErrorAction SilentlyContinue |
            Sort-Object Name -Descending | ForEach-Object {
                if (-not $cfgTextDir -and (Test-Path (Join-Path $_.FullName "text\config.pro"))) {
                    $cfgTextDir = Join-Path $_.FullName "text"
                }
            }
    }
    if (-not $cfgTextDir) {
        # 没有就建一个 Creo 3+ 风格的位置
        $cfgTextDir = Join-Path $t.Root "Common Files\text"
        New-Item -ItemType Directory -Force -Path $cfgTextDir | Out-Null
    }

    $cfg = Join-Path $cfgTextDir "config.pro"
    Write-Host ("    " + $cfg)

    $enc = Get-TextEncoding $cfg
    $lines = Read-Lines $cfg $enc
    $kept = @($lines | Where-Object { -not (Test-ManagedLine $_) })

    # 备份一次（存在就不覆盖，保住最原始的版本）
    $bak = "$cfg.cn_step.bak"
    if ((Test-Path $cfg) -and -not (Test-Path $bak)) {
        Copy-Item -LiteralPath $cfg -Destination $bak -Force
        Write-Host ("    备份 -> " + (Split-Path -Leaf $bak))
    }

    $block = @($MARK_BEGIN)
    $block += ("toolkit_registry_file " + (Join-Path $t.Plugin "protk.dat"))
    if ($t.Major -ge 9) {
        # Creo 9 起对未签名 DLL 会弹「未知来源」对话框（实测 9.0.10.0 会弹，
        # 8.0.12.0 不会）；无人值守/免打扰场景要放开
        $block += "open_protk_unsigned_apps always"
    }
    $block += $MARK_END

    $out = @($kept)
    while ($out.Count -gt 0 -and $out[-1].Trim() -eq "") { $out = $out[0..($out.Count-2)] }
    $out += ""
    $out += $block

    [IO.File]::WriteAllText($cfg, (($out -join "`r`n") + "`r`n"), $enc)
    Write-Host ("    注入 toolkit_registry_file -> " + (Join-Path $t.Plugin "protk.dat"))
    if ($t.Major -ge 9) { Write-Host "    注入 open_protk_unsigned_apps always" }
}

# ---------------------------------------------------------------- 收尾
Write-Head "完成"
Write-Host "  部署了 $($targets.Count) 个 Creo："
foreach ($t in $targets) {
    Write-Host ("    {0,-14}  {1}" -f $t.Leaf, $t.Plugin)
}
Write-Host ""
if ($script:locked) {
    Write-Host "  [注意] 有文件因为 Creo 正在运行而没能覆盖（见上面的 [占用]）。" -ForegroundColor Yellow
    Write-Host "         关掉所有 Creo 窗口，再运行一次本脚本即可。"
    Write-Host ""
}
Write-Host "  启动 Creo 后，在「工具(Utilities)」菜单里能看到「落落工具」。"
Write-Host "  功能区选项卡也会自动出现，见同名目录下的《功能区选项卡.txt》。"
Write-Host ""
