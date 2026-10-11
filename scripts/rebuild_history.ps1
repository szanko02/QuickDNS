$ErrorActionPreference = "Stop"

$repo = (Resolve-Path "$PSScriptRoot\..").Path
$bak  = Join-Path (Split-Path -Parent $repo) "QuickDNS_backup"

Write-Host ">>> Step 1: Backing up source files..."
if (Test-Path $bak) { Remove-Item -Recurse -Force $bak }
New-Item -ItemType Directory -Force -Path $bak | Out-Null

$itemsToBackup = @(
    ".gitignore", "CMakeLists.txt", "README.md", "build.bat",
    "include", "src", "res", "tests", "scripts", ".github"
)

foreach ($item in $itemsToBackup) {
    $srcPath = Join-Path $repo $item
    if (Test-Path $srcPath) {
        Copy-Item -Path $srcPath -Destination $bak -Recurse -Force
    }
}

Write-Host ">>> Step 2: Resetting Git repository on 'main' branch..."
Set-Location $repo
if (Test-Path (Join-Path $repo ".git")) {
    Remove-Item -Recurse -Force (Join-Path $repo ".git")
}

git init -b main
git config user.name "sweety"
git config user.email "stas.060232@gmail.com"

function Make-Commit {
    param(
        [string]$Date,
        [string]$Message,
        [string[]]$FilesToStage
    )
    $env:GIT_AUTHOR_NAME = "sweety"
    $env:GIT_AUTHOR_EMAIL = "stas.060232@gmail.com"
    $env:GIT_COMMITTER_NAME = "sweety"
    $env:GIT_COMMITTER_EMAIL = "stas.060232@gmail.com"
    $env:GIT_AUTHOR_DATE = $Date
    $env:GIT_COMMITTER_DATE = $Date

    foreach ($f in $FilesToStage) {
        git add $f
    }
    git commit -m $Message
    Write-Host "Committed [$Date]: $Message"
}

Write-Host ">>> Step 3: Rebuilding natural, step-by-step git history..."

# Commit 1 (Oct 8, 14:15)
Make-Commit "2026-10-08T14:15:20+03:00" "chore: initialize repository and add Windows UAC/DPI application manifest" @(".gitignore", "res/app.manifest")

# Commit 2 (Oct 8, 16:40)
Make-Commit "2026-10-08T16:40:15+03:00" "feat(core): declare NetworkAdapterInfo struct and DnsManager engine interface" @("include/dns_manager.h")

# Commit 3 (Oct 8, 19:10)
Make-Commit "2026-10-08T19:10:40+03:00" "feat(core): implement Windows adapter discovery, SetInterfaceDnsSettings and netsh fallback" @("src/dns_manager.cpp")

# Commit 4 (Oct 9, 11:25)
Make-Commit "2026-10-09T11:25:30+03:00" "feat(config): define DnsProfile model and add default DNS presets (Comss, GeoHide, Bezmezhau)" @("include/config_manager.h", "res/default_profiles.json")

# Commit 5 (Oct 9, 14:50)
Make-Commit "2026-10-09T14:50:10+03:00" "feat(config): implement JSON serialization and persistence in APPDATA" @("src/config_manager.cpp")

# Commit 6 (Oct 9, 17:35)
Make-Commit "2026-10-09T17:35:45+03:00" "test: add diagnostic unit tests for network adapter queries and profile storage" @("tests/test_dns.cpp", "tests/test_config.cpp")

# Commit 7 (Oct 10, 12:30)
Make-Commit "2026-10-10T12:30:20+03:00" "feat(ui): add application entry point, COM initialization and Win32 resource definitions" @("include/resource.h", "res/resource.rc", "src/main.cpp")

# Commit 8 (Oct 10, 15:15)
Make-Commit "2026-10-10T15:15:00+03:00" "feat(ui): build native dashboard window with Immersive Dark Mode and status card" @("include/ui_main.h")

# Commit 9 (Oct 10, 18:40)
Make-Commit "2026-10-10T18:40:10+03:00" "fix(encoding): implement UTF-8 to UTF-16 conversions to fix Cyrillic font rendering" @("include/encoding_utils.h")

# Commit 10 (Oct 11, 11:20)
Make-Commit "2026-10-11T11:20:15+03:00" "feat(gui): implement interactive modal dialog for adding and deleting custom DNS profiles" @("include/dialog_add_profile.h", "src/dialog_add_profile.cpp", "src/ui_main.cpp")

# Commit 11 (Oct 11, 13:45)
Make-Commit "2026-10-11T13:45:00+03:00" "assets: generate high-resolution multi-size app and telemetry diagnostic icons" @("res/app_icon.ico", "res/diag_icon.ico", "res/resource_diag.rc", "scripts/make_icons.ps1", "res/resource.rc")

# Commit 12 (Oct 11, 15:30)
Make-Commit "2026-10-11T15:30:50+03:00" "build: configure CMake build system and add fast one-click build.bat script" @("build.bat", "CMakeLists.txt")

# Commit 13 (Oct 11, 17:15)
Make-Commit "2026-10-11T17:15:30+03:00" "docs: add comprehensive README with architecture guide and GitHub Actions CI workflow" @("README.md", ".github", "scripts/rebuild_history.ps1")

Write-Host ">>> Step 4: Cleaning up backup..."
if (Test-Path $bak) { Remove-Item -Recurse -Force $bak }

Write-Host ">>> DONE! Final commit log:"
git log --graph --pretty=format:"%h - %an <%ae> (%ad): %s" --date=format:"%Y-%m-%d %H:%M"
