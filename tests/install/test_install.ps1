<#
.SYNOPSIS
    Tests for scripts\install.ps1 / uninstall.ps1 against a throw-away fake Steam library.

.DESCRIPTION
    Nothing outside a temporary folder is touched: game detection is redirected with the
    FC3PP_STEAM_ROOT / FC3PP_SKIP_REGISTRY environment variables, the profile folder with
    -ProfileDir, and system changes (Control Flow Guard) are disabled with
    FC3PP_TEST_NO_SYSTEM_CHANGES.

.PARAMETER PatchDll
    A built version.dll to install. Without it a stand-in file is used.
#>
[CmdletBinding()]
param([string] $PatchDll)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSCommandPath))
$scripts = Join-Path $repo 'scripts'
$script:Checks = 0
$script:Failures = 0
$script:Current = ''

function Check([bool] $Condition, [string] $Message) {
    $script:Checks++
    if (-not $Condition) {
        $script:Failures++
        Write-Host "  FAIL [$script:Current] $Message" -ForegroundColor Red
    }
}

function Hash([string] $Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash }

# ---- independent PE checks (imagehlp is the reference implementation) -------------------
Add-Type -Namespace Fc3ppTest -Name Native -MemberDefinition @'
[DllImport("imagehlp.dll", SetLastError = true)]
public static extern System.IntPtr CheckSumMappedFile(byte[] baseAddress, uint fileLength, out uint headerSum, out uint checkSum);
'@

function Test-PeChecksumValid([string] $Path) {
    $bytes = [IO.File]::ReadAllBytes($Path)
    [uint32] $header = 0; [uint32] $computed = 0
    [void][Fc3ppTest.Native]::CheckSumMappedFile($bytes, [uint32]$bytes.Length, [ref]$header, [ref]$computed)
    $header -eq $computed
}

function Test-Laa([string] $Path) {
    $bytes = [IO.File]::ReadAllBytes($Path)
    $pe = [BitConverter]::ToInt32($bytes, 0x3C)
    ([BitConverter]::ToUInt16($bytes, $pe + 22) -band 0x20) -ne 0
}

# A minimal but structurally valid PE32 file with a correct checksum.
function New-DummyExe([string] $Path, [int] $Seed) {
    $b = New-Object byte[] 0x600
    $b[0] = 0x4D; $b[1] = 0x5A
    [BitConverter]::GetBytes([int]0x80).CopyTo($b, 0x3C)
    $b[0x80] = 0x50; $b[0x81] = 0x45
    [BitConverter]::GetBytes([uint16]0x014C).CopyTo($b, 0x84)   # machine
    [BitConverter]::GetBytes([uint16]1).CopyTo($b, 0x86)        # sections
    [BitConverter]::GetBytes([uint16]0xE0).CopyTo($b, 0x94)     # optional header size
    [BitConverter]::GetBytes([uint16]0x0102).CopyTo($b, 0x96)   # characteristics (no LAA)
    [BitConverter]::GetBytes([uint16]0x010B).CopyTo($b, 0x98)   # PE32
    [BitConverter]::GetBytes([uint32]0x00400000).CopyTo($b, 0x98 + 28)
    [BitConverter]::GetBytes([uint32]0x1000).CopyTo($b, 0x98 + 32)
    [BitConverter]::GetBytes([uint32]0x200).CopyTo($b, 0x98 + 36)
    [BitConverter]::GetBytes([uint32]0x2000).CopyTo($b, 0x98 + 56)
    [BitConverter]::GetBytes([uint32]0x200).CopyTo($b, 0x98 + 60)
    [BitConverter]::GetBytes([uint32]16).CopyTo($b, 0x98 + 92)
    for ($i = 0x200; $i -lt $b.Length; $i++) { $b[$i] = [byte](($i * 7 + $Seed) -band 0xFF) }
    [uint32] $header = 0; [uint32] $sum = 0
    [void][Fc3ppTest.Native]::CheckSumMappedFile($b, [uint32]$b.Length, [ref]$header, [ref]$sum)
    [BitConverter]::GetBytes([uint32]$sum).CopyTo($b, 0x98 + 64)
    [IO.File]::WriteAllBytes($Path, $b)
}

