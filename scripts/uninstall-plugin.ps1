# Thin wrapper: removal is the same script run backwards.
# Kept as its own file so 'uninstall-plugin.ps1' is a name you can type.
# ASCII only on purpose -- same reason as scripts\check-panel-keys.ps1.

[CmdletBinding()]
param(
    [string] $ObsRoot = 'D:\obs-studio',
    [switch] $PurgeConfig,
    [switch] $Force
)

& (Join-Path $PSScriptRoot 'install-plugin.ps1') -ObsRoot $ObsRoot -Uninstall `
    -PurgeConfig:$PurgeConfig -Force:$Force
