# Prepare every third-party dependency this repo needs, from scratch.
#
#   .\scripts\fetch-deps.ps1                 # into <repo>\deps, \scrcpy, \buildenv
#   .\scripts\fetch-deps.ps1 -DepsRoot D:\x  # somewhere else
#   .\scripts\fetch-deps.ps1 -Force          # redo even if it looks present
#   .\scripts\fetch-deps.ps1 -UpdateLock     # record hashes of what was just fetched
#
# These dependencies are not committed: this script materializes them into deps/,
# scrcpy/ and buildenv/ from the network, or from files you already have.
# scripts/deps.lock.json holds the pinned SHA-256 of each download.
#
# ASCII only on purpose -- same reason as scripts\check-panel-keys.ps1
# (Windows PowerShell 5.1 reads a BOM-less .ps1 as ANSI).

[CmdletBinding()]
param(
    [string] $DepsRoot = (Split-Path $PSScriptRoot -Parent),
    [string] $Cache    = $null,          # defaults to <DepsRoot>\deps-cache
    [string] $Python   = 'D:\miniconda\python.exe',
    [string] $ObsTag   = '32.0.4',
    [string] $ScrcpyTag = 'v5.0.1',
    [string] $ScrcpyZip = '',             # use a zip you already have instead of downloading
    [string] $FfmpegVersion = '9.0.2',
    [string] $Sdl3Version   = '3.4.18',
    [string] $SevenZip = $null,          # explicit extractor for .7z (else auto-detect)
    [string] $Proxy    = '',             # e.g. http://127.0.0.1:10808; else env, else the Windows setting
    [switch] $Force,
    [switch] $UpdateLock,
    [switch] $SkipFfmpeg,                # handy while iterating on the rest
    [switch] $SkipScrcpyTree             # skip the official scrcpy release
)

$ErrorActionPreference = 'Stop'

$script:failures = New-Object System.Collections.Generic.List[string]

function Say([string] $m)  { Write-Host "[deps] $m" }
function Warn([string] $m) { Write-Warning "[deps] $m"; $script:failures.Add($m) | Out-Null }

# ---------------------------------------------------------------- lock file
$lockPath = Join-Path $PSScriptRoot 'deps.lock.json'
$lock = @{}
if (Test-Path $lockPath) {
    $raw = Get-Content $lockPath -Raw | ConvertFrom-Json
    foreach ($p in $raw.PSObject.Properties) { $lock[$p.Name] = $p.Value }
}
$lockedNow = [ordered]@{}
foreach ($k in $lock.Keys) { $lockedNow[$k] = $lock[$k] }

function Get-Sha256([string] $Path) {
    (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
}

# curl / pip / git all ignore the Windows proxy setting (that one belongs to
# WinINET), so a machine with v2rayN switched on still gets 20 s connect
# timeouts from a script. Translate it for them.
function Get-System-Proxy {
    $key = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Internet Settings'
    try { $p = Get-ItemProperty -Path $key -ErrorAction Stop } catch { return '' }
    if (-not $p.ProxyEnable) { return '' }
    $s = $p.ProxyServer
    if (-not $s) { return '' }
    if ($s -match '=') {
        # e.g. "http=127.0.0.1:10809;https=127.0.0.1:10808"
        foreach ($part in ($s -split ';')) {
            if ($part -match '^(?i)https=(.+)$') { return 'http://' + $Matches[1] }
            if ($part -match '^(?i)http=(.+)$')  { return 'http://' + $Matches[1] }
        }
        return ''
    }
    if ($s -notmatch '^[a-z]+://') { $s = 'http://' + $s }
    return $s
}

# v2rayN's core restarts on its own (and a stale subscription means the port may
# be open while nothing gets through), so never assume: probe before promising.
function Test-ProxyAlive([string] $ProxyUrl) {
    try {
        $u = [Uri] $ProxyUrl
        if (-not $u.Host) { return $false }
        $c = New-Object System.Net.Sockets.TcpClient
        $iar = $c.BeginConnect($u.Host, $u.Port, $null, $null)
        $ok = $iar.AsyncWaitHandle.WaitOne(1500, $false)
        $c.Close()
        return $ok
    } catch { return $false }
}

# Delete a directory without ever walking INTO a junction.
# (Remove-Item -Recurse on a reparse point can recurse into the target and wipe
#  the other repo -- exactly what would happen while the old junctions are still
#  in place, which is the normal upgrade path.)
function Remove-Safely([string] $Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return }
    $item = Get-Item -LiteralPath $Path -Force
    if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
        Say "removing junction (link only): $Path"
        [IO.Directory]::Delete($Path, $false)
        return
    }
    # Is this path only REACHABLE through a junction? Then it really lives
    # elsewhere, and deleting it would edit that other tree instead.
    $parent = [IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($Path))
    while ($parent) {
        $up = Get-Item -LiteralPath $parent -Force -ErrorAction SilentlyContinue
        if ($up -and ($up.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw ("refusing to delete '$Path': it is reached through the junction '$parent'. " +
                   "Remove the junction first (that is what the target argument is for).")
        }
        $next = [IO.Path]::GetDirectoryName($parent)
        if (-not $next -or $next -eq $parent) { break }
        $parent = $next
    }
    Remove-Item -LiteralPath $Path -Recurse -Force
}

