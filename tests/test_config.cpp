#include "config_manager.h"
#include <iostream>
#include <cassert>
#include <filesystem>
#include <fstream>

int main() {
    _putenv("TESTING=1");
    std::cout << "=== QuickDNS: Test Config Manager ===" << std::endl;

    std::filesystem::path testPath = std::filesystem::temp_directory_path() / "quickdns_test_profiles.json";
    if (std::filesystem::exists(testPath)) {
        std::filesystem::remove(testPath);
    }

    // 1. Creation and default presets
    {
        ConfigManager cfg(testPath);
        auto profiles = cfg.get_all_profiles();
        assert(!profiles.empty());
        std::cout << "[PASS] Default profiles created: count = " << profiles.size() << std::endl;

        // Verify presets exist
        auto comss = cfg.get_profile_by_id("comss");
        assert(comss.has_value());
        assert(comss->primary_ipv4 == "83.220.169.155");
        assert(comss->doh_template == "https://dns.comss.one/dns-query");
        assert(comss->is_smart_dns == true);
        assert(comss->website_url == "https://dns.comss.one/");
        std::cout << "[PASS] Comss preset verified: " << comss->primary_ipv4 << " (DoH: " << comss->doh_template << ")" << std::endl;

        auto geohide = cfg.get_profile_by_id("geohide");
        assert(geohide.has_value());
        assert(geohide->primary_ipv4 == "193.233.112.67");
        assert(geohide->doh_template == "https://dns.geohide.ru/dns-query");
        assert(geohide->is_smart_dns == true);
        assert(geohide->website_url == "https://geohide.ru/");
        std::cout << "[PASS] GeoHide preset verified: " << geohide->primary_ipv4 << " (DoH: " << geohide->doh_template << ")" << std::endl;

        auto bezmezhau = cfg.get_profile_by_id("bezmezhau");
        assert(bezmezhau.has_value());
        assert(bezmezhau->primary_ipv4 == "181.215.40.49");
        assert(bezmezhau->doh_template == "https://dns.bezmezhau.com/dns-query");
        assert(bezmezhau->is_smart_dns == true);
        assert(bezmezhau->ip_pool.size() >= 4);
        std::cout << "[PASS] Bezmezhau preset verified: " << bezmezhau->primary_ipv4 << " (DoH: " << bezmezhau->doh_template << ")" << std::endl;

        auto bezmezhau_ru = cfg.get_profile_by_id("bezmezhau_ru");
        assert(bezmezhau_ru.has_value());
        assert(bezmezhau_ru->primary_ipv4 == "185.236.10.189");
        assert(bezmezhau_ru->secondary_ipv4 == "31.58.216.201");
        assert(bezmezhau_ru->doh_template == "https://sync.bezmezhau.com/dns-query");
        assert(bezmezhau_ru->is_smart_dns == true);
        assert(bezmezhau_ru->is_builtin == true);
        assert(bezmezhau_ru->ip_pool.size() == 4);
        assert(bezmezhau_ru->ip_pool[0] == "185.236.10.189");
        assert(bezmezhau_ru->ip_pool[1] == "31.58.216.201");
        std::cout << "[PASS] Bezmezhau RU preset verified: " << bezmezhau_ru->primary_ipv4 << " (DoH: " << bezmezhau_ru->doh_template << ")" << std::endl;

        auto zapret = cfg.get_profile_by_id("zapret");
        assert(zapret.has_value());
        assert(zapret->primary_ipv4 == "83.217.211.149");
        assert(zapret->doh_template == "https://dns.zapret.moe/dns-query");
        assert(zapret->is_smart_dns == true);
        std::cout << "[PASS] Zapret preset verified: " << zapret->primary_ipv4 << " (DoH: " << zapret->doh_template << ")" << std::endl;

        auto malw = cfg.get_profile_by_id("malw");
        assert(malw.has_value());
        assert(malw->primary_ipv4 == "179.254.166.171");
        assert(malw->doh_template == "https://dns.malw.link/dns-query");
        assert(malw->is_smart_dns == true);
        std::cout << "[PASS] Malw.link preset verified: " << malw->primary_ipv4 << " (DoH: " << malw->doh_template << ")" << std::endl;

        auto geohide_eu = cfg.get_profile_by_id("geohide_eu");
        assert(geohide_eu.has_value());
        assert(geohide_eu->primary_ipv4 == "45.13.237.213");
        assert(geohide_eu->doh_template == "https://eu.geohide.ru/dns-query");
        assert(geohide_eu->is_smart_dns == true);
        std::cout << "[PASS] GeoHide EU preset verified: " << geohide_eu->primary_ipv4 << std::endl;

        auto geohide_us = cfg.get_profile_by_id("geohide_us");
        assert(geohide_us.has_value());
        assert(geohide_us->primary_ipv4 == "192.255.159.241");
        assert(geohide_us->doh_template == "https://us.geohide.ru/dns-query");
        assert(geohide_us->is_smart_dns == true);
        std::cout << "[PASS] GeoHide US preset verified: " << geohide_us->primary_ipv4 << std::endl;

        auto astracat = cfg.get_profile_by_id("astracat");
        assert(astracat.has_value());
        assert(astracat->primary_ipv4 == "135.106.217.200");
        assert(astracat->is_smart_dns == true);
        std::cout << "[PASS] Astracat preset verified: " << astracat->primary_ipv4 << std::endl;

        auto mafioznik = cfg.get_profile_by_id("mafioznik");
        assert(mafioznik.has_value());
        assert(mafioznik->primary_ipv4 == "103.27.157.38");
        assert(mafioznik->is_smart_dns == true);
        std::cout << "[PASS] Mafioznik preset verified: " << mafioznik->primary_ipv4 << std::endl;

        auto shecan = cfg.get_profile_by_id("shecan");
        assert(shecan.has_value());
        assert(shecan->primary_ipv4 == "178.22.122.100");
        assert(shecan->is_smart_dns == true);
        std::cout << "[PASS] Shecan preset verified: " << shecan->primary_ipv4 << std::endl;

        auto controld = cfg.get_profile_by_id("controld");
        assert(controld.has_value());
        assert(controld->primary_ipv4 == "76.76.2.0");
        assert(controld->is_smart_dns == true);
        std::cout << "[PASS] Control D preset verified: " << controld->primary_ipv4 << std::endl;

        auto akile = cfg.get_profile_by_id("akile");
        assert(akile.has_value());
        assert(akile->primary_ipv4 == "66.66.66.66");
        assert(akile->is_smart_dns == true);
        std::cout << "[PASS] Akile preset verified: " << akile->primary_ipv4 << std::endl;

        auto cloudflare = cfg.get_profile_by_id("cloudflare");
        assert(cloudflare.has_value());
        assert(cloudflare->is_smart_dns == false);

        auto dhcp = cfg.get_profile_by_id("dhcp");
        assert(dhcp.has_value());
        assert(dhcp->is_dhcp == true);
        assert(dhcp->is_smart_dns == false);
        std::cout << "[PASS] DHCP preset verified." << std::endl;

        // 2. Add custom profile
        DnsProfile custom;
        custom.id = "my_custom";
        custom.name = "My Custom DNS";
        custom.description = "Test custom DNS server";
        custom.primary_ipv4 = "77.88.8.8";
        custom.secondary_ipv4 = "77.88.8.1";
        custom.is_builtin = false;
        custom.is_dhcp = false;
        custom.is_smart_dns = true;
        custom.ip_pool = {"77.88.8.8", "77.88.8.1", "77.88.8.88"};

        assert(cfg.add_profile(custom) == true);
        assert(cfg.get_profile_by_id("my_custom").has_value());
        assert(cfg.get_profile_by_id("my_custom")->is_smart_dns == true);
        std::cout << "[PASS] Custom profile with is_smart_dns added successfully." << std::endl;

        cfg.set_active_profile_id("my_custom");
        assert(cfg.save_to_file() == true);
        std::cout << "[PASS] Configuration saved to disk." << std::endl;
    }

    // 3. Reload from disk
    {
        ConfigManager cfgReloaded(testPath);
        assert(cfgReloaded.get_active_profile_id() == "my_custom");
        auto custom = cfgReloaded.get_profile_by_id("my_custom");
        assert(custom.has_value());
        assert(custom->primary_ipv4 == "77.88.8.8");
        assert(custom->is_smart_dns == true);
        assert(custom->ip_pool.size() == 3);
        assert(custom->ip_pool[2] == "77.88.8.88");
        std::cout << "[PASS] Configuration reloaded from disk with is_smart_dns and ip_pool persistence intact." << std::endl;

        // 4. Delete profile
        assert(cfgReloaded.delete_profile("my_custom") == true);
        assert(!cfgReloaded.get_profile_by_id("my_custom").has_value());
        std::cout << "[PASS] Custom profile deleted." << std::endl;

        // Built-in profiles cannot be deleted
        assert(cfgReloaded.delete_profile("comss") == false);
        std::cout << "[PASS] Built-in profile protection verified." << std::endl;
    }

    // 5. Round-trip verification: quotes, backslashes, Cyrillic, control characters
    {
        std::filesystem::path specialPath = std::filesystem::temp_directory_path() / "quickdns_special_chars.json";
        if (std::filesystem::exists(specialPath)) {
            std::filesystem::remove(specialPath);
        }

        std::string specialName = "DNS \"Super\" \\ Тест \t Профиль \n Строка 2";
        std::string specialDesc = "Кавычки: \"\", Слэши: C:\\Windows\\System32, Символы: !@#$%^&*()_+-=";

        {
            ConfigManager cfgSpecial(specialPath);
            DnsProfile p;
            p.id = "special_id";
            p.name = specialName;
            p.description = specialDesc;
            p.primary_ipv4 = "1.2.3.4";
            assert(cfgSpecial.add_profile(p) == true);
            assert(cfgSpecial.save_to_file() == true);
        }

        {
            ConfigManager cfgLoaded(specialPath);
            auto p = cfgLoaded.get_profile_by_id("special_id");
            assert(p.has_value());
            assert(p->name == specialName);
            assert(p->description == specialDesc);
            std::cout << "[PASS] Round-trip test with quotes, backslashes, Cyrillic and control characters passed!" << std::endl;
        }

        if (std::filesystem::exists(specialPath)) {
            std::filesystem::remove(specialPath);
        }
    }

    // 6. Reading old format v2.0.1 fixture
    {
        std::filesystem::path fixturePath = "tests/fixtures/profiles_v201.json";
        if (!std::filesystem::exists(fixturePath)) {
            fixturePath = "../tests/fixtures/profiles_v201.json";
        }
        if (!std::filesystem::exists(fixturePath)) {
            fixturePath = "C:/Users/SWEETY/QuickDNS/tests/fixtures/profiles_v201.json";
        }
        if (std::filesystem::exists(fixturePath)) {
            std::filesystem::path tempV201Path = std::filesystem::temp_directory_path() / "quickdns_test_v201.json";
            std::filesystem::copy_file(fixturePath, tempV201Path, std::filesystem::copy_options::overwrite_existing);
            ConfigManager cfgOld(tempV201Path);
            assert(cfgOld.get_active_profile_id() == "comss");
            auto comss = cfgOld.get_profile_by_id("comss");
            assert(comss.has_value());
            assert(comss->primary_ipv4 == "83.220.169.155");
            assert(comss->ip_pool.size() >= 3);
            auto bezmezhau_ru = cfgOld.get_profile_by_id("bezmezhau_ru");
            assert(bezmezhau_ru.has_value());
            assert(bezmezhau_ru->primary_ipv4 == "185.236.10.189");
            std::filesystem::remove(tempV201Path);
            std::cout << "[PASS] Successfully parsed and migrated v2.0.1 fixture config!" << std::endl;
        } else {
            std::cout << "[WARN] Fixture tests/fixtures/profiles_v201.json not found, skipping." << std::endl;
        }
    }

    // 7. Broken JSON handling and backup creation
    {
        std::filesystem::path brokenPath = std::filesystem::temp_directory_path() / "quickdns_broken.json";
        if (std::filesystem::exists(brokenPath)) {
            std::filesystem::remove(brokenPath);
        }

        // Write incomplete/invalid JSON
        {
            std::ofstream f(brokenPath);
            f << "{ \"active_profile_id\": \"broken\", \"profiles\": [";
        }

        ConfigManager cfgBroken(brokenPath);
        // ConfigManager should back up corrupted file to .broken-<timestamp> and populate default presets
        auto all = cfgBroken.get_all_profiles();
        assert(!all.empty());

        bool foundBackup = false;
        for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::temp_directory_path())) {
            std::string filename = entry.path().filename().string();
            if (filename.rfind("quickdns_broken.json.broken-", 0) == 0) {
                foundBackup = true;
                std::filesystem::remove(entry.path());
                break;
            }
        }
        assert(foundBackup);
        std::cout << "[PASS] Corrupted JSON backed up to .broken-<timestamp> and presets restored!" << std::endl;

        if (std::filesystem::exists(brokenPath)) {
            std::filesystem::remove(brokenPath);
        }
    }

    // Cleanup
    if (std::filesystem::exists(testPath)) {
        std::filesystem::remove(testPath);
    }

    std::cout << "=== All Config Manager tests passed! ===" << std::endl;
    return 0;
}
