#include "config_manager.h"
#include "dns_manager.h"
#include "encoding_utils.h"
#include "nlohmann/json.hpp"

#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <iostream>
#include <chrono>

using json = nlohmann::json;

namespace {

char ascii_tolower(char c) {
    if (c >= 'A' && c <= 'Z') {
        return static_cast<char>(c - 'A' + 'a');
    }
    return c;
}

bool starts_with_icase_ascii(std::string_view str, std::string_view prefix) {
    if (str.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (ascii_tolower(str[i]) != ascii_tolower(prefix[i])) {
            return false;
        }
    }
    return true;
}

bool is_valid_website_url(const std::string& url) {
    if (url.empty() || url.size() > 2048) {
        return false;
    }

    size_t scheme_len = 0;
    if (starts_with_icase_ascii(url, "https://")) {
        scheme_len = 8;
    } else if (starts_with_icase_ascii(url, "http://")) {
        scheme_len = 7;
    } else {
        return false;
    }

    if (url.size() <= scheme_len) {
        return false;
    }

    for (char ch : url) {
        unsigned char uc = static_cast<unsigned char>(ch);
        if (uc <= 0x1F || uc == 0x7F || ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
            return false;
        }
    }

    std::string_view rest(url.data() + scheme_len, url.size() - scheme_len);
    size_t authority_end = rest.find_first_of("/?#");
    std::string_view authority = (authority_end == std::string_view::npos)
                                     ? rest
                                     : rest.substr(0, authority_end);

    if (authority.empty() ||
        authority[0] == ':' ||
        authority[0] == '.' ||
        authority.find('@') != std::string_view::npos ||
        authority.find('\\') != std::string_view::npos) {
        return false;
    }

    return true;
}

bool sanitize_profile_fields(DnsProfile& p, bool log_warnings, const char* caller_name) {
    bool modified = false;

    if (!p.primary_ipv4.empty() && !quickdns::DnsManager::is_valid_ipv4(p.primary_ipv4)) {
        if (log_warnings) {
            std::wcerr << L"[QuickDNS::ConfigManager] WARNING: Invalid primary_ipv4 in "
                       << QuickDns::Encoding::utf8_to_wide(caller_name) << L" cleared.\n";
        }
        p.primary_ipv4.clear();
        modified = true;
    }

    if (!p.secondary_ipv4.empty() && !quickdns::DnsManager::is_valid_ipv4(p.secondary_ipv4)) {
        if (log_warnings) {
            std::wcerr << L"[QuickDNS::ConfigManager] WARNING: Invalid secondary_ipv4 in "
                       << QuickDns::Encoding::utf8_to_wide(caller_name) << L" cleared.\n";
        }
        p.secondary_ipv4.clear();
        modified = true;
    }

    if (!p.primary_ipv6.empty() && !quickdns::DnsManager::is_valid_ipv6(p.primary_ipv6)) {
        if (log_warnings) {
            std::wcerr << L"[QuickDNS::ConfigManager] WARNING: Invalid primary_ipv6 in "
                       << QuickDns::Encoding::utf8_to_wide(caller_name) << L" cleared.\n";
        }
        p.primary_ipv6.clear();
        modified = true;
    }

    if (!p.secondary_ipv6.empty() && !quickdns::DnsManager::is_valid_ipv6(p.secondary_ipv6)) {
        if (log_warnings) {
            std::wcerr << L"[QuickDNS::ConfigManager] WARNING: Invalid secondary_ipv6 in "
                       << QuickDns::Encoding::utf8_to_wide(caller_name) << L" cleared.\n";
        }
        p.secondary_ipv6.clear();
        modified = true;
    }

    if (!p.doh_template.empty()) {
        std::wstring w_doh = QuickDns::Encoding::utf8_to_wide(p.doh_template);
        if (!quickdns::DnsManager::is_valid_doh_template(w_doh)) {
            if (log_warnings) {
                std::wcerr << L"[QuickDNS::ConfigManager] WARNING: Invalid doh_template in "
                           << QuickDns::Encoding::utf8_to_wide(caller_name) << L" cleared: " << w_doh << L"\n";
            }
            p.doh_template.clear();
            modified = true;
        }
    }

    if (!p.website_url.empty() && !is_valid_website_url(p.website_url)) {
        if (log_warnings) {
            std::wcerr << L"[QuickDNS::ConfigManager] WARNING: Invalid website_url in "
                       << QuickDns::Encoding::utf8_to_wide(caller_name) << L" cleared.\n";
        }
        p.website_url.clear();
        modified = true;
    }

    std::vector<std::string> clean_pool;
    clean_pool.reserve(std::min(p.ip_pool.size(), ConfigManager::MAX_IP_POOL_SIZE));
    for (const auto& ip : p.ip_pool) {
        if (ip.empty() || !quickdns::DnsManager::is_valid_ipv4(ip)) {
            modified = true;
            continue;
        }
        if (std::find(clean_pool.begin(), clean_pool.end(), ip) != clean_pool.end()) {
            modified = true;
            continue;
        }
        if (clean_pool.size() >= ConfigManager::MAX_IP_POOL_SIZE) {
            modified = true;
            continue;
        }
        clean_pool.push_back(ip);
    }
    if (clean_pool != p.ip_pool) {
        p.ip_pool = std::move(clean_pool);
        modified = true;
    }

    return modified;
}

bool sync_active_ipv4_into_pool(DnsProfile& p) {
    if (p.is_dhcp) {
        return false;
    }

    bool modified = false;
    auto ensure_in_pool = [&](const std::string& ip) {
        if (ip.empty() || !quickdns::DnsManager::is_valid_ipv4(ip)) {
            return;
        }
        if (std::find(p.ip_pool.begin(), p.ip_pool.end(), ip) != p.ip_pool.end()) {
            return;
        }
        if (p.ip_pool.size() >= ConfigManager::MAX_IP_POOL_SIZE) {
            for (auto it = p.ip_pool.end(); it != p.ip_pool.begin();) {
                --it;
                if (*it != p.primary_ipv4 && *it != p.secondary_ipv4) {
                    p.ip_pool.erase(it);
                    break;
                }
            }
        }
        if (p.ip_pool.size() < ConfigManager::MAX_IP_POOL_SIZE) {
            p.ip_pool.push_back(ip);
            modified = true;
        }
    };

    ensure_in_pool(p.primary_ipv4);
    ensure_in_pool(p.secondary_ipv4);
    return modified;
}

bool has_env_var(const wchar_t* name) {
    return GetEnvironmentVariableW(name, nullptr, 0) > 0;
}

std::filesystem::path get_executable_directory() {
    std::wstring buf(MAX_PATH, L'\0');
    while (buf.size() <= 32768) {
        DWORD len = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (len == 0) {
            break;
        }
        if (len < buf.size()) {
            buf.resize(len);
            std::filesystem::path exe_path(buf);
            if (exe_path.has_parent_path()) {
                return exe_path.parent_path();
            }
            break;
        }
        buf.resize(buf.size() * 2, L'\0');
    }
    return {};
}

void clear_readonly_attribute(const std::filesystem::path& p) {
    DWORD attrs = GetFileAttributesW(p.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY) != 0) {
        DWORD next_attrs = attrs & ~FILE_ATTRIBUTE_READONLY;
        if (next_attrs == 0) next_attrs = FILE_ATTRIBUTE_NORMAL;
        SetFileAttributesW(p.c_str(), next_attrs);
    }
}

