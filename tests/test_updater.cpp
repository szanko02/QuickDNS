#include "app_updater.h"
#include <iostream>
#include <cassert>
#include <unordered_map>
#include <string_view>

using namespace quickdns;

static int g_pass = 0;
static int g_fail = 0;

#define TEST_CHECK(cond, name) \
    do { \
        if (cond) { \
            std::cout << "[PASS] " << (name) << std::endl; \
            g_pass++; \
        } else { \
            std::cout << "[FAIL] " << (name) << " at line " << __LINE__ << std::endl; \
            g_fail++; \
        } \
    } while(0)

void test_semver_parsing() {
    auto v1 = AppUpdater::parse_semver("2.0.0");
    TEST_CHECK(v1.size() >= 3 && v1[0] == 2 && v1[1] == 0 && v1[2] == 0, "Parse 2.0.0");

    auto v2 = AppUpdater::parse_semver("v2.1.3");
    TEST_CHECK(v2.size() >= 3 && v2[0] == 2 && v2[1] == 1 && v2[2] == 3, "Parse v2.1.3");

    auto v3 = AppUpdater::parse_semver("V3.0.0-rc1");
    TEST_CHECK(v3.size() >= 3 && v3[0] == 3 && v3[1] == 0 && v3[2] == 0, "Parse V3.0.0-rc1");

    auto v4 = AppUpdater::parse_semver("1.8");
    TEST_CHECK(v4.size() >= 3 && v4[0] == 1 && v4[1] == 8 && v4[2] == 0, "Parse 1.8 with padding");
}

void test_version_comparison() {
    TEST_CHECK(AppUpdater::is_version_newer("2.0.0", "2.0.1"), "2.0.1 is newer than 2.0.0");
    TEST_CHECK(AppUpdater::is_version_newer("2.0.0", "v2.1.0"), "v2.1.0 is newer than 2.0.0");
    TEST_CHECK(AppUpdater::is_version_newer("2.0.0", "3.0.0"), "3.0.0 is newer than 2.0.0");
    TEST_CHECK(AppUpdater::is_version_newer("1.7.0", "2.0.0"), "2.0.0 is newer than 1.7.0");

    TEST_CHECK(!AppUpdater::is_version_newer("2.0.0", "2.0.0"), "2.0.0 is not newer than 2.0.0");
    TEST_CHECK(!AppUpdater::is_version_newer("2.0.0", "v2.0.0"), "v2.0.0 is not newer than 2.0.0");
    TEST_CHECK(!AppUpdater::is_version_newer("2.0.0", "1.9.9"), "1.9.9 is not newer than 2.0.0");
    TEST_CHECK(!AppUpdater::is_version_newer("2.1.0", "2.0.9"), "2.0.9 is not newer than 2.1.0");
}

void test_github_api_query() {
    // Check against live repository
    auto optInfo = AppUpdater::check_github_update("1.7.0");
    if (optInfo.has_value()) {
        const auto& info = optInfo.value();
        TEST_CHECK(!info.tag_name.empty(), "GitHub API returned release tag: " + info.tag_name);
        TEST_CHECK(info.semver >= "2.0.0", "GitHub API latest tag is >= 2.0.0: " + info.semver);
        TEST_CHECK(!info.download_url.empty(), "Download URL found: " + info.download_url);
        TEST_CHECK(info.is_newer, "v" + info.semver + " is correctly detected as newer than 1.7.0");

        auto optSame = AppUpdater::check_github_update(info.semver);
        if (optSame.has_value()) {
            TEST_CHECK(!optSame->is_newer, "Latest release is correctly detected as not newer than itself");
        }
    } else {
        std::cout << "[WARN] GitHub API unreachable (network offline?); skipping remote query." << std::endl;
    }
    const auto embedded = AppUpdater::check_github_update("1.7.0", "szanko02", "QuickDNS", UpdateEdition::Embedded);
    if (embedded) {
        TEST_CHECK(embedded->download_url.ends_with("/QuickDNS-Embedded.exe") &&
                   embedded->sha256_url.ends_with("/QuickDNS-Embedded.exe.sha256"),
                   "Live GitHub embedded pair found in " + embedded->tag_name);
    } else {
        std::cout << "[WARN] GitHub did not provide a compatible embedded release; skipping remote query." << std::endl;
    }
}

