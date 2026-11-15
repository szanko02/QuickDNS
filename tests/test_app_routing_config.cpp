#include "app_routing_config.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

static void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

int main() {
    const auto path = std::filesystem::temp_directory_path() / "quickdns_app_routing_test.json";
    std::filesystem::remove(path);
    try {
        AppRoutingConfigStore store(path);
        require(store.load(), "missing file must load empty");
        require(store.rules().empty(), "new configuration must be empty");
        require(!std::filesystem::exists(path), "loading must not create a file");
        const AppRoutingRule rule{"rule-1", "C:\\Apps\\Тест\\client.exe", "{01234567-89AB-CDEF-0123-456789ABCDEF}", true};
        require(store.replace_rules({rule}), "valid rule rejected");
        require(store.save(), "save failed");
        AppRoutingConfigStore loaded(path);
        require(loaded.load(), "reload failed");
        require(loaded.rules() == std::vector<AppRoutingRule>{rule}, "rule did not survive round trip");
        require(!loaded.replace_rules({rule, rule}), "duplicate ids accepted");
        require(loaded.rules() == std::vector<AppRoutingRule>{rule}, "invalid replacement changed rules");
        require(!loaded.replace_rules({AppRoutingRule{"", rule.application_path, rule.adapter_id, true}}), "empty id accepted");
        require(!loaded.replace_rules({AppRoutingRule{"x", "", rule.adapter_id, true}}), "empty path accepted");
        require(!loaded.replace_rules({AppRoutingRule{"x", rule.application_path, "", true}}), "empty adapter accepted");
        require(!loaded.replace_rules({AppRoutingRule{"x", "client.exe", rule.adapter_id, true}}), "relative executable path accepted");
        require(!loaded.replace_rules({AppRoutingRule{"x", rule.application_path, "not-a-guid", true}}), "invalid adapter identifier accepted");
        require(!loaded.replace_rules({AppRoutingRule{"x", rule.application_path + std::string("\0evil", 5), rule.adapter_id, true}}), "embedded NUL accepted");
        const AppRoutingRule conflicting{"rule-2", "c:/apps/Тест/CLIENT.EXE", "{11234567-89AB-CDEF-0123-456789ABCDEF}", true};
        require(!loaded.replace_rules({rule, conflicting}), "conflicting rules for same Windows executable accepted");
        const AppRoutingRule packaged{"store-app", "", rule.adapter_id, true, "OpenAI.Codex_2p2nqsd0c76g0"};
        AppRoutingConfigStore packaged_store(path);
        require(packaged_store.load(), "package store load failed");
        require(packaged_store.replace_rules({packaged}), "valid package family rule rejected");
        require(packaged_store.save(), "package rule save failed");
        AppRoutingConfigStore reload_package(path);
        require(reload_package.load() && reload_package.rules() == std::vector<AppRoutingRule>{packaged}, "package rule did not round-trip");
        require(!reload_package.replace_rules({AppRoutingRule{"x", rule.application_path, rule.adapter_id, true, packaged.package_family}}),
                "ambiguous application and package identities accepted");
        require(!reload_package.replace_rules({AppRoutingRule{"x", "", rule.adapter_id, true, "bad/family"}}), "invalid package family accepted");
        require(!reload_package.replace_rules({packaged, AppRoutingRule{"other", "", rule.adapter_id, true, "openai.codex_2p2nqsd0c76g0"}}),
                "case-insensitive duplicate package family accepted");
        const std::string legacy = R"({"version":1,"rules":[{"id":"old","application_path":"C:\\old.exe","adapter_id":"{01234567-89AB-CDEF-0123-456789ABCDEF}","enabled":true}]})";
        { std::ofstream f(path, std::ios::binary | std::ios::trunc); f << legacy; }
        AppRoutingConfigStore legacy_store(path);
        require(legacy_store.load() && legacy_store.rules().size() == 1 && legacy_store.rules()[0].package_family.empty(),
                "legacy configuration without package family rejected");
        const std::string bad = R"({"version":1,"rules":[{"id":"x","application_path":"C:\\x.exe","adapter_id":"adapter","enabled":"yes"}]})";
        { std::ofstream f(path, std::ios::binary | std::ios::trunc); f << bad; }
        require(!loaded.load(), "malformed rule accepted");
        require(loaded.rules() == std::vector<AppRoutingRule>{rule}, "failed load changed in-memory rules");
        std::ifstream f(path, std::ios::binary);
        require(std::string(std::istreambuf_iterator<char>(f), {}) == bad, "failed load changed file");
        f.close();
        std::filesystem::remove(path);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        std::filesystem::remove(path);
        return 1;
    }
}