bool backup_corrupt_config(const std::filesystem::path& config_path, std::filesystem::path& out_broken_path) {
    auto now = std::chrono::system_clock::now();
    auto sec = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

    std::filesystem::path broken_path = config_path;
    broken_path += L".broken-" + std::to_wstring(sec);

    std::error_code ec;
    if (std::filesystem::exists(broken_path, ec)) {
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        broken_path = config_path;
        broken_path += L".broken-" + std::to_wstring(ms);
    }

    clear_readonly_attribute(config_path);
    if (MoveFileExW(config_path.c_str(), broken_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        out_broken_path = std::move(broken_path);
        return true;
    }

    if (CopyFileW(config_path.c_str(), broken_path.c_str(), FALSE)) {
        DeleteFileW(config_path.c_str());
        out_broken_path = std::move(broken_path);
        return true;
    }

    return false;
}

void notify_corrupt_config_unlocked(const std::filesystem::path& broken_path) {
    bool is_ci_or_test = (has_env_var(L"CI") ||
                          has_env_var(L"GITHUB_ACTIONS") ||
                          has_env_var(L"TESTING"));
    if (!is_ci_or_test && GetConsoleWindow() == nullptr) {
        std::wstring warn = L"Обнаружена ошибка в файле конфигурации profiles.json. Файл переименован в " +
                            broken_path.filename().wstring() + L", созданы настройки по умолчанию.";
        MessageBoxW(nullptr, warn.c_str(), L"QuickDNS - Ошибка конфигурации", MB_OK | MB_ICONWARNING);
    }
}

bool parse_profile_from_json_object(const json& j, DnsProfile& p, bool* out_was_sanitized) {
    bool sanitized = false;

    auto get_str = [&](const char* key) -> std::string {
        if (j.contains(key)) {
            if (j[key].is_string()) {
                return j[key].get<std::string>();
            }
            if (!j[key].is_null()) {
                sanitized = true;
            }
        }
        return "";
    };

    auto get_bool = [&](const char* key, bool def = false) -> bool {
        if (j.contains(key)) {
            if (j[key].is_boolean()) {
                return j[key].get<bool>();
            }
            sanitized = true;
        }
        return def;
    };

    p.id = get_str("id");
    p.name = get_str("name");
    p.description = get_str("description");
    p.primary_ipv4 = get_str("primary_ipv4");
    p.secondary_ipv4 = get_str("secondary_ipv4");
    p.primary_ipv6 = get_str("primary_ipv6");
    p.secondary_ipv6 = get_str("secondary_ipv6");
    p.doh_template = get_str("doh_template");
    p.is_builtin = get_bool("is_builtin", false);
    p.is_dhcp = get_bool("is_dhcp", false);
    p.is_smart_dns = get_bool("is_smart_dns", false);
    p.website_url = get_str("website_url");
    p.unblocked_summary = get_str("unblocked_summary");
    p.ip_pool.clear();

    if (j.contains("ip_pool")) {
        if (j["ip_pool"].is_array()) {
            for (const auto& item : j["ip_pool"]) {
                if (item.is_string()) {
                    p.ip_pool.push_back(item.get<std::string>());
                } else {
                    sanitized = true;
                }
            }
        } else {
            sanitized = true;
        }
    }

    if (sanitize_profile_fields(p, false, "from_json")) {
        sanitized = true;
    }

    if (out_was_sanitized) {
        *out_was_sanitized = sanitized;
    }
    return !p.id.empty();
}

} // namespace

