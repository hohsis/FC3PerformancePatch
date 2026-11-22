<#
.SYNOPSIS
    Removes FC3PerformancePatch and restores every file it changed.

.DESCRIPTION
    Thin wrapper around "install.ps1 -Uninstall"; accepts the same -GameDir, -Game,
    -ProfileDir, -KeepConfig and -Unattended parameters.

.EXAMPLE
    .\uninstall.ps1
.EXAMPLE
    .\uninstall.ps1 -GameDir "D:\Games\Far Cry 3" -KeepConfig
#>
[CmdletBinding()]
param(
    [string] $GameDir,
    [ValidateSet('Auto', 'FarCry3', 'BloodDragon')] [string] $Game = 'Auto',
    [string] $ProfileDir,
    [switch] $KeepConfig,
    [switch] $Unattended
)

$forward = @{ Uninstall = $true; Game = $Game }
if ($GameDir) { $forward.GameDir = $GameDir }
if ($ProfileDir) { $forward.ProfileDir = $ProfileDir }
if ($KeepConfig) { $forward.KeepConfig = $true }
if ($Unattended) { $forward.Unattended = $true }

& (Join-Path (Split-Path -Parent $PSCommandPath) 'install.ps1') @forward
exit $LASTEXITCODE