# "Present" must mean present as a REAL directory. Test-Path follows junctions,
# so a junction would look like a satisfied dependency and later writes would go
# straight through the link into whatever it points at.
function Test-Real([string] $Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return $false }
    $item = Get-Item -LiteralPath $Path -Force
    return -not ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)
}

# curl.exe beats Invoke-WebRequest by ~9x on the FFmpeg package (measured:
# 239 KB/s vs 26 KB/s) and gives us resume + retry for free. Fall back to
# Invoke-WebRequest only if curl is missing (it ships with Windows 10+).
function Download-File([string] $Url, [string] $Dst) {
    $curl = Get-Command curl.exe -ErrorAction SilentlyContinue
    if ($curl) {
        $part = "$Dst.part"
        # --speed-limit/--speed-time abort a transfer that goes quiet (a 60 MB
        # download over a flaky link otherwise hangs forever), and
        # --retry-all-errors makes --retry cover that abort too.
        $common = @('-sS', '-L', '--fail', '--retry', '5', '--retry-delay', '2',
                    '--retry-all-errors', '--connect-timeout', '30',
                    '--speed-limit', '10240', '--speed-time', '60')
        & $curl.Source @common -C - -o $part $Url
        if ($LASTEXITCODE -ne 0) {
            # A complete .part makes "resume" trip over itself (HTTP 416): start over.
            Remove-Item -LiteralPath $part -Force -ErrorAction SilentlyContinue
            & $curl.Source @common -o $part $Url
        }
        if ($LASTEXITCODE -ne 0) { throw "curl failed for $Url" }
        Move-Item -LiteralPath $part -Destination $Dst -Force
    } else {
        $ProgressPreference = 'SilentlyContinue'
        Invoke-WebRequest -Uri $Url -OutFile $Dst -TimeoutSec 3600
    }
}

# Ask the server how big the file should be. Used to catch a truncated download
# BEFORE its hash gets recorded into the lock file (which would then "pin" a
# broken artifact forever).
function Get-Remote-Length([string] $Url) {
    $curl = Get-Command curl.exe -ErrorAction SilentlyContinue
    if (-not $curl) { return -1 }
    $h = & $curl.Source -sS -L -I -m 30 $Url 2>$null
    foreach ($line in $h) {
        if ($line -match '(?i)^content-length:\s*(\d+)') { return [int64] $Matches[1] }
    }
    return -1
}

