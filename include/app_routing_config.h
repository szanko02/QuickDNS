#pragma once
#include <filesystem>
#include <string>
#include <vector>

struct AppRoutingRule {
    std::string id;
    std::string application_path;
    std::string adapter_id;
    bool enabled = false;
    // Exactly one of application_path and package_family identifies the rule.
    std::string package_family;
    bool operator==(const AppRoutingRule&) const = default;
};

class AppRoutingConfigStore {
public:
    explicit AppRoutingConfigStore(std::filesystem::path path = {});
    bool load();
    bool save() const;
    bool replace_rules(const std::vector<AppRoutingRule>& rules);
    std::vector<AppRoutingRule> rules() const;
    std::string last_error() const;
private:
    std::filesystem::path path_;
    std::vector<AppRoutingRule> rules_;
    mutable std::string error_;
    bool loaded_ = false;
};
