# Copy the runtime DLLs scrcpy.exe needs right next to it.
#
# Why: the plugin starts scrcpy from inside OBS, and OBS' PATH has neither
# deps\ffmpeg\bin nor deps\sdl3\bin (let alone the MinGW runtime). Without these
# copies scrcpy.exe dies instantly with 0xC0000135 (STATUS_DLL_NOT_FOUND) and the
# source just sits at "waiting for a producer".
#
# Idempotent: only copies what is missing. Imports that live in System32 are left
# alone (the OS resolves those).

[CmdletBinding()]
param(
    [string] $Repo = (Split-Path $PSScriptRoot -Parent),
    [string] $Exe  = '',
    [switch] $WhatIf
)

$ErrorActionPreference = 'Stop'
if (-not $Exe) { $Exe = Join-Path $Repo 'work\scrcpy\build\app\scrcpy.exe' }
if (-not (Test-Path $Exe)) { throw "scrcpy.exe not found: $Exe (build it with meson/ninja first)" }

$dest = Split-Path $Exe -Parent
$sys32 = Join-Path $env:SystemRoot 'system32'

# Where a missing import may live. Order matters only for reporting.
$search = @()
foreach ($d in 'deps\ffmpeg\bin', 'deps\sdl3\bin') {
    $p = Join-Path $Repo $d
    if (Test-Path $p) { $search += $p }
}
$search += @(Get-ChildItem (Join-Path $Repo 'deps') -Directory -ErrorAction SilentlyContinue |
             ForEach-Object { Join-Path $_.FullName 'bin' } | Where-Object { Test-Path $_ })
# MinGW runtime (libgcc_s_seh-1.dll, libwinpthread-1.dll, ...)
$search += @('D:\CPLUSPLUS_Compiler\Strawberry\c\bin') | Where-Object { Test-Path $_ }

$objdump = Get-Command objdump -ErrorAction SilentlyContinue
if (-not $objdump) {
    # build.ps1 invokes this without build_env.ps1, so fall back to the toolchain
    $cand = 'D:\CPLUSPLUS_Compiler\Strawberry\c\bin\objdump.exe'
    if (Test-Path $cand) { $objdump = Get-Item $cand }
}
if (-not $objdump) { throw 'objdump not found (source build_env.ps1 first)' }
$objdumpExe = if ($objdump.Source) { $objdump.Source } else { $objdump.FullName }

function Get-Imports([string] $file) {
    $imports = & $objdumpExe -p $file 2>$null
    if ($LASTEXITCODE -ne 0) {
        # A failed read must not look like "no imports" (F08): that would
        # silently hide a whole dependency subtree.
        throw "objdump could not read '$file' (exit $LASTEXITCODE) -- the dependency walk cannot continue"
    }
    @($imports |
      Select-String -Pattern 'DLL Name:\s*(\S+)' |
      ForEach-Object { $_.Matches[0].Groups[1].Value } | Sort-Object -Unique)
}

# Transitive walk: a DLL's own imports matter too (avcodec pulls in codecs, the
# MinGW runtime pulls in libgcc/libwinpthread, ...). Missing one of those is
# exactly the 0xC0000135 we are fixing, so follow the whole graph.
$queue = New-Object System.Collections.Queue
$queue.Enqueue($Exe)
$seen = @{}
$copied = 0
$missing = @()
$system = 0
$present = 0

while ($queue.Count -gt 0) {
    $cur = $queue.Dequeue()
    foreach ($dll in Get-Imports $cur) {
        if ($seen.ContainsKey($dll)) { continue }
        $seen[$dll] = $true
        if (Test-Path (Join-Path $sys32 $dll)) { $system++; continue }
        $destPath = Join-Path $dest $dll
        if (Test-Path $destPath) {
            $present++
            $queue.Enqueue($destPath)      # it may have imports of its own
            continue
        }
        $src = @($search | ForEach-Object { Join-Path $_ $dll } | Where-Object { Test-Path $_ })[0]
        if (-not $src) { $missing += $dll; continue }
        if ($WhatIf) {
            Write-Host ("[runtime] would copy {0}  <- {1}" -f $dll, $src)
            # Walk the file that WOULD be copied, not the destination:
            # under -WhatIf nothing is copied, so $destPath does not exist
            # and enqueueing it made the walk miss its indirect imports (F08).
            $queue.Enqueue($src)
        } else {
            Copy-Item $src $destPath -Force
            Write-Host ("[runtime] copied {0}" -f $dll)
            $queue.Enqueue($destPath)
        }
        $copied++
    }
}

Write-Host ("[runtime] {0} import(s): {1} from the OS, {2} already here, {3} {4}" -f `
    $seen.Count, $system, $present, $copied, $(if ($WhatIf) { 'to copy' } else { 'copied' }))
if ($missing.Count -gt 0) {
    Write-Warning ("not found for: {0} (scrcpy may still fail to start)" -f ($missing -join ', '))
}

# ---------------------------------------------------------------------------
# Runtime companions (not DLL imports, so the walk above cannot see them)
#
# scrcpy looks for both adb and its own server *next to itself*, so they have to
# sit beside scrcpy.exe:
#   adb.exe / AdbWinApi.dll / AdbWinUsbApi.dll  -> scrcpy finds adb there
#   scrcpy-server                               -> what -Dportable=true looks for
# scrcpy-server MUST be the one built from work\scrcpy: the official release
# server does not know about --obs-video/--obs-audio.
#
# The exe directory is the only place that needs these companions, because the
# plugin starts nothing itself.
$companions = @(
    @{ Name = 'adb.exe';        Src = Join-Path $Repo 'scrcpy\scrcpy-win64-v5.0.1\adb.exe' },
    @{ Name = 'AdbWinApi.dll';  Src = Join-Path $Repo 'scrcpy\scrcpy-win64-v5.0.1\AdbWinApi.dll' },
    @{ Name = 'AdbWinUsbApi.dll'; Src = Join-Path $Repo 'scrcpy\scrcpy-win64-v5.0.1\AdbWinUsbApi.dll' },
    @{ Name = 'scrcpy-server';  Src = Join-Path $Repo 'work\scrcpy\build\server\scrcpy-server' }
)
$n = 0
foreach ($c in $companions) {
    if (-not (Test-Path $c.Src)) { Write-Warning ("companion missing: {0}" -f $c.Src); continue }
    $dst = Join-Path $dest $c.Name
    if (Test-Path $dst) {
        if ((Get-Item $c.Src).LastWriteTime -le (Get-Item $dst).LastWriteTime) { continue }
    }
    if ($WhatIf) {
        Write-Host ("[runtime] would copy {0}  -> {1}" -f $c.Name, $dest)
    } else {
        Copy-Item $c.Src $dst -Force
        Write-Host ("[runtime] copied {0}  -> {1}" -f $c.Name, $dest)
    }
    $n++
}
Write-Host ("[runtime] companions: {0} {1} (adb + AdbWinApi/UsbApi + scrcpy-server, next to scrcpy.exe)" -f `
    $n, $(if ($WhatIf) { 'to copy' } else { 'copied/refreshed' }))
