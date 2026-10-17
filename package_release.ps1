# Thin Windows wrapper for the shared Windows/macOS release packager.
param(
    [Parameter(Mandatory = $true)][string]$Version,
    [string]$BuildDir = (Join-Path $PSScriptRoot 'build'),
    [string]$OutputDir = (Join-Path $PSScriptRoot 'dist')
)
$ErrorActionPreference = 'Stop'
$binary = Join-Path $BuildDir 'Release\FlareLensAE.aex'
$output = Join-Path $OutputDir ('FlareSimAE_' + $Version + '_Windows.zip')
python (Join-Path $PSScriptRoot 'scripts\package_release.py') --platform Windows --version $Version --binary $binary --output $output
if ($LASTEXITCODE -ne 0) { throw 'Release packaging failed' }
