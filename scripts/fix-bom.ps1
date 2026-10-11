<#
本仓库所有 .ps1 必须是"UTF-8 with BOM"。

原因：Windows PowerShell 5.1 读没有 BOM 的 .ps1 时会按系统 ANSI 码页解码，
中文立刻变乱码并报一堆莫名其妙的语法错误（"字符串缺少终止符"之类）。
而各种编辑器/写文件工具（含 AI 的）默认常常写成无 BOM，于是每次改完脚本都可能踩一次。

用法：
  .\scripts\fix-bom.ps1          # 补 BOM（只加签名，不动内容）
  .\scripts\fix-bom.ps1 -Check   # 只检查；有缺就 exit 1（可挂到提交前）
#>
param([switch]$Check)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

# 只扫本仓库自己的脚本目录，不进 junction/第三方/构建产物
$scanDirs = @('', 'scripts', 'tools', 'obs-plugin')
$files = foreach ($d in $scanDirs) {
    $p = if ($d) { Join-Path $root $d } else { $root }
    if (Test-Path $p) { Get-ChildItem -LiteralPath $p -Recurse -Depth 4 -Filter '*.ps1' -File -ErrorAction SilentlyContinue }
}

$bad = 0
foreach ($f in $files) {
    $bytes = [System.IO.File]::ReadAllBytes($f.FullName)
    $hasBom = $bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF
    if ($hasBom) { continue }
    $bad++
    $rel = $f.FullName.Substring($root.Length + 1)
    if ($Check) { "缺 BOM: $rel" }
    else {
        $text = [System.IO.File]::ReadAllText($f.FullName, (New-Object System.Text.UTF8Encoding($false)))
        [System.IO.File]::WriteAllText($f.FullName, $text, (New-Object System.Text.UTF8Encoding($true)))
        "补上 BOM: $rel"
    }
}
if ($bad -eq 0) { "全部 .ps1 都带 BOM ✅（共扫 $($files.Count) 个）" }
elseif ($Check) { "共 $bad 个文件缺 BOM：跑 .\scripts\fix-bom.ps1 修"; exit 1 }
