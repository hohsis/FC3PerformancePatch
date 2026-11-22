<#
.SYNOPSIS
    Configures, builds and tests FC3PerformancePatch with MSVC (32-bit x86).

.DESCRIPTION
    Locates CMake (on PATH or bundled with Visual Studio / Build Tools), builds
    the Release configuration into .\build and runs the test suite.

.PARAMETER Config
    Build configuration (Release or Debug). Default: Release.

.PARAMETER NoTest
    Build only; skip the tests.

.PARAMETER Clean
    Delete the build folder first.

.EXAMPLE
    .\build.ps1
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')] [string] $Config = 'Release',
    [switch] $NoTest,
    [switch] $Clean,
    # One or more game "bin" folders (separated by ';') for the real_game_binaries test.
    [string] $GameBin
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$build = Join-Path $root 'build'

function Find-CMake {
    $cmd = Get-Command cmake -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $vs = & $vswhere -latest -products * -property installationPath
        if ($vs) {
            $bundled = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
            if (Test-Path $bundled) { return $bundled }
        }
    }
    throw 'CMake was not found. Install CMake, or the "C++ CMake tools" component of Visual Studio Build Tools.'
}

$cmake = Find-CMake
$ctest = Join-Path (Split-Path -Parent $cmake) 'ctest.exe'

if ($Clean -and (Test-Path $build)) {
    Remove-Item -Recurse -Force $build
}

& $cmake -S $root -B $build -A Win32
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }

& $cmake --build $build --config $Config -- /nologo /verbosity:minimal
if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }

if (-not $NoTest) {
    if ($GameBin) { $env:FC3PP_GAME_BIN = $GameBin }
    & $ctest --test-dir $build -C $Config --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'Tests failed.' }
}

Write-Host ''
Write-Host "Build finished. Package: build\package\FC3PerformancePatch"
