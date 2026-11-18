# Packages QuickDNS Portable ZIP archive (e.g. dist/QuickDNS-v2.3.1-Portable.zip).
# Auto-detects version from CMakeLists.txt (2.3.1) unless -Version is explicitly passed.
[CmdletBinding()]
param(
    [string]$Version,
    [Alias('Executable')]
    [string]$ExePath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$rootDir = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path

if (-not $Version) {
    $cmakePath = Join-Path $rootDir 'CMakeLists.txt'
    if (-not (Test-Path -LiteralPath $cmakePath -PathType Leaf)) {
        throw "CMakeLists.txt not found at '$cmakePath' and -Version was not specified."
    }
    $cmakeContent = Get-Content -LiteralPath $cmakePath -Raw -Encoding UTF8
    if ($cmakeContent -match 'project\s*\(\s*QuickDNS\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)') {
        $Version = $matches[1]
    } else {
        throw "Unable to extract QuickDNS VERSION from '$cmakePath'; specify -Version explicitly."
    }
}

$exeSource = $null
if ($ExePath) {
    $candidatePath = if ([System.IO.Path]::IsPathRooted($ExePath)) {
        $ExePath
    } else {
        Join-Path $rootDir $ExePath
    }
    if (-not (Test-Path -LiteralPath $candidatePath -PathType Leaf)) {
        throw "Specified -ExePath does not exist: '$candidatePath'"
    }
    $exeSource = (Resolve-Path -LiteralPath $candidatePath).Path
} else {
    $candidates = @(
        (Join-Path $rootDir 'build\Release\QuickDNS.exe'),
        (Join-Path $rootDir 'build\QuickDNS.exe'),
        (Join-Path $rootDir 'QuickDNS.exe')
    ) | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | ForEach-Object { Get-Item -LiteralPath $_ }

    if (@($candidates).Count -eq 0) {
        throw "QuickDNS.exe not found in build\Release\, build\, or repository root. Build the project or pass -ExePath."
    }
    $exeSource = ($candidates | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1).FullName
}

$exeItem = Get-Item -LiteralPath $exeSource
if ($exeItem.Length -le 102400) {
    throw "Executable '$exeSource' is unexpectedly small ($($exeItem.Length) bytes; must be > 100 KB)."
}

$prodVersion = $exeItem.VersionInfo.ProductVersion
if ($prodVersion -and $prodVersion.Trim().Length -gt 0) {
    $cleanProd = $prodVersion.Trim()
    if (-not $cleanProd.StartsWith($Version)) {
        throw "Executable '$exeSource' ProductVersion '$cleanProd' does not match target package version '$Version'."
    }
}

$distRoot = Join-Path $rootDir 'dist'
$distDir = Join-Path $distRoot 'QuickDNS'
if (Test-Path -LiteralPath $distDir) {
    Remove-Item -LiteralPath $distDir -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $distDir | Out-Null

# Clean any stale portable archives so `dist/*.zip` globs only match the newly built version
if (Test-Path -LiteralPath $distRoot) {
    Get-ChildItem -LiteralPath $distRoot -Filter 'QuickDNS-*-Portable.zip' -File -ErrorAction SilentlyContinue |
        Remove-Item -Force
}

$readmeTxt = Join-Path $rootDir 'README.txt'
if (-not (Test-Path -LiteralPath $readmeTxt -PathType Leaf)) {
    throw "Required portable documentation file missing: '$readmeTxt'"
}

Copy-Item -LiteralPath $exeSource -Destination (Join-Path $distDir 'QuickDNS.exe') -Force
Copy-Item -LiteralPath $readmeTxt -Destination (Join-Path $distDir 'README.txt') -Force

$readmeMd = Join-Path $rootDir 'README.md'
if (Test-Path -LiteralPath $readmeMd -PathType Leaf) {
    Copy-Item -LiteralPath $readmeMd -Destination (Join-Path $distDir 'README.md') -Force
}

$licensePath = Join-Path $rootDir 'LICENSE'
if (Test-Path -LiteralPath $licensePath -PathType Leaf) {
    Copy-Item -LiteralPath $licensePath -Destination (Join-Path $distDir 'LICENSE') -Force
}

# Include QuickDNS.exe.sha256 sidecar if present next to the binary or in repo root
$shaCandidates = @(
    "$exeSource.sha256",
    (Join-Path $rootDir 'QuickDNS.exe.sha256')
)
foreach ($shaCandidate in $shaCandidates) {
    if (Test-Path -LiteralPath $shaCandidate -PathType Leaf) {
        Copy-Item -LiteralPath $shaCandidate -Destination (Join-Path $distDir 'QuickDNS.exe.sha256') -Force
        break
    }
}

$zipPath = Join-Path $distRoot "QuickDNS-v$Version-Portable.zip"
Compress-Archive -Path (Join-Path $distDir '*') -DestinationPath $zipPath -Force

$zipItem = Get-Item -LiteralPath $zipPath
if ($zipItem.Length -le 102400) {
    throw "Generated archive '$zipPath' is unexpectedly small ($($zipItem.Length) bytes)."
}

Write-Host "Created archive: $zipPath (from $exeSource)"
$zipItem | Select-Object Name, Length, LastWriteTime
