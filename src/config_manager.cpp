#include "config_manager.h"
#include "dns_manager.h"
#include "encoding_utils.h"
#include "nlohmann/json.hpp"

#include <windows.h>
#include <shlobj.h>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <iostream>
#include <chrono>

using json = nlohmann::json;

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
        {"unblocked_summary", p.unblocked_summary}
    };
}

void from_json(const json& j, DnsProfile& p) {
    auto get_str = [&j](const char* key) -> std::string {
        if (j.contains(key) && j[key].is_string()) {
            return j[key].get<std::string>();
        }
        return "";
    };
    auto get_bool = [&j](const char* key, bool def = false) -> bool {
        if (j.contains(key) && j[key].is_boolean()) {
            return j[key].get<bool>();
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
}

ConfigManager::ConfigManager(const std::filesystem::path& config_path) {
    if (config_path.empty()) {
        m_config_path = resolve_default_config_path();
    } else {
        m_config_path = config_path;
    }

    bool loaded = load_from_file();

    bool needs_reset = false;
    if (loaded) {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& p : m_profiles) {
            if (p.name.find("\xEF\xBF\xBD") != std::string::npos ||
                p.description.find("\xEF\xBF\xBD") != std::string::npos) {
                needs_reset = true;
                break;
            }
        }
    }

    if (!loaded || needs_reset) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_profiles = get_default_presets();
            m_active_profile_id = "dhcp";
        }
        save_to_file();
    } else {
        // Synchronize built-in presets with the latest authoritative IPs and DoH templates
        auto defaultPresets = get_default_presets();
        bool updatedAny = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (auto& p : m_profiles) {
                if (p.is_builtin) {
                    for (const auto& def : defaultPresets) {
                        if (def.id == p.id) {
                            if (p.primary_ipv4 != def.primary_ipv4 ||
                                p.secondary_ipv4 != def.secondary_ipv4 ||
                                p.doh_template != def.doh_template ||
                                p.name != def.name ||
                                p.description != def.description ||
                                p.is_smart_dns != def.is_smart_dns ||
                                p.website_url != def.website_url ||
                                p.unblocked_summary != def.unblocked_summary) {
                                p.primary_ipv4 = def.primary_ipv4;
                                p.secondary_ipv4 = def.secondary_ipv4;
                                p.primary_ipv6 = def.primary_ipv6;
                                p.secondary_ipv6 = def.secondary_ipv6;
                                p.doh_template = def.doh_template;
                                p.name = def.name;
                                p.description = def.description;
                                p.is_smart_dns = def.is_smart_dns;
                                p.website_url = def.website_url;
                                p.unblocked_summary = def.unblocked_summary;
                                updatedAny = true;
                            }
                            break;
                        }
                    }
                }
            }
            for (const auto& def : defaultPresets) {
                bool found = false;
                for (const auto& p : m_profiles) {
                    if (p.id == def.id) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    if (!m_profiles.empty() && m_profiles.back().id == "dhcp") {
                        m_profiles.insert(m_profiles.end() - 1, def);
                    } else {
                        m_profiles.push_back(def);
                    }
                    updatedAny = true;
                }
            }
        }
        if (updatedAny) {
            save_to_file();
        }
    }
}

std::filesystem::path ConfigManager::resolve_default_config_path() {
    PWSTR roamingPath = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roamingPath))) {
        std::filesystem::path p = std::filesystem::path(roamingPath) / L"QuickDNS" / L"profiles.json";
        CoTaskMemFree(roamingPath);
        return p;
    }
    const wchar_t* wAppData = _wgetenv(L"APPDATA");
    if (wAppData) {
        return std::filesystem::path(wAppData) / L"QuickDNS" / L"profiles.json";
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
    if (profile.id.empty()) return false;

    DnsProfile clean_profile = profile;
    if (!clean_profile.doh_template.empty()) {
        std::wstring w_doh = QuickDns::Encoding::utf8_to_wide(clean_profile.doh_template);
        if (!quickdns::DnsManager::is_valid_doh_template(w_doh)) {
            std::wcerr << L"[QuickDNS::ConfigManager] WARNING: Invalid doh_template in add_profile cleared: " << w_doh << L"\n";
            clean_profile.doh_template.clear();
        }
    }

    for (const auto& p : m_profiles) {
        if (p.id == clean_profile.id) {
            return false;
        }
    }

    m_profiles.push_back(clean_profile);
    return true;
}

