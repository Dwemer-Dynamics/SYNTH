[CmdletBinding()]
param(
    [ValidateSet('Debug', 'RelWithDebInfo', 'Release')]
    [string]$Configuration = 'RelWithDebInfo',
    [string]$DependenciesDirectory = (Join-Path $PSScriptRoot '..\out\dependencies'),
    [string]$BuildDirectory = (Join-Path $PSScriptRoot '..\out\build-vr'),
    [string]$VcpkgRoot = $env:VCPKG_ROOT,
    [switch]$SkipFetch
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Invoke-NativeCommand {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FilePath,
        [Parameter(ValueFromRemainingArguments = $true)]
        [string[]]$ArgumentList
    )

    & $FilePath @ArgumentList
    if ($LASTEXITCODE -ne 0) {
        throw "Native command failed with exit code ${LASTEXITCODE}: $FilePath $($ArgumentList -join ' ')"
    }
}

$Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Invoke-NativeCommand python (Join-Path $Root 'scripts/verify_papyrus.py')
$IsWindowsVariable = Get-Variable -Name IsWindows -ErrorAction SilentlyContinue
$RunningOnWindows = if ($null -ne $IsWindowsVariable) {
    [bool]$IsWindowsVariable.Value
} else {
    $env:OS -eq 'Windows_NT'
}
if (-not $RunningOnWindows -or -not [Environment]::Is64BitOperatingSystem) {
    throw 'The VR lane requires Windows x64.'
}

$Toolchain = if ([string]::IsNullOrWhiteSpace($VcpkgRoot)) {
    $null
} else {
    Join-Path $VcpkgRoot 'scripts\buildsystems\vcpkg.cmake'
}
if ($null -eq $Toolchain -or -not (Test-Path -LiteralPath $Toolchain -PathType Leaf)) {
    throw 'The VR lane requires vcpkg. Set VCPKG_ROOT (or -VcpkgRoot), then install rsm-mmio:x64-windows-static-md and spdlog:x64-windows-static-md.'
}

# Configure and run the engine-free suite in this lane before requiring a plugin entry point.
Invoke-NativeCommand -FilePath cmake -ArgumentList @(
    '-S', $Root, '-B', $BuildDirectory, '-A', 'x64',
    "-DCMAKE_TOOLCHAIN_FILE=$Toolchain",
    '-DVCPKG_TARGET_TRIPLET=x64-windows-static-md',
    '-DSYNTH_BUILD_CORE_TESTS=ON',
    '-DSYNTH_BUILD_VR=OFF'
)
Invoke-NativeCommand -FilePath cmake -ArgumentList @('--build', $BuildDirectory, '--config', $Configuration)
Invoke-NativeCommand -FilePath ctest -ArgumentList @('--test-dir', $BuildDirectory, '-C', $Configuration, '--output-on-failure')

$Entry = Join-Path $Root 'src\vr\plugin_entry.cpp'
if (-not (Test-Path -LiteralPath $Entry -PathType Leaf)) {
    throw 'VR plugin entry src/vr/plugin_entry.cpp is not implemented. Native tests passed, but refusing to fabricate SYNTHVR.dll.'
}

$Repo = 'https://github.com/ArthurHub/CommonLibF4VR.git'
$Revision = '1c7b4fc860261eabad9f044e336965c26abe8ee6'
$Dependency = Join-Path $DependenciesDirectory 'CommonLibF4VR'
if (-not (Test-Path -LiteralPath (Join-Path $Dependency '.git'))) {
    if ($SkipFetch) { throw "Pinned CommonLibF4VR checkout missing: $Dependency" }
    New-Item -ItemType Directory -Force -Path $DependenciesDirectory | Out-Null
    Invoke-NativeCommand -FilePath git -ArgumentList @('clone', '--filter=blob:none', $Repo, $Dependency)
}
Invoke-NativeCommand -FilePath git -ArgumentList @('-C', $Dependency, 'fetch', '--no-tags', 'origin', $Revision)
Invoke-NativeCommand -FilePath git -ArgumentList @('-C', $Dependency, 'checkout', '--detach', $Revision)
Invoke-NativeCommand -FilePath git -ArgumentList @('-C', $Dependency, 'submodule', 'update', '--init', '--recursive')
$ActualRevision = (& git -C $Dependency rev-parse HEAD)
if ($LASTEXITCODE -ne 0) {
    throw "Native command failed with exit code ${LASTEXITCODE}: git -C $Dependency rev-parse HEAD"
}
$ActualRevision = $ActualRevision.Trim()
if ($ActualRevision -ne $Revision) { throw "CommonLibF4VR revision mismatch: $ActualRevision" }

Invoke-NativeCommand -FilePath cmake -ArgumentList @(
    '-S', $Root, '-B', $BuildDirectory, '-A', 'x64',
    "-DCMAKE_TOOLCHAIN_FILE=$Toolchain",
    '-DVCPKG_TARGET_TRIPLET=x64-windows-static-md',
    '-DSYNTH_BUILD_CORE_TESTS=ON',
    '-DSYNTH_BUILD_VR=ON',
    '-DSYNTH_FETCH_VR_DEPENDENCIES=OFF',
    "-DSYNTH_COMMONLIBF4VR_SOURCE_DIR=$Dependency"
)
Invoke-NativeCommand -FilePath cmake -ArgumentList @('--build', $BuildDirectory, '--config', $Configuration, '--target', 'SYNTHVR')
Invoke-NativeCommand -FilePath ctest -ArgumentList @('--test-dir', $BuildDirectory, '-C', $Configuration, '--output-on-failure')
