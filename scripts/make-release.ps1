# Assemble a redistributable folder (and optionally a zip): the patched scrcpy
# plus the OBS plugin, laid out so scripts\install-plugin.ps1 works unmodified.
#
#   .\scripts\make-release.ps1                 -> dist\RscrcpyOBS-win64-<version>\
#   .\scripts\make-release.ps1 -Zip            -> plus dist\RscrcpyOBS-win64-<version>.zip
#   .\scripts\make-release.ps1 -Out D:\rel
#
# It never compiles anything: a release is always the thing you just built and
# tested. When a prerequisite is missing it prints the command that makes it.
#
# NOTE: it calls deploy-runtime-dlls.ps1, which also refreshes the companions in
# obs-plugin\data (adb + server). That is the same thing the build does, it is
# idempotent, and it keeps the DLL list from being hand-maintained here.
#
# ASCII only on purpose -- same reason as scripts\check-panel-keys.ps1.

[CmdletBinding()]
param(
    [string] $Repo     = (Split-Path $PSScriptRoot -Parent),
    [string] $Out      = '',
    [string] $Version  = '',
    [string] $ScrcpyExe = '',   # defaults to work\scrcpy\build\app\scrcpy.exe
    [switch] $Zip
)

$ErrorActionPreference = 'Stop'

function Say([string] $m) { Write-Host "[release] $m" }

# ---------------------------------------------------------------- version
$manifest = Join-Path $Repo 'obs-plugin\data\manifest.json'
if (-not (Test-Path $manifest)) { throw "manifest not found: $manifest" }
$manifestVersion = (Get-Content $manifest -Raw | ConvertFrom-Json).version
if (-not $Version) { $Version = $manifestVersion }

$name = "RscrcpyOBS-win64-$Version"
if (-not $Out) { $Out = Join-Path $Repo 'dist' }
$pkg = Join-Path $Out $name

# ---------------------------------------------------------------- inputs
$pluginDll = Join-Path $Repo 'obs-plugin\build\rscrcpy-audio.dll'
$pluginData = Join-Path $Repo 'obs-plugin\data'

# Which scrcpy.exe to package? Candidates, in order:
#   1. -ScrcpyExe <path>
#   2. work\scrcpy\build\app\scrcpy.exe     (the normal dev build)
#   3. .tmp\scrcpy-portable\app\scrcpy.exe  (a portable build made in the scratch
#      directory: on this machine the harness runs child processes at LOW integrity,
#      so meson cannot write into work\ at all -- the user's own terminal is fine)
#
# Portability guard. With -Dportable=false scrcpy looks for its server at the path
# baked in on the BUILD machine (upstream: PREFIX "/share/scrcpy/scrcpy-server")
# instead of next to itself. Such an exe still works through scrcpy-obs.bat (the
# wrapper sets SCRCPY_SERVER_PATH), but somebody who runs scrcpy.exe directly gets
#     ERROR: 'c://share/scrcpy/scrcpy-server' does not exist or is not a regular file
# which is a terrible first impression, so never ship one. That string is compiled in
# only under #ifndef PORTABLE, which makes scanning for it a reliable check.
$needle = 'share/scrcpy/scrcpy-server'
$candidates = @()
if ($ScrcpyExe) { $candidates += $ScrcpyExe }
$candidates += (Join-Path $Repo 'work\scrcpy\build\app\scrcpy.exe')
$candidates += (Join-Path $Repo '.tmp\scrcpy-portable\app\scrcpy.exe')

# NOTE: not $scrcpyExe -- the parameter is [string] and variables are
# case-insensitive, so reusing the name invites exactly the -Zip/$zip class of bug.
$chosen = $null
$nonPortable = @()
foreach ($c in $candidates) {
    if (-not (Test-Path $c)) { continue }
    if ([Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($c)).Contains($needle)) {
        $nonPortable += $c
        continue
    }
    $chosen = $c
    break
}

if (-not $chosen) {
    $howTo = @(
        "",
        "Build a portable one:",
        "  . .\build_env.ps1",
        "  meson setup work/scrcpy/build work/scrcpy -Dbuildtype=release -Dstrip=true -Dusb=false -Dportable=true " +
        "`"-Dprebuilt_server=../../scrcpy/scrcpy-win64-v5.0.1/scrcpy-server`"",
        "  ninja -C work/scrcpy/build")
    if ($nonPortable.Count) {
        $why = @(
            "refusing to package: every scrcpy.exe in sight was built with -Dportable=false, so it",
            "looks for its server at the build machine's path ('$needle') and a user who runs",
            "scrcpy.exe directly would get 'does not exist or is not a regular file'.",
            "",
            "Not portable:")
        throw ((@($why) + @($nonPortable | ForEach-Object { "  $_" }) + $howTo +
                @("", "  (or pass -ScrcpyExe <path> to package a portable build made elsewhere)")) -join "`n")
    }
    throw ((@("scrcpy.exe is not built -- looked for:") +
            @($candidates | ForEach-Object { "  $_" }) + $howTo) -join "`n")
}
Say ("scrcpy.exe: {0}" -f $chosen)
Say 'portable scrcpy.exe OK (finds its server next to itself)'
if (-not (Test-Path $pluginDll)) {
    throw ("the plugin is not built: $pluginDll`n" +
           "  . .\build_env.ps1 ; .\obs-plugin\build.ps1")
}
foreach ($f in 'release\README.txt', 'release\scrcpy-obs.bat') {
    if (-not (Test-Path (Join-Path $Repo $f))) { throw "missing package file: $f" }
}

