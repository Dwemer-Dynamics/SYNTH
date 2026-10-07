[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Dll,
    [string]$Output = (Join-Path $PSScriptRoot '..\out\packages\SYNTH-FO4.zip')
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
python (Join-Path $Root 'scripts\package_release.py') --lane flat --dll $Dll --output $Output
if ($LASTEXITCODE -ne 0) { throw 'Flat packaging failed.' }
