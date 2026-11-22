<#
.SYNOPSIS
    Installs (or removes) FC3PerformancePatch for Far Cry 3 and Far Cry 3 Blood Dragon on Windows.

.DESCRIPTION
    The patch itself is two files that can simply be copied into the game's "bin" folder.
    This script does that for you and adds the optional extras:

      * finds the game (registry, Steam libraries, or -GameDir)
      * copies version.dll and FC3PerformancePatch.ini into <game>\bin
      * enables the Large Address Aware flag on the game executables
      * optional: -EnableDXVK   installs 32-bit DXVK (Direct3D on Vulkan) into <game>\bin
      * optional: -DisableCFG   turns Control Flow Guard off for the game executables (needs admin)

    Every file that is modified or replaced is first copied to
    "<name>.fc3pp-<UTC date>-<time>.bak". Running the script again changes nothing.
    -Uninstall (or uninstall.bat) puts everything back.

.PARAMETER GameDir
    Game folder (the one that contains "bin"), or the "bin" folder itself. Skips auto-detection.

.PARAMETER Game
    Auto (default: every supported game that is found), FarCry3 or BloodDragon.

.PARAMETER EnableDXVK
    Download the pinned DXVK release, verify its SHA-256 and install the 32-bit d3d11.dll, d3d9.dll
    and dxgi.dll. Also sets D3D11MultithreadedRendering="1" in GamerProfile.xml, which is only
    recommended together with DXVK.

.PARAMETER DxvkArchive
    Use this already downloaded dxvk-<version>.tar.gz instead of downloading (hash is still verified).

.PARAMETER DisableCFG
    Disable Control Flow Guard for the game executables (Windows Exploit Protection, per program).
    This lowers a security mitigation for those executables; it is off unless you ask for it.

.PARAMETER NoLAA
    Do not touch the game executables (the DLL can still do it at first launch, see the INI file).

.PARAMETER ResetConfig
    Replace an existing FC3PerformancePatch.ini with the default one (the old one is backed up).

.PARAMETER Uninstall
    Remove the patch and restore every backup.

.PARAMETER KeepConfig
    With -Uninstall: leave FC3PerformancePatch.ini in place.

.PARAMETER Unattended
    Never ask questions; fail instead of prompting.

.EXAMPLE
    .\install.ps1
.EXAMPLE
    .\install.ps1 -GameDir "D:\Games\Far Cry 3" -EnableDXVK
.EXAMPLE
    .\install.ps1 -Uninstall
