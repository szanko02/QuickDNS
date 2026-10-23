param(
    [string]$Version
)

$rootDir = (Resolve-Path "$PSScriptRoot\..").Path

if (-not $Version) {
    $cmakePath = Join-Path $rootDir "CMakeLists.txt"
    if (Test-Path $cmakePath) {
        $cmakeContent = Get-Content $cmakePath -Raw
        if ($cmakeContent -match 'project\s*\(\s*QuickDNS\s+VERSION\s+([0-9.]+)') {
            $Version = $matches[1]
        }
    }
}

if (-not $Version) {
    $Version = "1.7.0"
}

$distDir = Join-Path $rootDir "dist\QuickDNS"
if (Test-Path $distDir) { Remove-Item -Recurse -Force $distDir }
New-Item -ItemType Directory -Force -Path $distDir | Out-Null

$exeSource = Join-Path $rootDir "QuickDNS.exe"
if (Test-Path (Join-Path $rootDir "build\Release\QuickDNS.exe")) {
    $exeSource = Join-Path $rootDir "build\Release\QuickDNS.exe"
} elseif (Test-Path (Join-Path $rootDir "build\QuickDNS.exe")) {
    $exeSource = Join-Path $rootDir "build\QuickDNS.exe"
}

Copy-Item -Path $exeSource -Destination (Join-Path $distDir "QuickDNS.exe") -Force
Copy-Item -Path (Join-Path $rootDir "README.txt") -Destination (Join-Path $distDir "README.txt") -Force
if (Test-Path (Join-Path $rootDir "README.md")) {
    Copy-Item -Path (Join-Path $rootDir "README.md") -Destination (Join-Path $distDir "README.md") -Force
}
if (Test-Path (Join-Path $rootDir "LICENSE")) {
    Copy-Item -Path (Join-Path $rootDir "LICENSE") -Destination (Join-Path $distDir "LICENSE") -Force
}

$zipPath = Join-Path $rootDir "dist\QuickDNS-v$Version-Portable.zip"
if (Test-Path $zipPath) { Remove-Item -Force $zipPath }
Compress-Archive -Path "$distDir\*" -DestinationPath $zipPath -Force

Write-Host "Created archive: $zipPath"
Get-Item $zipPath | Select-Object Name, Length, LastWriteTime