# ---------------------------------------------------------------- panel check
# The strings of the two sources ARE the user interface: a key that does not match
# the obs_module_text() call in the code is shown in OBS as a raw key
# ("Source.Name"). Fail the release here instead of shipping that.
& (Join-Path $PSScriptRoot 'check-panel-keys.ps1') -Repo $Repo
if ($LASTEXITCODE -ne 0) {
    throw 'panel key check failed -- fix obs-plugin\data\locale\<lang>.ini before releasing'
}
Say 'panel keys and property ids OK'

# ---------------------------------------------------------------- fresh dir
if (Test-Path $pkg) {
    try {
        Remove-Item $pkg -Recurse -Force -ErrorAction Stop
    } catch {
        throw (@("cannot replace the existing package: $pkg",
                 "  " + $_.Exception.Message,
                 "",
                 "  Something is holding that directory. The usual suspects:",
                 "    - a shell whose CURRENT DIRECTORY is inside it  (cd somewhere else first),",
                 "    - a scrcpy.exe / adb.exe started from it that is still running.",
                 "",
                 "  Close that, or build somewhere else:",
                 "    .\scripts\make-release.ps1 -Out <another directory>") -join "`n")
    }
}
New-Item -ItemType Directory -Force -Path $pkg | Out-Null
Say "assembling $pkg"

# ---------------------------------------------------------------- scrcpy\
# Copy the exe first, then let the existing deploy script work out which DLLs it
# actually imports and put them next to it (plus adb + the server). No hand-kept
# list of DLLs, so the release cannot drift from what the plugin runs with.
$dstScrcpy = Join-Path $pkg 'scrcpy'
New-Item -ItemType Directory -Force -Path $dstScrcpy | Out-Null
Copy-Item $chosen (Join-Path $dstScrcpy 'scrcpy.exe') -Force
& (Join-Path $PSScriptRoot 'deploy-runtime-dlls.ps1') -Repo $Repo -Exe (Join-Path $dstScrcpy 'scrcpy.exe')

# The wrapper (sets SCRCPY_SERVER_PATH and forwards the user's options).
Copy-Item (Join-Path $Repo 'release\scrcpy-obs.bat') (Join-Path $dstScrcpy 'scrcpy-obs.bat') -Force

# ---------------------------------------------------------------- obs-plugin\
$dstBuild = Join-Path $pkg 'obs-plugin\build'
$dstData = Join-Path $pkg 'obs-plugin\data'
New-Item -ItemType Directory -Force -Path $dstBuild, $dstData | Out-Null
Copy-Item $pluginDll (Join-Path $dstBuild 'rscrcpy-audio.dll') -Force

# data\* : locale + manifest. Nothing machine-specific lives here.
$copied = 0
Get-ChildItem $pluginData -Force | ForEach-Object {
    Copy-Item $_.FullName (Join-Path $dstData $_.Name) -Recurse -Force
    $copied++
}
Say "plugin data: $copied item(s)"

# ---------------------------------------------------------------- scripts\
# install-plugin.ps1 derives -Repo from its own location's parent, so placing it
# in <pkg>\scripts\ makes it resolve to the package root: no fork of that script,
# no second implementation to keep in sync.
$dstScripts = Join-Path $pkg 'scripts'
New-Item -ItemType Directory -Force -Path $dstScripts | Out-Null
foreach ($s in 'install-plugin.ps1', 'uninstall-plugin.ps1') {
    Copy-Item (Join-Path $PSScriptRoot $s) (Join-Path $dstScripts $s) -Force
}

# ---------------------------------------------------------------- docs + licenses
Copy-Item (Join-Path $Repo 'release\README.txt') (Join-Path $pkg 'README.txt') -Force
foreach ($f in 'LICENSE', 'NOTICE.md') {
    Copy-Item (Join-Path $Repo $f) (Join-Path $pkg $f) -Force
}
Copy-Item (Join-Path $Repo 'LICENSE-scrcpy.txt') (Join-Path $pkg 'LICENSE-scrcpy.txt') -Force