void to_json(json& j, const DnsProfile& p) {
    j = json{
        {"id", p.id},
        {"name", p.name},
        {"description", p.description},
        {"primary_ipv4", p.primary_ipv4},
        {"secondary_ipv4", p.secondary_ipv4},
        {"primary_ipv6", p.primary_ipv6},
        {"secondary_ipv6", p.secondary_ipv6},
        {"doh_template", p.doh_template},
        {"is_builtin", p.is_builtin},
        {"is_dhcp", p.is_dhcp},
        {"is_smart_dns", p.is_smart_dns},
        {"website_url", p.website_url},
        {"unblocked_summary", p.unblocked_summary},
        {"ip_pool", p.ip_pool}
    };
}

void from_json(const json& j, DnsProfile& p) {
    if (!j.is_object()) {
        p = DnsProfile{};
        return;
    }
    parse_profile_from_json_object(j, p, nullptr);
}

ConfigManager::ConfigManager(const std::filesystem::path& config_path) {
    if (config_path.empty()) {
        m_config_path = resolve_default_config_path();
    } else {
        m_config_path = config_path;
    }

    load_from_file();
}

std::filesystem::path ConfigManager::resolve_default_config_path() {
    PWSTR roamingPath = nullptr;
    HRESULT hr = SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roamingPath);
    if (SUCCEEDED(hr) && roamingPath != nullptr && roamingPath[0] != L'\0') {
        std::filesystem::path p = std::filesystem::path(roamingPath) / L"QuickDNS" / L"profiles.json";
        CoTaskMemFree(roamingPath);
        return p;
    }
    if (roamingPath != nullptr) {
        CoTaskMemFree(roamingPath);
    }

    DWORD appDataLen = GetEnvironmentVariableW(L"APPDATA", nullptr, 0);
    if (appDataLen > 1) {
        std::wstring appData(appDataLen - 1, L'\0');
        DWORD written = GetEnvironmentVariableW(L"APPDATA", appData.data(), appDataLen);
        if (written > 0 && appData[0] != L'\0') {
            appData.resize(written);
            return std::filesystem::path(appData) / L"QuickDNS" / L"profiles.json";
        }
    }

    std::filesystem::path exeDir = get_executable_directory();
    if (!exeDir.empty()) {
        return exeDir / L"profiles.json";
    }

    return std::filesystem::path(L"profiles.json");
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
    m_active_profile_id = id.empty() ? "dhcp" : id;
}

std::string ConfigManager::get_theme_mode() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_theme_mode;
}

void ConfigManager::set_theme_mode(const std::string& theme) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_theme_mode = (theme == "light") ? "light" : "dark";
}

