#include "app_updater.h"
#include "encoding_utils.h"

#include <cctype>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

using namespace quickdns;

namespace {

int g_pass = 0;
int g_fail = 0;

#define TEST_CHECK(cond, name) \
    do { \
        if (cond) { \
            std::cout << "[PASS] " << (name) << std::endl; \
            ++g_pass; \
        } else { \
            std::cout << "[FAIL] " << (name) << " at line " << __LINE__ << std::endl; \
            ++g_fail; \
        } \
    } while (0)

std::string read_file_content(const std::wstring& path) {
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return "";
    DWORD size = GetFileSize(hFile, nullptr);
    if (size == INVALID_FILE_SIZE || size == 0) {
        CloseHandle(hFile);
        return "";
    }
    std::string content(size, '\0');
    DWORD read = 0;
    BOOL ok = ReadFile(hFile, content.data(), size, &read, nullptr);
    CloseHandle(hFile);
    if (!ok || read != size) return "";
    return content;
}

bool write_file_content(const std::wstring& path, const std::string& content) {
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(hFile, content.data(), static_cast<DWORD>(content.size()), &written, nullptr);
    CloseHandle(hFile);
    return ok && (written == content.size());
}

void test_semver_parsing() {
    auto v1 = AppUpdater::parse_semver("2.0.0");
    TEST_CHECK(v1.size() >= 3 && v1[0] == 2 && v1[1] == 0 && v1[2] == 0, "Parse 2.0.0");

    auto v2 = AppUpdater::parse_semver("v2.1.3");
    TEST_CHECK(v2.size() >= 3 && v2[0] == 2 && v2[1] == 1 && v2[2] == 3, "Parse v2.1.3");

    auto v3 = AppUpdater::parse_semver("V3.0.0-rc1");
    TEST_CHECK(v3.size() >= 3 && v3[0] == 3 && v3[1] == 0 && v3[2] == 0, "Parse V3.0.0-rc1");

    auto v4 = AppUpdater::parse_semver("1.8");
    TEST_CHECK(v4.size() >= 3 && v4[0] == 1 && v4[1] == 8 && v4[2] == 0, "Parse 1.8 with zero padding");
}

void test_version_comparison() {
    TEST_CHECK(AppUpdater::is_version_newer("2.0.0", "2.0.1"), "2.0.1 is newer than 2.0.0");
    TEST_CHECK(AppUpdater::is_version_newer("2.0.0", "v2.1.0"), "v2.1.0 is newer than 2.0.0");
    TEST_CHECK(AppUpdater::is_version_newer("2.0.0", "3.0.0"), "3.0.0 is newer than 2.0.0");
    TEST_CHECK(AppUpdater::is_version_newer("1.7.0", "2.0.0"), "2.0.0 is newer than 1.7.0");
    TEST_CHECK(AppUpdater::is_version_newer("2.9.0", "10.0.0"), "10.0.0 is newer than 2.9.0 (numeric SemVer)");
    TEST_CHECK(!AppUpdater::is_version_newer("10.0.0", "2.9.0"), "2.9.0 is not newer than 10.0.0");

    TEST_CHECK(!AppUpdater::is_version_newer("2.0.0", "2.0.0"), "2.0.0 is not newer than 2.0.0");
    TEST_CHECK(!AppUpdater::is_version_newer("2.0.0", "v2.0.0"), "v2.0.0 is not newer than 2.0.0");
    TEST_CHECK(!AppUpdater::is_version_newer("2.0.0", "1.9.9"), "1.9.9 is not newer than 2.0.0");
    TEST_CHECK(!AppUpdater::is_version_newer("2.1.0", "2.0.9"), "2.0.9 is not newer than 2.1.0");

    // Pre-release SemVer 2.0.0 precedence: 2.1.0 > 2.1.0-rc2 > 2.1.0-rc1, and +build metadata ignored
    TEST_CHECK(AppUpdater::is_version_newer("2.1.0-rc1", "2.1.0"), "Stable 2.1.0 is newer than pre-release 2.1.0-rc1");
    TEST_CHECK(!AppUpdater::is_version_newer("2.1.0", "2.1.0-rc1"), "Pre-release 2.1.0-rc1 is not newer than stable 2.1.0");
    TEST_CHECK(AppUpdater::is_version_newer("2.1.0-rc1", "2.1.0-rc2"), "2.1.0-rc2 is newer than 2.1.0-rc1");
    TEST_CHECK(!AppUpdater::is_version_newer("2.1.0-rc2", "2.1.0-rc1"), "2.1.0-rc1 is not newer than 2.1.0-rc2");
    TEST_CHECK(!AppUpdater::is_version_newer("2.1.0", "2.1.0+20261118"), "Build metadata +20261118 does not make version newer");
    TEST_CHECK(!AppUpdater::is_version_newer("2.1.0+1", "2.1.0"), "2.1.0 is not newer than 2.1.0+1");
}

void test_allowed_hosts() {
    TEST_CHECK(AppUpdater::is_allowed_update_url(L"https://github.com/szanko02/QuickDNS/releases/download/v2.1.0/QuickDNS.exe"),
               "Allow github.com https");
    TEST_CHECK(AppUpdater::is_allowed_update_url(L"https://api.github.com/repos/szanko02/QuickDNS/releases/latest"),
               "Allow api.github.com https");
    TEST_CHECK(AppUpdater::is_allowed_update_url(L"https://objects.githubusercontent.com/github-production-release-asset-2e65be/123"),
               "Allow objects.githubusercontent.com https");
    TEST_CHECK(AppUpdater::is_allowed_update_url(L"https://release-assets.githubusercontent.com/123"),
               "Allow release-assets.githubusercontent.com https");

    TEST_CHECK(!AppUpdater::is_allowed_update_url(L"http://github.com/szanko02/QuickDNS/releases/download/v2.1.0/QuickDNS.exe"),
               "Reject cleartext http url on github.com");
    TEST_CHECK(!AppUpdater::is_allowed_update_url(L"https://evil.com/QuickDNS.exe"),
               "Reject unauthorized domain evil.com");
    TEST_CHECK(!AppUpdater::is_allowed_update_url(L"https://malicious-github.com/test"),
               "Reject malicious-github.com prefix spoof");
    TEST_CHECK(!AppUpdater::is_allowed_update_url(L"https://github.com.evil.com/QuickDNS.exe"),
               "Reject github.com.evil.com suffix spoof");
    TEST_CHECK(!AppUpdater::is_allowed_update_url(L"https://evil.github.com/QuickDNS.exe"),
               "Reject untrusted subdomain evil.github.com");
    TEST_CHECK(!AppUpdater::is_allowed_update_url(L"https://github.com@evil.com/QuickDNS.exe"),
               "Reject userinfo spoofing https://github.com@evil.com/...");
    TEST_CHECK(!AppUpdater::is_allowed_update_url(L"https://user:pass@github.com/szanko02/QuickDNS/releases/download/v2.1.0/QuickDNS.exe"),
               "Reject credentials embedded in github.com URL");
    TEST_CHECK(!AppUpdater::is_allowed_update_url(L"https://github.com:8443/szanko02/QuickDNS/releases/download/v2.1.0/QuickDNS.exe"),
               "Reject non-443 custom port on github.com");
    TEST_CHECK(!AppUpdater::is_allowed_update_url(L"ftp://github.com/test"),
               "Reject non-https scheme");
    TEST_CHECK(!AppUpdater::is_allowed_update_url(L""),
               "Reject empty url");
}

void test_integrity_verification(const std::filesystem::path& tempDir) {
    const std::wstring dummyFile = (tempDir / L"updater_test_binary.bin").wstring();
    const std::wstring missingFile = (tempDir / L"does_not_exist.bin").wstring();

    // 1. Create a valid dummy PE stub > 100 KB with MZ header
    {
        HANDLE hFile = CreateFileW(dummyFile.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        bool created = (hFile != INVALID_HANDLE_VALUE);
        TEST_CHECK(created, "Create dummy test binary file in per-PID temp dir");
        if (created) {
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

    std::string missingHash;
    bool missingOk = AppUpdater::compute_sha256(missingFile, missingHash);
    TEST_CHECK(!missingOk, "compute_sha256 returns false for missing file");

    // 3. Verify valid binary and matching hash
    TEST_CHECK(AppUpdater::verify_downloaded_executable(dummyFile, computedHash),
               "Verify valid executable with matching SHA-256");

    // 4. Case-insensitivity check
    std::string upperHash = computedHash;
    for (char& c : upperHash) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    TEST_CHECK(AppUpdater::verify_downloaded_executable(dummyFile, upperHash),
               "Verify SHA-256 comparison is case-insensitive");

    // 5. Mismatched & malformed SHA-256 inputs
    std::string badHash = computedHash;
    badHash[0] = (badHash[0] == 'a') ? 'b' : 'a';
    TEST_CHECK(!AppUpdater::verify_downloaded_executable(dummyFile, badHash),
               "Reject executable with mismatched SHA-256");
    TEST_CHECK(!AppUpdater::verify_downloaded_executable(dummyFile, ""),
               "Reject executable when expected SHA-256 is empty");
    TEST_CHECK(!AppUpdater::verify_downloaded_executable(dummyFile, "deadbeef"),
               "Reject executable when expected SHA-256 is truncated (<64 chars)");
    std::string nonHexHash = computedHash;
    nonHexHash[63] = 'z';
    TEST_CHECK(!AppUpdater::verify_downloaded_executable(dummyFile, nonHexHash),
               "Reject executable when expected SHA-256 contains non-hex char 'z'");
    TEST_CHECK(!AppUpdater::verify_downloaded_executable(missingFile, computedHash),
               "Reject verification when target file is missing");

    // 6. Reject file without MZ header
    {
        HANDLE hFile = CreateFileW(dummyFile.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile != INVALID_HANDLE_VALUE) {
            char header[2] = {'P', 'K'};
            DWORD written = 0;
            WriteFile(hFile, header, 2, &written, nullptr);
            std::vector<char> padding(105000, 'A');
            WriteFile(hFile, padding.data(), static_cast<DWORD>(padding.size()), &written, nullptr);
            CloseHandle(hFile);
        }
    }
    std::string zipHash;
    bool zipHashOk = AppUpdater::compute_sha256(dummyFile, zipHash);
    TEST_CHECK(zipHashOk && !AppUpdater::verify_downloaded_executable(dummyFile, zipHash),
               "Reject non-MZ file (ZIP header) even with matching SHA-256");

    // 7. Reject file <= 100 KB
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
    bool smallHashOk = AppUpdater::compute_sha256(dummyFile, smallHash);
    TEST_CHECK(smallHashOk && !AppUpdater::verify_downloaded_executable(dummyFile, smallHash),
               "Reject MZ executable <= 100 KB");
}

void test_file_replacement_and_rollback(const std::filesystem::path& tempDir) {
    const std::wstring currPath = (tempDir / L"current.bin").wstring();
    const std::wstring newPath  = (tempDir / L"downloaded.bin").wstring();
    const std::wstring oldPath  = (tempDir / L"current.bin.old").wstring();

    // 1. Success case: target -> old, downloaded -> target
    {
        bool w1 = write_file_content(currPath, "CURRENT_VERSION_203");
        bool w2 = write_file_content(newPath,  "NEW_VERSION_204");
        DeleteFileW(oldPath.c_str());

        std::wstring err;
        bool ok = w1 && w2 && AppUpdater::perform_file_replacement(currPath, newPath, oldPath, &err);
        TEST_CHECK(ok, "perform_file_replacement returns true on valid files");
        TEST_CHECK(read_file_content(currPath) == "NEW_VERSION_204", "Target file updated to new version content");
        TEST_CHECK(read_file_content(oldPath) == "CURRENT_VERSION_203", "Old backup file contains previous version content");
        TEST_CHECK(GetFileAttributesW(newPath.c_str()) == INVALID_FILE_ATTRIBUTES, "Downloaded file moved out of staging path");
    }

    // 2. Pre-existing .old file is cleaned up before next update
    {
        bool w = write_file_content(newPath, "NEW_VERSION_205");
        std::wstring err;
        bool ok = w && AppUpdater::perform_file_replacement(currPath, newPath, oldPath, &err);
        TEST_CHECK(ok, "perform_file_replacement succeeds when .old already exists");
        TEST_CHECK(read_file_content(currPath) == "NEW_VERSION_205", "Target file updated with existing .old");
        TEST_CHECK(read_file_content(oldPath) == "NEW_VERSION_204", "Old backup replaced with previous target");
    }

    // 3. Rollback case: if downloaded file is missing, target is restored from .old
    {
        const std::wstring missingNew = (tempDir / L"nonexistent_download.bin").wstring();
        DeleteFileW(missingNew.c_str());

        std::wstring err;
        bool ok = AppUpdater::perform_file_replacement(currPath, missingNew, oldPath, &err);
        TEST_CHECK(!ok, "perform_file_replacement fails when downloaded file is missing");
        TEST_CHECK(!err.empty(), "Error diagnostics populated: " + QuickDns::Encoding::wide_to_utf8(err));
        TEST_CHECK(read_file_content(currPath) == "NEW_VERSION_205", "Target file safely restored via rollback");
    }
}

void test_github_api_query() {
    auto optInfo = AppUpdater::check_github_update("1.7.0");
    TEST_CHECK(optInfo.has_value(), "GitHub Releases API returned latest release metadata");
    if (optInfo.has_value()) {
        const auto& info = optInfo.value();
        TEST_CHECK(!info.tag_name.empty(), "GitHub API returned release tag: " + info.tag_name);
        TEST_CHECK(!AppUpdater::is_version_newer(info.semver, "2.0.0"),
                   "GitHub API latest semver is >= 2.0.0: " + info.semver);
        TEST_CHECK(!info.download_url.empty(), "QuickDNS.exe download URL found: " + info.download_url);
        TEST_CHECK(info.is_newer, "v" + info.semver + " detected as newer than 1.7.0");
        TEST_CHECK(!AppUpdater::is_version_newer(info.semver, info.semver),
                   "Latest release semver is not newer than itself");
    }
}

} // namespace

int main(int argc, char* argv[]) {
    bool offline_only = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--offline") == 0) {
            offline_only = true;
        } else if (std::strcmp(argv[i], "--network") == 0) {
            offline_only = false;
        }
    }

    std::cout << "========================================" << std::endl;
    std::cout << "  QuickDNS App Updater Tests (" << (offline_only ? "OFFLINE" : "NETWORK") << ")" << std::endl;
    std::cout << "========================================" << std::endl;

    std::error_code ec;
    const std::filesystem::path tempDir =
        std::filesystem::temp_directory_path(ec) /
        (L"quickdns_updater_test_" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::remove_all(tempDir, ec);
    std::filesystem::create_directories(tempDir, ec);

    test_semver_parsing();
    test_version_comparison();
    test_allowed_hosts();
    test_integrity_verification(tempDir);
    test_file_replacement_and_rollback(tempDir);

    if (!offline_only) {
        test_github_api_query();
    } else {
        std::cout << "[SKIP] Live GitHub API query skipped in --offline mode." << std::endl;
    }

    std::filesystem::remove_all(tempDir, ec);

    std::cout << "========================================" << std::endl;
    std::cout << "Results: " << g_pass << " PASSED, " << g_fail << " FAILED" << std::endl;
    std::cout << "========================================" << std::endl;

    return (g_fail == 0) ? 0 : 1;
}
