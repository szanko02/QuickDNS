#include "config_manager.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstdlib>

namespace {

std::string escape_json_string(const std::string& input) {
    std::ostringstream ss;
    for (char c : input) {
        switch (c) {
            case '"': ss << "\\\""; break;
            case '\\': ss << "\\\\"; break;
            case '\b': ss << "\\b"; break;
            case '\f': ss << "\\f"; break;
            case '\n': ss << "\\n"; break;
            case '\r': ss << "\\r"; break;
            case '\t': ss << "\\t"; break;
            default:
                if ('\x00' <= c && c <= '\x1f') {
                    // Control character
                } else {
                    ss << c;
                }
        }
    }
    return ss.str();
}

std::string extract_json_value(const std::string& json, const std::string& key) {
    std::string searchKey = "\"" + key + "\"";
    size_t pos = json.find(searchKey);
    if (pos == std::string::npos) return "";

    pos = json.find(':', pos);
    if (pos == std::string::npos) return "";

    pos++; // move past ':'
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\r' || json[pos] == '\n')) {
        pos++;
    }

    if (pos >= json.size()) return "";

    if (json[pos] == '"') {
        // String value
        size_t endPos = pos + 1;
        while (endPos < json.size()) {
            if (json[endPos] == '"' && json[endPos - 1] != '\\') {
                break;
            }
            endPos++;
        }
        return json.substr(pos + 1, endPos - pos - 1);
    } else {
        // Boolean or numeric
        size_t endPos = pos;
        while (endPos < json.size() && json[endPos] != ',' && json[endPos] != '}' && json[endPos] != '\n' && json[endPos] != '\r') {
            endPos++;
        }
        std::string val = json.substr(pos, endPos - pos);
        val.erase(std::remove_if(val.begin(), val.end(), ::isspace), val.end());
        return val;
    }
}

} // anonymous namespace

ConfigManager::ConfigManager(const std::filesystem::path& config_path) {
    if (config_path.empty()) {
        m_config_path = resolve_default_config_path();
    } else {
        m_config_path = config_path;
    }

    bool loaded = load_from_file();

    bool needs_reset = false;
    if (loaded) {
        for (const auto& p : m_profiles) {
            if (p.name.find("\xEF\xBF\xBD") != std::string::npos ||
                p.description.find("\xEF\xBF\xBD") != std::string::npos) {
                needs_reset = true;
                break;
            }
        }
    }

    if (!loaded || needs_reset) {
        m_profiles = get_default_presets();
        m_active_profile_id = "dhcp";
        save_to_file();
    } else {
        // Synchronize built-in presets with the latest authoritative IPs and DoH templates
        auto defaultPresets = get_default_presets();
        bool updatedAny = false;
        for (auto& p : m_profiles) {
            if (p.is_builtin) {
                for (const auto& def : defaultPresets) {
                    if (def.id == p.id) {
                        if (p.primary_ipv4 != def.primary_ipv4 ||
                            p.secondary_ipv4 != def.secondary_ipv4 ||
                            p.doh_template != def.doh_template ||
                            p.name != def.name ||
                            p.description != def.description) {
                            p.primary_ipv4 = def.primary_ipv4;
                            p.secondary_ipv4 = def.secondary_ipv4;
                            p.primary_ipv6 = def.primary_ipv6;
                            p.secondary_ipv6 = def.secondary_ipv6;
                            p.doh_template = def.doh_template;
                            p.name = def.name;
                            p.description = def.description;
                            updatedAny = true;
                        }
                        break;
                    }
                }
            }
        }
        if (updatedAny) {
            save_to_file();
        }
    }
}

std::filesystem::path ConfigManager::resolve_default_config_path() {
    const char* appData = std::getenv("APPDATA");
    if (appData) {
        return std::filesystem::path(appData) / "QuickDNS" / "profiles.json";
    }
    return std::filesystem::path("profiles.json");
}

std::filesystem::path ConfigManager::get_config_file_path() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_config_path;
}

