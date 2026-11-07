<#
.SYNOPSIS
    Build AJAInput.dll on Windows, run the unit tests and optionally
    install the plugin into Resolume.

.DESCRIPTION
    Expects this sibling layout (the folder names of the dependencies
    matter; this one's does not):

        <workspace>\AJA-Input-FFGL  (this folder)
        <workspace>\ffgl            (github.com/resolume/ffgl)
        <workspace>\libajantv2      (github.com/aja-video/libajantv2, tag ntv2_18_0_0)
        <workspace>\vcpkg           (github.com/microsoft/vcpkg, with glew:x64-windows-static)

    -Setup clones and bootstraps whatever is missing. GLEW is installed
    through vcpkg automatically when it is not there yet.

    Uses Visual Studio's own CMake when cmake is not on PATH, and the
    Visual Studio generator by default, so it runs from any PowerShell
    window (no Developer Command Prompt needed).

.PARAMETER Config
    Release (default), Debug or RelWithDebInfo.

.PARAMETER Install
    Copy the built DLL into Documents\Resolume <Arena|Avenue>\Extra Effects.
    Resolume must be closed (Windows locks a loaded DLL).

.PARAMETER Resolume
    Arena (default) or Avenue: which Extra Effects folder -Install uses.

.PARAMETER Setup
    Clone ffgl / libajantv2 / vcpkg when missing and bootstrap vcpkg.

.PARAMETER Clean
    Delete the build folder first (full rebuild).

.PARAMETER SkipTests
    Do not run AJAInputTests.exe after building.

.PARAMETER Generator
    CMake generator override, e.g. "Ninja". Default: the Visual Studio
    version found by vswhere.

.PARAMETER Workspace
    Folder holding ffgl, libajantv2 and vcpkg. Default: the parent of
    this script's folder.

.EXAMPLE
    .\build.ps1
    Release build + tests.

.EXAMPLE
    .\build.ps1 -Install
    Release build + tests, then copy the DLL into Resolume Arena.

.EXAMPLE
    .\build.ps1 -Setup -Install
    First run on a new machine.
#>
# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (c) 2026 Joaquin Villodre
# (Kept below the help block: Get-Help only reads a help block that
#  is the first thing in the file.)

[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug', 'RelWithDebInfo')]
    [string]$Config = 'Release',
    [switch]$Install,
    [ValidateSet('Arena', 'Avenue')]
    [string]$Resolume = 'Arena',
    [switch]$Setup,
    [switch]$Clean,
    [switch]$SkipTests,
    [string]$Generator = '',
    [string]$Workspace = ''
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

# Pinned versions, kept in step with .github/workflows/ci.yml.
$AjaSdkTag  = 'ntv2_18_0_0'
$FfglCommit = '5b6e07840277df9ec35a228b9eb067636316350f'
$Triplet    = 'x64-windows-static'

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
function Write-Step([string]$Text) {
    Write-Host ''
    Write-Host "==> $Text" -ForegroundColor Cyan
}

function Fail([string]$Text) {
    Write-Host ''
    Write-Host "ERROR: $Text" -ForegroundColor Red
    exit 1
}

# Run a native program with its output going straight to the console;
# stop on a non-zero exit code.
function Invoke-Native([string]$Exe, [string[]]$Arguments) {
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) {
        Fail "'$Exe $($Arguments -join ' ')' failed with exit code $LASTEXITCODE."
    }
}

# Run a native program and capture stdout, tolerating a non-zero exit code
# and stderr output (Windows PowerShell 5.1 turns redirected stderr into
# errors under ErrorActionPreference=Stop).
function Get-NativeOutput([string]$Exe, [string[]]$Arguments) {
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $out = & $Exe @Arguments 2>$null
        return [pscustomobject]@{ Output = ($out -join "`n").Trim(); ExitCode = $LASTEXITCODE }
    }
    finally {
        $ErrorActionPreference = $previous
    }
}

function Find-VisualStudio {
    $programFiles = ${env:ProgramFiles(x86)}
    if (-not $programFiles) { $programFiles = $env:ProgramFiles }
    if (-not $programFiles) { return $null }
    $vswhere = Join-Path $programFiles 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { return $null }
    $common = @('-latest', '-products', '*',
                '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64')
    $path    = (Get-NativeOutput $vswhere ($common + @('-property', 'installationPath'))).Output
    $version = (Get-NativeOutput $vswhere ($common + @('-property', 'installationVersion'))).Output
    if (-not $path) { return $null }
    return [pscustomobject]@{ Path = $path; Major = [int]($version.Split('.')[0]) }
}

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
$SourceDir = $PSScriptRoot
if (-not $Workspace) { $Workspace = Split-Path $SourceDir -Parent }
$FfglDir   = Join-Path $Workspace 'ffgl'
$AjaDir    = Join-Path $Workspace 'libajantv2'
$VcpkgDir  = Join-Path $Workspace 'vcpkg'
$BuildDir  = Join-Path $SourceDir 'build'