#>
[CmdletBinding()]
param(
    [string] $GameDir,
    [ValidateSet('Auto', 'FarCry3', 'BloodDragon')] [string] $Game = 'Auto',
    [string] $ProfileDir,
    [string] $SourceDir,
    [switch] $EnableDXVK,
    [string] $DxvkArchive,
    [switch] $DisableCFG,
    [switch] $NoLAA,
    [switch] $ResetConfig,
    [switch] $Uninstall,
    [switch] $KeepConfig,
    [switch] $Unattended,
    # Internal: the elevated half of -DisableCFG / its removal.
    [ValidateSet('', 'disable', 'restore')] [string] $InternalCfg = '',
    [string[]] $InternalCfgNames = @(),
    [string] $InternalCfgState = ''
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$PatchName = 'FC3PerformancePatch'
$DllName = 'version.dll'
$IniName = "$PatchName.ini"
$LogName = "$PatchName.log"
$ManifestName = "$PatchName.install.json"
$BackupInfix = '.fc3pp-'
$DllMarker = 'FC3PerformancePatch'      # present in our version.dll, used to tell it from other mods

# Pinned DXVK release (https://github.com/doitsujin/dxvk). Update all three together.
$DxvkVersion = '3.1.1'
$DxvkUrl = "https://github.com/doitsujin/dxvk/releases/download/v$DxvkVersion/dxvk-$DxvkVersion.tar.gz"
$DxvkSha256 = '40565b4a724aadc4433fa4e010b4b23916d9b1f1baeee64e17186db94f54e608'
$DxvkFiles = @('d3d11.dll', 'd3d9.dll', 'dxgi.dll')

$Games = @(
    [pscustomobject]@{
        Id = 'FarCry3'; Name = 'Far Cry 3'; SteamAppId = 220240; ProfileFolder = 'Far Cry 3'
        Exes = @('farcry3.exe', 'farcry3_d3d11.exe')
        Registry = @(
            @{ Key = 'HKLM:\SOFTWARE\WOW6432Node\Ubisoft\Far Cry 3'; Value = 'installdir' },
            @{ Key = 'HKLM:\SOFTWARE\Ubisoft\Far Cry 3'; Value = 'installdir' },
            @{ Key = 'HKLM:\SOFTWARE\WOW6432Node\Ubisoft\Launcher\Installs\46'; Value = 'InstallDir' },
            @{ Key = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Steam App 220240'; Value = 'InstallLocation' }
        )
    },
    [pscustomobject]@{
        Id = 'BloodDragon'; Name = 'Far Cry 3 Blood Dragon'; SteamAppId = 233270; ProfileFolder = 'Far Cry 3 Blood Dragon'
        Exes = @('fc3_blooddragon.exe', 'fc3_blooddragon_d3d11.exe')
        Registry = @(
            @{ Key = 'HKLM:\SOFTWARE\WOW6432Node\Ubisoft\Far Cry 3 Blood Dragon'; Value = 'installdir' },
            @{ Key = 'HKLM:\SOFTWARE\Ubisoft\Far Cry 3 Blood Dragon'; Value = 'installdir' },
            @{ Key = 'HKLM:\SOFTWARE\WOW6432Node\Ubisoft\Launcher\Installs\205'; Value = 'InstallDir' },
            @{ Key = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Steam App 233270'; Value = 'InstallLocation' }
        )
    }
)

# ---------------------------------------------------------------------------- output

function Write-Step([string] $Text) { Write-Host "  $Text" }
function Write-Note([string] $Text) { Write-Host "  note: $Text" -ForegroundColor Yellow }
function Write-Done([string] $Text) { Write-Host "  $Text" -ForegroundColor Green }

# ---------------------------------------------------------------------------- small helpers

function Get-Stamp { (Get-Date).ToUniversalTime().ToString('yyyyMMdd-HHmmss') }

# Deliberately not Get-FileHash: that is a script function which goes missing when
# Windows PowerShell is started from a PowerShell 7 session (inherited PSModulePath).
function Get-Sha256([string] $Path) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $bytes = $sha.ComputeHash([IO.File]::ReadAllBytes($Path))
    } finally {
        $sha.Dispose()
    }
    ([BitConverter]::ToString($bytes) -replace '-', '').ToLowerInvariant()
}

function Test-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    (New-Object Security.Principal.WindowsPrincipal $id).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

# Copies a file to "<file>.fc3pp-<stamp>.bak" and returns the backup's name.
function New-Backup([string] $Path) {
    $stamp = Get-Stamp
    $backup = "$Path$BackupInfix$stamp.bak"
    $n = 1
    while (Test-Path -LiteralPath $backup) { $backup = "$Path$BackupInfix$stamp-$n.bak"; $n++ }
    Copy-Item -LiteralPath $Path -Destination $backup
    Split-Path -Leaf $backup
}

# All backups of one file, oldest first (the timestamp in the name sorts correctly).
function Get-Backups([string] $Path) {
    $dir = Split-Path -Parent $Path
    $leaf = Split-Path -Leaf $Path
    if (-not (Test-Path -LiteralPath $dir)) { return @() }
    @(Get-ChildItem -LiteralPath $dir -File |
        Where-Object { $_.Name.StartsWith("$leaf$BackupInfix", [StringComparison]::OrdinalIgnoreCase) -and $_.Name.EndsWith('.bak') } |
        Sort-Object Name)
}

function Test-IsOurDll([string] $Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return $false }
    $bytes = [IO.File]::ReadAllBytes($Path)
    $text = [Text.Encoding]::ASCII.GetString($bytes)
    $text.Contains($DllMarker)
}

# ---------------------------------------------------------------------------- PE: Large Address Aware

