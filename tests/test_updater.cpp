#include "app_updater.h"
#include <iostream>
#include <cassert>

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

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "     QuickDNS App Updater Tests" << std::endl;
    std::cout << "========================================" << std::endl;

    test_semver_parsing();
    test_version_comparison();
    test_allowed_hosts();
    test_integrity_verification();
    test_file_replacement_and_rollback();
    test_github_api_query();

    std::cout << "========================================" << std::endl;
    std::cout << "Results: " << g_pass << " PASSED, " << g_fail << " FAILED" << std::endl;
    std::cout << "========================================" << std::endl;

    return (g_fail == 0) ? 0 : 1;
}
