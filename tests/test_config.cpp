#include "config_manager.h"
#include "encoding_utils.h"
#include "nlohmann/json.hpp"

#include <windows.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <string>
#include <vector>

namespace {

int g_pass = 0;
int g_fail = 0;

#define TEST_CHECK(cond, name) \
    do { \
        if (cond) { \
            std::cout << "[PASS] " << (name) << std::endl; \
            ++g_pass; \
        } else { \
            std::cout << "[FAIL] " << (name) << " (line " << __LINE__ << ")" << std::endl; \
            ++g_fail; \
        } \
    } while (0)

std::filesystem::path resolve_repo_root() {
#ifdef QUICKDNS_SOURCE_DIR
    {
        std::filesystem::path p(QuickDns::Encoding::utf8_to_wide(QUICKDNS_SOURCE_DIR));
        if (std::filesystem::exists(p / L"CMakeLists.txt")) {
            return p;
        }
    }
#endif
    std::error_code ec;
    std::filesystem::path cwd = std::filesystem::current_path(ec);
    if (!ec) {
        if (std::filesystem::exists(cwd / L"CMakeLists.txt")) {
            return cwd;
        }
        if (cwd.has_parent_path() && std::filesystem::exists(cwd.parent_path() / L"CMakeLists.txt")) {
            return cwd.parent_path();
        }
    }
    return cwd;
}

bool pool_contains(const std::vector<std::string>& pool, const std::string& ip) {
    return std::find(pool.begin(), pool.end(), ip) != pool.end();
}

} // namespace

