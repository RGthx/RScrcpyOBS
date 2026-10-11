# Install (or remove) the built plugin in a local OBS Studio installation.
#
#   .\scripts\install-plugin.ps1                          # install into D:\obs-studio
#   .\scripts\install-plugin.ps1 -ObsRoot 'C:\Program Files\obs-studio'
#   .\scripts\uninstall-plugin.ps1                        # clean removal
#   .\scripts\install-plugin.ps1 -Uninstall -PurgeConfig  # also drop prefs/jobs
#
# obs-plugin\build.ps1 -Install already copies the files as a side effect of
# building; this script exists for the other direction (a real uninstall), for
# installing a build you already have, and for reporting the manifest version.
#
# What goes where (OBS layout):
#   <obs>\obs-plugins\64bit\rscrcpy-audio.dll
#   <obs>\data\obs-plugins\rscrcpy-audio\{manifest.json,locale\,ui\,adb.exe,...}
# User data is NOT touched unless -PurgeConfig:
#   %APPDATA%\obs-studio\plugin_config\rscrcpy-audio
#
# ASCII only on purpose -- same reason as scripts\check-panel-keys.ps1.

[CmdletBinding()]
param(
    [string] $ObsRoot = 'D:\obs-studio',
    [string] $Repo    = (Split-Path $PSScriptRoot -Parent),
    [string] $Dll     = '',
    [switch] $Uninstall,
    [switch] $PurgeConfig,
    [switch] $Force
)

$ErrorActionPreference = 'Stop'

function Say([string] $m) { Write-Host "[plugin] $m" }

$pluginId   = 'rscrcpy-audio'
$obsPlugins = Join-Path $ObsRoot 'obs-plugins\64bit'
$obsData    = Join-Path $ObsRoot "data\obs-plugins\$pluginId"
$dllTarget  = Join-Path $obsPlugins "$pluginId.dll"

if (-not (Test-Path $ObsRoot))    { throw "OBS not found: $ObsRoot (pass -ObsRoot)" }
if (-not (Test-Path $obsPlugins)) { throw "not an OBS install? missing $obsPlugins" }

if (Get-Process obs64 -ErrorAction SilentlyContinue) {
    throw "OBS is running -- close it first (the DLL is locked while loaded)."
}

if ($Uninstall) {
    $removed = 0
    if (Test-Path $dllTarget) {
        Remove-Item -LiteralPath $dllTarget -Force
        Say "removed $dllTarget"
        $removed++
    } else { Say "not installed: $dllTarget" }

    if (Test-Path $obsData) {
        # Guard against a wrong -ObsRoot: only recurse into something that is
        # recognisably ours, unless the caller insists with -Force.
        $looksOurs = (Test-Path (Join-Path $obsData 'manifest.json')) -or
                     (Test-Path (Join-Path $obsData 'locale'))
        if ($looksOurs -or $Force) {
            Remove-Item -LiteralPath $obsData -Recurse -Force
            Say "removed $obsData"
            $removed++
        } else {
            Write-Warning "refusing to delete $obsData (no manifest.json / locale there); use -Force if you are sure"
        }
    } else { Say "not installed: $obsData" }

    $cfg = Join-Path $env:APPDATA "obs-studio\plugin_config\$pluginId"
    if ($PurgeConfig) {
        if (Test-Path $cfg) {
            Remove-Item -LiteralPath $cfg -Recurse -Force
            Say "removed user config $cfg"
            $removed++
        } else { Say "no user config at $cfg" }
    } else {
        Say "kept user config; pass -PurgeConfig to remove it"
    }

    Say "uninstall done ($removed path(s) removed)"
    return
}

# ---------------------------------------------------------------- install
if (-not $Dll) { $Dll = Join-Path $Repo 'obs-plugin\build\rscrcpy-audio.dll' }
if (-not (Test-Path $Dll)) {
    throw "no built DLL at $Dll -- build it first:  . .\build_env.ps1 ; .\obs-plugin\build.ps1"
}

# Report the version from the manifest so the DLL and the manifest can never
# drift apart silently.
$version = '(no manifest)'
$manifest = Join-Path $Repo 'obs-plugin\data\manifest.json'
if (Test-Path $manifest) {
    $version = (Get-Content $manifest -Raw | ConvertFrom-Json).version
}

Copy-Item -LiteralPath $Dll -Destination $dllTarget -Force
Say "installed $(Split-Path $Dll -Leaf) (manifest version $version) -> $dllTarget"

$dataSrc = Join-Path $Repo 'obs-plugin\data'
if (Test-Path $dataSrc) {
    $items = @(Get-ChildItem -LiteralPath $dataSrc -Force)
    if ($items.Count -eq 0) { throw "plugin data dir is empty: $dataSrc" }
    New-Item -ItemType Directory -Force -Path $obsData | Out-Null
    foreach ($i in $items) {
        Copy-Item -LiteralPath $i.FullName -Destination $obsData -Recurse -Force
    }
    $n = (Get-ChildItem $obsData -Recurse -File | Measure-Object).Count
    Say "installed $n data file(s) -> $obsData"
} else {
    Write-Warning "no plugin data dir at $dataSrc (locale/manifest will be missing)"
}

Say "done -- manifest version $version"
