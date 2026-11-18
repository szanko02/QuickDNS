#pragma once

#include <string>
#include <vector>
#include <optional>
#include <functional>
#include <windows.h>

namespace quickdns {

#ifdef OVERRIDE_APP_VERSION
constexpr const char* APP_VERSION = OVERRIDE_APP_VERSION;
constexpr const wchar_t* APP_VERSION_W = OVERRIDE_APP_VERSION_W;
#else
constexpr const char* APP_VERSION = "2.2.5";
constexpr const wchar_t* APP_VERSION_W = L"2.2.5";
#endif

enum class UpdateEdition { Classic, Embedded };

struct ReleaseInfo {
    std::string tag_name;       // e.g. "v2.0.1"
    std::string semver;         // e.g. "2.0.1"
    std::string title;          // Release title
    std::string notes;          // Release body/changelog
    std::string download_url;   // Direct URL for the executable of the selected edition
    std::string sha256_url;     // SHA-256 asset paired with the executable
    size_t asset_size = 0;
    bool is_newer = false;
};

class AppUpdater {
public:
    // Returns a response body, or the Location header for a redirect-only request.
    using ReleaseFetcher = std::function<std::optional<std::string>(const std::string&, bool redirectOnly)>;

    // Compare semver: returns true if remoteVer > currentVer
    static bool is_version_newer(const std::string& currentVer, const std::string& remoteVer);

    // Finds the newest published release compatible with the installed edition.
    static std::optional<ReleaseInfo> check_github_update(
        const std::string& currentVersion = APP_VERSION,
        const std::string& repoOwner = "szanko02",
        const std::string& repoName = "QuickDNS",
        UpdateEdition edition = UpdateEdition::Classic,
        ReleaseFetcher fetch = {}
    );

    // Downloads file from URL to destPath with progress callback
    static bool download_file(
        const std::wstring& url,
        const std::wstring& destPath,
        std::function<void(DWORD downloaded, DWORD total)> onProgress = nullptr
    );

    // Replaces running executable with downloaded one and restarts the process.
    // Optionally populates outError with diagnostic details on failure.
    static bool apply_update_and_restart(const std::wstring& downloadedExePath, std::wstring* outError = nullptr);

    // Atomically replaces targetExePath with downloadedExePath using oldExePath as backup,
    // retrying on sharing/locking errors and rolling back on failure.
    static bool perform_file_replacement(
        const std::wstring& targetExePath,
        const std::wstring& downloadedExePath,
        const std::wstring& oldExePath,
        std::wstring* outError = nullptr
    );

    // Cleans up QuickDNS.exe.old if it exists from a previous update
    static void cleanup_previous_update();

    // Checks if the given URL is a secure HTTPS URL pointing to an authorized domain
    static bool is_allowed_update_url(const std::wstring& url);

    // Downloads and parses SHA-256 hex string (at most 1 KB response)
    static std::string download_sha256(const std::wstring& sha256Url);

    // Computes SHA-256 hash hex string of local file via Windows BCrypt API
    static bool compute_sha256(const std::wstring& filePath, std::string& outHashHex);

    // Verifies executable integrity: size > 100 KB, 'MZ' header, matching SHA-256
    static bool verify_downloaded_executable(const std::wstring& filePath, const std::string& expectedSha256);

    // Reads resources from the candidate without loading it as executable code.
    static bool verify_executable_edition(const std::wstring& filePath, UpdateEdition edition);

    // Helper: parse semantic version components [major, minor, patch]
    static std::vector<int> parse_semver(const std::string& ver);
};

} // namespace quickdns