int main() {
    _putenv_s("TESTING", "1");
    std::cout << "=== QuickDNS: Test Config Manager ===" << std::endl;

    std::error_code ec;
    const std::filesystem::path tempDir =
        std::filesystem::temp_directory_path(ec) /
        (L"quickdns_test_config_" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::remove_all(tempDir, ec);
    std::filesystem::create_directories(tempDir, ec);
    TEST_CHECK(std::filesystem::exists(tempDir), "Created isolated per-PID temp directory");

    const std::filesystem::path repoRoot = resolve_repo_root();
    const std::filesystem::path testPath = tempDir / L"profiles_main.json";

    // 1. Creation, default presets, custom profile add, duplicate/empty ID rejection
    {
        ConfigManager cfg(testPath);
        auto profiles = cfg.get_all_profiles();
        TEST_CHECK(!profiles.empty(), "Default profiles populated on missing config file");
        TEST_CHECK(cfg.get_theme_mode() == "dark", "Default theme mode is 'dark'");
        TEST_CHECK(cfg.get_active_profile_id() == "dhcp", "Default active profile ID is 'dhcp'");

        auto comss = cfg.get_profile_by_id("comss");
        TEST_CHECK(comss.has_value(), "Preset 'comss' exists");
        if (comss) {
            TEST_CHECK(comss->primary_ipv4 == "83.220.169.155", "Comss primary IPv4 matches");
            TEST_CHECK(comss->doh_template == "https://dns.comss.one/dns-query", "Comss DoH matches");
            TEST_CHECK(comss->is_smart_dns && comss->is_builtin && !comss->is_dhcp, "Comss flags match");
            TEST_CHECK(comss->website_url == "https://dns.comss.one/", "Comss website URL matches");
        }

        auto geohide = cfg.get_profile_by_id("geohide");
        TEST_CHECK(geohide.has_value() && geohide->primary_ipv4 == "193.233.112.67" &&
                   geohide->doh_template == "https://dns.geohide.ru/dns-query" && geohide->is_smart_dns,
                   "Preset 'geohide' verified");

        auto geohide_eu = cfg.get_profile_by_id("geohide_eu");
        TEST_CHECK(geohide_eu.has_value() && geohide_eu->primary_ipv4 == "45.13.237.213" &&
                   geohide_eu->doh_template == "https://eu.geohide.ru/dns-query" && geohide_eu->is_smart_dns,
                   "Preset 'geohide_eu' verified");

        auto geohide_us = cfg.get_profile_by_id("geohide_us");
        TEST_CHECK(geohide_us.has_value() && geohide_us->primary_ipv4 == "192.255.159.241" &&
                   geohide_us->doh_template == "https://us.geohide.ru/dns-query" && geohide_us->is_smart_dns,
                   "Preset 'geohide_us' verified");

        auto bezmezhau = cfg.get_profile_by_id("bezmezhau");
        TEST_CHECK(bezmezhau.has_value() && bezmezhau->primary_ipv4 == "181.215.40.49" &&
                   bezmezhau->doh_template == "https://dns.bezmezhau.com/dns-query" &&
                   bezmezhau->ip_pool.size() >= 4,
                   "Preset 'bezmezhau' verified");

        auto bezmezhau_ru = cfg.get_profile_by_id("bezmezhau_ru");
        TEST_CHECK(bezmezhau_ru.has_value() && bezmezhau_ru->primary_ipv4 == "185.236.10.189" &&
                   bezmezhau_ru->secondary_ipv4 == "31.58.216.201" &&
                   bezmezhau_ru->doh_template == "https://sync.bezmezhau.com/dns-query" &&
                   bezmezhau_ru->is_smart_dns && bezmezhau_ru->is_builtin &&
                   bezmezhau_ru->ip_pool.size() == 4,
                   "Preset 'bezmezhau_ru' verified");

        auto zapret = cfg.get_profile_by_id("zapret");
        TEST_CHECK(zapret.has_value() && zapret->primary_ipv4 == "83.217.211.149" &&
                   zapret->doh_template == "https://dns.zapret.moe/dns-query" && zapret->is_smart_dns,
                   "Preset 'zapret' verified");

        auto malw = cfg.get_profile_by_id("malw");
        TEST_CHECK(malw.has_value() && malw->primary_ipv4 == "179.254.166.171" &&
                   malw->doh_template == "https://dns.malw.link/dns-query" && malw->is_smart_dns,
                   "Preset 'malw' verified");

        auto astracat = cfg.get_profile_by_id("astracat");
        TEST_CHECK(astracat.has_value() && astracat->primary_ipv4 == "135.106.217.200" && astracat->is_smart_dns,
                   "Preset 'astracat' verified");

        auto mafioznik = cfg.get_profile_by_id("mafioznik");
        TEST_CHECK(mafioznik.has_value() && mafioznik->primary_ipv4 == "103.27.157.38" && mafioznik->is_smart_dns,
                   "Preset 'mafioznik' verified");

        auto shecan = cfg.get_profile_by_id("shecan");
        TEST_CHECK(shecan.has_value() && shecan->primary_ipv4 == "178.22.122.100" && shecan->is_smart_dns,
                   "Preset 'shecan' verified");

        auto controld = cfg.get_profile_by_id("controld");
        TEST_CHECK(controld.has_value() && controld->primary_ipv4 == "76.76.2.0" && controld->is_smart_dns,
                   "Preset 'controld' verified");

        auto akile = cfg.get_profile_by_id("akile");
        TEST_CHECK(akile.has_value() && akile->primary_ipv4 == "66.66.66.66" && akile->is_smart_dns,
                   "Preset 'akile' verified");

        auto dns_ai = cfg.get_profile_by_id("dns_ai");
        TEST_CHECK(dns_ai.has_value() &&
                   dns_ai->name == "DNS-AI (DoH)" &&
                   dns_ai->primary_ipv4 == "194.87.196.13" &&
                   dns_ai->secondary_ipv4 == "45.151.102.219" &&
                   dns_ai->primary_ipv6 == "2a00:b700:5::3:62" &&
                   dns_ai->secondary_ipv6 == "2a12:5940:d4ef::219" &&
                   dns_ai->doh_template == "https://dns.dns-ai.ru/dns-query" &&
                   dns_ai->website_url == "https://dns-ai.ru/" &&
                   dns_ai->is_builtin && !dns_ai->is_dhcp && dns_ai->is_smart_dns &&
                   dns_ai->ip_pool.size() == 2 &&
                   pool_contains(dns_ai->ip_pool, "194.87.196.13") &&
                   pool_contains(dns_ai->ip_pool, "45.151.102.219"),
                   "Preset 'dns_ai' (DNS-AI DoH) verified");

        auto cloudflare = cfg.get_profile_by_id("cloudflare");
        TEST_CHECK(cloudflare.has_value() && !cloudflare->is_smart_dns && cloudflare->is_builtin,
                   "Preset 'cloudflare' verified");

        auto dhcp = cfg.get_profile_by_id("dhcp");
        TEST_CHECK(dhcp.has_value() && dhcp->is_dhcp && !dhcp->is_smart_dns && dhcp->is_builtin,
                   "Preset 'dhcp' verified");

        // Reject empty ID
        DnsProfile emptyId;
        emptyId.id = "";
        emptyId.name = "Invalid Empty ID";
        emptyId.primary_ipv4 = "8.8.8.8";
        bool addedEmpty = cfg.add_profile(emptyId);
        TEST_CHECK(!addedEmpty, "add_profile rejects empty profile ID");

        // Reject duplicate built-in ID
        DnsProfile dupBuiltin;
        dupBuiltin.id = "comss";
        dupBuiltin.name = "Duplicate Comss";
        dupBuiltin.primary_ipv4 = "1.2.3.4";
        bool addedDupBuiltin = cfg.add_profile(dupBuiltin);
        TEST_CHECK(!addedDupBuiltin, "add_profile rejects duplicate built-in ID");

        // Add valid custom profile
        DnsProfile custom;
        custom.id = "my_custom";
        custom.name = "My Custom DNS";
        custom.description = "Test custom DNS server";
        custom.primary_ipv4 = "77.88.8.8";
        custom.secondary_ipv4 = "77.88.8.1";
        custom.doh_template = "https://dns.yandex.ru/dns-query";
        custom.is_builtin = false;
        custom.is_dhcp = false;
        custom.is_smart_dns = true;
        custom.ip_pool = {"77.88.8.8", "77.88.8.1", "77.88.8.88"};

        bool addedCustom = cfg.add_profile(custom);
        TEST_CHECK(addedCustom, "add_profile accepts valid custom profile");

        // Reject duplicate custom ID
        bool addedDupCustom = cfg.add_profile(custom);
        TEST_CHECK(!addedDupCustom, "add_profile rejects duplicate custom ID");

        auto fetchedCustom = cfg.get_profile_by_id("my_custom");
        TEST_CHECK(fetchedCustom.has_value() && fetchedCustom->is_smart_dns,
                   "Custom profile retrievable with is_smart_dns == true");

        // Theme mode persistence & normalization
        cfg.set_theme_mode("light");
        TEST_CHECK(cfg.get_theme_mode() == "light", "set_theme_mode('light') sets light theme");

        cfg.set_active_profile_id("my_custom");
        bool saved = cfg.save_to_file();
        TEST_CHECK(saved, "save_to_file writes configuration to disk");
    }

    // 2. Reload from disk, update_profile (ip_pool sync), doh_template sanitization, delete_profile
    {
        ConfigManager cfgReloaded(testPath);
        TEST_CHECK(cfgReloaded.get_active_profile_id() == "my_custom", "Reloaded active_profile_id is 'my_custom'");
        TEST_CHECK(cfgReloaded.get_theme_mode() == "light", "Reloaded theme mode persisted as 'light'");

        cfgReloaded.set_theme_mode("unexpected_theme_value");
        TEST_CHECK(cfgReloaded.get_theme_mode() == "dark", "Invalid theme string normalizes to 'dark'");

        auto custom = cfgReloaded.get_profile_by_id("my_custom");
        TEST_CHECK(custom.has_value(), "Custom profile reloaded from disk");
        if (custom) {
            TEST_CHECK(custom->primary_ipv4 == "77.88.8.8", "Reloaded custom primary_ipv4 matches");
            TEST_CHECK(custom->is_smart_dns, "Reloaded custom is_smart_dns matches");
            TEST_CHECK(custom->ip_pool.size() == 3 && pool_contains(custom->ip_pool, "77.88.8.88"),
                       "Reloaded custom ip_pool persisted");

            // Test update_profile ip_pool auto-sync & invalid doh_template sanitization
            DnsProfile modified = *custom;
            modified.primary_ipv4 = "9.9.9.10";
            modified.secondary_ipv4 = "149.112.112.10";
            modified.doh_template = "http://insecure.example.com/dns-query"; // non-HTTPS must be sanitized
            bool updated = cfgReloaded.update_profile(modified);
            TEST_CHECK(updated, "update_profile succeeds for existing custom profile");

            auto afterUpdate = cfgReloaded.get_profile_by_id("my_custom");
            TEST_CHECK(afterUpdate.has_value(), "Updated profile exists in memory");
            if (afterUpdate) {
                TEST_CHECK(afterUpdate->primary_ipv4 == "9.9.9.10" &&
                           afterUpdate->secondary_ipv4 == "149.112.112.10",
                           "update_profile updated primary and secondary IPv4");
                TEST_CHECK(pool_contains(afterUpdate->ip_pool, "9.9.9.10") &&
                           pool_contains(afterUpdate->ip_pool, "149.112.112.10"),
                           "update_profile auto-synced new primary and secondary IPv4 into ip_pool");
                TEST_CHECK(afterUpdate->doh_template.empty(),
                           "update_profile sanitized invalid http:// doh_template to empty string");
            }
        }

        // Non-existent update_profile must return false
        DnsProfile ghost;
        ghost.id = "does_not_exist";
        ghost.name = "Ghost";
        ghost.primary_ipv4 = "1.1.1.1";
        bool updatedGhost = cfgReloaded.update_profile(ghost);
        TEST_CHECK(!updatedGhost, "update_profile returns false for unknown profile ID");

        // Invalid doh_template in add_profile must be sanitized
        DnsProfile badDoh;
        badDoh.id = "bad_doh_custom";
        badDoh.name = "Bad DoH Test";
        badDoh.primary_ipv4 = "8.8.4.4";
        badDoh.doh_template = "https://evil.example/dns-query\" ; calc";
        bool addedBadDoh = cfgReloaded.add_profile(badDoh);
        TEST_CHECK(addedBadDoh, "add_profile accepts profile while sanitizing bad doh_template");
        auto fetchedBadDoh = cfgReloaded.get_profile_by_id("bad_doh_custom");
        TEST_CHECK(fetchedBadDoh.has_value() && fetchedBadDoh->doh_template.empty() &&
                   pool_contains(fetchedBadDoh->ip_pool, "8.8.4.4"),
                   "add_profile cleared injection doh_template and synced primary_ipv4 into ip_pool");

        // Delete active custom profile -> active_profile_id falls back to dhcp
        bool deletedCustom = cfgReloaded.delete_profile("my_custom");
        TEST_CHECK(deletedCustom, "delete_profile removes custom profile");
        TEST_CHECK(!cfgReloaded.get_profile_by_id("my_custom").has_value(), "Deleted profile no longer present");
        TEST_CHECK(cfgReloaded.get_active_profile_id() == "dhcp",
                   "Deleting active custom profile resets active_profile_id to 'dhcp'");

        // Built-in profiles cannot be deleted
        bool deletedComss = cfgReloaded.delete_profile("comss");
        bool deletedDhcp = cfgReloaded.delete_profile("dhcp");
        TEST_CHECK(!deletedComss && !deletedDhcp, "Built-in profiles cannot be deleted");
    }

    // 3. Round-trip verification: quotes, backslashes, Cyrillic UTF-8, control characters
    {
        const std::filesystem::path specialPath = tempDir / L"special_chars.json";
        const std::string specialName = "DNS \"Super\" \\ \xD0\xA2\xD0\xB5\xD1\x81\xD1\x82 \t \xD0\x9F\xD1\x80\xD0\xBE\xD1\x84\xD0\xB8\xD0\xBB\xD1\x8C \n Line 2";
        const std::string specialDesc = "Quotes: \"\", Path: C:\\Windows\\System32, Symbols: !@#$%^&*()_+-=";

        {
            ConfigManager cfgSpecial(specialPath);
            DnsProfile p;
            p.id = "special_id";
            p.name = specialName;
            p.description = specialDesc;
            p.primary_ipv4 = "1.2.3.4";
            bool added = cfgSpecial.add_profile(p);
            bool saved = cfgSpecial.save_to_file();
            TEST_CHECK(added && saved, "Saved special-character custom profile");
        }

        {
            ConfigManager cfgLoaded(specialPath);
            auto p = cfgLoaded.get_profile_by_id("special_id");
            TEST_CHECK(p.has_value() && p->name == specialName && p->description == specialDesc,
                       "Round-trip preserved quotes, backslashes, Cyrillic UTF-8, and control chars");
        }
    }

    // 4. Migration of legacy v2.0.1 fixture (hard-fail if fixture missing)
    {
        const std::filesystem::path fixturePath = repoRoot / L"tests" / L"fixtures" / L"profiles_v201.json";
        bool fixtureExists = std::filesystem::exists(fixturePath);
        TEST_CHECK(fixtureExists, "Located v2.0.1 fixture at tests/fixtures/profiles_v201.json");
        if (fixtureExists) {
            const std::filesystem::path tempV201Path = tempDir / L"profiles_v201_copy.json";
            bool copied = std::filesystem::copy_file(
                fixturePath, tempV201Path, std::filesystem::copy_options::overwrite_existing, ec);
            TEST_CHECK(copied && !ec, "Copied v2.0.1 fixture into isolated temp directory");

            ConfigManager cfgOld(tempV201Path);
            TEST_CHECK(cfgOld.get_active_profile_id() == "comss", "Migrated v2.0.1 active_profile_id is 'comss'");
            auto comss = cfgOld.get_profile_by_id("comss");
            TEST_CHECK(comss.has_value() && comss->primary_ipv4 == "83.220.169.155" && comss->ip_pool.size() >= 3,
                       "Migrated v2.0.1 'comss' populated ip_pool");
            auto bezRu = cfgOld.get_profile_by_id("bezmezhau_ru");
            TEST_CHECK(bezRu.has_value() && bezRu->primary_ipv4 == "185.236.10.189",
                       "Migrated v2.0.1 inserted missing 'bezmezhau_ru' preset");
            auto dnsAiMigrated = cfgOld.get_profile_by_id("dns_ai");
            TEST_CHECK(dnsAiMigrated.has_value() &&
                       dnsAiMigrated->primary_ipv4 == "194.87.196.13" &&
                       dnsAiMigrated->doh_template == "https://dns.dns-ai.ru/dns-query" &&
                       dnsAiMigrated->is_smart_dns && dnsAiMigrated->is_builtin,
                       "Migrated v2.0.1 auto-inserted missing 'dns_ai' (DNS-AI DoH) preset");
        }
    }

    // 5. Broken JSON handling and .broken-<timestamp> backup creation (wide path inspection)
    {
        const std::filesystem::path brokenPath = tempDir / L"quickdns_broken.json";
        {
            std::ofstream f(brokenPath, std::ios::out | std::ios::binary);
            f << "{ \"active_profile_id\": \"broken\", \"profiles\": [";
        }

        ConfigManager cfgBroken(brokenPath);
        auto all = cfgBroken.get_all_profiles();
        TEST_CHECK(!all.empty(), "Corrupted JSON recovered by restoring default presets");

        bool foundBackup = false;
        for (const auto& entry : std::filesystem::directory_iterator(tempDir, ec)) {
            std::wstring filename = entry.path().filename().wstring();
            if (filename.rfind(L"quickdns_broken.json.broken-", 0) == 0) {
                foundBackup = true;
                break;
            }
        }
        TEST_CHECK(foundBackup, "Corrupted JSON backed up to quickdns_broken.json.broken-<timestamp>");
    }

    // 6. Parity between res/default_profiles.json and ConfigManager::get_default_presets()
    {
        const std::filesystem::path defaultJsonPath = repoRoot / L"res" / L"default_profiles.json";
        bool exists = std::filesystem::exists(defaultJsonPath);
        TEST_CHECK(exists, "Located res/default_profiles.json for preset parity verification");
        if (exists) {
            std::ifstream f(defaultJsonPath, std::ios::in | std::ios::binary);
            std::string raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            nlohmann::json root = nlohmann::json::parse(raw, nullptr, false);
            TEST_CHECK(!root.is_discarded() && root.is_object(), "res/default_profiles.json is valid JSON object");
            if (!root.is_discarded() && root.is_object()) {
                const auto presets = ConfigManager::get_default_presets();
                TEST_CHECK(root.value("active_profile_id", "") == "dhcp",
                           "res/default_profiles.json active_profile_id is 'dhcp'");
                if (root.contains("theme")) {
                    TEST_CHECK(root["theme"].is_string() && root["theme"].get<std::string>() == "dark",
                               "res/default_profiles.json theme is 'dark'");
                }
                const bool hasArr = root.contains("profiles") && root["profiles"].is_array();
                TEST_CHECK(hasArr && root["profiles"].size() == presets.size(),
                           "res/default_profiles.json preset count matches ConfigManager::get_default_presets()");
                if (hasArr && root["profiles"].size() == presets.size()) {
                    bool allMatch = true;
                    for (size_t i = 0; i < presets.size(); ++i) {
                        const auto& j = root["profiles"][i];
                        const auto& d = presets[i];
                        std::vector<std::string> jPool;
                        if (j.contains("ip_pool") && j["ip_pool"].is_array()) {
                            for (const auto& item : j["ip_pool"]) {
                                if (item.is_string()) jPool.push_back(item.get<std::string>());
                            }
                        }
                        const bool same =
                            j.value("id", "") == d.id &&
                            j.value("name", "") == d.name &&
                            j.value("description", "") == d.description &&
                            j.value("primary_ipv4", "") == d.primary_ipv4 &&
                            j.value("secondary_ipv4", "") == d.secondary_ipv4 &&
                            j.value("primary_ipv6", "") == d.primary_ipv6 &&
                            j.value("secondary_ipv6", "") == d.secondary_ipv6 &&
                            j.value("doh_template", "") == d.doh_template &&
                            j.value("is_builtin", false) == d.is_builtin &&
                            j.value("is_dhcp", false) == d.is_dhcp &&
                            j.value("is_smart_dns", false) == d.is_smart_dns &&
                            j.value("website_url", "") == d.website_url &&
                            j.value("unblocked_summary", "") == d.unblocked_summary &&
                            jPool == d.ip_pool;
                        if (!same) {
                            std::cout << "[DIFF] Mismatch at preset index " << i << " (id=" << d.id << ")" << std::endl;
                            allMatch = false;
                        }
                    }
                    TEST_CHECK(allMatch, "All fields of res/default_profiles.json match ConfigManager::get_default_presets()");
                }
            }
        }
    }

    std::filesystem::remove_all(tempDir, ec);

    std::cout << "========================================" << std::endl;
    std::cout << "Results: " << g_pass << " PASSED, " << g_fail << " FAILED" << std::endl;
    std::cout << "========================================" << std::endl;
    return (g_fail == 0) ? 0 : 1;
}
