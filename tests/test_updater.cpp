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

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "     QuickDNS App Updater Tests" << std::endl;
    std::cout << "========================================" << std::endl;

    test_semver_parsing();
    test_version_comparison();
    test_github_api_query();

    std::cout << "========================================" << std::endl;
    std::cout << "Results: " << g_pass << " PASSED, " << g_fail << " FAILED" << std::endl;
    std::cout << "========================================" << std::endl;

    return (g_fail == 0) ? 0 : 1;
}