bool ConfigManager::update_profile(const DnsProfile& profile) {
    std::lock_guard<std::mutex> lock(m_mutex);
    DnsProfile clean_profile = profile;
    if (!clean_profile.doh_template.empty()) {
        std::wstring w_doh = QuickDns::Encoding::utf8_to_wide(clean_profile.doh_template);
        if (!quickdns::DnsManager::is_valid_doh_template(w_doh)) {
            std::wcerr << L"[QuickDNS::ConfigManager] WARNING: Invalid doh_template in update_profile cleared: " << w_doh << L"\n";
            clean_profile.doh_template.clear();
        }
    }
    for (auto& p : m_profiles) {
        if (p.id == clean_profile.id) {
            p = clean_profile;
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
            false,
            true,
            "https://dns.comss.one/",
            "ChatGPT, Copilot, Claude, Spotify, Canva, Notion, Instagram, Twitter/X"
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
            "ChatGPT, Claude, Gemini, Grok, Cursor, DeepL, Spotify, Notion, Canva, Autodesk"
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
            "ChatGPT, Claude, Gemini, Grok, Deezer, Spotify, Canva, Notion, JetBrains"
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
            "ChatGPT, Claude, Gemini, Copilot, Grok, Spotify, Netflix US, Notion"
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
            "ChatGPT, YouTube, Discord, Spotify, Twitter/X, Instagram, Reddit, TikTok"
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
            "Discord, YouTube, ChatGPT, Claude, Instagram, Twitter/X, RuTracker"
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
            "ChatGPT, Gemini, Spotify, Discord, Instagram, Twitter/X, Notion, Canva"
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
            "ChatGPT, Claude, Discord, YouTube, Spotify, Twitter/X, Instagram, Notion"
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
            "ChatGPT, Discord, YouTube, Steam, Spotify, Instagram, Twitter/X"
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
            "Docker Hub, Android Dev, Oracle, NVIDIA, Gradle, Google Services, AMD"
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
            "ChatGPT, Claude, стриминги, защита от трекеров и фишинга"
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
            "Netflix, Disney+, YouTube, ChatGPT, Spotify, TikTok (требуется привязка IP)"
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
            ""
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
            ""
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
            ""
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
            ""
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
            ""
        }
    };
}

bool ConfigManager::save_to_file() {
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
            std::filesystem::create_directories(path.parent_path());
        }

        json root;
        root["active_profile_id"] = active_id;
        root["theme"] = theme;
        root["profiles"] = json::array();
        for (const auto& p : profiles_copy) {
            root["profiles"].push_back(p);
        }

        std::string json_str = root.dump(2);

        std::filesystem::path tmp_path = path;
        tmp_path += L".tmp";

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

        if (!write_ok || bytes_written != json_str.size()) {
            CloseHandle(hFile);
            DeleteFileW(tmp_path.c_str());
            return false;
        }

        FlushFileBuffers(hFile);
        CloseHandle(hFile);

        if (std::filesystem::exists(path)) {
            if (!ReplaceFileW(path.c_str(), tmp_path.c_str(), nullptr, REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr)) {
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
    std::lock_guard<std::mutex> lock(m_mutex);
    try {
        if (!std::filesystem::exists(m_config_path)) {
            return false;
        }

        std::ifstream file(m_config_path, std::ios::in | std::ios::binary);
        if (!file.is_open()) return false;

        std::string content((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());
        file.close();

        json root;
        try {
            root = json::parse(content);
        } catch (const std::exception&) {
            auto now = std::chrono::system_clock::now();
            auto ts = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
            std::filesystem::path broken_path = m_config_path;
            broken_path += L".broken-" + std::to_wstring(ts);

            std::error_code ec;
            std::filesystem::rename(m_config_path, broken_path, ec);

            std::wstring warn = L"Обнаружена ошибка в файле конфигурации profiles.json. Файл переименован в " +
                                broken_path.filename().wstring() + L", созданы настройки по умолчанию.";
            bool is_ci_or_test = (std::getenv("CI") != nullptr ||
                                  std::getenv("GITHUB_ACTIONS") != nullptr ||
                                  std::getenv("TESTING") != nullptr);
            if (!is_ci_or_test && GetConsoleWindow() == nullptr) {
                MessageBoxW(nullptr, warn.c_str(), L"QuickDNS - Ошибка конфигурации", MB_OK | MB_ICONWARNING);
            }
            return false;
        }

        if (root.contains("active_profile_id") && root["active_profile_id"].is_string()) {
            m_active_profile_id = root["active_profile_id"].get<std::string>();
        }

        if (root.contains("theme") && root["theme"].is_string()) {
            std::string t = root["theme"].get<std::string>();
            m_theme_mode = (t == "light") ? "light" : "dark";
        }

        if (root.contains("profiles") && root["profiles"].is_array()) {
            std::vector<DnsProfile> loaded;
            for (const auto& item : root["profiles"]) {
                if (item.is_object() && item.contains("id") && item["id"].is_string()) {
                    DnsProfile p = item.get<DnsProfile>();
                    if (!p.id.empty()) {
                        if (!p.doh_template.empty()) {
                            std::wstring w_doh = QuickDns::Encoding::utf8_to_wide(p.doh_template);
                            if (!quickdns::DnsManager::is_valid_doh_template(w_doh)) {
                                p.doh_template.clear();
                            }
                        }
                        loaded.push_back(p);
                    }
                }
            }
            if (!loaded.empty()) {
                m_profiles = std::move(loaded);
                return true;
            }
        }

        return false;
    } catch (...) {
        return false;
    }
}
