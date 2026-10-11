<#
从已安装的 OBS 生成 libobs 导入库：deps\obs-lib\{obs.def, libobs.a}

为什么要这个：插件要链接 libobs，但 OBS 官方不随包分发 .lib/.a。做法是从 obs.dll 的导出表
（gendef）生成 .def，再用 dlltool 生成 MinGW 可用的 libobs.a。deps\obs-lib\ 不进 git。

用法：
  .\scripts\deps-obs.ps1                                              # 默认 D:\obs-studio
  .\scripts\deps-obs.ps1 -ObsRoot 'C:\Program Files\obs-studio'
  .\scripts\deps-obs.ps1 -ObsDll D:\obs-studio\bin\64bit\obs.dll -OutDir .\deps\obs-lib

需要 gendef / dlltool 在 PATH 上（先执行 . .\build_env.ps1）。
libobs 头文件不在这里准备：放到 deps\obs-studio-src\（或用 scripts\link-deps.ps1 软链）。
#>
param(
    [string]$ObsRoot = 'D:\obs-studio',
    [string]$ObsDll  = '',
    [string]$OutDir  = (Join-Path $PSScriptRoot '..\deps\obs-lib')
)
$ErrorActionPreference = 'Stop'
if (-not $ObsDll) { $ObsDll = Join-Path $ObsRoot 'bin\64bit\obs.dll' }
if (-not (Test-Path $ObsDll)) { throw "找不到 obs.dll：$ObsDll（用 -ObsRoot / -ObsDll 指定）" }

foreach ($t in 'gendef', 'dlltool') {
    if (-not (Get-Command $t -ErrorAction SilentlyContinue)) {
        throw "$t 不在 PATH 上。先执行：. .\build_env.ps1"
    }
}

$tmp = Join-Path ([IO.Path]::GetTempPath()) ("deps-obs-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
try {
    Push-Location $tmp
    & gendef $ObsDll | Out-Null
    $def = Get-ChildItem $tmp -Filter '*.def' | Select-Object -First 1
    if (-not $def) { throw "gendef 没有生成 .def" }

    # -D 用 DLL 基名：导入库里记的就是 obs.dll，运行时由 OBS 自己加载
    & dlltool -d $def.FullName -l "$tmp\libobs.a" -D (Split-Path $ObsDll -Leaf)
    if (-not (Test-Path "$tmp\libobs.a")) { throw "dlltool 没有生成 libobs.a" }

    New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
    Copy-Item $def.FullName (Join-Path $OutDir 'obs.def') -Force
    Copy-Item "$tmp\libobs.a" (Join-Path $OutDir 'libobs.a') -Force
    "生成完毕（$OutDir）："
    Get-ChildItem $OutDir | Select-Object Name, Length | Format-Table -AutoSize
} finally {
    Pop-Location
    Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
}