# Download (or reuse a cached copy) and verify against the lock, if pinned.
function Get-Archive([string] $Url, [string] $FileName) {
    $dst = Join-Path $Cache $FileName
    if ((Test-Path $dst) -and -not $Force) {
        Say "cached  $FileName"
    } else {
        Say "download $FileName"
        Download-File $Url $dst
    }

    $have = (Get-Item -LiteralPath $dst).Length
    $want = Get-Remote-Length $Url
    if ($want -gt 0 -and $have -ne $want) {
        throw ("$FileName is $have bytes but the server reports $want" + "`n" +
               "  Looks truncated (interrupted download). Delete the cached file and re-run.")
    }
    $hash = Get-Sha256 $dst
    $pinned = $lock[$FileName]
    if ($pinned -and ($pinned -ne $hash)) {
        throw ("$FileName does not match scripts\deps.lock.json`n" +
               "  expected $pinned`n  actual   $hash`n" +
               "Upstream changed the file: do NOT silently accept it. Review, then -UpdateLock.")
    }
    if (-not $pinned) {
        Say "unpinned $FileName -> $hash"
    } else {
        Say "verified $FileName"
    }
    $lockedNow[$FileName] = $hash
    return $dst
}

# ---------------------------------------------------------------- extraction
function Resolve-SevenZip {
    if ($SevenZip) {
        if (-not (Test-Path $SevenZip)) { throw "SevenZip not found: $SevenZip" }
        return $SevenZip
    }
    foreach ($n in '7z.exe', '7za.exe', '7zr.exe') {
        $c = Get-Command $n -ErrorAction SilentlyContinue
        if ($c) { return $c.Source }
    }
    foreach ($p in 'C:\Program Files\7-Zip\7z.exe', 'D:\7-Zip\7z.exe', 'D:\bandizip\bz.exe') {
        if (Test-Path $p) { return $p }
    }
    # Windows ships bsdtar (libarchive), which reads 7z as well as zip.
    $tar = Get-Command tar.exe -ErrorAction SilentlyContinue
    if ($tar) { return $tar.Source }
    return $null
}

# Extract an archive into a directory. Returns the list of top-level entries.
function Expand-ArchiveTo([string] $Archive, [string] $Dest) {
    New-Item -ItemType Directory -Force -Path $Dest | Out-Null
    $exe = Resolve-SevenZip
    if (-not $exe) { throw "no extractor available for $Archive (install 7-Zip or use -SevenZip)" }
    $name = Split-Path $exe -Leaf
    Say ("extract {0} with {1}" -f (Split-Path $Archive -Leaf), $name)

    if ($name -eq 'bz.exe') {
        # Bandizip CLI: bz x -y -o:<dir> <archive>
        & $exe x -y ("-o:" + $Dest) $Archive | Out-Null
    } elseif ($name -like 'tar*') {
        # bsdtar picks the format from the file itself
        & $exe -xf $Archive -C $Dest
    } else {
        & $exe x -y -bso0 -bsp0 ("-o" + $Dest) $Archive | Out-Null
    }
    if ($LASTEXITCODE -ne 0) { throw "$name failed on $Archive (exit $LASTEXITCODE)" }
    return @(Get-ChildItem $Dest -Force | Where-Object { $_.Name -notlike '_*' })
}