void test_edition_selection() {
    const std::string repo = "https://github.com/szanko02/QuickDNS";
    const std::string api = "https://api.github.com/repos/szanko02/QuickDNS";
    const auto asset = [&](const std::string& tag, const std::string& name) {
        return "{\"name\":\"" + name + "\",\"browser_download_url\":\"" +
            repo + "/releases/download/" + tag + "/" + name + "\"}";
    };
    const auto release = [&](const std::string& tag, const std::string& assets,
                             bool prerelease = false, bool draft = false) {
        return "{\"tag_name\":\"" + tag + "\",\"prerelease\":" + (prerelease ? "true" : "false") +
            ",\"draft\":" + (draft ? "true" : "false") + ",\"assets\":[" + assets + "]}";
    };
    const auto pair = [&](const std::string& tag, const std::string& name) {
        return asset(tag, name) + "," + asset(tag, name + ".sha256");
    };
    const std::string classic = "QuickDNS.exe";
    const std::string embedded = "QuickDNS-Embedded.exe";
    std::unordered_map<std::string, std::string> responses;
    const auto check = [&](UpdateEdition edition, const std::string& version = "2.2.3") {
        return AppUpdater::check_github_update(version, "szanko02", "QuickDNS", edition,
            [&](const std::string& url, bool) -> std::optional<std::string> {
                const auto it = responses.find(url);
                if (it == responses.end()) return std::nullopt;
                return it->second;
            });
    };

    responses[api + "/releases/latest"] = release("v2.3.1", pair("v2.3.1", classic));
    responses[api + "/releases?per_page=100"] = "[" +
        release("v2.2.9", pair("v2.2.9", embedded), true) + "," +
        release("v2.3.1", pair("v2.3.1", classic)) + "," +
        release("v2.2.10", pair("v2.2.10", embedded), true) + "," +
        release("v2.3.2", asset("v2.3.2", embedded) + "," + asset("v2.3.1", embedded + ".sha256")) + "," +
        release("v2.3.3", pair("v2.3.3", embedded), false, true) + "," +
        release("v2.3.4", pair("v2.3.4", embedded)) + "]";
    auto& catalog = responses[api + "/releases?per_page=100"];
    const auto wrong_repo = catalog.find("/szanko02/QuickDNS/releases/download/v2.3.4/");
    catalog.replace(wrong_repo, std::string("/szanko02/QuickDNS").size(), "/other/QuickDNS");
    const auto stable = check(UpdateEdition::Classic);
    TEST_CHECK(stable && stable->tag_name == "v2.3.1" && stable->download_url.ends_with("/QuickDNS.exe"),
               "Classic stays on the stable release and its own executable");
    const auto routing = check(UpdateEdition::Embedded);
    TEST_CHECK(routing && routing->tag_name == "v2.2.10" && routing->is_newer &&
               routing->download_url.ends_with("/QuickDNS-Embedded.exe") &&
               routing->sha256_url.ends_with("/QuickDNS-Embedded.exe.sha256"),
               "Embedded picks the highest numeric compatible prerelease and skips invalid pairs and drafts");
    const auto current = check(UpdateEdition::Embedded, "2.2.10");
    TEST_CHECK(current && !current->is_newer, "No update when already running the newest compatible release");
    responses[api + "/releases/latest"] = release("v2.3.1", pair("v2.3.1", embedded));
    TEST_CHECK(!check(UpdateEdition::Classic), "Classic never accepts the embedded executable as a fallback");

    responses.clear();
    const auto release_link = [&](const std::string& tag) {
        return "<link rel=\"alternate\" href=\"" + repo + "/releases/tag/" + tag + "\"/>";
    };
    const auto asset_link = [](const std::string& tag, const std::string& name) {
        return "<a href=\"/szanko02/QuickDNS/releases/download/" + tag + "/" + name + "\">asset</a>";
    };
    responses[repo + "/releases.atom"] = release_link("v2.2.4") + release_link("v2.3.1");
    responses[repo + "/releases/expanded_assets/v2.2.4"] =
        asset_link("v2.2.4", embedded) + asset_link("v2.2.4", embedded + ".sha256");
    responses[repo + "/releases/expanded_assets/v2.3.1"] =
        asset_link("v2.3.1", classic) + asset_link("v2.3.1", classic + ".sha256");
    responses[repo + "/releases/latest"] = repo + "/releases/tag/v2.3.1";
    const auto fallback = check(UpdateEdition::Embedded);
    TEST_CHECK(fallback && fallback->tag_name == "v2.2.4" && fallback->download_url.ends_with("/QuickDNS-Embedded.exe"),
               "Public GitHub fallback verifies the embedded pair and skips a newer classic release");
    const auto classic_fallback = check(UpdateEdition::Classic);
    TEST_CHECK(classic_fallback && classic_fallback->tag_name == "v2.3.1" &&
               classic_fallback->sha256_url.ends_with("/QuickDNS.exe.sha256"),
               "Stable fallback verifies the classic checksum asset before offering an update");
    responses[repo + "/releases/expanded_assets/v2.2.4"] =
        asset_link("v2.2.4", embedded) + asset_link("v2.2.3", embedded + ".sha256");
    TEST_CHECK(!check(UpdateEdition::Embedded), "Public GitHub fallback rejects a checksum from another release");
}

