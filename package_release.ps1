# Package a built Windows AEX without redundant embedded lens sidecar files.
# The Adobe SDK and CUDA Toolkit are not distributed.
param(
    [Parameter(Mandatory = $true)]
    [string]$Version,
    [string]$BuildDir = (Join-Path $PSScriptRoot 'build'),
    [string]$OutputDir = (Join-Path $PSScriptRoot 'dist')
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if ($Version -notmatch '^v[0-9]+\.[0-9]+\.[0-9]+(?:-[a-zA-Z0-9.]+)?$') {
    throw 'Version must be in vX.Y.Z or vX.Y.Z-label format.'
}
$binary = Join-Path $BuildDir 'Release\FlareLensAE.aex'
$sourceLicense = Join-Path $PSScriptRoot 'LICENSE'
$upstreamLicense = Join-Path $PSScriptRoot 'upstream\LICENSE'
$inputs = @($binary, $sourceLicense, $upstreamLicense,
    (Join-Path $PSScriptRoot 'README.md'),
    (Join-Path $PSScriptRoot 'INSTALL.md'),
    (Join-Path $PSScriptRoot 'NOTICE.md'))
foreach ($f in $inputs) {
    if (-not (Test-Path -LiteralPath $f -PathType Leaf)) {
        throw "Missing release input: $f"
    }
}

New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
$zipPath = Join-Path $OutputDir ('FlareSimAE_' + $Version + '_Windows.zip')
if (Test-Path -LiteralPath $zipPath) {
    throw "Release archive already exists; won't overwrite: $zipPath"
}

$stage = Join-Path $OutputDir ("_stage_" + [guid]::NewGuid().ToString('N'))
try {
    New-Item -ItemType Directory -Path $stage -Force | Out-Null
    Copy-Item -LiteralPath $binary -Destination (Join-Path $stage 'FlareLensAE.aex')
    Copy-Item -LiteralPath $sourceLicense -Destination (Join-Path $stage 'LICENSE.txt')
    Copy-Item -LiteralPath $upstreamLicense -Destination (Join-Path $stage 'LICENSE_UPSTREAM.txt')
    foreach ($name in @('README.md', 'INSTALL.md', 'NOTICE.md')) {
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination (Join-Path $stage $name)
    }

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [System.IO.Compression.ZipFile]::CreateFromDirectory(
        $stage, $zipPath, [System.IO.Compression.CompressionLevel]::Optimal, $false)

    $archive = [System.IO.Compression.ZipFile]::OpenRead($zipPath)
    try {
        $expected = @('FlareLensAE.aex', 'README.md', 'INSTALL.md',
                      'NOTICE.md', 'LICENSE.txt', 'LICENSE_UPSTREAM.txt')
        $names = @($archive.Entries | ForEach-Object { $_.FullName.Replace('\', '/') })
        $diff = @($names | Where-Object { $_ -notin $expected })
        if ($names.Count -ne $expected.Count -or $diff.Count -gt 0) {
            throw "Unexpected ZIP contents: $($names -join ', ')"
        }
        foreach ($name in $expected) {
            if ($names -notcontains $name) { throw "ZIP missing: $name" }
        }
        $stream = $archive.GetEntry('FlareLensAE.aex').Open()
        try {
            $sha = [System.Security.Cryptography.SHA256]::Create()
            try {
                $packedHash = [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '')
            } finally { $sha.Dispose() }
        } finally { $stream.Dispose() }
        $binaryHash = (Get-FileHash -LiteralPath $binary -Algorithm SHA256).Hash
        if ($packedHash -ne $binaryHash) { throw 'Packed AEX hash differs from built AEX.' }
    } finally { $archive.Dispose() }

    Write-Output "PACKAGE_PATH=$zipPath"
    Write-Output "PACKAGE_BYTES=$((Get-Item -LiteralPath $zipPath).Length)"
    Write-Output "PACKAGE_SHA256=$((Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash)"
    Write-Output "AEX_SHA256=$binaryHash"
    Write-Output 'PACKAGE_READY=1'
} catch {
    if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force }
    throw
} finally {
    if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
}