# ---------------------------------------------------------------- 1) ffmpeg
function Fetch-Ffmpeg([string] $Target) {
    $url = "https://www.gyan.dev/ffmpeg/builds/packages/ffmpeg-$FfmpegVersion-full_build-shared.7z"
    $zip = Get-Archive $url "ffmpeg-$FfmpegVersion-full_build-shared.7z"

    $stage = Join-Path $Cache 'x-ffmpeg'
    Remove-Safely $stage
    $null = Expand-ArchiveTo $zip $stage

    # gyan packs a single "ffmpeg-<ver>-full_build-shared\" root; find bin/include/lib under it.
    $root = Get-ChildItem $stage -Directory | Select-Object -First 1
    if (-not $root) { throw "ffmpeg archive layout not understood" }

    Remove-Safely $Target
    New-Item -ItemType Directory -Force -Path $Target | Out-Null
    foreach ($sub in 'bin', 'include', 'lib') {
        $src = Join-Path $root.FullName $sub
        if (-not (Test-Path $src)) { throw "ffmpeg archive has no $sub\" }
        Copy-Item $src (Join-Path $Target $sub) -Recurse -Force
    }

    # HAND PATCH 1/2: gyan's build ships no pkg-config files at all, so meson
    # cannot find FFmpeg. These five .pc files are written by hand here.
    # The versions are NOT guessed either: read them back from the DLLs, which
    # report themselves as e.g. "libavcodec  63.  1.102 / 63.  1.102".
    $ver = @{}
    $ff = Join-Path $Target 'bin\ffmpeg.exe'
    if (Test-Path $ff) {
        foreach ($line in (& $ff -version 2>&1)) {
            if ($line -match '^\s*lib(\w+)\s+(\d+)\.\s*(\d+)\.\s*(\d+)') {
                $ver['lib' + $Matches[1]] = "$($Matches[2]).$($Matches[3]).$($Matches[4])"
            }
        }
    }
    $pcDir = Join-Path $Target 'lib\pkgconfig'
    New-Item -ItemType Directory -Force -Path $pcDir | Out-Null
    $pc = @(
        @{ n = 'libavcodec';    d = 'FFmpeg codec library';                        l = 'avcodec'    },
        @{ n = 'libavformat';   d = 'FFmpeg container format library';             l = 'avformat'   },
        @{ n = 'libavutil';     d = 'FFmpeg utility library';                      l = 'avutil'     },
        @{ n = 'libswresample'; d = 'FFmpeg audio resampling library';             l = 'swresample' },
        @{ n = 'libswscale';    d = 'FFmpeg image scaling and colorspace conversion library'; l = 'swscale' }
    )
    foreach ($p in $pc) {
        $v = $ver[$p.n]
        if (-not $v) { Warn "no version reported for $($p.n); sdl/ffmpeg detection may fail"; $v = '0.0.0' }
        $text = @"
prefix=`${pcfiledir}/../..
exec_prefix=`${prefix}
libdir=`${prefix}/lib
includedir=`${prefix}/include

Name: $($p.n)
Description: $($p.d) (gyan.dev shared build)
Version: $v
Libs: -L`${libdir} -l$($p.l)
Cflags: -I`${includedir}
"@
        Set-Content -LiteralPath (Join-Path $pcDir ($p.n + '.pc')) -Value $text -Encoding ascii -NoNewline
    }
    Say ("wrote 5 hand-made .pc files (gyan ships none); libavcodec=" + $ver['libavcodec'])
    Remove-Safely $stage
}

# ---------------------------------------------------------------- 2) sdl3
function Fetch-Sdl3([string] $Target) {
    $url = "https://github.com/libsdl-org/SDL/releases/download/release-$Sdl3Version/SDL3-devel-$Sdl3Version-mingw.zip"
    $zip = Get-Archive $url "SDL3-devel-$Sdl3Version-mingw.zip"

    $stage = Join-Path $Cache 'x-sdl3'
    Remove-Safely $stage
    $null = Expand-ArchiveTo $zip $stage

    # The zip ships i686 and x86_64 side by side, so matching "any directory that
    # contains include\SDL3" is ambiguous. Pin the architecture, then verify the
    # payload's PE machine below.
    $archName = 'x86_64-w64-mingw32'
    $arch = Get-ChildItem $stage -Directory -Recurse -Filter $archName -ErrorAction SilentlyContinue |
            Select-Object -First 1
    if (-not $arch) { throw "SDL3 archive has no $archName directory" }
    if (-not (Test-Path (Join-Path $arch.FullName 'include\SDL3'))) {
        throw "$archName does not contain include\SDL3"
    }

    Remove-Safely $Target
    New-Item -ItemType Directory -Force -Path $Target | Out-Null
    foreach ($sub in 'bin', 'include', 'lib', 'share') {
        $src = Join-Path $arch.FullName $sub
        if (Test-Path $src) { Copy-Item $src (Join-Path $Target $sub) -Recurse -Force }
    }

    # Hard check: a 32-bit SDL3.dll here means every later link fails in a
    # confusing way, so refuse it now.
    $dll = Join-Path $Target 'bin\SDL3.dll'
    if (Test-Path $dll) {
        $bytes = [IO.File]::ReadAllBytes($dll)
        $peOffset = [BitConverter]::ToInt32($bytes, 0x3C)
        $machine = [BitConverter]::ToUInt16($bytes, $peOffset + 4)
        if ($machine -ne 0x8664) {
            throw ("bin\SDL3.dll is not x64 (PE machine 0x{0:X}) -- wrong architecture picked" -f $machine)
        }
        Say "SDL3.dll verified x64"
    } else {
        Warn 'SDL3 archive had no bin\SDL3.dll'
    }

    # HAND PATCH 2/2: the shipped sdl3.pc carries -mwindows, which turns any
    # linked exe into a console-less GUI binary -- scrcpy then logs nothing at
    # all. Strip it from the local deps\ copy.
    $pcDir = Join-Path $Target 'lib\pkgconfig'
    $pc3 = Join-Path $pcDir 'sdl3.pc'
    if (Test-Path $pc3) {
        $t = Get-Content $pc3 -Raw
        $before = $t
        $t = $t -replace '(?m)^(Libs:.*?)\s+-mwindows\s*$', '$1'
        $t = $t -replace '(?m)^(Cflags:.*?)\s+-mwindows\s*$', '$1'
        if ($t -ne $before) {
            Set-Content -LiteralPath $pc3 -Value $t -Encoding ascii -NoNewline
            Say "stripped -mwindows from sdl3.pc"
        } else {
            Say "sdl3.pc has no -mwindows (fine)"
        }
    } else {
        Warn "sdl3.pc not found in $pcDir -- meson will not see SDL3"
    }
    Remove-Safely $stage
}

# ---------------------------------------------------------------- 3) obs-studio headers
function Fetch-ObsHeaders([string] $Target) {
    # Sparse, partial clone: the plugin needs libobs headers + w32-pthreads +
    # cmake, i.e. ~15 MB instead of ~700 MB. The tag is immutable, so pin it.
    $want = @('cmake', 'deps/w32-pthreads', 'libobs')
    if ((Test-Real $Target) -and (Test-Path (Join-Path $Target '.git'))) {
        Say "obs-studio-src: existing checkout, moving it to $ObsTag"
        & git -C $Target fetch --quiet --depth 1 origin "refs/tags/$ObsTag"
        if ($LASTEXITCODE -ne 0) { throw "git fetch failed for obs-studio" }
        & git -C $Target checkout --quiet FETCH_HEAD
        if ($LASTEXITCODE -ne 0) { throw "git checkout FETCH_HEAD failed" }
    } else {
        Remove-Safely $Target
        Say "clone obs-studio $ObsTag (sparse, blob:none, depth 1)"
        & git clone --quiet --filter=blob:none --no-checkout --depth 1 `
            --branch $ObsTag https://github.com/obsproject/obs-studio.git $Target
        if ($LASTEXITCODE -ne 0) { throw "git clone failed for obs-studio" }
    }

    # --no-checkout first, then set the cone patterns, then materialise: doing it
    # in that order avoids fetching the whole tree. git prints nothing on success,
    # so $LASTEXITCODE is the only signal that the checkout worked.
    & git -C $Target sparse-checkout set --cone @want
    if ($LASTEXITCODE -ne 0) { throw "git sparse-checkout set failed" }
    & git -C $Target checkout --quiet
    if ($LASTEXITCODE -ne 0) { throw "git checkout of the sparse tree failed" }

    foreach ($sub in $want) {
        if (-not (Test-Path (Join-Path $Target $sub))) {
            throw "obs-studio-src is missing $sub\ after checkout"
        }
    }
}

# ---------------------------------------------------------------- 4) obs import lib
function Fetch-ObsLib([string] $Target) {
    $gen = Join-Path $PSScriptRoot 'deps-obs.ps1'
    if (-not (Test-Path $gen)) { Warn "scripts\deps-obs.ps1 is missing; skipping obs-lib"; return }
    # deps-obs.ps1 needs gendef/dlltool on PATH, which build_env.ps1 provides.
    # (. build_env.ps1 is safe even before buildenv\ exists -- it only edits PATH.)
    $envScript = Join-Path (Split-Path $PSScriptRoot -Parent) 'build_env.ps1'
    if (Test-Path $envScript) { . $envScript | Out-Null }
    Say "obs-lib: generating from the installed obs.dll (scripts/deps-obs.ps1)"
    # deps-obs.ps1 copies INTO the target; drop a junction first so nothing lands
    # in the directory the link points at.
    Remove-Safely $Target
    try {
        & $gen -OutDir $Target
    } catch {
        Warn ("deps-obs.ps1 failed: " + $_.Exception.Message)
    }
}

# ---------------------------------------------------------------- 5) scrcpy release
# The official release is the only piece here that GitHub exclusively hosts, and
# GitHub release downloads are frequently unreachable from CN networks (the
# previous repo hit exactly this). So -ScrcpyZip takes a zip
# you already have; otherwise we try GitHub and tell you where to drop the file.
# Either way the PAYLOAD is verified by file hash, so a mirror or a hand-copied
# zip is as trustworthy as the official download -- that is also why these are
# file hashes and not an archive hash.
$ScrcpyFileHashes = [ordered]@{
    'adb.exe'          = 'B4A6B455702684652CCCF7B46258B29E653538904359A58FD4931CF3EF286B3F'
    'AdbWinApi.dll'    = 'C1D653030B4BDE65D3E07E4D0B0979E17BE56DF1436CDD15528630F27808050D'
    'AdbWinUsbApi.dll' = '0710E894D9B40F71A670C13C694079D564C92C1279DA382CFE4850983AAEBE1B'
    'scrcpy.exe'       = '64BCEE493D9243DAB1E84093928014348738E31F3225E67558210E6B6CFB9CD6'
    'scrcpy-server'    = '764EB6F79811D5211FE9DF341120882BA9994C7A61B897D7BF3FB662E53BC536'
}

function Fetch-ScrcpyRelease([string] $Target) {
    $fileName = "scrcpy-win64-$ScrcpyTag.zip"
    if ($ScrcpyZip) {
        if (-not (Test-Path -LiteralPath $ScrcpyZip)) { throw "ScrcpyZip not found: $ScrcpyZip" }
        $zip = Join-Path $Cache $fileName
        if (-not (Test-Path $zip) -or $Force) {
            if ([IO.Path]::GetFullPath($ScrcpyZip) -ne [IO.Path]::GetFullPath($zip)) {
                Copy-Item -LiteralPath $ScrcpyZip -Destination $zip -Force
            }
        }
        Say "using the archive you supplied: $ScrcpyZip"
    } else {
        $url = "https://github.com/Genymobile/scrcpy/releases/download/$ScrcpyTag/$fileName"
        try {
            $zip = Get-Archive $url $fileName
        } catch {
            throw (@("could not fetch the official scrcpy release",
                     "  " + $_.Exception.Message,
                     "  GitHub release downloads are often blocked. Download",
                     "    $fileName",
                     "  by hand, then either pass -ScrcpyZip <path>, or drop it into",
                     "    $Cache",
                     "  and re-run (it will be picked up as cached).") -join "`n")
        }
    }

    $stage = Join-Path $Cache 'x-scrcpy'
    Remove-Safely $stage
    $null = Expand-ArchiveTo $zip $stage

    $root = Get-ChildItem $stage -Directory | Select-Object -First 1
    if (-not $root) { throw "scrcpy archive layout not understood (no top-level directory)" }
    if ($root.Name -ne "scrcpy-win64-$ScrcpyTag") {
        Say "note: archive root is '$($root.Name)', expected scrcpy-win64-$ScrcpyTag"
    }

    foreach ($name in $ScrcpyFileHashes.Keys) {
        $f = Join-Path $root.FullName $name
        if (-not (Test-Path $f)) { throw "scrcpy release is missing $name" }
        $actual = Get-Sha256 $f
        if ($actual -ne $ScrcpyFileHashes[$name]) {
            throw (@("$name does not match the pinned hash (wrong build or corrupt payload)",
                     "  expected $($ScrcpyFileHashes[$name])",
                     "  actual   $actual") -join "`n")
        }
    }
    Say "payload verified against $($ScrcpyFileHashes.Count) pinned file hash(es)"

    $dest = Join-Path $Target $root.Name
    # Drop the TARGET first (it may be a junction): Remove-Safely would refuse to
    # delete $dest otherwise, because $dest is then reached through that link.
    Remove-Safely $Target
    New-Item -ItemType Directory -Force -Path $Target | Out-Null
    Copy-Item $root.FullName $dest -Recurse -Force
    Remove-Safely $stage
}