void test_allowed_hosts() {
    TEST_CHECK(AppUpdater::is_allowed_update_url(L"https://github.com/szanko02/QuickDNS/releases/download/v2.0.1/QuickDNS.exe"),
               "Allow github.com https");
    TEST_CHECK(AppUpdater::is_allowed_update_url(L"https://api.github.com/repos/szanko02/QuickDNS/releases/latest"),
               "Allow api.github.com https");
    TEST_CHECK(AppUpdater::is_allowed_update_url(L"https://objects.githubusercontent.com/github-production-release-asset-2e65be/123"),
               "Allow objects.githubusercontent.com https");
    TEST_CHECK(AppUpdater::is_allowed_update_url(L"https://release-assets.githubusercontent.com/123"),
               "Allow release-assets.githubusercontent.com https");

    TEST_CHECK(!AppUpdater::is_allowed_update_url(L"http://github.com/szanko02/QuickDNS/releases/download/v2.0.1/QuickDNS.exe"),
               "Reject http url on github.com");
    TEST_CHECK(!AppUpdater::is_allowed_update_url(L"https://evil.com/QuickDNS.exe"),
               "Reject unauthorized domain evil.com");
    TEST_CHECK(!AppUpdater::is_allowed_update_url(L"https://malicious-github.com/test"),
               "Reject malicious domain");
    TEST_CHECK(!AppUpdater::is_allowed_update_url(L"ftp://github.com/test"),
               "Reject non-https scheme");
    TEST_CHECK(!AppUpdater::is_allowed_update_url(L""),
               "Reject empty url");
}

void test_integrity_verification() {
    wchar_t tempPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tempPath);
    std::wstring dummyFile = std::wstring(tempPath) + L"quickdns_updater_test_binary.bin";

    // 1. Create a dummy binary > 100 KB with MZ header
    {
        HANDLE hFile = CreateFileW(dummyFile.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        TEST_CHECK(hFile != INVALID_HANDLE_VALUE, "Create dummy test binary file");
        if (hFile != INVALID_HANDLE_VALUE) {
            char header[2] = {'M', 'Z'};
            DWORD written = 0;
            WriteFile(hFile, header, 2, &written, nullptr);

            std::vector<char> padding(105000, 'A');
            WriteFile(hFile, padding.data(), static_cast<DWORD>(padding.size()), &written, nullptr);
            CloseHandle(hFile);
        }
    }

    // 2. Compute SHA-256
    std::string computedHash;
    bool hashOk = AppUpdater::compute_sha256(dummyFile, computedHash);
    TEST_CHECK(hashOk, "Compute SHA-256 via BCrypt");
    TEST_CHECK(computedHash.length() == 64, "SHA-256 hash length is 64 hex chars: " + computedHash);

    // 3. Verify valid binary and matching hash
    TEST_CHECK(AppUpdater::verify_downloaded_executable(dummyFile, computedHash),
               "Verify valid executable with matching hash");

    // 4. Case-insensitivity check
    std::string upperHash = computedHash;
    for (char& c : upperHash) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    TEST_CHECK(AppUpdater::verify_downloaded_executable(dummyFile, upperHash),
               "Verify hash comparison is case-insensitive");

    // 5. Mismatched hash
    std::string badHash = computedHash;
    badHash[0] = (badHash[0] == 'a') ? 'b' : 'a';
    TEST_CHECK(!AppUpdater::verify_downloaded_executable(dummyFile, badHash),
               "Reject executable with mismatched hash");

    // 6. Test file without MZ header
    {
        HANDLE hFile = CreateFileW(dummyFile.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile != INVALID_HANDLE_VALUE) {
            char header[2] = {'P', 'K'}; // ZIP header
            DWORD written = 0;
            WriteFile(hFile, header, 2, &written, nullptr);
            std::vector<char> padding(105000, 'A');
            WriteFile(hFile, padding.data(), static_cast<DWORD>(padding.size()), &written, nullptr);
            CloseHandle(hFile);
        }
    }
    std::string zipHash;
    AppUpdater::compute_sha256(dummyFile, zipHash);
    TEST_CHECK(!AppUpdater::verify_downloaded_executable(dummyFile, zipHash),
               "Reject non-MZ file (e.g. ZIP file)");

    // 7. Test file <= 100 KB
    {
        HANDLE hFile = CreateFileW(dummyFile.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile != INVALID_HANDLE_VALUE) {
            char header[2] = {'M', 'Z'};
            DWORD written = 0;
            WriteFile(hFile, header, 2, &written, nullptr);
            std::vector<char> padding(500, 'A');
            WriteFile(hFile, padding.data(), static_cast<DWORD>(padding.size()), &written, nullptr);
            CloseHandle(hFile);
        }
    }
    std::string smallHash;
    AppUpdater::compute_sha256(dummyFile, smallHash);
    TEST_CHECK(!AppUpdater::verify_downloaded_executable(dummyFile, smallHash),
               "Reject file smaller than 100 KB");

    DeleteFileW(dummyFile.c_str());
}