bool ConfigManager::add_profile(const DnsProfile& profile) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (profile.id.empty()) {
        return false;
    }

    for (const auto& p : m_profiles) {
        if (p.id == profile.id) {
            return false;
        }
    }
    for (const auto& def : get_default_presets()) {
        if (def.id == profile.id) {
            return false;
        }
    }

    DnsProfile clean_profile = profile;
    clean_profile.is_builtin = false;
    clean_profile.is_dhcp = false;
    sanitize_profile_fields(clean_profile, true, "add_profile");
    sync_active_ipv4_into_pool(clean_profile);

    m_profiles.push_back(std::move(clean_profile));
    return true;
}

bool ConfigManager::update_profile(const DnsProfile& profile) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (profile.id.empty()) {
        return false;
    }

    for (auto& p : m_profiles) {
        if (p.id == profile.id) {
            DnsProfile clean_profile = profile;
            clean_profile.is_builtin = p.is_builtin;
            clean_profile.is_dhcp = p.is_dhcp;

            if (clean_profile.is_dhcp) {
                clean_profile.primary_ipv4.clear();
                clean_profile.secondary_ipv4.clear();
                clean_profile.primary_ipv6.clear();
                clean_profile.secondary_ipv6.clear();
                clean_profile.doh_template.clear();
                clean_profile.ip_pool.clear();
                clean_profile.is_smart_dns = false;
                sanitize_profile_fields(clean_profile, true, "update_profile");
            } else {
                sanitize_profile_fields(clean_profile, true, "update_profile");
                sync_active_ipv4_into_pool(clean_profile);
            }

            p = std::move(clean_profile);
            return true;
        }
    }
    return false;
}