std::vector<DnsProfile> ConfigManager::get_all_profiles() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_profiles;
}

std::optional<DnsProfile> ConfigManager::get_profile_by_id(const std::string& id) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& p : m_profiles) {
        if (p.id == id) {
            return p;
        }
    }
    return std::nullopt;
}

std::string ConfigManager::get_active_profile_id() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_active_profile_id;
}

void ConfigManager::set_active_profile_id(const std::string& id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_active_profile_id = id;
}

bool ConfigManager::add_profile(const DnsProfile& profile) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (profile.id.empty()) return false;

    for (const auto& p : m_profiles) {
        if (p.id == profile.id) {
            return false;
        }
    }

    m_profiles.push_back(profile);
    return true;
}

bool ConfigManager::update_profile(const DnsProfile& profile) {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& p : m_profiles) {
        if (p.id == profile.id) {
            p = profile;
            return true;
        }
    }
    return false;
}

bool ConfigManager::delete_profile(const std::string& id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto it = m_profiles.begin(); it != m_profiles.end(); ++it) {
        if (it->id == id) {
            if (it->is_builtin) return false;
            m_profiles.erase(it);
            return true;
        }
    }
    return false;
}

std::vector<DnsProfile> ConfigManager::get_default_presets() {
    return {
        {
            "comss",
            "Comss.one DNS",
            "Comss.one DNS - Защита от цензуры, рекламы и быстрый доступ",
            "83.220.169.155",
            "212.109.195.93",
            "",
            "",
            "https://dns.comss.one/dns-query",
            true,
            false
        },
        {
            "geohide",
            "GeoHide DNS",
            "GeoHide DNS - Обход региональных и географических ограничений",
            "193.233.112.67",
            "193.233.112.88",
            "",
            "",
            "https://dns.geohide.ru/dns-query",
            true,
            false
        },
        {
            "bezmezhau",
            "Без межаў DNS",
            "Без межаў DNS - Независимый и свободный доступ без цензуры",
            "143.20.64.55",
            "31.58.216.201",
            "",
            "",
            "https://dns.bezmezhau.com/dns-query",
            true,
            false
        },
        {
            "cloudflare",
            "Cloudflare (1.1.1.1)",
            "Cloudflare - Быстрый глобальный DNS с высокой приватностью",
            "1.1.1.1",
            "1.0.0.1",
            "2606:4700:4700::1111",
            "2606:4700:4700::1001",
            "https://cloudflare-dns.com/dns-query",
            true,
            false
        },
        {
            "google",
            "Google Public DNS",
            "Google DNS - Высокая надежность и стабильность",
            "8.8.8.8",
            "8.8.4.4",
            "2001:4860:4860::8888",
            "2001:4860:4860::8844",
            "https://dns.google/dns-query",
            true,
            false
        },
        {
            "quad9",
            "Quad9 DNS",
            "Quad9 - Защита от фишинга, вредоносных программ и ботнетов",
            "9.9.9.9",
            "149.112.112.112",
            "2620:fe::fe",
            "2620:fe::9",
            "https://dns.quad9.net/dns-query",
            true,
            false
        },
        {
            "adguard",
            "AdGuard DNS",
            "AdGuard DNS - Блокировка рекламы, трекеров и вредоносных сайтов",
            "94.140.14.14",
            "94.140.15.15",
            "2a10:50c0::ad1:ff",
            "2a10:50c0::ad2:ff",
            "https://dns.adguard-dns.com/dns-query",
            true,
            false
        },
        {
            "dhcp",
            "Автоматически (DHCP)",
            "DNS по умолчанию от вашего сетевого маршрутизатора / интернет-провайдера",
            "",
            "",
            "",
            "",
            "",
            true,
            true
        }
    };
}