static std::string read_file_content(const std::wstring& path) {
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return "";
    DWORD size = GetFileSize(hFile, nullptr);
    std::string content(size, '\0');
    DWORD read = 0;
    ReadFile(hFile, &content[0], size, &read, nullptr);
    CloseHandle(hFile);
    return content;
}

static bool write_file_content(const std::wstring& path, const std::string& content) {
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(hFile, content.data(), static_cast<DWORD>(content.size()), &written, nullptr);
    CloseHandle(hFile);
    return ok && (written == content.size());
}

void test_file_replacement_and_rollback() {
    wchar_t tempPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tempPath);
    std::wstring currPath = std::wstring(tempPath) + L"quickdns_test_current.bin";
    std::wstring newPath  = std::wstring(tempPath) + L"quickdns_test_downloaded.bin";
    std::wstring oldPath  = std::wstring(tempPath) + L"quickdns_test_current.bin.old";

    // 1. Success case: target -> old, downloaded -> target
    {
        write_file_content(currPath, "CURRENT_VERSION_203");
        write_file_content(newPath,  "NEW_VERSION_204");
        DeleteFileW(oldPath.c_str());

        std::wstring err;
        bool ok = AppUpdater::perform_file_replacement(currPath, newPath, oldPath, &err);
        TEST_CHECK(ok, "perform_file_replacement returns true on valid files");
        TEST_CHECK(read_file_content(currPath) == "NEW_VERSION_204", "Target file updated to new version content");
        TEST_CHECK(read_file_content(oldPath) == "CURRENT_VERSION_203", "Old backup file contains previous version content");
        TEST_CHECK(GetFileAttributesW(newPath.c_str()) == INVALID_FILE_ATTRIBUTES, "Downloaded file moved out of download path");
    }

    // 2. Pre-existing .old file is cleaned up before next update
    {
        write_file_content(newPath, "NEW_VERSION_205");
        std::wstring err;
        bool ok = AppUpdater::perform_file_replacement(currPath, newPath, oldPath, &err);
        TEST_CHECK(ok, "perform_file_replacement succeeds when .old already exists");
        TEST_CHECK(read_file_content(currPath) == "NEW_VERSION_205", "Target file updated with existing .old");
        TEST_CHECK(read_file_content(oldPath) == "NEW_VERSION_204", "Old backup replaced with previous target");
    }

    // 3. Rollback case: if downloaded file is missing or invalid, target is restored from .old
    {
        std::wstring missingNew = std::wstring(tempPath) + L"quickdns_nonexistent_download.bin";
        DeleteFileW(missingNew.c_str());

        std::wstring err;
        bool ok = AppUpdater::perform_file_replacement(currPath, missingNew, oldPath, &err);
        TEST_CHECK(!ok, "perform_file_replacement fails when downloaded file is missing");
        TEST_CHECK(!err.empty(), "Error diagnostics populated: " + std::string(err.begin(), err.end()));
        TEST_CHECK(read_file_content(currPath) == "NEW_VERSION_205", "Target file safely restored to original via rollback");
    }

    // Clean up temporary test files
    DeleteFileW(currPath.c_str());
    DeleteFileW(newPath.c_str());
    DeleteFileW(oldPath.c_str());
}

int main(int argc, char** argv) {
    std::cout << "========================================" << std::endl;
    std::cout << "     QuickDNS App Updater Tests" << std::endl;
    std::cout << "========================================" << std::endl;

    if (argc == 2 && std::string_view(argv[1]) == "--live") {
        test_github_api_query();
    } else {
        test_semver_parsing();
        test_version_comparison();
        test_edition_selection();
        test_allowed_hosts();
        test_integrity_verification();
        test_file_replacement_and_rollback();
    }

    std::cout << "========================================" << std::endl;
    std::cout << "Results: " << g_pass << " PASSED, " << g_fail << " FAILED" << std::endl;
    std::cout << "========================================" << std::endl;

    return (g_fail == 0) ? 0 : 1;
}