bool ConfigManager::delete_profile(const std::string& id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto it = m_profiles.begin(); it != m_profiles.end(); ++it) {
        if (it->id == id) {
            if (it->is_builtin) {
                return false;
            }
            m_profiles.erase(it);

            bool active_exists = false;
            for (const auto& p : m_profiles) {
                if (p.id == m_active_profile_id) {
                    active_exists = true;
                    break;
                }
            }
            if (!active_exists) {
                m_active_profile_id = "dhcp";
            }
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
            false,
            true,
            "https://dns.comss.one/",
            "ChatGPT, Copilot, Claude, Spotify, Canva, Notion, Instagram, Twitter/X",
            {"83.220.169.155", "212.109.195.93", "195.133.25.16"}
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
            false,
            true,
            "https://geohide.ru/",
            "ChatGPT, Claude, Gemini, Grok, Cursor, DeepL, Spotify, Notion, Canva, Autodesk",
            {"193.233.112.67", "193.233.112.88", "45.144.31.119", "162.248.241.106"}
        },
        {
            "geohide_eu",
            "GeoHide EU (Европа)",
            "GeoHide DNS - Европейские серверы (Нидерланды / Германия) для обхода блокировок",
            "45.13.237.213",
            "5.175.161.3",
            "",
            "",
            "https://eu.geohide.ru/dns-query",
            true,
            false,
            true,
            "https://geohide.ru/",
            "ChatGPT, Claude, Gemini, Grok, Deezer, Spotify, Canva, Notion, JetBrains",
            {"45.13.237.213", "5.175.161.3", "45.144.31.119", "193.233.112.67", "193.233.112.88"}
        },
        {
            "geohide_us",
            "GeoHide US (США)",
            "GeoHide DNS - Североамериканские серверы для обхода блокировок и сервисов США",
            "192.255.159.241",
            "192.3.233.253",
            "",
            "",
            "https://us.geohide.ru/dns-query",
            true,
            false,
            true,
            "https://geohide.ru/",
            "ChatGPT, Claude, Gemini, Copilot, Grok, Spotify, Netflix US, Notion",
            {"192.255.159.241", "192.3.233.253", "162.248.241.106", "45.144.31.119", "193.233.112.67"}
        },
        {
            "bezmezhau",
            "Без межаў DNS",
            "Без межаў DNS - Независимый и свободный доступ без цензуры",
            "181.215.40.49",
            "143.20.64.55",
            "",
            "",
            "https://dns.bezmezhau.com/dns-query",
            true,
            false,
            true,
            "https://bezmezhau.com/",
            "ChatGPT, YouTube, Discord, Spotify, Twitter/X, Instagram, Reddit, TikTok",
            {"181.215.40.49", "143.20.64.55", "31.58.216.201", "185.236.10.189"}
        },
        {
            "bezmezhau_ru",
            "Без межаў DNS (РФ)",
            "Без межаў DNS (РФ) - Выделенный сервер для пользователей из России",
            "185.236.10.189",
            "31.58.216.201",
            "",
            "",
            "https://sync.bezmezhau.com/dns-query",
            true,
            false,
            true,
            "https://bezmezhau.com/",
            "ChatGPT, YouTube, Discord, Spotify, Twitter/X, Instagram, Reddit, TikTok",
            {"185.236.10.189", "31.58.216.201", "143.20.64.55", "181.215.40.49"}
        },
        {
            "zapret",
            "Zapret DNS",
            "Zapret DNS - Открытие заблокированных сервисов и нейросетей (Нидерланды)",
            "83.217.211.149",
            "",
            "2a03:afc0:9::235d",
            "",
            "https://dns.zapret.moe/dns-query",
            true,
            false,
            true,
            "https://zapret.moe/",
            "Discord, YouTube, ChatGPT, Claude, Instagram, Twitter/X, RuTracker",
            {"83.217.211.149"}
        },
        {
            "malw",
            "Malw.link DNS",
            "dns.malw.link - Разблокировка сайтов, блокировка рекламы и трекеров",
            "179.254.166.171",
            "164.5.224.81",
            "2a12:bec4:1bb0:27dd::2",
            "2a13:82c0:0:d::3",
            "https://dns.malw.link/dns-query",
            true,
            false,
            true,
            "https://info.dns.malw.link/",
            "ChatGPT, Gemini, Spotify, Discord, Instagram, Twitter/X, Notion, Canva",
            {"179.254.166.171", "164.5.224.81", "84.21.189.133", "64.188.98.242", "193.23.209.189"}
        },
        {
            "astracat",
            "Astracat DNS",
            "Astracat DNS - Обход блокировок нейросетей, Discord и YouTube",
            "135.106.217.200",
            "185.139.69.24",
            "",
            "",
            "https://dns.astracat.ru/dns-query",
            true,
            false,
            true,
            "https://astracat.ru/",
            "ChatGPT, Claude, Discord, YouTube, Spotify, Twitter/X, Instagram, Notion",
            {"135.106.217.200", "185.139.69.24", "185.236.104.104", "45.131.186.108"}
        },
        {
            "mafioznik",
            "Mafioznik DNS",
            "Mafioznik.xyz - Комьюнити SmartDNS для разблокировки сервисов и игр",
            "103.27.157.38",
            "103.27.157.100",
            "",
            "",
            "https://dns.mafioznik.xyz/dns-query",
            true,
            false,
            true,
            "https://dns.mafioznik.xyz/",
            "ChatGPT, Discord, YouTube, Steam, Spotify, Instagram, Twitter/X",
            {"103.27.157.38", "103.27.157.100", "45.95.233.23", "109.107.181.65"}
        },
        {
            "shecan",
            "Shecan DNS",
            "Shecan.ir - Антисанкционный SmartDNS для разработчиков и сервисов",
            "178.22.122.100",
            "185.51.200.2",
            "",
            "",
            "https://free.shecan.ir/dns-query",
            true,
            false,
            true,
            "https://shecan.ir/",
            "Docker Hub, Android Dev, Oracle, NVIDIA, Gradle, Google Services, AMD",
            {"178.22.122.100", "185.51.200.2"}
        },
        {
            "controld",
            "Control D (ControlID)",
            "Control D - Anycast SmartDNS от Windscribe с защитой от трекеров и блокировок",
            "76.76.2.0",
            "76.76.10.0",
            "2606:1a40::",
            "2606:1a40:1::",
            "https://freedns.controld.com/p0",
            true,
            false,
            true,
            "https://controld.com/",
            "ChatGPT, Claude, стриминги, защита от трекеров и фишинга",
            {"76.76.2.0", "76.76.10.0", "76.76.2.2", "76.76.10.2"}
        },
        {
            "akile",
            "Akile DNS (AKDNS)",
            "AkileCloud SmartDNS - Разблокировка зарубежных стримингов и сервисов",
            "66.66.66.66",
            "108.160.138.51",
            "",
            "",
            "https://dns.akile.ai/dns-query",
            true,
            false,
            true,
            "https://dns.akile.ai/",
            "Netflix, Disney+, YouTube, ChatGPT, Spotify, TikTok (требуется привязка IP)",
            {"66.66.66.66", "108.160.138.51"}
        },
        {
            "dns_ai",
            "DNS-AI (DoH)",
            "DNS-AI.ru - Шифрованный SmartDNS (DoH) для нейросетей без рекламы и телеметрии (Win11+)",
            "194.87.196.13",
            "45.151.102.219",
            "2a00:b700:5::3:62",
            "2a12:5940:d4ef::219",
            "https://dns.dns-ai.ru/dns-query",
            true,
            false,
            true,
            "https://dns-ai.ru/",
            "ChatGPT, OpenAI, Claude, Gemini, AI Studio, Copilot, Grok, Cursor + AdBlock (только DoH)",
            {"194.87.196.13", "45.151.102.219"}
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
            false,
            false,
            "https://1.1.1.1/",
            "",
            {"1.1.1.1", "1.0.0.1", "1.1.1.2", "1.0.0.2"}
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
            false,
            false,
            "https://developers.google.com/speed/public-dns",
            "",
            {"8.8.8.8", "8.8.4.4"}
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
            false,
            false,
            "https://quad9.net/",
            "",
            {"9.9.9.9", "149.112.112.112", "9.9.9.11", "149.112.112.11"}
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
            false,
            false,
            "https://adguard-dns.io/",
            "",
            {"94.140.14.14", "94.140.15.15", "94.140.14.140", "94.140.14.141"}
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
            true,
            false,
            "",
            "",
            {}
        }
    };
}