bool ConfigManager::save_to_file() {
    try {
        if (m_config_path.has_parent_path()) {
            std::filesystem::create_directories(m_config_path.parent_path());
        }

        std::ofstream file(m_config_path, std::ios::out | std::ios::trunc | std::ios::binary);
        if (!file.is_open()) return false;

        std::ostringstream ss;
        ss << "{\n";
        ss << "  \"active_profile_id\": \"" << escape_json_string(m_active_profile_id) << "\",\n";
        ss << "  \"profiles\": [\n";

        for (size_t i = 0; i < m_profiles.size(); ++i) {
            const auto& p = m_profiles[i];
            ss << "    {\n";
            ss << "      \"id\": \"" << escape_json_string(p.id) << "\",\n";
            ss << "      \"name\": \"" << escape_json_string(p.name) << "\",\n";
            ss << "      \"description\": \"" << escape_json_string(p.description) << "\",\n";
            ss << "      \"primary_ipv4\": \"" << escape_json_string(p.primary_ipv4) << "\",\n";
            ss << "      \"secondary_ipv4\": \"" << escape_json_string(p.secondary_ipv4) << "\",\n";
            ss << "      \"primary_ipv6\": \"" << escape_json_string(p.primary_ipv6) << "\",\n";
            ss << "      \"secondary_ipv6\": \"" << escape_json_string(p.secondary_ipv6) << "\",\n";
            ss << "      \"doh_template\": \"" << escape_json_string(p.doh_template) << "\",\n";
            ss << "      \"is_builtin\": " << (p.is_builtin ? "true" : "false") << ",\n";
            ss << "      \"is_dhcp\": " << (p.is_dhcp ? "true" : "false") << "\n";
            ss << "    }" << (i + 1 < m_profiles.size() ? "," : "") << "\n";
        }

        ss << "  ]\n";
        ss << "}\n";

        std::string jsonStr = ss.str();
        file.write(jsonStr.data(), jsonStr.size());
        return true;
    } catch (...) {
        return false;
    }
}

bool ConfigManager::load_from_file() {
    try {
        if (!std::filesystem::exists(m_config_path)) {
            return false;
        }

        std::ifstream file(m_config_path, std::ios::in | std::ios::binary);
        if (!file.is_open()) return false;

        std::stringstream buffer;
        buffer << file.rdbuf();
        std::string content = buffer.str();

        std::string activeId = extract_json_value(content, "active_profile_id");
        if (!activeId.empty()) {
            m_active_profile_id = activeId;
        }

        // Parse profile objects
        std::vector<DnsProfile> loadedProfiles;
        size_t searchPos = 0;
        while (true) {
            size_t objStart = content.find('{', searchPos);
            if (objStart == std::string::npos) break;

            if (objStart == 0 || (objStart < 5 && searchPos == 0)) {
                searchPos = objStart + 1;
                continue;
            }

            size_t objEnd = content.find('}', objStart);
            if (objEnd == std::string::npos) break;

            std::string objStr = content.substr(objStart, objEnd - objStart + 1);
            std::string id = extract_json_value(objStr, "id");

            if (!id.empty()) {
                DnsProfile p;
                p.id = id;
                p.name = extract_json_value(objStr, "name");
                p.description = extract_json_value(objStr, "description");
                p.primary_ipv4 = extract_json_value(objStr, "primary_ipv4");
                p.secondary_ipv4 = extract_json_value(objStr, "secondary_ipv4");
                p.primary_ipv6 = extract_json_value(objStr, "primary_ipv6");
                p.secondary_ipv6 = extract_json_value(objStr, "secondary_ipv6");
                p.doh_template = extract_json_value(objStr, "doh_template");
                p.is_builtin = (extract_json_value(objStr, "is_builtin") == "true");
                p.is_dhcp = (extract_json_value(objStr, "is_dhcp") == "true");
                loadedProfiles.push_back(p);
            }

            searchPos = objEnd + 1;
        }

        if (!loadedProfiles.empty()) {
            m_profiles = std::move(loadedProfiles);
            return true;
        }

        return false;
    } catch (...) {
        return false;
    }
}
