<#
.SYNOPSIS
    Build the rscrcpy-audio OBS plugin with MinGW-w64 GCC.

.DESCRIPTION
    Compiles obs-plugin/src/*.c into obs-plugin/build/rscrcpy-audio.dll against
    the OBS import library generated in deps/obs-lib (obs.def + libobs.a).

    The import library and the headers are produced from a sparse checkout of
    the OBS sources in deps/obs-studio-src.

.PARAMETER Install
    Copy the built DLL into the OBS plugin directory (OBS must not be running).

.PARAMETER Clean
    Delete the build directory first.

.EXAMPLE
    .\obs-plugin\build.ps1
    .\obs-plugin\build.ps1 -Clean -Install
#>
[CmdletBinding()]
param(
    [switch] $Install,
    [switch] $Clean,
    [string] $ObsInstall = 'D:\obs-studio'
)

$ErrorActionPreference = 'Stop'

$Plugin = $PSScriptRoot
$Root   = Split-Path $Plugin -Parent
$ObsSrc = Join-Path $Root 'deps\obs-studio-src'
$ObsLib = Join-Path $Root 'deps\obs-lib'
$OutDir = Join-Path $Plugin 'build'
$Dll    = Join-Path $OutDir 'rscrcpy-audio.dll'
$BinDir = 'D:\CPLUSPLUS_Compiler\Strawberry\c\bin'
$Gcc    = Join-Path $BinDir 'gcc.exe'

foreach ($req in @(
    @{ Path = $Gcc;                                   What = 'MinGW gcc' },
    @{ Path = (Join-Path $ObsLib 'libobs.a');         What = 'OBS import library' },
    @{ Path = (Join-Path $ObsSrc 'libobs\obs.h');     What = 'libobs headers' },
    @{ Path = (Join-Path $ObsSrc 'deps\w32-pthreads\pthread.h'); What = 'w32-pthreads headers' }
)) {
    if (-not (Test-Path $req.Path)) { throw "$($req.What) not found: $($req.Path)" }
}

if ($Clean -and (Test-Path $OutDir)) {
    Remove-Item $OutDir -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

# The plugin is a consumer: the shared-memory / shared-texture readers and the
# two sources built on them. It speaks the protocol and nothing else.
$sources = @(Get-ChildItem (Join-Path $Plugin 'src') -Filter '*.c' -Recurse |
             Sort-Object FullName | ForEach-Object { $_.FullName })
if ($sources.Count -eq 0) { throw "no C sources in $(Join-Path $Plugin 'src')" }

$gccArgs = @(
    '-shared'
    '-O2'
    '-Wall'
    '-static-libgcc'
    '-static'
    # Fail the link on any unresolved symbol instead of producing a DLL that
    # silently fails to load inside OBS
    '-Wl,--no-undefined'
    '-o', $Dll
    '-I', (Join-Path $Plugin 'include')
    '-I', (Join-Path $Plugin 'src')
    # obs_shm_proto.h: included straight from the scrcpy source tree, so the
    # producer and the consumer can never diverge
    '-I', (Join-Path $Root 'work\scrcpy\app\src')
    '-I', (Join-Path $ObsSrc 'libobs')
    '-I', (Join-Path $ObsSrc 'deps\w32-pthreads')
) + $sources + @(
    # GNU ld resolves libraries left to right: -lobs must come after the
    # objects that reference it, and the .def export list last.
    # d3d11/dxgi/dxguid are for the adapter-LUID check in capture.c
    # (gs_get_device_obj() -> IDXGIDevice -> GetAdapter -> DXGI_ADAPTER_DESC),
    # which is what turns a silent black picture on a hybrid GPU into a clear
    # "different adapters" error.
    '-L', $ObsLib
    '-lobs'
    '-ld3d11'
    '-ldxgi'
    '-ldxguid'
    '-lshell32'
    '-luser32'
    '-lgdi32'
    '-lole32'
    '-loleaut32'
    '-luuid'
    '-ladvapi32'
    (Join-Path $Plugin 'rscrcpy-audio.def')
)

Write-Host "[rscrcpy-audio] compiling $($sources.Count) source(s)"

& $Gcc @gccArgs
if ($LASTEXITCODE -ne 0) {
    throw "gcc failed with exit code $LASTEXITCODE"
}

# The .def export list is critical: GCC does not emit PE exports for plain
# visibility("default") symbols, and OBS hard-fails on a missing
# obs_module_load / obs_module_set_pointer / obs_module_ver.
$required = @('obs_module_load', 'obs_module_unload',
              'obs_module_set_pointer', 'obs_module_ver',
              'obs_module_set_locale', 'obs_module_free_locale')
$gendef = Join-Path $BinDir 'gendef.exe'
if (Test-Path $gendef) {
    # gendef writes <dll>.def into the current directory, it has no usable -o
    Push-Location $OutDir
    try {
        & $gendef 'rscrcpy-audio.dll' | Out-Null
    } finally {
        Pop-Location
    }
    $checkDef = Join-Path $OutDir 'rscrcpy-audio.def'
    if (-not (Test-Path $checkDef)) { throw "gendef produced no output" }
    $exported = Get-Content $checkDef
    $missing = @($required | Where-Object { -not ($exported -match "^\s*$_\s*$") })
    Remove-Item $checkDef -Force
    if ($missing.Count -gt 0) {
        throw "exports missing from ${Dll}: $($missing -join ', ')"
    }
    Write-Host "[rscrcpy-audio] exports verified: $($required -join ', ')"
} else {
    Write-Warning "gendef.exe not found, skipping export verification"
}

$built = Get-Item $Dll
Write-Host ("[rscrcpy-audio] built {0} ({1:N0} bytes)" -f $built.Name, $built.Length)

# The plugin needs no per-machine paths: it launches nothing. Installing it
# therefore means the DLL and the data directory only; the runtime DLLs that sit
# next to scrcpy.exe are the job of scripts\deploy-runtime-dlls.ps1.

if ($Install) {
    $target = Join-Path $ObsInstall 'obs-plugins\64bit'
    if (-not (Test-Path $target)) { throw "OBS plugin directory not found: $target" }
    if (Get-Process obs64 -ErrorAction SilentlyContinue) {
        throw "OBS is running, close it before installing"
    }
    Copy-Item $Dll (Join-Path $target 'rscrcpy-audio.dll') -Force
    Write-Host "[rscrcpy-audio] installed $($built.Name) -> $target"

    # The locale .ini files and the module manifest live in the OBS *data*
    # directory, not next to the DLL.
    $dataSrc = Join-Path $Plugin 'data'
    if (Test-Path $dataSrc) {
        $dataDst = Join-Path $ObsInstall 'data\obs-plugins\rscrcpy-audio'
        New-Item -ItemType Directory -Force -Path $dataDst | Out-Null
        Copy-Item (Join-Path $dataSrc '*') $dataDst -Recurse -Force
        $n = (Get-ChildItem $dataDst -Recurse -File | Measure-Object).Count
        Write-Host "[rscrcpy-audio] installed $n data file(s) -> $dataDst"
    }
}
