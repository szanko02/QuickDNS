#pragma once

#include <string>
#include <vector>
#include <optional>
#include <functional>
#include <windows.h>

namespace quickdns {

struct ReleaseInfo {
    std::string tag_name;       // e.g. "v2.0.1"
    std::string semver;         // e.g. "2.0.1"
    std::string title;          // Release title
    std::string notes;          // Release body/changelog
    std::string download_url;   // Direct download URL for QuickDNS.exe
    size_t asset_size = 0;
    bool is_newer = false;
};

class AppUpdater {
public:
    // Compare semver: returns true if remoteVer > currentVer
    static bool is_version_newer(const std::string& currentVer, const std::string& remoteVer);

    // Queries GitHub Releases API for szanko02/QuickDNS/releases/latest
    static std::optional<ReleaseInfo> check_github_update(
        const std::string& currentVersion = "2.0.0",
        const std::string& repoOwner = "szanko02",
        const std::string& repoName = "QuickDNS"
    );

    // Downloads file from URL to destPath with progress callback
    static bool download_file(
        const std::wstring& url,
        const std::wstring& destPath,
        std::function<void(DWORD downloaded, DWORD total)> onProgress = nullptr
    );

    // Replaces running executable with downloaded one and restarts the process
    static bool apply_update_and_restart(const std::wstring& downloadedExePath);

    // Cleans up QuickDNS.exe.old if it exists from a previous update
    static void cleanup_previous_update();

    // Helper: parse semantic version components [major, minor, patch]
    static std::vector<int> parse_semver(const std::string& ver);
};

} // namespace quickdns
