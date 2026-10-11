$rootDir = (Resolve-Path "$PSScriptRoot\..").Path
$distDir = Join-Path $rootDir "dist\QuickDNS"
if (Test-Path $distDir) { Remove-Item -Recurse -Force $distDir }
New-Item -ItemType Directory -Force -Path $distDir | Out-Null

Copy-Item -Path (Join-Path $rootDir "QuickDNS.exe") -Destination (Join-Path $distDir "QuickDNS.exe") -Force

$readmeText = @"
==================================================
QuickDNS — Быстрая смена DNS в 1 клик для Windows
Версия: 1.2.0
==================================================

КАК ЗАПУСТИТЬ:
1. Запустите файл QuickDNS.exe.
2. При запросе Windows подтвердите запуск от имени Администратора (необходимо для изменения сетевых параметров системы).
3. Выберите нужный DNS-сервер (Comss.one, GeoHide, Без межаў, Zapret, Malw.link, Cloudflare, Google или ваш собственный).
4. Нажмите «Применить DNS в 1 клик» — готово!

ОСОБЕННОСТИ:
- Приложение не требует установки (Portable).
- Встроенный тест скорости (пинг) всех DNS-серверов в реальном времени с цветовой градацией.
- Поддержка DoH адресов (DNS-over-HTTPS) для всех популярных обходных DNS.
- Сворачивается в системный трей (панель задач) возле часов.
- Правый клик по иконке в трее позволяет переключать DNS в 1 клик без открытия главного окна.
- Настройки и добавленные профили сохраняются в %APPDATA%\QuickDNS\profiles.json.

Исходный код и обновления:
https://github.com/szanko02/QuickDNS
==================================================
"@

[System.IO.File]::WriteAllText((Join-Path $distDir "README.txt"), $readmeText, [System.Text.Encoding]::UTF8)

$zipPath = Join-Path $rootDir "dist\QuickDNS-v1.2.0-Portable.zip"
if (Test-Path $zipPath) { Remove-Item -Force $zipPath }
Compress-Archive -Path "$distDir\*" -DestinationPath $zipPath -Force

Write-Host "Created archive: $zipPath"
Get-Item $zipPath | Select-Object Name, Length, LastWriteTime