# ---------------------------------------------------------------- 6) buildenv venv
function Fetch-BuildEnv([string] $Target) {
    if ((Test-Real $Target) -and (Test-Path (Join-Path $Target 'Scripts\meson.exe'))) {
        Say "buildenv already has meson -- skipping"
        return
    }
    if (-not (Test-Path $Python)) { Warn "python not found at $Python (pass -Python)"; return }
    Remove-Safely $Target
    Say "venv from $Python"
    # Keep pip's cache and temp on the same drive as the repo: the project rule
    # is "nothing lands on C:", and a cache on C: also made the first real run
    # sit for half an hour instead of hitting the wheels it already had.
    $pipCache = Join-Path (Split-Path $Target -Parent) '.pipcache'
    New-Item -ItemType Directory -Force -Path $pipCache | Out-Null
    $env:PIP_CACHE_DIR = $pipCache
    & $Python -m venv $Target
    if ($LASTEXITCODE -ne 0) { Warn "python -m venv failed"; return }
    # Versions pinned to what the working setup had.
    # --no-input + explicit timeouts: a stalled index must fail in bounded time,
    # not hang the whole script.
    & (Join-Path $Target 'Scripts\pip.exe') install --quiet --disable-pip-version-check `
        --no-input --progress-bar off --timeout 30 --retries 3 `
        'meson==1.12.1' 'pkgconf==3.0.7.post0'
    if ($LASTEXITCODE -ne 0) { Warn "pip install failed; buildenv is incomplete"; return }
    Say ("meson " + (& (Join-Path $Target 'Scripts\meson.exe') --version))
}

