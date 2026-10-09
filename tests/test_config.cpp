#include "config_manager.h"
#include <iostream>
#include <cassert>
#include <filesystem>

int main() {
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
        assert(comss->primary_ipv4 == "92.223.109.31");
        std::cout << "[PASS] Comss preset verified: " << comss->primary_ipv4 << std::endl;

        auto geohide = cfg.get_profile_by_id("geohide");
        assert(geohide.has_value());
        assert(geohide->primary_ipv4 == "193.187.91.189");
        std::cout << "[PASS] GeoHide preset verified: " << geohide->primary_ipv4 << std::endl;

        auto bezmezhau = cfg.get_profile_by_id("bezmezhau");
        assert(bezmezhau.has_value());
        assert(bezmezhau->primary_ipv4 == "185.162.9.228");
        std::cout << "[PASS] Bezmezhau preset verified: " << bezmezhau->primary_ipv4 << std::endl;

        auto dhcp = cfg.get_profile_by_id("dhcp");
        assert(dhcp.has_value());
        assert(dhcp->is_dhcp == true);
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

        assert(cfg.add_profile(custom) == true);
        assert(cfg.get_profile_by_id("my_custom").has_value());
        std::cout << "[PASS] Custom profile added successfully." << std::endl;

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
        std::cout << "[PASS] Configuration reloaded from disk with persistence intact." << std::endl;

        // 4. Delete profile
        assert(cfgReloaded.delete_profile("my_custom") == true);
        assert(!cfgReloaded.get_profile_by_id("my_custom").has_value());
        std::cout << "[PASS] Custom profile deleted." << std::endl;

        // Built-in profiles cannot be deleted
        assert(cfgReloaded.delete_profile("comss") == false);
        std::cout << "[PASS] Built-in profile protection verified." << std::endl;
    }

    // Cleanup
    if (std::filesystem::exists(testPath)) {
        std::filesystem::remove(testPath);
    }

    std::cout << "=== All Config Manager tests passed! ===" << std::endl;
    return 0;
}
