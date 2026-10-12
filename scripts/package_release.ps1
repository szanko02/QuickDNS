$rootDir = (Resolve-Path "$PSScriptRoot\..").Path
$distDir = Join-Path $rootDir "dist\QuickDNS"
if (Test-Path $distDir) { Remove-Item -Recurse -Force $distDir }
New-Item -ItemType Directory -Force -Path $distDir | Out-Null

Copy-Item -Path (Join-Path $rootDir "QuickDNS.exe") -Destination (Join-Path $distDir "QuickDNS.exe") -Force

Copy-Item -Path (Join-Path $rootDir "README.txt") -Destination (Join-Path $distDir "README.txt") -Force

$zipPath = Join-Path $rootDir "dist\QuickDNS-v1.5.0-Portable.zip"
if (Test-Path $zipPath) { Remove-Item -Force $zipPath }
Compress-Archive -Path "$distDir\*" -DestinationPath $zipPath -Force

Write-Host "Created archive: $zipPath"
Get-Item $zipPath | Select-Object Name, Length, LastWriteTime