# ---------------------------------------------------------------- run
# Relative -DepsRoot / -Cache are resolved against the repo root, never the CWD.
$repoRoot = Split-Path $PSScriptRoot -Parent
if (-not [IO.Path]::IsPathRooted($DepsRoot)) { $DepsRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot $DepsRoot)) }
if (-not $Cache) { $Cache = Join-Path $DepsRoot 'deps-cache' }
if (-not [IO.Path]::IsPathRooted($Cache))    { $Cache    = [IO.Path]::GetFullPath((Join-Path $repoRoot $Cache)) }
New-Item -ItemType Directory -Force -Path $Cache | Out-Null
Say "target   $DepsRoot"
Say "cache    $Cache"

# ---- proxy: the thing that decides whether GitHub works at all ----
# Priority: -Proxy, then HTTP(S)_PROXY from the environment, then the Windows
# setting. Exported to the child processes (curl, pip, git all read env vars).
$proxySource   = ''
$effectiveProxy = $Proxy
if (-not $effectiveProxy) { $effectiveProxy = $env:HTTPS_PROXY; if ($effectiveProxy) { $proxySource = 'HTTPS_PROXY' } }
if (-not $effectiveProxy) { $effectiveProxy = $env:HTTP_PROXY;  if ($effectiveProxy) { $proxySource = 'HTTP_PROXY'  } }
if (-not $effectiveProxy) {
    $effectiveProxy = Get-System-Proxy
    if ($effectiveProxy) { $proxySource = 'Windows system proxy' }
}
if (-not $effectiveProxy) { $effectiveProxy = $env:ALL_PROXY; if ($effectiveProxy) { $proxySource = 'ALL_PROXY' } }