$ProfileXml = @'
<GamerProfile>
	<RenderProfile MSAALevel="0" UseD3D11="1" D3D11MultithreadedRendering="0" VSync="0" />
	<UplayProfile LockString="" />
</GamerProfile>
'@

# ---- sandbox ---------------------------------------------------------------------------------
$sandbox = Join-Path ([IO.Path]::GetTempPath()) ("fc3pp-installtest-" + [Guid]::NewGuid().ToString('N'))
$steam = Join-Path $sandbox 'Steam'
$lib2 = Join-Path $sandbox 'Second Library'
$fc3 = Join-Path $lib2 'steamapps\common\Far Cry 3'
$bd = Join-Path $steam 'steamapps\common\Far Cry 3 Blood Dragon'
$profileDir = Join-Path $sandbox 'Documents\My Games\Far Cry 3'
$source = Join-Path $sandbox 'package'
$snapshots = @{}

function Reset-Sandbox {
    if (Test-Path -LiteralPath $sandbox) { Remove-Item -LiteralPath $sandbox -Recurse -Force }
    foreach ($d in @("$steam\steamapps", "$fc3\bin", "$bd\bin", $profileDir, $source)) {
        New-Item -ItemType Directory -Path $d -Force | Out-Null
    }
    $vdfPath = $lib2.Replace('\', '\\')
    $steamPath = $steam.Replace('\', '\\')
    Set-Content -LiteralPath "$steam\steamapps\libraryfolders.vdf" -Encoding ASCII -Value @"
"libraryfolders"
{
	"0"
	{
		"path"		"$steamPath"
		"apps"
		{
			"233270"		"1"
		}
	}
	"1"
	{
		"path"		"$vdfPath"
		"apps"
		{
			"220240"		"1"
		}
	}
}
"@
    New-DummyExe "$fc3\bin\farcry3.exe" 1
    New-DummyExe "$fc3\bin\farcry3_d3d11.exe" 2
    New-DummyExe "$bd\bin\fc3_blooddragon.exe" 3
    New-DummyExe "$bd\bin\fc3_blooddragon_d3d11.exe" 4
    [IO.File]::WriteAllBytes("$fc3\bin\FC3.dll", [byte[]](1..64))
    [IO.File]::WriteAllText("$profileDir\GamerProfile.xml", $ProfileXml)
    if ($PatchDll) {
        Copy-Item -LiteralPath $PatchDll -Destination "$source\version.dll"
    } else {
        [IO.File]::WriteAllText("$source\version.dll", "MZ stand-in for the FC3PerformancePatch proxy DLL")
    }
    Copy-Item -LiteralPath (Join-Path $repo 'config\FC3PerformancePatch.ini') -Destination $source
    $script:snapshots = Get-Snapshot
}

# Relative path -> hash for everything in the sandbox except the package folder.
function Get-Snapshot {
    $map = @{}
    Get-ChildItem -LiteralPath $sandbox -Recurse -File | Where-Object { -not $_.FullName.StartsWith($source) } | ForEach-Object {
        $map[$_.FullName.Substring($sandbox.Length)] = Hash $_.FullName
    }
    $map
}

function Compare-Snapshot($Expected, $Actual) {
    $diff = @()
    foreach ($k in $Expected.Keys) {
        if (-not $Actual.ContainsKey($k)) { $diff += "missing: $k" }
        elseif ($Actual[$k] -ne $Expected[$k]) { $diff += "changed: $k" }
    }
    foreach ($k in $Actual.Keys) { if (-not $Expected.ContainsKey($k)) { $diff += "left over: $k" } }
    $diff
}

$env:FC3PP_STEAM_ROOT = $steam
$env:FC3PP_SKIP_REGISTRY = '1'
$env:FC3PP_TEST_NO_SYSTEM_CHANGES = '1'

function Invoke-Script([string] $Name, [string[]] $Arguments) {
    $argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $scripts $Name)) + $Arguments
    $out = & powershell.exe @argList 2>&1 | Out-String
    @{ Code = $LASTEXITCODE; Output = $out }
}

