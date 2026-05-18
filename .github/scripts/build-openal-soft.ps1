# Configure and build static OpenAL Soft (Win32) for OrcOutFit.
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [string]$ProjectDir = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
)

$ErrorActionPreference = 'Stop'
$ProjectDir = $ProjectDir.Trim('"', ' ').TrimEnd('\', '/')

function Get-ProgramFilesRoot {
    # MSBuild PreBuild may run under 32-bit host: ProgramFiles -> (x86); VS/CMake live under 64-bit tree.
    if ($env:ProgramW6432) { return $env:ProgramW6432 }
    return $env:ProgramFiles
}

function Find-CMake {
    $pf = Get-ProgramFilesRoot
    $candidates = @(
        "$pf\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "$pf\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "$pf\CMake\bin\cmake.exe"
    )
    foreach ($p in $candidates) {
        if (Test-Path -LiteralPath $p) { return $p }
    }
    $cmd = Get-Command cmake -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    throw 'cmake.exe not found (install CMake or Visual Studio CMake component)'
}

$cmake = Find-CMake
$src = Join-Path $ProjectDir 'source\external\openal-soft'
$build = Join-Path $ProjectDir 'build\openal-soft'
$lib = Join-Path $build "$Configuration\OpenAL32.lib"

if (-not (Test-Path -LiteralPath $src)) {
    throw "openal-soft submodule missing at $src (git submodule update --init --recursive)"
}

$cache = Join-Path $build 'CMakeCache.txt'
if (-not (Test-Path -LiteralPath $cache)) {
    $gen = 'Visual Studio 18 2026'
    $vsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path -LiteralPath $vsWhere) {
        $vsMajor = & $vsWhere -latest -property catalog_buildVersion -requires Microsoft.Component.MSBuild 2>$null
        if ($vsMajor -match '^17\.') { $gen = 'Visual Studio 17 2022' }
    }
    $toolset = $env:ORC_OPENAL_TOOLSET
    if (-not $toolset) { $toolset = 'v145' }
    $cmakeArgs = @(
        '-S', $src,
        '-B', $build,
        '-G', $gen,
        '-A', 'Win32',
        '-DLIBTYPE=STATIC',
        '-DALSOFT_UTILS=OFF',
        '-DALSOFT_EXAMPLES=OFF',
        '-DALSOFT_TESTS=OFF',
        '-DCMAKE_POLICY_DEFAULT_CMP0091=NEW',
        '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded$<$<CONFIG:Debug>:Debug>',
        "-T$toolset"
    )
    & $cmake @cmakeArgs
}

& $cmake --build $build --config $Configuration --target OpenAL -j

if (-not (Test-Path -LiteralPath $lib)) {
    throw "OpenAL static lib not found: $lib"
}