$versionLine = Select-String -Path (Join-Path $SourceDir 'CMakeLists.txt') `
                             -Pattern '^\s*VERSION\s+([0-9.]+)' | Select-Object -First 1
$ProjectVersion = if ($versionLine) { $versionLine.Matches[0].Groups[1].Value } else { '?' }

Write-Host "AJA-Input-FFGL $ProjectVersion  ($Config)" -ForegroundColor White
Write-Host "  source    : $SourceDir"
Write-Host "  workspace : $Workspace"

# ---------------------------------------------------------------------------
# Toolchain
# ---------------------------------------------------------------------------
Write-Step 'Looking for Visual Studio and CMake'

$vs = Find-VisualStudio
if (-not $vs) {
    Fail ("Visual Studio with the C++ workload was not found. Install Visual Studio 2022 " +
          "(Community is fine) with 'Desktop development with C++'.")
}
Write-Host "  Visual Studio $($vs.Major) at $($vs.Path)"

$cmake = (Get-Command cmake -ErrorAction SilentlyContinue | Select-Object -First 1)
if ($cmake) {
    $cmake = $cmake.Path
}
else {
    $cmake = Join-Path $vs.Path 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    if (-not (Test-Path $cmake)) {
        Fail "cmake not found on PATH nor inside Visual Studio. Install CMake (cmake.org) or the VS 'C++ CMake tools' component."
    }
}
Write-Host "  cmake: $cmake"

if (-not $Generator) {
    switch ($vs.Major) {
        16      { $Generator = 'Visual Studio 16 2019' }
        17      { $Generator = 'Visual Studio 17 2022' }
        18      { $Generator = 'Visual Studio 18 2026' }
        default { $Generator = 'Visual Studio 17 2022' }
    }
}
$IsVsGenerator   = $Generator -like 'Visual Studio*'
$IsMultiConfig   = $IsVsGenerator -or ($Generator -like '*Multi-Config')
Write-Host "  generator: $Generator"

# Single-config generators (Ninja, NMake) need the MSVC environment.
if (-not $IsVsGenerator -and -not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $devShell = Join-Path $vs.Path 'Common7\Tools\Launch-VsDevShell.ps1'
    if (-not (Test-Path $devShell)) { Fail "Generator '$Generator' needs the MSVC environment and Launch-VsDevShell.ps1 was not found." }
    Write-Host '  entering the Visual Studio developer environment (x64)'
    $here = Get-Location
    & $devShell -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
    Set-Location $here
}

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
Write-Step 'Checking dependencies'

$git = (Get-Command git -ErrorAction SilentlyContinue | Select-Object -First 1)
if ($git) { $git = $git.Path }

function Require-Git {
    if (-not $git) { Fail 'git is needed for -Setup. Install Git for Windows (git-scm.com).' }
}

if (-not (Test-Path (Join-Path $FfglDir 'CMakeLists.txt'))) {
    if (-not $Setup) { Fail "FFGL SDK not found at $FfglDir. Run again with -Setup to clone it." }
    Require-Git
    Write-Host "  cloning FFGL into $FfglDir"
    Invoke-Native $git @('clone', 'https://github.com/resolume/ffgl.git', $FfglDir)
    $co = Get-NativeOutput $git @('-C', $FfglDir, 'checkout', '-q', $FfglCommit)
    if ($co.ExitCode -ne 0) { Write-Warning "Could not check out the pinned FFGL commit; building against its default branch." }
}
Write-Host "  FFGL       : $FfglDir"

if (-not (Test-Path (Join-Path $AjaDir 'CMakeLists.txt'))) {
    if (-not $Setup) { Fail "libajantv2 not found at $AjaDir. Run again with -Setup to clone it." }
    Require-Git
    Write-Host "  cloning libajantv2 ($AjaSdkTag) into $AjaDir"
    Invoke-Native $git @('clone', '--depth', '1', '--branch', $AjaSdkTag,
                         'https://github.com/aja-video/libajantv2.git', $AjaDir)
}
$ajaTag = ''
if ($git) { $ajaTag = (Get-NativeOutput $git @('-C', $AjaDir, 'describe', '--tags', '--exact-match')).Output }
if ($ajaTag -and $ajaTag -ne $AjaSdkTag) {
    Write-Warning "libajantv2 is at '$ajaTag', the project is tested with '$AjaSdkTag'."
}
Write-Host "  libajantv2 : $AjaDir $(if ($ajaTag) { "($ajaTag)" })"

$vcpkgExe = Join-Path $VcpkgDir 'vcpkg.exe'
if (-not (Test-Path $vcpkgExe)) {
    if (-not $Setup) { Fail "vcpkg not found at $VcpkgDir. Run again with -Setup to clone and bootstrap it." }
    if (-not (Test-Path (Join-Path $VcpkgDir 'bootstrap-vcpkg.bat'))) {
        Require-Git
        Write-Host "  cloning vcpkg into $VcpkgDir"
        Invoke-Native $git @('clone', 'https://github.com/microsoft/vcpkg.git', $VcpkgDir)
    }
    Write-Host '  bootstrapping vcpkg'
    Invoke-Native (Join-Path $VcpkgDir 'bootstrap-vcpkg.bat') @('-disableMetrics')
}
$glewHeader = Join-Path $VcpkgDir "installed\$Triplet\include\GL\glew.h"
if (-not (Test-Path $glewHeader)) {
    Write-Host "  installing glew:$Triplet with vcpkg"
    Invoke-Native $vcpkgExe @('install', "glew:$Triplet")
}
Write-Host "  vcpkg      : $VcpkgDir (glew:$Triplet)"

# ---------------------------------------------------------------------------
# Configure
# ---------------------------------------------------------------------------
$cache = Join-Path $BuildDir 'CMakeCache.txt'
if ($Clean -and (Test-Path $BuildDir)) {
    Write-Step 'Removing the build folder'
    Remove-Item -Recurse -Force $BuildDir
}
elseif (Test-Path $cache) {
    # A build folder made with another generator (e.g. Ninja from an
    # older session) cannot be reused; CMake would refuse to configure.
    $prev = Select-String -Path $cache -Pattern '^CMAKE_GENERATOR:INTERNAL=(.*)$' | Select-Object -First 1
    if ($prev -and $prev.Matches[0].Groups[1].Value -ne $Generator) {
        Write-Step "Build folder was made with '$($prev.Matches[0].Groups[1].Value)'; recreating it for '$Generator'"
        Remove-Item -Recurse -Force $BuildDir
    }
}

Write-Step 'Configuring'
$toolchain = Join-Path $VcpkgDir 'scripts\buildsystems\vcpkg.cmake'
$configureArgs = @('-S', $SourceDir, '-B', $BuildDir, '-G', $Generator,
                   "-DVCPKG_TARGET_TRIPLET=$Triplet",
                   "-DCMAKE_TOOLCHAIN_FILE=$toolchain",
                   "-DFFGL_ROOT=$FfglDir",
                   "-DAJANTV2_ROOT=$AjaDir")
if ($IsVsGenerator)     { $configureArgs += @('-A', 'x64') }
if (-not $IsMultiConfig) { $configureArgs += @("-DCMAKE_BUILD_TYPE=$Config") }
Invoke-Native $cmake $configureArgs

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------
Write-Step "Building ($Config)"
Invoke-Native $cmake @('--build', $BuildDir, '--config', $Config, '--parallel')

$outDir = if ($IsMultiConfig) { Join-Path $BuildDir $Config } else { $BuildDir }
$dll    = Join-Path $outDir 'AJAInput.dll'
$tests  = Join-Path $outDir 'AJAInputTests.exe'
if (-not (Test-Path $dll)) { Fail "Build finished but $dll was not found." }

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if (-not $SkipTests) {
    Write-Step 'Running unit tests'
    if (-not (Test-Path $tests)) { Fail "$tests was not found." }
    & $tests
    if ($LASTEXITCODE -ne 0) { Fail "Unit tests failed ($LASTEXITCODE failing)." }
}

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
if ($Install) {
    Write-Step "Installing into Resolume $Resolume"
    $running = Get-Process -Name $Resolume -ErrorAction SilentlyContinue
    if ($running) {
        Fail "Resolume $Resolume is running and keeps the plugin DLL locked. Close it and run again with -Install (add -SkipTests to save time)."
    }
    # The Documents known folder (follows OneDrive / folder redirection),
    # falling back to %USERPROFILE%\Documents.
    $documents = [Environment]::GetFolderPath('MyDocuments')
    if (-not $documents -and $env:USERPROFILE) { $documents = Join-Path $env:USERPROFILE 'Documents' }
    if (-not $documents) { Fail 'Could not locate the Documents folder.' }
    $target    = Join-Path $documents "Resolume $Resolume\Extra Effects"
    if (-not (Test-Path $target)) { New-Item -ItemType Directory -Path $target | Out-Null }
    Copy-Item -Path $dll -Destination $target -Force
    Write-Host "  copied to $target"
}

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
$info = Get-Item $dll
Write-Host ''
Write-Host "OK  AJAInput.dll $ProjectVersion ($Config)" -ForegroundColor Green
Write-Host ("    {0}  {1:N0} bytes  {2}" -f $info.FullName, $info.Length, $info.LastWriteTime)
if (-not $Install) {
    Write-Host '    Run with -Install to copy it into Resolume (Resolume closed).'
}