if ($effectiveProxy) {
    if (Test-ProxyAlive $effectiveProxy) {
        foreach ($n in 'HTTP_PROXY', 'HTTPS_PROXY', 'http_proxy', 'https_proxy') {
            Set-Item -Path "Env:$n" -Value $effectiveProxy
        }
        Say "proxy    $effectiveProxy  (from $proxySource; exported to curl/pip/git)"
    } else {
        Warn (@("a proxy is configured ($effectiveProxy, from $proxySource) but nothing answers there.",
                "  GitHub downloads will fail until it is back. Nothing else needs it.",
                "  Start v2rayN (refresh its subscription if it went stale) and re-run, or:",
                "    -Proxy <url>            point at a working proxy",
                "    -ScrcpyZip <zip path>   supply the one file that GitHub exclusively hosts",
                "  (a half-finished run is fine: downloaded archives are kept in",
                "   $Cache and reused.)") -join "`n")
        $effectiveProxy = ''
    }
} else {
    Say 'proxy    none configured (GitHub may still be directly reachable)'
}

# Everything downloaded before the proxy was needed stays valid: the cache is
# keyed by file name, so a re-run after fixing the proxy only fetches the gap.

# One step failing must not abandon the others (a blocked GitHub must not cost
# you the 250 MB FFmpeg you already downloaded). Collected and reported at the end.
function Step([string] $Label, [scriptblock] $Body) {
    try { & $Body } catch { Warn ("$Label failed: " + $_.Exception.Message) }
}

