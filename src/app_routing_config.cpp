#include "app_routing_config.h"
#include <windows.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <set>
#include <utility>
#include <algorithm>

using nlohmann::json;

namespace {
std::filesystem::path default_path() {
    const DWORD length = GetEnvironmentVariableW(L"APPDATA", nullptr, 0);
    if (!length) return {};
    std::wstring value(length, L'\0');
    const DWORD written = GetEnvironmentVariableW(L"APPDATA", value.data(), length);
    if (!written || written >= length) return {};
    value.resize(written);
    return std::filesystem::path(value) / L"QuickDNS" / L"app-routing.json";
}

bool valid_guid(const std::string& text) {
    if (text.size() != 38 || text.front() != '{' || text.back() != '}') return false;
    for (size_t i = 1; i < 37; ++i) {
        if (i == 9 || i == 14 || i == 19 || i == 24) {
            if (text[i] != '-') return false;
        } else if (!((text[i] >= '0' && text[i] <= '9') ||
                     (text[i] >= 'a' && text[i] <= 'f') ||
                     (text[i] >= 'A' && text[i] <= 'F'))) return false;
    }
    return true;
}

bool executable_path(const std::string& text, std::wstring& normalized) {
    if (text.empty() || text.find('\0') != std::string::npos || text.size() > INT_MAX) return false;
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0);
    if (!length) return false;
    std::wstring value(length, L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                             static_cast<int>(text.size()), value.data(), length)) return false;
    std::replace(value.begin(), value.end(), L'/', L'\\');
    std::filesystem::path path(value);
    if (!path.is_absolute() || path.filename().empty()) return false;
    normalized = path.lexically_normal().native();
    return true;
}

bool package_family(const std::string& text, std::string& normalized) {
    const auto separator = text.find('_');
    if (separator == std::string::npos || separator < 1 || separator > 50 ||
        text.size() - separator - 1 != 13 || text.find('_', separator + 1) != std::string::npos) return false;
    normalized.clear();
    for (size_t i = 0; i < text.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c + ('a' - 'A'));
        const bool alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (!(alnum || i == separator || (i < separator && (c == '.' || c == '-')))) return false;
        normalized.push_back(static_cast<char>(c));
    }
    return true;
}

bool valid_rules(const std::vector<AppRoutingRule>& rules) {
    std::set<std::string> ids;
    std::set<std::string> packages;
    std::vector<std::wstring> paths;
    for (const auto& rule : rules) {
        std::wstring path;
        if (rule.id.empty() || rule.id.find('\0') != std::string::npos ||
            !valid_guid(rule.adapter_id) || !ids.insert(rule.id).second ||
            (rule.application_path.empty() == rule.package_family.empty())) return false;
        if (!rule.package_family.empty()) {
            std::string package;
            if (!package_family(rule.package_family, package) || !packages.insert(package).second) return false;
            continue;
        }
        if (!executable_path(rule.application_path, path)) return false;
        for (const auto& existing : paths) {
            if (CompareStringOrdinal(existing.c_str(), -1, path.c_str(), -1, TRUE) == CSTR_EQUAL) return false;
        }
        paths.push_back(std::move(path));
    }
    return true;
}
}

AppRoutingConfigStore::AppRoutingConfigStore(std::filesystem::path path)
    : path_(path.empty() ? default_path() : std::move(path)) {}

bool AppRoutingConfigStore::load() {
    error_.clear();
    loaded_ = false;
    try {
        if (path_.empty()) throw std::runtime_error("configuration path unavailable");
        if (!std::filesystem::exists(path_)) {
            rules_.clear();
            loaded_ = true;
            return true;
        }
        std::ifstream in(path_, std::ios::binary);
        if (!in) throw std::runtime_error("cannot open configuration");
        const auto root = json::parse(in);
        if (!root.is_object() || !root.contains("version") || !root["version"].is_number_integer() ||
            root["version"] != 1 || !root.contains("rules") || !root["rules"].is_array())
            throw std::runtime_error("unsupported configuration format");
        std::vector<AppRoutingRule> next;
        for (const auto& item : root["rules"]) {
            if (!item.is_object() || !item.contains("id") || !item["id"].is_string() ||
                !item.contains("application_path") || !item["application_path"].is_string() ||
                !item.contains("adapter_id") || !item["adapter_id"].is_string() ||
                !item.contains("enabled") || !item["enabled"].is_boolean() ||
                (item.contains("package_family") && !item["package_family"].is_string()))
                throw std::runtime_error("invalid routing rule");
            next.push_back({item["id"].get<std::string>(), item["application_path"].get<std::string>(),
                            item["adapter_id"].get<std::string>(), item["enabled"].get<bool>(),
                            item.value("package_family", std::string{})});
        }
        if (!valid_rules(next)) throw std::runtime_error("invalid or duplicate rule");
        rules_ = std::move(next);
        loaded_ = true;
        return true;
    } catch (const std::exception& ex) {
        error_ = ex.what();
        return false;
    }
}

bool AppRoutingConfigStore::save() const {
    error_.clear();
    std::filesystem::path temp;
    try {
        if (!loaded_) throw std::runtime_error("configuration has not been loaded successfully");
        json root{{"version", 1}, {"rules", json::array()}};
        for (const auto& rule : rules_)
            root["rules"].push_back({{"id", rule.id}, {"application_path", rule.application_path},
                                     {"adapter_id", rule.adapter_id}, {"enabled", rule.enabled},
                                     {"package_family", rule.package_family}});
        const std::string data = root.dump(2);
        auto directory = path_.parent_path();
        if (directory.empty()) directory = L".";
        std::filesystem::create_directories(directory);
        wchar_t name[MAX_PATH]{};
        if (!GetTempFileNameW(directory.c_str(), L"qdr", 0, name))
            throw std::runtime_error("cannot create temporary configuration");
        temp = name;
        HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, TRUNCATE_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot open temporary configuration");
        DWORD written = 0;
        const bool ok = data.size() <= MAXDWORD && WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) &&
                        written == data.size() && FlushFileBuffers(file);
        const bool closed = CloseHandle(file) != FALSE;
        if (!ok || !closed) throw std::runtime_error("cannot write temporary configuration");
        if (!MoveFileExW(temp.c_str(), path_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("cannot replace configuration");
        return true;
    } catch (const std::exception& ex) {
        if (!temp.empty()) DeleteFileW(temp.c_str());
        error_ = ex.what();
        return false;
    }
}

bool AppRoutingConfigStore::replace_rules(const std::vector<AppRoutingRule>& rules) {
    if (!loaded_ || !valid_rules(rules)) {
        error_ = "configuration not loaded or rules invalid";
        return false;
    }
    rules_ = rules;
    error_.clear();
    return true;
}
std::vector<AppRoutingRule> AppRoutingConfigStore::rules() const { return rules_; }
std::string AppRoutingConfigStore::last_error() const { return error_; }