# ---------------------------------------------------------------- completeness
# F07: deploy-runtime-dlls.ps1 only *warns* about missing companions (it is
# also a dev tool), so a package without adb/scrcpy-server could still pass
# the --version smoke test below and ship broken (scrcpy.exe --version does
# not touch adb or the server). The release must be self-contained: every
# file it needs at run time has to be present in the folder.
$required = @(
    'scrcpy\scrcpy.exe',
    'scrcpy\scrcpy-server',
    'scrcpy\adb.exe',
    'scrcpy\AdbWinApi.dll',
    'scrcpy\AdbWinUsbApi.dll',
    'scrcpy\scrcpy-obs.bat',
    'obs-plugin\build\rscrcpy-audio.dll',
    'obs-plugin\data\manifest.json',
    'obs-plugin\data\locale\en-US.ini',
    'obs-plugin\data\locale\zh-CN.ini',
    'scripts\install-plugin.ps1',
    'scripts\uninstall-plugin.ps1',
    'README.txt',
    'LICENSE',
    'LICENSE-scrcpy.txt',
    'NOTICE.md'
)
$missingPkg = @($required | Where-Object { -not (Test-Path (Join-Path $pkg $_)) })
if ($missingPkg.Count -gt 0) {
    throw ((@("incomplete package: missing required file(s) -- a release must be self-contained:") +
            ($missingPkg | ForEach-Object { "  $_" }) +
            @("", "deploy-runtime-dlls.ps1 only warns about missing companions (it is also a dev tool);",
              "check its warnings above -- usually adb or scrcpy-server is not built/downloaded.")) -join "`n")
}
Say "package completeness OK ($($required.Count) required files present)"

# ---------------------------------------------------------------- smoke test
# Running the packaged exe is the only way to know the DLL set is complete
# (a missing one is 0xC0000135 at load, which is silent in a folder listing).
$verLine = ''
try {
    $verLine = (& (Join-Path $dstScrcpy 'scrcpy.exe') --version 2>&1 | Select-Object -First 1)
    if ($LASTEXITCODE -ne 0 -or -not $verLine) { throw "exit $LASTEXITCODE" }
    Say "smoke test: $verLine"
} catch {
    throw ("the packaged scrcpy.exe does not start: $($_.Exception.Message)`n" +
           "  A missing runtime DLL usually means deploy-runtime-dlls.ps1 could not find it;`n" +
           "  run it by hand with -WhatIf to see what it wanted to copy.")
}

$dllCount = (Get-ChildItem $dstScrcpy -Filter '*.dll' -File | Measure-Object).Count
$lines = @(
    "RscrcpyOBS $Version",
    "built       $(Get-Date -Format 'yyyy-MM-dd HH:mm')",
    "scrcpy      $verLine",
    "runtime     $dllCount DLL(s) next to scrcpy.exe (computed from the import graph)",
    "",
    "Shared memory protocol:",
    "  Local\scrcpy_video_v1_<name>   BGRA shared texture, 3 rotating slots",
    "  Local\scrcpy_audio_v1_<name>   float32 PCM ring + QPC timestamps",
    "",
    "See README.txt for how to use it."
)
Set-Content -LiteralPath (Join-Path $pkg 'VERSION.txt') -Value ($lines -join "`r`n") -Encoding ascii

# ---------------------------------------------------------------- summary
$files = Get-ChildItem $pkg -Recurse -File -Force
$mb = [math]::Round((($files | Measure-Object Length -Sum).Sum) / 1MB, 1)
Say ("package  {0}" -f $pkg)
Say ("         {0} file(s), {1} MB" -f $files.Count, $mb)

if ($Zip) {
    # NOTE: do not name this $zip -- PowerShell variables are case-insensitive,
    # so it would collide with the [switch] $Zip parameter and assigning a
    # string to it fails with "cannot convert ... to SwitchParameter".
    $zipPath = Join-Path $Out "$name.zip"
    Remove-Item $zipPath -Force -ErrorAction SilentlyContinue
    # tar.exe (bsdtar) is far faster than Compress-Archive here: most of the
    # package is avcodec-63.dll.
    $tar = Get-Command tar.exe -ErrorAction SilentlyContinue
    if ($tar) {
        & $tar.Source -a -c -f $zipPath -C $Out $name
        if ($LASTEXITCODE -ne 0) { throw "tar failed with exit $LASTEXITCODE" }
    } else {
        Compress-Archive -Path $pkg -DestinationPath $zipPath -CompressionLevel Optimal
    }
    Say ("zip      {0} ({1} MB)" -f $zipPath, [math]::Round((Get-Item $zipPath).Length / 1MB, 1))
}

Say 'done'