# Returns a hashtable describing the PE headers, or $null if this is not a 32-bit x86 executable.
function Get-PeInfo([byte[]] $Bytes) {
    if ($Bytes.Length -lt 0x100 -or $Bytes[0] -ne 0x4D -or $Bytes[1] -ne 0x5A) { return $null }
    $pe = [BitConverter]::ToInt32($Bytes, 0x3C)
    if ($pe -lt 0 -or $pe + 24 + 68 -gt $Bytes.Length) { return $null }
    if ($Bytes[$pe] -ne 0x50 -or $Bytes[$pe + 1] -ne 0x45 -or $Bytes[$pe + 2] -ne 0 -or $Bytes[$pe + 3] -ne 0) { return $null }
    if ([BitConverter]::ToUInt16($Bytes, $pe + 4) -ne 0x014C) { return $null }      # i386
    if ([BitConverter]::ToUInt16($Bytes, $pe + 24) -ne 0x010B) { return $null }     # PE32
    @{
        CharacteristicsOffset = $pe + 22
        ChecksumOffset        = $pe + 24 + 64
        Characteristics       = [BitConverter]::ToUInt16($Bytes, $pe + 22)
        Checksum              = [BitConverter]::ToUInt32($Bytes, $pe + 24 + 64)
    }
}

function Get-PeChecksum([byte[]] $Bytes, [int] $ChecksumOffset) {
    [uint64] $sum = 0
    $len = $Bytes.Length
    $even = $len - ($len % 2)
    for ($i = 0; $i -lt $even; $i += 2) {
        if ($i -eq $ChecksumOffset -or $i -eq $ChecksumOffset + 2) { continue }
        $sum += [uint64]($Bytes[$i] + ($Bytes[$i + 1] * 256))
        $sum = ($sum -band 0xFFFF) + ($sum -shr 16)
    }
    if ($len % 2) {
        $sum += [uint64]$Bytes[$len - 1]
        $sum = ($sum -band 0xFFFF) + ($sum -shr 16)
    }
    $sum = ($sum -band 0xFFFF) + ($sum -shr 16)
    [uint32](($sum + [uint64]$len) -band 0xFFFFFFFFL)
}

# Sets the Large Address Aware flag. Returns 'patched', 'already' or 'not-pe'.
function Set-LargeAddressAware([string] $Path) {
    $bytes = [IO.File]::ReadAllBytes($Path)
    $info = Get-PeInfo $bytes
    if (-not $info) { return 'not-pe' }
    if ($info.Characteristics -band 0x0020) { return 'already' }
    [void](New-Backup $Path)
    $flags = [uint16]($info.Characteristics -bor 0x0020)
    $bytes[$info.CharacteristicsOffset] = [byte]($flags -band 0xFF)
    $bytes[$info.CharacteristicsOffset + 1] = [byte](($flags -shr 8) -band 0xFF)
    if ($info.Checksum -ne 0) {
        $sum = Get-PeChecksum $bytes $info.ChecksumOffset
        [BitConverter]::GetBytes([uint32]$sum).CopyTo($bytes, $info.ChecksumOffset)
    }
    [IO.File]::WriteAllBytes($Path, $bytes)
    'patched'
}

# True when two files differ only by the LAA flag byte and the four checksum bytes.
function Test-OnlyLaaDifference([string] $Current, [string] $Original) {
    $a = [IO.File]::ReadAllBytes($Current)
    $b = [IO.File]::ReadAllBytes($Original)
    if ($a.Length -ne $b.Length) { return $false }
    $info = Get-PeInfo $b
    if (-not $info) { return $false }
    $allowed = @($info.CharacteristicsOffset) + (0..3 | ForEach-Object { $info.ChecksumOffset + $_ })
    for ($i = 0; $i -lt $a.Length; $i++) {
        if ($a[$i] -ne $b[$i] -and $allowed -notcontains $i) { return $false }
    }
    $true
}

# ---------------------------------------------------------------------------- locating the game

function Get-RegistryValue([string] $Key, [string] $Value) {
    try {
        $item = Get-ItemProperty -LiteralPath $Key -ErrorAction Stop
        $prop = $item.PSObject.Properties[$Value]
        if ($prop) { return [string]$prop.Value }
    } catch { }
    $null
}

