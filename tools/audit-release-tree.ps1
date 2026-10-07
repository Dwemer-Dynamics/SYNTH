[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('flat', 'vr')]
    [string]$Lane,
    [Parameter(Mandatory = $true)]
    [string]$Path
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
python (Join-Path $Root 'tools\audit_release_tree.py') --lane $Lane --path $Path
if ($LASTEXITCODE -ne 0) { throw "$Lane release audit failed." }
