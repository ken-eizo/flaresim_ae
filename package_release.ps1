# Creates the same simple downloadable ZIP layout as the upstream
# FlareSim releases, adapted for After Effects on Windows.
# The Adobe SDK and CUDA Toolkit are NOT copied into the package.
param(
    [string]$Version = 'v1.0.2',
    [string]$BuildDir = (Join-Path $PSScriptRoot 'build'),
    [string]$OutputDir = (Join-Path $PSScriptRoot 'build')
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if ($Version -notmatch '^v[0-9]+\.[0-9]+\.[0-9]+(?:-[a-zA-Z0-9.]+)?$') {
    throw 'Version must be in vX.Y.Z or vX.Y.Z-label format.'
}
if (-not (Test-Path -LiteralPath $BuildDir -PathType Container)) {
    throw 'Build directory does not exist. Build the AEX first.'
}

$binary = Join-Path $BuildDir 'Release\FlareLensAE.aex'
$manifest = Join-Path $BuildDir 'Release\FlareLensCatalog.tsv'
$lensDir = Join-Path $PSScriptRoot 'upstream\lenses\lens_files'
$upstreamLicense = Join-Path $PSScriptRoot 'upstream\LICENSE'
$sourceLicense = Join-Path $PSScriptRoot 'LICENSE'
$files = @($binary, $manifest, $upstreamLicense, $sourceLicense,
    (Join-Path $PSScriptRoot 'README.md'),
    (Join-Path $PSScriptRoot 'INSTALL.md'),
    (Join-Path $PSScriptRoot 'NOTICE.md'))
foreach ($f in $files) {
    if (-not (Test-Path -LiteralPath $f -PathType Leaf)) {
        throw "Missing release input: $f"
    }
}

$lensCount = @(Get-ChildItem -LiteralPath $lensDir -Filter '*.lens' -File).Count
if ($lensCount -ne 1370) {
    throw "Expected 1,370 original lens files, got $lensCount."
}

New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
$zipName = "FlareSimAE_${Version}_Windows.zip"
$zipPath = Join-Path $OutputDir $zipName
if (Test-Path -LiteralPath $zipPath) {
    throw "Release archive already exists; won't overwrite: $zipPath"
}

$stage = Join-Path $BuildDir ("_release_stage_" + [guid]::NewGuid().ToString('N'))
try {
    New-Item -ItemType Directory -Path $stage -Force | Out-Null
    Copy-Item -LiteralPath $binary -Destination (Join-Path $stage 'FlareLensAE.aex')
    Copy-Item -LiteralPath $manifest -Destination (Join-Path $stage 'FlareLensCatalog.tsv')
    Copy-Item -LiteralPath $sourceLicense -Destination (Join-Path $stage 'LICENSE.txt')
    Copy-Item -LiteralPath $upstreamLicense -Destination (Join-Path $stage 'LICENSE_UPSTREAM.txt')
    foreach ($name in @('README.md','INSTALL.md','NOTICE.md')) {
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination (Join-Path $stage $name)
    }

    $lensTarget = Join-Path $stage 'lenses\lens_files'
    New-Item -ItemType Directory -Path $lensTarget -Force | Out-Null
    Copy-Item -Path (Join-Path $lensDir '*.lens') -Destination $lensTarget
    foreach ($name in @('convert_ob.py','convert_p2p.py')) {
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot "upstream\lenses\$name") -Destination (Join-Path $stage "lenses\$name")
    }

    Add-Type -AssemblyName 'System.IO.Compression.FileSystem'
    [System.IO.Compression.ZipFile]::CreateFromDirectory(
        $stage, $zipPath, [System.IO.Compression.CompressionLevel]::Optimal, $false)

    $archive = [System.IO.Compression.ZipFile]::OpenRead($zipPath)
    try {
        $names = @($archive.Entries | ForEach-Object { $_.FullName.Replace('\', '/') })
        $packedLensCount = @($names | Where-Object { $_ -like 'lenses/lens_files/*.lens' }).Count
        if ($packedLensCount -ne 1370) {
            throw "ZIP lens catalog incomplete: $packedLensCount."
        }
        foreach ($name in @('FlareLensAE.aex','FlareLensCatalog.tsv','README.md',
                            'INSTALL.md','NOTICE.md','LICENSE.txt','LICENSE_UPSTREAM.txt')) {
            if ($names -notcontains $name) {
                throw "ZIP missing: $name"
            }
        }
        $entry = $archive.GetEntry('FlareLensAE.aex')
        $stream = $entry.Open()
        try {
            $sha = [System.Security.Cryptography.SHA256]::Create()
            try {
                $archiveHash = [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '')
            } finally { $sha.Dispose() }
        } finally { $stream.Dispose() }
        $originalHash = (Get-FileHash -LiteralPath $binary -Algorithm SHA256).Hash
        if ($archiveHash -ne $originalHash) {
            throw 'Packed AEX checksum differs from the built AEX.'
        }
    } finally { $archive.Dispose() }

    $zipHash = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash
    Write-Output "PACKAGE_PATH=$zipPath"
    Write-Output "PACKAGE_BYTES=$((Get-Item -LiteralPath $zipPath).Length)"
    Write-Output "PACKAGE_SHA256=$zipHash"
    Write-Output "AEX_SHA256=$originalHash"
    Write-Output "LENS_FILES=$packedLensCount"
    Write-Output "PACKAGE_READY=1"
} catch {
    if (Test-Path -LiteralPath $zipPath) {
        Remove-Item -LiteralPath $zipPath -Force
    }
    throw
} finally {
    if (Test-Path -LiteralPath $stage) {
        Remove-Item -LiteralPath $stage -Recurse -Force
    }
}