function Get-SteamRoots {
    $roots = @()
    foreach ($k in @(
            @{ Key = 'HKCU:\SOFTWARE\Valve\Steam'; Value = 'SteamPath' },
            @{ Key = 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam'; Value = 'InstallPath' },
            @{ Key = 'HKLM:\SOFTWARE\Valve\Steam'; Value = 'InstallPath' })) {
        $v = Get-RegistryValue $k.Key $k.Value
        if ($v) { $roots += $v.Replace('/', '\').TrimEnd('\') }
    }
    $override = [Environment]::GetEnvironmentVariable('FC3PP_STEAM_ROOT')     # used by the test suite
    if ($override) { $roots = @($override) }
    @($roots | Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -Unique)
}

# Parses Steam's libraryfolders.vdf: every "path" "<dir>" entry is a library.
function Get-SteamLibraries {
    $libs = @()
    foreach ($root in Get-SteamRoots) {
        $libs += $root
        foreach ($vdf in @("$root\steamapps\libraryfolders.vdf", "$root\config\libraryfolders.vdf")) {
            if (Test-Path -LiteralPath $vdf) {
                foreach ($line in [IO.File]::ReadAllLines($vdf)) {
                    if ($line -match '^\s*"path"\s+"(.+)"\s*$') {
                        $libs += $Matches[1].Replace('\\', '\').TrimEnd('\')
                    }
                }
            }
        }
    }
    @($libs | Where-Object { $_ } | Select-Object -Unique)
}

function Test-GameRoot($Def, [string] $Root) {
    if (-not $Root) { return $false }
    foreach ($exe in $Def.Exes) {
        if (Test-Path -LiteralPath (Join-Path $Root "bin\$exe")) { return $true }
    }
    $false
}

function Find-GameRoot($Def) {
    if (-not [Environment]::GetEnvironmentVariable('FC3PP_SKIP_REGISTRY')) {
        foreach ($r in $Def.Registry) {
            $v = Get-RegistryValue $r.Key $r.Value
            if ($v) {
                $v = $v.Replace('/', '\').TrimEnd('\')
                if (Test-GameRoot $Def $v) { return $v }
            }
        }
    }
    foreach ($lib in Get-SteamLibraries) {
        $candidate = Join-Path $lib "steamapps\common\$($Def.Name)"
        if (Test-GameRoot $Def $candidate) { return $candidate }
    }
    $null
}

# Returns @{ Def; Root } for every game to work on.
function Resolve-Targets {
    $defs = @($Games | Where-Object { $Game -eq 'Auto' -or $_.Id -eq $Game })
    $targets = @()
    if ($GameDir) {
        $root = $GameDir.TrimEnd('\', '/')
        if ((Split-Path -Leaf $root) -ieq 'bin') { $root = Split-Path -Parent $root }
        foreach ($d in $defs) {
            if (Test-GameRoot $d $root) { $targets += @{ Def = $d; Root = $root } }
        }
        if (-not $targets) { throw "No supported game executable was found under '$GameDir\bin'." }
        return $targets
    }
    foreach ($d in $defs) {
        $root = Find-GameRoot $d
        if ($root) { $targets += @{ Def = $d; Root = $root } }
    }
    if (-not $targets) {
        if ($Unattended) { throw 'The game was not found. Pass -GameDir "<folder that contains bin>".' }
        Write-Host ''
        Write-Host 'The game was not found automatically.'
        $answer = Read-Host 'Enter the game folder (the one that contains "bin")'
        if (-not $answer) { throw 'No folder given.' }
        $root = $answer.Trim('"', ' ').TrimEnd('\', '/')
        if ((Split-Path -Leaf $root) -ieq 'bin') { $root = Split-Path -Parent $root }
        foreach ($d in $defs) {
            if (Test-GameRoot $d $root) { $targets += @{ Def = $d; Root = $root } }
        }
        if (-not $targets) { throw "No supported game executable was found under '$root\bin'." }
    }
    $targets
}

function Get-ProfilePath($Def) {
    if ($ProfileDir) { return (Join-Path $ProfileDir 'GamerProfile.xml') }
    $docs = [Environment]::GetFolderPath('MyDocuments')
    Join-Path $docs "My Games\$($Def.ProfileFolder)\GamerProfile.xml"
}

function Assert-GameNotRunning($Def, [string] $Bin) {
    $prefix = [IO.Path]::GetFullPath($Bin).TrimEnd('\') + '\'
    foreach ($exe in $Def.Exes) {
        $name = [IO.Path]::GetFileNameWithoutExtension($exe)
        foreach ($process in @(Get-Process -Name $name -ErrorAction SilentlyContinue)) {
            # Only a game started from THIS folder keeps its files in use; a second
            # copy elsewhere does not. A process whose location cannot be read
            # (started elevated) is treated as this one, to be safe.
            $path = $null
            try { $path = $process.Path } catch { $path = $null }
            if (-not $path -or $path.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
                throw "$($Def.Name) is running ($exe). Close the game and run this again."
            }
        }
    }
}

# ---------------------------------------------------------------------------- manifest

function Read-Manifest([string] $Bin) {
    $path = Join-Path $Bin $ManifestName
    $m = @{ files = @(); dxvk = @(); cfg = @(); profileAttribute = $null }
    if (Test-Path -LiteralPath $path) {
        try {
            $j = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
            foreach ($p in $j.PSObject.Properties) {
                if ($p.Value -is [Array]) { $m[$p.Name] = @($p.Value) } else { $m[$p.Name] = $p.Value }
            }
        } catch {
            Write-Note 'the install record could not be read and is ignored'
        }
    }
    $m
}

function Write-Manifest([string] $Bin, $Manifest) {
    $Manifest.patchVersion = '1.1.0'
    $Manifest.updatedUtc = (Get-Date).ToUniversalTime().ToString('s') + 'Z'
    ($Manifest | ConvertTo-Json -Depth 5) | Set-Content -LiteralPath (Join-Path $Bin $ManifestName) -Encoding UTF8
}

function Add-Unique($Manifest, [string] $Key, [string] $Value) {
    if (@($Manifest[$Key]) -notcontains $Value) { $Manifest[$Key] = @($Manifest[$Key]) + $Value }
}

# ---------------------------------------------------------------------------- GamerProfile.xml

# Byte-exact attribute edit (the rest of the file is not re-encoded or re-formatted).
# Returns the previous value, or $null if the attribute is absent or already has the value.
function Set-ProfileAttribute([string] $Path, [string] $Name, [string] $Value, [switch] $NoBackup) {
    $latin1 = [Text.Encoding]::GetEncoding(28591)
    $text = $latin1.GetString([IO.File]::ReadAllBytes($Path))
    $rx = [regex]("(?<=\s)" + [regex]::Escape($Name) + '="([^"]*)"')
    $m = $rx.Match($text)
    if (-not $m.Success) { return $null }
    $old = $m.Groups[1].Value
    if ($old -eq $Value) { return $null }
    if (-not $NoBackup) { [void](New-Backup $Path) }
    $text = $text.Substring(0, $m.Index) + "$Name=`"$Value`"" + $text.Substring($m.Index + $m.Length)
    [IO.File]::WriteAllBytes($Path, $latin1.GetBytes($text))
    $old
}

# ---------------------------------------------------------------------------- DXVK

function Get-DxvkFolder {
    $archive = $DxvkArchive
    $temp = Join-Path ([IO.Path]::GetTempPath()) ("fc3pp-dxvk-" + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $temp | Out-Null
    if (-not $archive) {
        $archive = Join-Path $temp "dxvk-$DxvkVersion.tar.gz"
        Write-Step "downloading DXVK $DxvkVersion ..."
        [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
        $ProgressPreference = 'SilentlyContinue'
        Invoke-WebRequest -Uri $DxvkUrl -OutFile $archive -UseBasicParsing
    }
    if (-not (Test-Path -LiteralPath $archive)) { throw "DXVK archive not found: $archive" }
    $hash = Get-Sha256 $archive
    $expected = $DxvkSha256
    $override = [Environment]::GetEnvironmentVariable('FC3PP_TEST_DXVK_SHA256')  # used by the test suite
    if ($override) { $expected = $override }
    if ($hash -ne $expected) {
        throw "DXVK archive failed verification (SHA-256 $hash, expected $expected). Nothing was installed."
    }
    $tar = Join-Path $env:SystemRoot 'System32\tar.exe'
    if (-not (Test-Path -LiteralPath $tar)) { $tar = 'tar' }
    & $tar -xzf $archive -C $temp
    if ($LASTEXITCODE -ne 0) { throw 'Could not extract the DXVK archive (tar failed).' }
    $x32 = Get-ChildItem -LiteralPath $temp -Directory | ForEach-Object { Join-Path $_.FullName 'x32' } |
        Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    if (-not $x32) { throw 'The DXVK archive does not contain an x32 folder.' }
    @{ X32 = $x32; Temp = $temp }
}

# ---------------------------------------------------------------------------- Control Flow Guard

function Get-IfeoKey([string] $Exe) {
    "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\$Exe"
}

# Runs elevated. $State maps exe name -> 'existed' | 'new' for the restore step.
function Invoke-CfgElevatedPart {
    if (-not (Test-Admin)) { throw 'This step needs administrator rights.' }
    foreach ($exe in $InternalCfgNames) {
        if ($InternalCfg -eq 'disable') {
            Set-ProcessMitigation -Name $exe -Disable CFG
        } else {
            $prior = @{}
            foreach ($pair in ($InternalCfgState -split ';')) {
                $kv = $pair -split '=', 2
                if ($kv.Count -eq 2) { $prior[$kv[0]] = $kv[1] }
            }
            if ($prior[$exe] -eq 'existed') {
                # The program had its own Exploit Protection settings before: only undo ours.
                Set-ProcessMitigation -Name $exe -Enable CFG
            } else {
                Set-ProcessMitigation -Name $exe -Remove
            }
        }
    }
}

function Invoke-Cfg([string] $Mode, [string[]] $Exes, [string] $State) {
    if ([Environment]::GetEnvironmentVariable('FC3PP_TEST_NO_SYSTEM_CHANGES')) {
        Write-Step "(test mode) Control Flow Guard $Mode skipped for: $($Exes -join ', ')"
        return $true
    }
    $argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"",
        '-InternalCfg', $Mode, '-InternalCfgNames', ($Exes -join ','))
    if ($State) { $argList += @('-InternalCfgState', "`"$State`"") }
    try {
        if (Test-Admin) {
            $p = Start-Process powershell.exe -ArgumentList $argList -Wait -PassThru -WindowStyle Hidden
        } else {
            Write-Step 'Windows will ask for administrator permission for the Control Flow Guard change ...'
            $p = Start-Process powershell.exe -ArgumentList $argList -Wait -PassThru -Verb RunAs -WindowStyle Hidden
        }
        return ($p.ExitCode -eq 0)
    } catch {
        Write-Note "administrator permission was not granted: Control Flow Guard left unchanged"
        return $false
    }
}

# ---------------------------------------------------------------------------- install

function Install-Game($Target) {
    $def = $Target.Def
    $bin = Join-Path $Target.Root 'bin'
    Write-Host ''
    Write-Host "$($def.Name)"
    Assert-GameNotRunning $def $bin
    $manifest = Read-Manifest $bin
    $manifest.game = $def.Id

    # --- version.dll -----------------------------------------------------------
    $srcDll = Join-Path $SourceDir $DllName
    $dstDll = Join-Path $bin $DllName
    if (Test-Path -LiteralPath $dstDll) {
        if ((Get-Sha256 $dstDll) -eq (Get-Sha256 $srcDll)) {
            Write-Step "$DllName is already up to date"
        } elseif (Test-IsOurDll $dstDll) {
            Copy-Item -LiteralPath $srcDll -Destination $dstDll -Force
            Write-Step "$DllName updated"
        } else {
            $backup = New-Backup $dstDll
            Copy-Item -LiteralPath $srcDll -Destination $dstDll -Force
            Write-Note "another mod's $DllName was in the way; it was saved as $backup and will be restored on uninstall"
        }
    } else {
        Copy-Item -LiteralPath $srcDll -Destination $dstDll
        Write-Step "$DllName installed"
    }
    Add-Unique $manifest 'files' $DllName

    # --- INI ---------------------------------------------------------------------
    $srcIni = Join-Path $SourceDir $IniName
    $dstIni = Join-Path $bin $IniName
    if (-not (Test-Path -LiteralPath $dstIni)) {
        Copy-Item -LiteralPath $srcIni -Destination $dstIni
        Write-Step "$IniName installed"
    } elseif ($ResetConfig -and (Get-Sha256 $dstIni) -ne (Get-Sha256 $srcIni)) {
        $backup = New-Backup $dstIni
        Copy-Item -LiteralPath $srcIni -Destination $dstIni -Force
        Write-Step "$IniName reset to defaults (your version: $backup)"
    } else {
        Write-Step "$IniName kept (your settings are untouched)"
    }
    Add-Unique $manifest 'files' $IniName

    # --- Large Address Aware -------------------------------------------------------
    if ($NoLAA) {
        Write-Step 'Large Address Aware: skipped (-NoLAA)'
    } else {
        foreach ($exe in $def.Exes) {
            $path = Join-Path $bin $exe
            if (-not (Test-Path -LiteralPath $path)) { continue }
            switch (Set-LargeAddressAware $path) {
                'patched' { Write-Step "$exe : Large Address Aware enabled (original kept as a backup)" }
                'already' { Write-Step "$exe : already Large Address Aware" }
                default { Write-Note "$exe is not a 32-bit executable; left alone" }
            }
        }
    }

    # --- DXVK (optional) -------------------------------------------------------------
    if ($EnableDXVK) {
        $dxvk = Get-DxvkFolder
        try {
            foreach ($f in $DxvkFiles) {
                $src = Join-Path $dxvk.X32 $f
                $dst = Join-Path $bin $f
                if (-not (Test-Path -LiteralPath $src)) { throw "DXVK archive is missing x32\$f" }
                if ((Test-Path -LiteralPath $dst) -and (Get-Sha256 $dst) -eq (Get-Sha256 $src)) { continue }
                if (Test-Path -LiteralPath $dst) { [void](New-Backup $dst) }
                Copy-Item -LiteralPath $src -Destination $dst -Force
                Add-Unique $manifest 'dxvk' $f
            }
            $manifest.dxvkVersion = $DxvkVersion
            Write-Step "DXVK $DxvkVersion (32-bit) installed: $($DxvkFiles -join ', ')"
        } finally {
            Remove-Item -LiteralPath $dxvk.Temp -Recurse -Force -ErrorAction SilentlyContinue
        }

        # Multithreaded rendering is only switched on together with DXVK.
        $profilePath = Get-ProfilePath $def
        if (Test-Path -LiteralPath $profilePath) {
            $old = Set-ProfileAttribute $profilePath 'D3D11MultithreadedRendering' '1'
            if ($null -ne $old) {
                if (-not $manifest.profileAttribute) {
                    $manifest.profileAttribute = @{ name = 'D3D11MultithreadedRendering'; original = $old; set = '1' }
                }
                Write-Step 'GamerProfile.xml: D3D11MultithreadedRendering set to 1 (backup kept)'
            } else {
                Write-Step 'GamerProfile.xml: D3D11MultithreadedRendering already 1 or not present'
            }
        } else {
            Write-Note 'GamerProfile.xml not found yet (start the game once); multithreaded rendering was not changed'
        }
    } else {
        Write-Step 'GamerProfile.xml: not modified'
    }

    # --- Control Flow Guard (optional) ---------------------------------------------------
    if ($DisableCFG) {
        $exes = @($def.Exes | Where-Object { Test-Path -LiteralPath (Join-Path $bin $_) })
        $todo = @($exes | Where-Object { @($manifest.cfg | ForEach-Object { $_.exe }) -notcontains $_ })
        if ($todo) {
            $records = @($todo | ForEach-Object {
                    @{ exe = $_; prior = $(if (Test-Path -LiteralPath (Get-IfeoKey $_)) { 'existed' } else { 'new' }) }
                })
            if (Invoke-Cfg 'disable' $todo '') {
                $manifest.cfg = @($manifest.cfg) + $records
                Write-Step "Control Flow Guard disabled for: $($todo -join ', ')"
            }
        } else {
            Write-Step 'Control Flow Guard: already disabled by a previous run'
        }
    }

    Write-Manifest $bin $manifest
    Write-Done "$($def.Name): done"
}

# ---------------------------------------------------------------------------- uninstall

function Restore-OldestBackup([string] $Path, [switch] $Always) {
    $backups = @(Get-Backups $Path)
    if (-not $backups) { return 'none' }
    $oldest = $backups[0].FullName
    $result = 'restored'
    if (-not (Test-Path -LiteralPath $Path)) {
        Copy-Item -LiteralPath $oldest -Destination $Path
    } elseif ((Get-Sha256 $Path) -eq (Get-Sha256 $oldest)) {
        $result = 'unchanged'
    } elseif ($Always -or (Test-OnlyLaaDifference $Path $oldest)) {
        Copy-Item -LiteralPath $oldest -Destination $Path -Force
    } else {
        # The file was changed by something else since (a game update, for example): keep it.
        return 'kept'
    }
    foreach ($b in $backups) { Remove-Item -LiteralPath $b.FullName -Force }
    $result
}

function Uninstall-Game($Target) {
    $def = $Target.Def
    $bin = Join-Path $Target.Root 'bin'
    Write-Host ''
    Write-Host "$($def.Name)"
    Assert-GameNotRunning $def $bin
    $manifest = Read-Manifest $bin
    $did = $false

    # --- version.dll: remove ours, bring back another mod's if we displaced one ------
    $dll = Join-Path $bin $DllName
    if ((Test-Path -LiteralPath $dll) -and (Test-IsOurDll $dll)) {
        Remove-Item -LiteralPath $dll -Force
        Write-Step "$DllName removed"
        $did = $true
    }
    if ((Restore-OldestBackup $dll -Always) -eq 'restored') { Write-Step "the previous $DllName was restored"; $did = $true }

    # --- executables -----------------------------------------------------------------------
    foreach ($exe in $def.Exes) {
        $path = Join-Path $bin $exe
        switch (Restore-OldestBackup $path) {
            'restored' { Write-Step "$exe restored to the original"; $did = $true }
            'unchanged' { Write-Step "$exe was already original; backup removed"; $did = $true }
            'kept' { Write-Note "$exe has changed since it was patched (game update?); it was left alone and its backups were kept" }
        }
    }

    # --- DXVK ---------------------------------------------------------------------------------
    foreach ($f in @($manifest.dxvk)) {
        $path = Join-Path $bin $f
        if (Get-Backups $path) {
            [void](Restore-OldestBackup $path -Always)
            Write-Step "$f restored to the previous file"
        } elseif (Test-Path -LiteralPath $path) {
            Remove-Item -LiteralPath $path -Force
            Write-Step "$f (DXVK) removed"
        }
        $did = $true
    }

    # --- GamerProfile.xml: undo only our own change, keep everything the user changed since
    $profilePath = Get-ProfilePath $def
    if ($manifest.profileAttribute -and (Test-Path -LiteralPath $profilePath)) {
        $a = $manifest.profileAttribute
        $old = Set-ProfileAttribute $profilePath $a.name $a.original -NoBackup
        if ($null -ne $old) { Write-Step "GamerProfile.xml: $($a.name) set back to $($a.original)" }
        foreach ($b in (Get-Backups $profilePath)) { Remove-Item -LiteralPath $b.FullName -Force }
        $did = $true
    }

    # --- Control Flow Guard -----------------------------------------------------------------------
    if (@($manifest.cfg).Count) {
        $names = @($manifest.cfg | ForEach-Object { $_.exe })
        $state = (@($manifest.cfg | ForEach-Object { "$($_.exe)=$($_.prior)" }) -join ';')
        if (Invoke-Cfg 'restore' $names $state) {
            Write-Step "Control Flow Guard setting removed for: $($names -join ', ')"
        } else {
            Write-Note "Control Flow Guard is still disabled for: $($names -join ', ') (run uninstall again as administrator)"
        }
        $did = $true
    }

    # --- our own files ---------------------------------------------------------------------------------
    $ini = Join-Path $bin $IniName
    if (Test-Path -LiteralPath $ini) {
        if ($KeepConfig) {
            Write-Step "$IniName kept (-KeepConfig)"
        } else {
            Remove-Item -LiteralPath $ini -Force
            foreach ($b in (Get-Backups $ini)) { Remove-Item -LiteralPath $b.FullName -Force }
            Write-Step "$IniName removed"
            $did = $true
        }
    }
    foreach ($leftover in @($LogName, $ManifestName)) {
        $p = Join-Path $bin $leftover
        if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Force; $did = $true }
    }
    Get-ChildItem -LiteralPath $bin -File -Filter '*.fc3pp-new' -ErrorAction SilentlyContinue | Remove-Item -Force

    if ($did) { Write-Done "$($def.Name): the patch was removed" } else { Write-Step 'nothing to remove' }
}

# ---------------------------------------------------------------------------- main

if ($InternalCfg) {
    try { Invoke-CfgElevatedPart; exit 0 } catch { exit 1 }
}

try {
    if (-not $SourceDir) { $SourceDir = Split-Path -Parent $PSCommandPath }
    Write-Host "$PatchName $(if ($Uninstall) { 'uninstaller' } else { 'installer' })"

    if (-not $Uninstall) {
        foreach ($f in @($DllName, $IniName)) {
            if (-not (Test-Path -LiteralPath (Join-Path $SourceDir $f))) {
                throw "$f was not found next to this script. Download the release package (not the source code), or build it first."
            }
        }
    }
    $targets = @(Resolve-Targets)
    foreach ($t in $targets) {
        if ($Uninstall) { Uninstall-Game $t } else { Install-Game $t }
    }
    Write-Host ''
    if ($Uninstall) {
        Write-Host 'Finished.'
    } else {
        Write-Host 'Finished. Start the game as usual; the patch writes its log to'
        Write-Host "<game>\bin\$LogName. To remove everything again, run uninstall.bat."
    }
    exit 0
} catch {
    Write-Host ''
    Write-Host "ERROR: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}