function Install([string[]] $Extra = @()) {
    Invoke-Script 'install.ps1' (@('-SourceDir', $source, '-ProfileDir', $profileDir, '-Unattended') + $Extra)
}

function Uninstall([string[]] $Extra = @()) {
    Invoke-Script 'uninstall.ps1' (@('-ProfileDir', $profileDir, '-Unattended') + $Extra)
}

function Backups([string] $Dir, [string] $Leaf) {
    @(Get-ChildItem -LiteralPath $Dir -File | Where-Object { $_.Name -like "$Leaf.fc3pp-*.bak" })
}

try {
    # ------------------------------------------------------------------------------------------
    $script:Current = 'install: auto-detection through libraryfolders.vdf'
    Reset-Sandbox
    $r = Install
    Check ($r.Code -eq 0) "installer failed:`n$($r.Output)"
    Check ($r.Output -match 'Far Cry 3\s') 'Far Cry 3 was not found in the second Steam library'
    Check ($r.Output -match 'Blood Dragon') 'Blood Dragon was not found in the main Steam library'
    foreach ($bin in @("$fc3\bin", "$bd\bin")) {
        Check (Test-Path "$bin\version.dll") "version.dll missing in $(Split-Path -Leaf (Split-Path -Parent $bin))"
        Check (Test-Path "$bin\FC3PerformancePatch.ini") 'INI missing'
        Check (Test-Path "$bin\FC3PerformancePatch.install.json") 'install record missing'
        Check ((Hash "$bin\version.dll") -eq (Hash "$source\version.dll")) 'installed DLL differs from the package'
    }
    foreach ($exe in @("$fc3\bin\farcry3.exe", "$fc3\bin\farcry3_d3d11.exe", "$bd\bin\fc3_blooddragon.exe", "$bd\bin\fc3_blooddragon_d3d11.exe")) {
        $leaf = Split-Path -Leaf $exe
        Check (Test-Laa $exe) "$leaf is not Large Address Aware"
        Check (Test-PeChecksumValid $exe) "$leaf has an invalid PE checksum after patching"
        $b = @(Backups (Split-Path -Parent $exe) $leaf)
        Check ($b.Count -eq 1) "$leaf has $($b.Count) backups, expected 1"
        if ($b.Count -eq 1) {
            Check ($b[0].Name -match '\.fc3pp-\d{8}-\d{6}\.bak$') "backup name '$($b[0].Name)' is not timestamped"
            Check ((Hash $b[0].FullName) -eq $snapshots[$exe.Substring($sandbox.Length)]) "$leaf backup differs from the original"
        }
    }
    Check ((Hash "$profileDir\GamerProfile.xml") -eq $snapshots["$profileDir\GamerProfile.xml".Substring($sandbox.Length)]) 'GamerProfile.xml was modified by a default install'
    Check (@(Backups $profileDir 'GamerProfile.xml').Count -eq 0) 'GamerProfile.xml was backed up although it was not modified'
    Check ((Hash "$fc3\bin\FC3.dll") -eq $snapshots["$fc3\bin\FC3.dll".Substring($sandbox.Length)]) 'an engine DLL was modified on disk'
    $manifestText = Get-Content -LiteralPath "$fc3\bin\FC3PerformancePatch.install.json" -Raw
    Check (-not $manifestText.Contains($sandbox)) 'the install record contains an absolute path'

    # ------------------------------------------------------------------------------------------
    $script:Current = 'install: idempotent'
    $before = Get-Snapshot
    $r = Install
    Check ($r.Code -eq 0) "second install failed:`n$($r.Output)"
    $after = Get-Snapshot
    $diff = @(Compare-Snapshot $before $after | Where-Object { $_ -notmatch 'install\.json' })
    Check ($diff.Count -eq 0) "a second install changed things: $($diff -join '; ')"
    Check ($r.Output -match 'already Large Address Aware') 'second run did not report the executables as already patched'

    # ------------------------------------------------------------------------------------------
    $script:Current = 'uninstall: byte-identical restore'
    $r = Uninstall
    Check ($r.Code -eq 0) "uninstaller failed:`n$($r.Output)"
    $diff = @(Compare-Snapshot $snapshots (Get-Snapshot))
    Check ($diff.Count -eq 0) "uninstall did not restore the original state: $($diff -join '; ')"

    $script:Current = 'uninstall: idempotent'
    $r = Uninstall
    Check ($r.Code -eq 0) "second uninstall failed:`n$($r.Output)"
    Check ($r.Output -match 'nothing to remove') 'second uninstall did not report that nothing was left'
    $diff = @(Compare-Snapshot $snapshots (Get-Snapshot))
    Check ($diff.Count -eq 0) "second uninstall changed things: $($diff -join '; ')"

    # ------------------------------------------------------------------------------------------
    $script:Current = "another mod's version.dll"
    Reset-Sandbox
    [IO.File]::WriteAllText("$fc3\bin\version.dll", 'some other mod loader, definitely not ours')
    $snapshots = Get-Snapshot
    $r = Install @('-Game', 'FarCry3')
    Check ($r.Code -eq 0) "install failed:`n$($r.Output)"
    Check ($r.Output -match "another mod") 'the displaced DLL was not mentioned'
    Check (@(Backups "$fc3\bin" 'version.dll').Count -eq 1) 'the other mod''s DLL was not backed up'
    Check (-not (Test-Path "$bd\bin\version.dll")) '-Game FarCry3 also installed into Blood Dragon'
    $r = Uninstall @('-Game', 'FarCry3')
    $diff = @(Compare-Snapshot $snapshots (Get-Snapshot))
    Check ($r.Code -eq 0 -and $diff.Count -eq 0) "the other mod's DLL was not restored: $($diff -join '; ')"

    # ------------------------------------------------------------------------------------------
    $script:Current = 'configuration is preserved'
    Reset-Sandbox
    [void](Install @('-Game', 'FarCry3'))
    Add-Content -LiteralPath "$fc3\bin\FC3PerformancePatch.ini" -Value '; my own tweak'
    $mine = Hash "$fc3\bin\FC3PerformancePatch.ini"
    $r = Install @('-Game', 'FarCry3')
    Check ((Hash "$fc3\bin\FC3PerformancePatch.ini") -eq $mine) 'a re-install overwrote the user''s INI'
    $r = Install @('-Game', 'FarCry3', '-ResetConfig')
    Check ((Hash "$fc3\bin\FC3PerformancePatch.ini") -eq (Hash "$source\FC3PerformancePatch.ini")) '-ResetConfig did not restore the default INI'
    Check (@(Backups "$fc3\bin" 'FC3PerformancePatch.ini').Count -eq 1) '-ResetConfig did not back up the user''s INI'
    $r = Uninstall @('-Game', 'FarCry3', '-KeepConfig')
    Check (Test-Path "$fc3\bin\FC3PerformancePatch.ini") '-KeepConfig removed the INI'
    Check (-not (Test-Path "$fc3\bin\version.dll")) '-KeepConfig left the DLL behind'
    $r = Uninstall @('-Game', 'FarCry3')
    $diff = @(Compare-Snapshot $snapshots (Get-Snapshot))
    Check ($diff.Count -eq 0) "leftovers after uninstall: $($diff -join '; ')"

    # ------------------------------------------------------------------------------------------
    $script:Current = 'explicit -GameDir'
    Reset-Sandbox
    $r = Install @('-GameDir', "$fc3\bin")
    Check ($r.Code -eq 0 -and (Test-Path "$fc3\bin\version.dll")) "-GameDir <bin> did not install:`n$($r.Output)"
    Check (-not (Test-Path "$bd\bin\version.dll")) '-GameDir installed into another game as well'
    $r = Uninstall @('-GameDir', $fc3)
    $diff = @(Compare-Snapshot $snapshots (Get-Snapshot))
    Check ($r.Code -eq 0 -and $diff.Count -eq 0) "uninstall with -GameDir left changes: $($diff -join '; ')"
    $r = Install @('-GameDir', (Join-Path $sandbox 'no such folder'))
    Check ($r.Code -ne 0) 'a wrong -GameDir did not fail'
    $r = Install @('-NoLAA', '-Game', 'FarCry3')
    Check ($r.Code -eq 0 -and -not (Test-Laa "$fc3\bin\farcry3.exe")) '-NoLAA still patched the executable'
    [void](Uninstall)

    # ------------------------------------------------------------------------------------------
    $script:Current = 'game not found, unattended'
    Reset-Sandbox
    Remove-Item -LiteralPath "$steam\steamapps\libraryfolders.vdf"
    Remove-Item -LiteralPath $bd -Recurse -Force
    $r = Install
    Check ($r.Code -ne 0 -and $r.Output -match '-GameDir') 'a missing game did not produce a helpful error'

    # ------------------------------------------------------------------------------------------
    $script:Current = 'backup made by the DLL itself (drag-and-drop install)'
    Reset-Sandbox
    Copy-Item "$fc3\bin\farcry3_d3d11.exe" "$fc3\bin\farcry3_d3d11.exe.fc3pp-20200101-000000.bak"
    $bytes = [IO.File]::ReadAllBytes("$fc3\bin\farcry3_d3d11.exe")
    $bytes[0x96] = $bytes[0x96] -bor 0x20           # what the DLL does at first launch (checksum left stale here)
    [IO.File]::WriteAllBytes("$fc3\bin\farcry3_d3d11.exe", $bytes)
    Copy-Item "$source\version.dll" "$fc3\bin\version.dll"
    Copy-Item "$source\FC3PerformancePatch.ini" "$fc3\bin\"
    Set-Content -LiteralPath "$fc3\bin\FC3PerformancePatch.log" -Value 'log'
    $r = Uninstall @('-Game', 'FarCry3')
    $diff = @(Compare-Snapshot $snapshots (Get-Snapshot))
    Check ($r.Code -eq 0 -and $diff.Count -eq 0) "a drag-and-drop install was not cleaned up: $($diff -join '; ')"

    # ------------------------------------------------------------------------------------------
    $script:Current = 'game updated after patching'
    Reset-Sandbox
    [void](Install @('-Game', 'FarCry3'))
    New-DummyExe "$fc3\bin\farcry3.exe" 99           # the launcher replaced the executable
    $updated = Hash "$fc3\bin\farcry3.exe"
    $r = Uninstall @('-Game', 'FarCry3')
    Check ($r.Code -eq 0) "uninstall failed:`n$($r.Output)"
    Check ((Hash "$fc3\bin\farcry3.exe") -eq $updated) 'uninstall overwrote an updated executable with an old backup'
    Check ($r.Output -match 'has changed since') 'the kept executable was not mentioned'
    Check ((Hash "$fc3\bin\farcry3_d3d11.exe") -eq $snapshots["$fc3\bin\farcry3_d3d11.exe".Substring($sandbox.Length)]) 'the untouched executable was not restored'

    # ------------------------------------------------------------------------------------------
    $script:Current = 'DXVK option'
    Reset-Sandbox
    [IO.File]::WriteAllText("$fc3\bin\d3d9.dll", 'a d3d9 wrapper the user already had')
    $snapshots = Get-Snapshot
    $dx = Join-Path $sandbox 'dxvk-src\dxvk-test\x32'
    New-Item -ItemType Directory -Path $dx -Force | Out-Null
    foreach ($f in @('d3d11.dll', 'd3d9.dll', 'dxgi.dll', 'd3d8.dll')) { [IO.File]::WriteAllText("$dx\$f", "fake dxvk $f") }
    $archive = Join-Path $source 'dxvk-test.tar.gz'
    & "$env:SystemRoot\System32\tar.exe" -czf $archive -C (Join-Path $sandbox 'dxvk-src') 'dxvk-test'
    Remove-Item -LiteralPath (Join-Path $sandbox 'dxvk-src') -Recurse -Force

    $r = Install @('-Game', 'FarCry3', '-EnableDXVK', '-DxvkArchive', $archive)
    Check ($r.Code -ne 0 -and $r.Output -match 'failed verification') 'an archive with the wrong SHA-256 was accepted'
    Check (-not (Test-Path "$fc3\bin\d3d11.dll")) 'files from an unverified archive were installed'

    $env:FC3PP_TEST_DXVK_SHA256 = (Hash $archive).ToLowerInvariant()
    $r = Install @('-Game', 'FarCry3', '-EnableDXVK', '-DxvkArchive', $archive)
    Check ($r.Code -eq 0) "DXVK install failed:`n$($r.Output)"
    foreach ($f in @('d3d11.dll', 'd3d9.dll', 'dxgi.dll')) {
        Check ((Test-Path "$fc3\bin\$f") -and ((Get-Content "$fc3\bin\$f" -Raw) -eq "fake dxvk $f")) "$f was not installed"
    }
    Check (-not (Test-Path "$fc3\bin\d3d8.dll")) 'a DXVK file that was not asked for was installed'
    Check (@(Backups "$fc3\bin" 'd3d9.dll').Count -eq 1) 'the existing d3d9.dll was not backed up'
    $xml = Get-Content -LiteralPath "$profileDir\GamerProfile.xml" -Raw
    Check ($xml -match 'D3D11MultithreadedRendering="1"') 'multithreaded rendering was not enabled together with DXVK'
    Check (@(Backups $profileDir 'GamerProfile.xml').Count -eq 1) 'GamerProfile.xml was modified without a backup'
    $before = Get-Snapshot
    $r = Install @('-Game', 'FarCry3', '-EnableDXVK', '-DxvkArchive', $archive)
    $diff = @(Compare-Snapshot $before (Get-Snapshot) | Where-Object { $_ -notmatch 'install\.json' })
    Check ($r.Code -eq 0 -and $diff.Count -eq 0) "DXVK re-install is not idempotent: $($diff -join '; ')"
    $r = Uninstall @('-Game', 'FarCry3')
    $diff = @(Compare-Snapshot $snapshots (Get-Snapshot))
    Check ($r.Code -eq 0 -and $diff.Count -eq 0) "DXVK uninstall did not restore everything: $($diff -join '; ')"
    $env:FC3PP_TEST_DXVK_SHA256 = $null

    # ------------------------------------------------------------------------------------------
    $script:Current = 'user changes to GamerProfile.xml survive uninstall'
    Reset-Sandbox
    $archive2 = Join-Path $source 'dxvk-test.tar.gz'
    $dx = Join-Path $sandbox 'dxvk-src\dxvk-test\x32'
    New-Item -ItemType Directory -Path $dx -Force | Out-Null
    foreach ($f in @('d3d11.dll', 'd3d9.dll', 'dxgi.dll')) { [IO.File]::WriteAllText("$dx\$f", "fake dxvk $f") }
    & "$env:SystemRoot\System32\tar.exe" -czf $archive2 -C (Join-Path $sandbox 'dxvk-src') 'dxvk-test'
    Remove-Item -LiteralPath (Join-Path $sandbox 'dxvk-src') -Recurse -Force
    $env:FC3PP_TEST_DXVK_SHA256 = (Hash $archive2).ToLowerInvariant()
    [void](Install @('-Game', 'FarCry3', '-EnableDXVK', '-DxvkArchive', $archive2))
    $xml = (Get-Content -LiteralPath "$profileDir\GamerProfile.xml" -Raw).Replace('VSync="0"', 'VSync="1"')
    [IO.File]::WriteAllText("$profileDir\GamerProfile.xml", $xml)      # the user changes a setting in game
    [void](Uninstall @('-Game', 'FarCry3'))
    $xml = Get-Content -LiteralPath "$profileDir\GamerProfile.xml" -Raw
    Check ($xml -match 'VSync="1"') 'uninstall threw away a setting the user changed after installing'
    Check ($xml -match 'D3D11MultithreadedRendering="0"') 'uninstall did not undo its own GamerProfile.xml change'
    Check (@(Backups $profileDir 'GamerProfile.xml').Count -eq 0) 'GamerProfile.xml backups were left behind'
    $env:FC3PP_TEST_DXVK_SHA256 = $null

    # ------------------------------------------------------------------------------------------
    $script:Current = 'Control Flow Guard option (recorded, system untouched in test mode)'
    Reset-Sandbox
    $r = Install @('-Game', 'FarCry3', '-DisableCFG')
    Check ($r.Code -eq 0 -and $r.Output -match 'Control Flow Guard disabled for: farcry3.exe, farcry3_d3d11.exe') "CFG step missing:`n$($r.Output)"
    $manifest = Get-Content -LiteralPath "$fc3\bin\FC3PerformancePatch.install.json" -Raw | ConvertFrom-Json
    Check (@($manifest.cfg).Count -eq 2) 'the CFG change was not recorded for the uninstaller'
    $r = Install @('-Game', 'FarCry3', '-DisableCFG')
    Check ($r.Output -match 'already disabled by a previous run') 'CFG was applied twice'
    $r = Uninstall @('-Game', 'FarCry3')
    Check ($r.Code -eq 0 -and $r.Output -match 'Control Flow Guard setting removed') 'uninstall did not revert CFG'
    $diff = @(Compare-Snapshot $snapshots (Get-Snapshot))
    Check ($diff.Count -eq 0) "leftovers: $($diff -join '; ')"
    $r = Install @('-Game', 'FarCry3')
    Check ($r.Output -notmatch 'Control Flow Guard') 'CFG was touched without -DisableCFG'

    # ------------------------------------------------------------------------------------------
    $script:Current = 'batch wrappers'
    Reset-Sandbox
    $env:FC3PP_NO_PAUSE = '1'
    $out = & cmd.exe /c "`"$scripts\install.bat`" -SourceDir `"$source`" -ProfileDir `"$profileDir`" -Unattended -Game FarCry3" 2>&1 | Out-String
    Check ($LASTEXITCODE -eq 0 -and (Test-Path "$fc3\bin\version.dll")) "install.bat failed:`n$out"
    $out = & cmd.exe /c "`"$scripts\uninstall.bat`" -ProfileDir `"$profileDir`" -Unattended -Game FarCry3" 2>&1 | Out-String
    $diff = @(Compare-Snapshot $snapshots (Get-Snapshot))
    Check ($LASTEXITCODE -eq 0 -and $diff.Count -eq 0) "uninstall.bat failed: $($diff -join '; ')`n$out"
    $out = & cmd.exe /c "`"$scripts\install.bat`" -GameDir `"$sandbox\nowhere`" -SourceDir `"$source`" -Unattended" 2>&1 | Out-String
    Check ($LASTEXITCODE -ne 0) 'install.bat does not pass the failure exit code on'
}
finally {
    $env:FC3PP_STEAM_ROOT = $null
    $env:FC3PP_SKIP_REGISTRY = $null
    $env:FC3PP_TEST_NO_SYSTEM_CHANGES = $null
    $env:FC3PP_TEST_DXVK_SHA256 = $null
    $env:FC3PP_NO_PAUSE = $null
    if (Test-Path -LiteralPath $sandbox) { Remove-Item -LiteralPath $sandbox -Recurse -Force -ErrorAction SilentlyContinue }
}

Write-Host "$script:Checks checks, $script:Failures failures"
if ($script:Failures) { exit 1 } else { exit 0 }
