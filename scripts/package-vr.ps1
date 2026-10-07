[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Dll,
    [string]$Output = (Join-Path $PSScriptRoot '..\out\packages\SYNTH-FO4VR.zip')
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
python (Join-Path $Root 'scripts\package_release.py') --lane vr --dll $Dll --output $Output
if ($LASTEXITCODE -ne 0) { throw 'VR packaging failed.' }