if (-not $SkipFfmpeg) {
    $t = Join-Path $DepsRoot 'deps\ffmpeg'
    if ((Test-Real $t) -and (Test-Path (Join-Path $t 'bin')) -and -not $Force) { Say 'ffmpeg already present' }
    else { Step 'ffmpeg' { Fetch-Ffmpeg $t } }
}
$t = Join-Path $DepsRoot 'deps\sdl3'
if ((Test-Real $t) -and (Test-Path (Join-Path $t 'include\SDL3')) -and -not $Force) { Say 'sdl3 already present' }
else { Step 'sdl3' { Fetch-Sdl3 $t } }

$t = Join-Path $DepsRoot 'deps\obs-studio-src'
if ((Test-Real $t) -and (Test-Path (Join-Path $t 'libobs\obs.h')) -and -not $Force) { Say 'obs-studio-src already present' }
else { Step 'obs-studio-src' { Fetch-ObsHeaders $t } }

$t = Join-Path $DepsRoot 'deps\obs-lib'
if ((Test-Real $t) -and (Test-Path (Join-Path $t 'libobs.a')) -and -not $Force) { Say 'obs-lib already present' }
else { Step 'obs-lib' { Fetch-ObsLib $t } }

if (-not $SkipScrcpyTree) {
    $t = Join-Path $DepsRoot 'scrcpy'
    if ((Test-Real $t) -and (Test-Path (Join-Path $t "scrcpy-win64-$ScrcpyTag\scrcpy-server")) -and -not $Force) {
        Say 'scrcpy release already present'
    } else {
        Step 'scrcpy release' { Fetch-ScrcpyRelease $t }
    }
}

$t = Join-Path $DepsRoot 'buildenv'
if ((Test-Real $t) -and (Test-Path (Join-Path $t 'Scripts\meson.exe')) -and -not $Force) { Say 'buildenv already present' }
else { Step 'buildenv' { Fetch-BuildEnv $t } }

# ---------------------------------------------------------------- lock file
if ($UpdateLock) {
    $sorted = [ordered]@{}
    foreach ($k in ($lockedNow.Keys | Sort-Object)) { $sorted[$k] = $lockedNow[$k] }
    ($sorted | ConvertTo-Json) | Set-Content -LiteralPath $lockPath -Encoding ascii
    Say "wrote $lockPath"
}

if ($script:failures.Count -gt 0) {
    Write-Host ''
    Write-Host "[deps] finished WITH PROBLEMS:"
    foreach ($f in $script:failures) { Write-Host "  - $f" }
    exit 1
}
Say 'all dependencies are in place'