bool ConfigManager::save_to_file() {
    std::lock_guard<std::mutex> save_lock(m_save_mutex);

    std::string active_id;
    std::string theme;
    std::vector<DnsProfile> profiles_copy;
    std::filesystem::path path;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        active_id = m_active_profile_id;
        theme = m_theme_mode;
        profiles_copy = m_profiles;
        path = m_config_path;
    }

    try {
        if (path.has_parent_path()) {
            std::error_code dir_ec;
            std::filesystem::create_directories(path.parent_path(), dir_ec);
        }

        json root;
        root["active_profile_id"] = active_id;
        root["theme"] = theme;
        root["profiles"] = json::array();
        for (const auto& p : profiles_copy) {
            root["profiles"].push_back(p);
        }

        std::string json_str = root.dump(2, ' ', false, json::error_handler_t::replace);
        if (json_str.size() > static_cast<size_t>(MAXDWORD)) {
            return false;
        }

        std::filesystem::path tmp_path = path;
        tmp_path += L".tmp";
        clear_readonly_attribute(tmp_path);

        HANDLE hFile = CreateFileW(
            tmp_path.c_str(),
            GENERIC_WRITE,
            0,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );

        if (hFile == INVALID_HANDLE_VALUE) {
            return false;
        }

        DWORD bytes_written = 0;
        BOOL write_ok = WriteFile(
            hFile,
            json_str.data(),
            static_cast<DWORD>(json_str.size()),
            &bytes_written,
            nullptr
        );

        if (!write_ok || bytes_written != static_cast<DWORD>(json_str.size())) {
            CloseHandle(hFile);
            DeleteFileW(tmp_path.c_str());
            return false;
        }

        FlushFileBuffers(hFile);
        CloseHandle(hFile);

        clear_readonly_attribute(path);
        std::error_code exists_ec;
        if (std::filesystem::exists(path, exists_ec)) {
            if (!ReplaceFileW(
                    path.c_str(),
                    tmp_path.c_str(),
                    nullptr,
                    REPLACEFILE_IGNORE_MERGE_ERRORS | REPLACEFILE_IGNORE_ACL_ERRORS,
                    nullptr,
                    nullptr)) {
                if (!MoveFileExW(tmp_path.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                    DeleteFileW(tmp_path.c_str());
                    return false;
                }
            }
        } else {
            if (!MoveFileExW(tmp_path.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                DeleteFileW(tmp_path.c_str());
                return false;
            }
        }

        return true;
    } catch (...) {
        return false;
    }
}

bool ConfigManager::load_from_file() {
    std::filesystem::path path;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        path = m_config_path;
    }

    auto ensure_defaults_in_memory = [this](bool force_reset) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (force_reset || m_profiles.empty()) {
            m_profiles = get_default_presets();
            m_active_profile_id = "dhcp";
            m_theme_mode = "dark";
        }
    };

    auto handle_corrupt_file = [&](const std::filesystem::path& target_path) -> bool {
        std::filesystem::path broken_path;
        bool backed_up = backup_corrupt_config(target_path, broken_path);
        ensure_defaults_in_memory(true);
        if (backed_up) {
            notify_corrupt_config_unlocked(broken_path);
            save_to_file();
        }
        return false;
    };

    // 1. Open file with transient share-lock retries (5 x 50 ms)
    HANDLE hFile = INVALID_HANDLE_VALUE;
    DWORD open_err = ERROR_SUCCESS;
    for (int attempt = 0; attempt <= 5; ++attempt) {
        hFile = CreateFileW(
            path.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr
        );
        if (hFile != INVALID_HANDLE_VALUE) {
            open_err = ERROR_SUCCESS;
            break;
        }
        open_err = GetLastError();
        if (open_err == ERROR_FILE_NOT_FOUND || open_err == ERROR_PATH_NOT_FOUND) {
            break;
        }
        if (open_err != ERROR_SHARING_VIOLATION && open_err != ERROR_LOCK_VIOLATION) {
            break;
        }
        if (attempt < 5) {
            Sleep(50);
        }
    }

    if (hFile == INVALID_HANDLE_VALUE) {
        if (open_err == ERROR_FILE_NOT_FOUND || open_err == ERROR_PATH_NOT_FOUND) {
            // File does not exist: initialize default presets and persist to disk
            ensure_defaults_in_memory(true);
            save_to_file();
            return true;
        }
        // Transient lock or permission error: keep disk file untouched
        ensure_defaults_in_memory(false);
        return false;
    }

    // 2. Validate file size (cap at 4 MB) and read contents
    LARGE_INTEGER file_size = {};
    if (!GetFileSizeEx(hFile, &file_size) || file_size.QuadPart < 0) {
        CloseHandle(hFile);
        ensure_defaults_in_memory(false);
        return false;
    }

    if (file_size.QuadPart == 0 ||
        static_cast<uint64_t>(file_size.QuadPart) > MAX_CONFIG_FILE_BYTES) {
        CloseHandle(hFile);
        return handle_corrupt_file(path);
    }

    std::string content;
    try {
        content.resize(static_cast<size_t>(file_size.QuadPart));
    } catch (...) {
        CloseHandle(hFile);
        ensure_defaults_in_memory(false);
        return false;
    }

    size_t total_read = 0;
    bool read_ok = true;
    while (total_read < content.size()) {
        DWORD to_read = static_cast<DWORD>(std::min<size_t>(content.size() - total_read, 65536));
        DWORD bytes_read = 0;
        if (!ReadFile(hFile, content.data() + total_read, to_read, &bytes_read, nullptr)) {
            read_ok = false;
            break;
        }
        if (bytes_read == 0) {
            break;
        }
        total_read += bytes_read;
    }
    CloseHandle(hFile);

    if (!read_ok) {
        // I/O read error: do not overwrite existing disk file
        ensure_defaults_in_memory(false);
        return false;
    }
    content.resize(total_read);
    if (content.empty()) {
        return handle_corrupt_file(path);
    }

    // 3. Parse JSON and validate schema/invariants outside m_mutex
    try {
        json root = json::parse(content);
        if (!root.is_object() || !root.contains("profiles") || !root["profiles"].is_array()) {
            return handle_corrupt_file(path);
        }

        bool migrated = false;

        std::string active_id = "dhcp";
        if (root.contains("active_profile_id") && root["active_profile_id"].is_string()) {
            active_id = root["active_profile_id"].get<std::string>();
            if (active_id.empty()) {
                active_id = "dhcp";
                migrated = true;
            }
        } else {
            migrated = true;
        }

        std::string theme = "dark";
        if (root.contains("theme") && root["theme"].is_string()) {
            std::string raw_theme = root["theme"].get<std::string>();
            if (raw_theme == "light" || raw_theme == "dark") {
                theme = raw_theme;
            } else {
                migrated = true;
            }
        } else {
            migrated = true;
        }

        std::vector<DnsProfile> loaded;
        for (const auto& item : root["profiles"]) {
            if (!item.is_object() || !item.contains("id") || !item["id"].is_string()) {
                migrated = true;
                continue;
            }

            DnsProfile p;
            bool was_sanitized = false;
            if (!parse_profile_from_json_object(item, p, &was_sanitized)) {
                migrated = true;
                continue;
            }
            if (was_sanitized) {
                migrated = true;
            }

            // Deduplicate profile IDs (preserve first occurrence)
            bool duplicate_id = false;
            for (const auto& existing : loaded) {
                if (existing.id == p.id) {
                    duplicate_id = true;
                    break;
                }
            }
            if (duplicate_id) {
                migrated = true;
                continue;
            }

            loaded.push_back(std::move(p));
        }

        if (loaded.empty()) {
            return handle_corrupt_file(path);
        }

        const auto defaultPresets = get_default_presets();
        for (auto& p : loaded) {
            const DnsProfile* matched_def = nullptr;
            for (const auto& def : defaultPresets) {
                if (def.id == p.id) {
                    matched_def = &def;
                    break;
                }
            }

            if (matched_def == nullptr) {
                // Non-preset custom profile: enforce custom invariants and sync IPs into ip_pool
                if (p.is_builtin || p.is_dhcp) {
                    p.is_builtin = false;
                    p.is_dhcp = false;
                    migrated = true;
                }
                if (sync_active_ipv4_into_pool(p)) {
                    migrated = true;
                }
            } else {
                const DnsProfile& def = *matched_def;
                if (!p.is_builtin || p.is_dhcp != def.is_dhcp) {
                    p.is_builtin = true;
                    p.is_dhcp = def.is_dhcp;
                    migrated = true;
                }

                if (def.is_dhcp) {
                    if (p != def) {
                        p = def;
                        migrated = true;
                    }
                } else {
                    for (const auto& def_ip : def.ip_pool) {
                        if (!def_ip.empty() &&
                            std::find(p.ip_pool.begin(), p.ip_pool.end(), def_ip) == p.ip_pool.end()) {
                            if (p.ip_pool.size() < MAX_IP_POOL_SIZE) {
                                p.ip_pool.push_back(def_ip);
                                migrated = true;
                            }
                        }
                    }

                    bool primary_in_pool = !p.primary_ipv4.empty() &&
                        std::find(p.ip_pool.begin(), p.ip_pool.end(), p.primary_ipv4) != p.ip_pool.end();

                    if (!primary_in_pool) {
                        if (p.primary_ipv4 != def.primary_ipv4 || p.secondary_ipv4 != def.secondary_ipv4) {
                            p.primary_ipv4 = def.primary_ipv4;
                            p.secondary_ipv4 = def.secondary_ipv4;
                            migrated = true;
                        }
                    } else if (!p.secondary_ipv4.empty()) {
                        bool secondary_in_pool = (p.secondary_ipv4 != p.primary_ipv4) &&
                            std::find(p.ip_pool.begin(), p.ip_pool.end(), p.secondary_ipv4) != p.ip_pool.end();
                        if (!secondary_in_pool) {
                            if (!def.secondary_ipv4.empty() && def.secondary_ipv4 != p.primary_ipv4) {
                                p.secondary_ipv4 = def.secondary_ipv4;
                            } else if (!def.primary_ipv4.empty() && def.primary_ipv4 != p.primary_ipv4) {
                                p.secondary_ipv4 = def.primary_ipv4;
                            } else {
                                p.secondary_ipv4.clear();
                            }
                            migrated = true;
                        }
                    }

                    if (p.primary_ipv6 != def.primary_ipv6 ||
                        p.secondary_ipv6 != def.secondary_ipv6 ||
                        p.doh_template != def.doh_template ||
                        p.name != def.name ||
                        p.description != def.description ||
                        p.is_smart_dns != def.is_smart_dns ||
                        p.website_url != def.website_url ||
                        p.unblocked_summary != def.unblocked_summary) {
                        p.primary_ipv6 = def.primary_ipv6;
                        p.secondary_ipv6 = def.secondary_ipv6;
                        p.doh_template = def.doh_template;
                        p.name = def.name;
                        p.description = def.description;
                        p.is_smart_dns = def.is_smart_dns;
                        p.website_url = def.website_url;
                        p.unblocked_summary = def.unblocked_summary;
                        migrated = true;
                    }
                }
            }
        }

        // Ensure all built-in presets exist in canonical order
        for (size_t i = 0; i < defaultPresets.size(); ++i) {
            const auto& def = defaultPresets[i];
            bool found = false;
            for (const auto& p : loaded) {
                if (p.id == def.id) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                bool inserted = false;
                if (i > 0) {
                    const std::string& prev_id = defaultPresets[i - 1].id;
                    for (auto it = loaded.begin(); it != loaded.end(); ++it) {
                        if (it->id == prev_id) {
                            loaded.insert(it + 1, def);
                            inserted = true;
                            break;
                        }
                    }
                }
                if (!inserted) {
                    if (!loaded.empty() && loaded.back().id == "dhcp") {
                        loaded.insert(loaded.end() - 1, def);
                    } else {
                        loaded.push_back(def);
                    }
                }
                migrated = true;
            }
        }

        // Verify active_profile_id references an existing loaded profile
        bool active_found = false;
        for (const auto& p : loaded) {
            if (p.id == active_id) {
                active_found = true;
                break;
            }
        }
        if (!active_found) {
            active_id = "dhcp";
            migrated = true;
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_profiles = std::move(loaded);
            m_active_profile_id = std::move(active_id);
            m_theme_mode = std::move(theme);
        }

        if (migrated) {
            save_to_file();
        }
        return true;
    } catch (...) {
        return handle_corrupt_file(path);
    }
}
