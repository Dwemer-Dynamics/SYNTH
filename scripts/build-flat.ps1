[CmdletBinding()]
param(
    [ValidateSet('debug', 'releasedbg')]
    [string]$Mode = 'releasedbg',
    [string]$DependenciesDirectory = (Join-Path $PSScriptRoot '..\out\dependencies'),
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
    throw 'The flat lane requires Windows x64.'
}

$TestSuites = @(
    'synth-native-tests',
    'synth-transport-tests',
    'synth-json-tests',
    'synth-fake-server-tests',
    'synth-config-tests',
    'synth-diagnostics-tests',
    'synth-tasks-tests',
    'synth-lifecycle-tests',
    'synth-context-tests',
    'synth-targeting-tests',
    'synth-actions-tests',
    'synth-input-tests',
    'synth-audio-tests',
    'synth-presentation-tests',
    'synth-media-fetch-tests',
    'synth-adapters-tests'
)
if (Test-Path -LiteralPath (Join-Path $Root 'tests\protocol_native\protocol_native_tests.cpp') -PathType Leaf) {
    $TestSuites += 'synth-protocol-native-tests'
}

Push-Location $Root
try {
    Invoke-NativeCommand -FilePath xmake -ArgumentList @('f', '-y', '-p', 'windows', '-a', 'x64', '-m', $Mode, '--build_flat=n')
    # With the plugin disabled, every default target is an engine-free suite.
    # Older supported xmake releases accept only one positional build target.
    Invoke-NativeCommand -FilePath xmake -ArgumentList @('build')
    foreach ($TestSuite in $TestSuites) {
        Invoke-NativeCommand -FilePath xmake -ArgumentList @('run', $TestSuite)
    }
} finally {
    Pop-Location
}

$Entry = Join-Path $Root 'src\flat\plugin_entry.cpp'
if (-not (Test-Path -LiteralPath $Entry -PathType Leaf)) {
    throw 'Flat plugin entry src/flat/plugin_entry.cpp is not implemented. Native tests passed, but refusing to fabricate SYNTH.dll.'
}

$Repo = 'https://github.com/libxse/CommonLibF4.git'
$Revision = '6266ecc9014b473fc6b6efd04abac324477c63cd'
$Dependency = Join-Path $DependenciesDirectory 'CommonLibF4'
if (-not (Test-Path -LiteralPath (Join-Path $Dependency '.git'))) {
    if ($SkipFetch) { throw "Pinned CommonLibF4 checkout missing: $Dependency" }
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
if ($ActualRevision -ne $Revision) { throw "CommonLibF4 revision mismatch: $ActualRevision" }

# Optional in-game menu host. Only the header-only consumer API is fetched;
# SYNTH links nothing from it and resolves the host module at runtime.
$MenuFrameworkRepo = 'https://github.com/DCCStudios/F4SEMenuFramework.git'
$MenuFrameworkRevision = 'b031040dcb9b89d5b0accf8a4e4c99733f4dd63a'
$MenuFramework = Join-Path $DependenciesDirectory 'F4SEMenuFramework'
if (-not (Test-Path -LiteralPath (Join-Path $MenuFramework '.git'))) {
    if ($SkipFetch) { throw "Pinned F4SEMenuFramework checkout missing: $MenuFramework" }
    New-Item -ItemType Directory -Force -Path $DependenciesDirectory | Out-Null
    Invoke-NativeCommand -FilePath git -ArgumentList @('clone', '--filter=blob:none', '--no-checkout', $MenuFrameworkRepo, $MenuFramework)
    Invoke-NativeCommand -FilePath git -ArgumentList @('-C', $MenuFramework, 'sparse-checkout', 'set', '--no-cone', 'resources')
}
Invoke-NativeCommand -FilePath git -ArgumentList @('-C', $MenuFramework, 'fetch', '--no-tags', 'origin', $MenuFrameworkRevision)
Invoke-NativeCommand -FilePath git -ArgumentList @('-C', $MenuFramework, 'checkout', '--detach', $MenuFrameworkRevision)
$ActualMenuFrameworkRevision = (& git -C $MenuFramework rev-parse HEAD)
if ($LASTEXITCODE -ne 0) {
    throw "Native command failed with exit code ${LASTEXITCODE}: git -C $MenuFramework rev-parse HEAD"
}
$ActualMenuFrameworkRevision = $ActualMenuFrameworkRevision.Trim()
if ($ActualMenuFrameworkRevision -ne $MenuFrameworkRevision) {
    throw "F4SEMenuFramework revision mismatch: $ActualMenuFrameworkRevision"
}
$MenuFrameworkHeaders = Join-Path $MenuFramework 'resources'
foreach ($RequiredHeader in @('F4SEMenuFramework.h', 'DIK.h')) {
    if (-not (Test-Path -LiteralPath (Join-Path $MenuFrameworkHeaders $RequiredHeader) -PathType Leaf)) {
        throw "F4SEMenuFramework consumer header missing: $RequiredHeader"
    }
}

Push-Location $Root
try {
    Invoke-NativeCommand -FilePath xmake -ArgumentList @('f', '-y', '-p', 'windows', '-a', 'x64', '-m', $Mode, '--build_flat=y', "--commonlibf4_dir=$Dependency", "--menuframework_dir=$MenuFrameworkHeaders")
    Invoke-NativeCommand -FilePath xmake -ArgumentList @('build', 'SYNTH')
} finally {
    Pop-Location
}
