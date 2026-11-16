param(
    [string]$Version,
    [string]$Executable,
    [string]$RoutingEngine
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
    $Version = "2.2.3"
}

$distDir = Join-Path $rootDir "dist\QuickDNS"
$distRoot = Split-Path -Parent $distDir
foreach ($path in $distRoot, $distDir) {
    if ((Test-Path -LiteralPath $path) -and ((Get-Item -LiteralPath $path).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw "Packaging path is a reparse point: $path"
    }
}
if (Test-Path -LiteralPath $distDir) { Remove-Item -LiteralPath $distDir -Recurse -Force }
New-Item -ItemType Directory -Force -Path $distDir | Out-Null

$exeSource = Join-Path $rootDir "QuickDNS.exe"
if ($Executable) {
    $exeSource = (Resolve-Path -LiteralPath $Executable -ErrorAction Stop).Path
} elseif (Test-Path (Join-Path $rootDir "build\Release\QuickDNS.exe")) {
    $exeSource = Join-Path $rootDir "build\Release\QuickDNS.exe"
} elseif (Test-Path (Join-Path $rootDir "build\QuickDNS.exe")) {
    $exeSource = Join-Path $rootDir "build\QuickDNS.exe"
}

Copy-Item -LiteralPath $exeSource -Destination (Join-Path $distDir "QuickDNS.exe") -Force -ErrorAction Stop
if ($RoutingEngine) {
    Copy-Item -LiteralPath $RoutingEngine -Destination (Join-Path $distDir "sing-box.exe") -Force -ErrorAction Stop
}
Copy-Item -Path (Join-Path $rootDir "README.txt") -Destination (Join-Path $distDir "README.txt") -Force
if (Test-Path (Join-Path $rootDir "README.md")) {
    Copy-Item -Path (Join-Path $rootDir "README.md") -Destination (Join-Path $distDir "README.md") -Force
}
foreach ($license in @("LICENSE", "LICENSE-MIT")) {
    Copy-Item -LiteralPath (Join-Path $rootDir $license) -Destination $distDir -Force -ErrorAction Stop
}

$zipPath = Join-Path $rootDir "dist\QuickDNS-v$Version-Portable.zip"
if (Test-Path $zipPath) { Remove-Item -Force $zipPath }
Compress-Archive -Path "$distDir\*" -DestinationPath $zipPath -Force

Write-Host "Created archive: $zipPath"
Get-Item $zipPath | Select-Object Name, Length, LastWriteTime

