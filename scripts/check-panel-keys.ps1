# Static check for the OBS property panel.
#
# Two mistakes that only show up as a broken/half-translated dialog in OBS and
# are much cheaper to catch here:
#   1. obs_module_text("X") with no X=... line in data/locale/<lang>.ini
#      (OBS then shows the raw key in the UI)
#   2. two properties with the same id in one panel (OBS silently keeps one)
#
# ASCII only on purpose: Windows PowerShell 5.1 reads .ps1 without a BOM as
# ANSI, so Chinese comments here would corrupt the file (see scripts/fix-bom.ps1).

[CmdletBinding()]
param(
    [string] $Repo = (Split-Path $PSScriptRoot -Parent)
)

$ErrorActionPreference = 'Stop'
$srcDir = Join-Path $Repo 'obs-plugin\src'
$locDir = Join-Path $Repo 'obs-plugin\data\locale'

function Get-IniKeys([string] $Path) {
    # Case-SENSITIVE on purpose: OBS looks module text up by the exact key, so
    # "Source.Name" and "source.name" are different keys and the first one shows
    # up in the UI as a raw key. A default PowerShell hashtable is
    # case-insensitive and hides exactly that mistake, so build an ordinal one.
    $keys = New-Object 'System.Collections.Generic.Dictionary[string,bool]' ([StringComparer]::Ordinal)
    foreach ($line in Get-Content $Path) {
        $m = [regex]::Match($line, '^\s*([A-Za-z0-9_.]+)\s*=')
        if ($m.Success) { $keys[$m.Groups[1].Value] = $true }
    }
    return $keys
}

$locales = @{}
foreach ($f in Get-ChildItem $locDir -Filter '*.ini') {
    $locales[$f.BaseName] = Get-IniKeys $f.FullName
}
if ($locales.Count -eq 0) { throw "no locale .ini files in $locDir" }

$failed = $false
foreach ($c in Get-ChildItem $srcDir -File | Where-Object { $_.Extension -in '.c', '.cpp' }) {
    $text = Get-Content $c.FullName -Raw

    # 1) every obs_module_text() key must exist in every locale. Keys that live in
    #    a table (CTL_AUDIO_SOURCES[].label_key) are picked up by the second pass.
    $keys = @([regex]::Matches($text, '(?:obs_module_text|T)\("([^"]+)"\)') |
              ForEach-Object { $_.Groups[1].Value } |
              Sort-Object -Unique)
    foreach ($lang in $locales.Keys | Sort-Object) {
        $missing = @($keys | Where-Object { -not $locales[$lang].ContainsKey($_) })
        if ($missing.Count -gt 0) {
            Write-Host ("[FAIL] {0}: {1} key(s) missing from {2}.ini" -f $c.Name, $missing.Count, $lang)
            foreach ($m in $missing) { Write-Host "         $m" }
            $failed = $true
        }
    }

    # 2) property ids must be unique inside a file (one function == one panel),
    #    counting the group ids too, since those share the same namespace
    $ids = @([regex]::Matches($text, 'obs_properties_add_(?:group|text|int|bool|list|button|float)\s*\(\s*\w+\s*,\s*"([^"]+)"') |
             ForEach-Object { $_.Groups[1].Value })
    $dupes = @($ids | Group-Object | Where-Object { $_.Count -gt 1 } | ForEach-Object { $_.Name })
    if ($dupes.Count -gt 0) {
        Write-Host ("[FAIL] {0}: duplicate property id(s): {1}" -f $c.Name, ($dupes -join ', '))
        $failed = $true
    }
    Write-Host ("[ok]   {0}: {1} module_text key(s), {2} property id(s)" -f $c.Name, $keys.Count, $ids.Count)
}

# 3) an unused key is dead weight, not an error: report it, do not fail
#    "Used" includes keys that only appear as string literals (label_key tables)
#    covered by this script.
foreach ($lang in $locales.Keys | Sort-Object) {
    $all = @()
    foreach ($f in Get-ChildItem $srcDir -File -Recurse | Where-Object { $_.Extension -in '.c', '.cpp' }) {
        $all += @([regex]::Matches((Get-Content $f.FullName -Raw), '(?:obs_module_text|T)\("([^"]+)"\)') |
                  ForEach-Object { $_.Groups[1].Value })
    }
    foreach ($extra in Get-ChildItem $Repo -Recurse -Include '*.c', '*.cpp', '*.ps1' -File |
                      Where-Object { $_.FullName -notmatch '\\(build|deps|work|\.tmp|buildenv|scrcpy)\\' }) {
        $all += @([regex]::Matches((Get-Content $extra.FullName -Raw -ErrorAction SilentlyContinue), '"((?:Capture|Source|Status)\.[A-Za-z0-9_.]+)"') |
                  ForEach-Object { $_.Groups[1].Value })
    }
    $unused = @($locales[$lang].Keys | Where-Object { $all -cnotcontains $_ } | Sort-Object)
    if ($unused.Count -gt 0) {
        Write-Host ("[note] {0}.ini: {1} key(s) not referenced anywhere: {2}" -f $lang, $unused.Count, ($unused -join ', '))
    }
}

if ($failed) { Write-Host '[FAIL] panel checks failed'; exit 1 }
Write-Host '[ok] panel keys and property ids are consistent'
# Explicit: callers (scripts\make-release.ps1) read $LASTEXITCODE, which would
# otherwise keep whatever value the shell had before this script ran.
exit 0
