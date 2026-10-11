# RscrcpyOBS 构建环境入口 —— 在构建/运行前 dot-source 本文件：
#   . .\build_env.ps1
# 约定：C 盘不落任何东西。$Root 取本文件所在目录（= 仓库根），所以仓库搬去哪、从哪份拷贝
# 构建都成立。

$Root = $PSScriptRoot

# 编译器 + 构建工具（本地 Strawberry winlibs GCC 13.2 / ninja / gendef）
# + venv（meson + pkgconf 提供的 pkg-config.exe）+ Strawberry Perl
$env:PATH = "D:\CPLUSPLUS_Compiler\Strawberry\c\bin;" +
            "$Root\buildenv\Scripts;" +
            "D:\CPLUSPLUS_Compiler\Strawberry\perl\bin;" +
            $env:PATH

# 运行 scrcpy.exe 所需的 DLL（FFmpeg + SDL3，优先于系统其他同名 DLL）
# + adb（scrcpy 通过 PATH 调用 adb）
$env:PATH = "$Root\deps\ffmpeg\bin;" +
            "$Root\deps\sdl3\bin;" +
            "$Root\scrcpy\scrcpy-win64-v5.0.1;" +
            $env:PATH

# 依赖包的 .pc 搜索路径（FFmpeg + SDL3）
$env:PKG_CONFIG_PATH = "$Root\deps\ffmpeg\lib\pkgconfig;$Root\deps\sdl3\lib\pkgconfig"

# 卫生项：pip 缓存与临时目录重定向到 D 盘（目录不存在就先建，否则 TMP 失效会拖坏 meson/ninja）
foreach ($d in '.pipcache', '.tmp') {
    $p = Join-Path $Root $d
    if (-not (Test-Path $p)) { New-Item -ItemType Directory -Force -Path $p | Out-Null }
}
$env:PIP_CACHE_DIR = "$Root\.pipcache"
$env:TMP  = "$Root\.tmp"
$env:TEMP = "$Root\.tmp"

# 运行 scrcpy.exe 所需（prebuilt server 直接引用原始路径；meson 不拷贝它）
$env:SCRCPY_SERVER_PATH = "$Root\scrcpy\scrcpy-win64-v5.0.1\scrcpy-server"
$env:ADB                = "$Root\scrcpy\scrcpy-win64-v5.0.1\adb.exe"
$env:SCRCPY_ICON_DIR    = "$Root\work\scrcpy\app\data"

Write-Host "[build_env] root=$Root"
Write-Host "[build_env] gcc/ninja/meson/pkg-config 就绪；PKG_CONFIG_PATH=$env:PKG_CONFIG_PATH"
