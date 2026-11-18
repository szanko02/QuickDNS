#include "dns_manager.h"
#include "winsock_init.h"
#include "encoding_utils.h"

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <fstream>
#include <mutex>
#include <objbase.h>

// Dynamic function pointer signatures and constants for Windows 10 1809+ / Windows 11 DNS settings API
#ifndef DNS_INTERFACE_SETTINGS_VERSION1
#define DNS_INTERFACE_SETTINGS_VERSION1 1

typedef struct _DNS_INTERFACE_SETTINGS {
    ULONG Version;
    ULONG64 Flags;
    PWSTR Domain;
    PWSTR NameServer;
    PWSTR SearchList;
    ULONG RegistrationEnabled;
    ULONG RegisterAdapterName;
    ULONG EnableLLMNR;
    ULONG QueryAdapterName;
    PWSTR ProfileNameServer;
} DNS_INTERFACE_SETTINGS;
#endif

#ifndef DNS_INTERFACE_SETTINGS_VERSION3
#define DNS_INTERFACE_SETTINGS_VERSION3 3
#endif
#ifndef DNS_SERVER_PROPERTY_VERSION1
#define DNS_SERVER_PROPERTY_VERSION1 1
#endif

#ifndef DNS_SETTING_IPV6
#define DNS_SETTING_IPV6                 0x0001
#endif
#ifndef DNS_SETTING_NAMESERVER
#define DNS_SETTING_NAMESERVER           0x0002
#endif
#ifndef DNS_SETTING_SEARCHLIST
#define DNS_SETTING_SEARCHLIST           0x0004
#endif
#ifndef DNS_SETTING_REGISTRATION_ENABLED
#define DNS_SETTING_REGISTRATION_ENABLED 0x0008
#endif
#ifndef DNS_SETTING_DOMAIN
#define DNS_SETTING_DOMAIN               0x0020
#endif
#ifndef DNS_SETTING_PROFILE_NAMESERVER
#define DNS_SETTING_PROFILE_NAMESERVER   0x0200
#endif
#ifndef DNS_SETTING_DOH
#define DNS_SETTING_DOH                  0x1000
#endif
#ifndef DNS_SETTING_DOH_PROFILE
#define DNS_SETTING_DOH_PROFILE          0x2000
#endif

#ifndef DNS_DOH_SERVER_SETTINGS_ENABLE_AUTO
#define DNS_DOH_SERVER_SETTINGS_ENABLE_AUTO     0x0001
#endif
#ifndef DNS_DOH_SERVER_SETTINGS_ENABLE
#define DNS_DOH_SERVER_SETTINGS_ENABLE          0x0002
#endif
#ifndef DNS_DOH_SERVER_SETTINGS_FALLBACK_TO_UDP
#define DNS_DOH_SERVER_SETTINGS_FALLBACK_TO_UDP 0x0004
#endif
#ifndef DNS_DOH_AUTO_UPGRADE_SERVER
#define DNS_DOH_AUTO_UPGRADE_SERVER             0x0008
#endif

typedef DWORD (WINAPI *PFN_SetInterfaceDnsSettings)(GUID InterfaceGuid, const DNS_INTERFACE_SETTINGS *Settings);
typedef DWORD (WINAPI *PFN_GetInterfaceDnsSettings)(GUID InterfaceGuid, DNS_INTERFACE_SETTINGS *Settings);
typedef VOID (WINAPI *PFN_FreeInterfaceDnsSettings)(DNS_INTERFACE_SETTINGS *Settings);
typedef BOOL (WINAPI *PFN_DnsFlushResolverCache)(VOID);

namespace {

using quickdns::ensure_winsock;
using QuickDns::Encoding::utf8_to_wide;

std::mutex g_log_mutex;
std::mutex g_state_mutex;

// ABI-compatible Windows 11 DNS_INTERFACE_SETTINGS3 layout (avoids SDK version header conflicts)
struct QuickDnsDohServerSettings {
    PWSTR Template;
    ULONG64 Flags;
};

enum QuickDnsServerPropertyType : int {
    QuickDnsServerInvalidProperty = 0,
    QuickDnsServerDohProperty = 1
};

union QuickDnsServerPropertyTypes {
    QuickDnsDohServerSettings* DohSettings;
};

struct QuickDnsServerProperty {
    ULONG Version;
    ULONG ServerIndex;
    QuickDnsServerPropertyType Type;
    QuickDnsServerPropertyTypes Property;
};

struct QuickDnsInterfaceSettings3 {
    ULONG Version;
    ULONG64 Flags;
    PWSTR Domain;
    PWSTR NameServer;
    PWSTR SearchList;
    ULONG RegistrationEnabled;
    ULONG RegisterAdapterName;
    ULONG EnableLLMNR;
    ULONG QueryAdapterName;
    PWSTR ProfileNameServer;
    ULONG DisableUnconstrainedQueries;
    PWSTR SupplementalSearchList;
    ULONG cServerProperties;
    QuickDnsServerProperty* ServerProperties;
    ULONG cProfileServerProperties;
    QuickDnsServerProperty* ProfileServerProperties;
};

constexpr ULONGLONG QUICKDNS_INTERFACE_DOH_FLAGS =
    DNS_DOH_SERVER_SETTINGS_ENABLE | DNS_DOH_SERVER_SETTINGS_FALLBACK_TO_UDP; // 0x6

std::wstring get_env_var_wide(const wchar_t* name) {
    if (!name || *name == L'\0') {
        return L"";
    }
    DWORD req_len = GetEnvironmentVariableW(name, nullptr, 0);
    if (req_len <= 1) {
        return L"";
    }
    std::wstring buf(req_len, L'\0');
    DWORD got_len = GetEnvironmentVariableW(name, buf.data(), req_len);
    if (got_len == 0 || got_len >= req_len) {
        return L"";
    }
    buf.resize(got_len);
    return buf;
}

void write_log_entry(const wchar_t* level, const std::wstring& msg) {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t time_buf[64] = {};
    std::swprintf(time_buf, sizeof(time_buf) / sizeof(wchar_t),
                  L"[%04u-%02u-%02u %02u:%02u:%02u.%03u]",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    std::wstring line = std::wstring(time_buf) + L" [" + level + L"] " + msg + L"\r\n";
    OutputDebugStringW(line.c_str());

    std::lock_guard<std::mutex> lock(g_log_mutex);
    std::wstring local_app_data = get_env_var_wide(L"LOCALAPPDATA");
    if (local_app_data.empty()) {
        return;
    }

    std::wstring dir = local_app_data + L"\\QuickDNS";
    CreateDirectoryW(dir.c_str(), nullptr);

    std::wstring log_path = dir + L"\\quickdns.log";
    std::wstring old_path = dir + L"\\quickdns.log.old";

    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (GetFileAttributesExW(log_path.c_str(), GetFileExInfoStandard, &fad)) {
        LARGE_INTEGER size{};
        size.LowPart = fad.nFileSizeLow;
        size.HighPart = fad.nFileSizeHigh;
        if (size.QuadPart >= 1024 * 1024) { // 1 MB limit
            MoveFileExW(log_path.c_str(), old_path.c_str(), MOVEFILE_REPLACE_EXISTING);
        }
    }

    std::string utf8_line = QuickDns::Encoding::wide_to_utf8(line);
    if (!utf8_line.empty()) {
        std::ofstream out(log_path.c_str(), std::ios::binary | std::ios::app);
        if (out.is_open()) {
            out.write(utf8_line.data(), static_cast<std::streamsize>(utf8_line.size()));
            out.flush();
        }
    }
}

// Internal structure to assist in prioritizing network adapters during sorting
struct PrioritizedAdapter {
    quickdns::NetworkAdapterInfo info;
    bool is_ethernet_or_wifi{false};
    bool has_gateway{false};
    bool is_virtual{false};
};

std::vector<std::wstring> split_string_by_delimiters(const std::wstring& str, const std::wstring& delims) {
    std::vector<std::wstring> tokens;
    size_t start = 0;
    while (start < str.size()) {
        size_t end = str.find_first_of(delims, start);
        if (end == std::wstring::npos) {
            end = str.size();
        }
        if (end > start) {
            std::wstring token = str.substr(start, end - start);
            size_t first = token.find_first_not_of(L" \t\r\n");
            size_t last = token.find_last_not_of(L" \t\r\n");
            if (first != std::wstring::npos && last != std::wstring::npos) {
                tokens.push_back(token.substr(first, last - first + 1));
            }
        }
        start = end + 1;
    }
    return tokens;
}

bool has_non_whitespace(const wchar_t* s) {
    if (!s) return false;
    for (const wchar_t* p = s; *p != L'\0'; ++p) {
        if (!iswspace(*p) && *p != L',' && *p != L';') {
            return true;
        }
    }
    return false;
}

bool has_non_whitespace(const std::wstring& s) {
    for (wchar_t ch : s) {
        if (!iswspace(ch) && ch != L',' && ch != L';') {
            return true;
        }
    }
    return false;
}

std::wstring get_system32_path() {
    wchar_t sys_dir[MAX_PATH] = {};
    UINT len = GetSystemDirectoryW(sys_dir, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        return std::wstring(sys_dir, len);
    }
    wchar_t win_dir[MAX_PATH] = {};
    UINT wlen = GetWindowsDirectoryW(win_dir, MAX_PATH);
    if (wlen > 0 && wlen < MAX_PATH) {
        std::wstring base(win_dir, wlen);
        if (!base.empty() && (base.back() == L'\\' || base.back() == L'/')) {
            base.pop_back();
        }
        return base + L"\\System32";
    }
    return L"C:\\Windows\\System32";
}

bool is_windows_11_or_greater() {
    static const bool is_win11 = []() -> bool {
        struct RtlOsVersionInfoExW {
            ULONG dwOSVersionInfoSize;
            ULONG dwMajorVersion;
            ULONG dwMinorVersion;
            ULONG dwBuildNumber;
            ULONG dwPlatformId;
            WCHAR szCSDVersion[128];
            USHORT wServicePackMajor;
            USHORT wServicePackMinor;
            USHORT wSuiteMask;
            UCHAR wProductType;
            UCHAR wReserved;
        };
        using PFN_RtlGetVersion = LONG(WINAPI*)(RtlOsVersionInfoExW*);
        HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
        if (hNtdll) {
            auto pfn = reinterpret_cast<PFN_RtlGetVersion>(
                reinterpret_cast<void*>(GetProcAddress(hNtdll, "RtlGetVersion")));
            if (pfn) {
                RtlOsVersionInfoExW osvi{};
                osvi.dwOSVersionInfoSize = sizeof(osvi);
                if (pfn(&osvi) == 0) {
                    return (osvi.dwMajorVersion > 10) ||
                           (osvi.dwMajorVersion == 10 && osvi.dwBuildNumber >= 22000);
                }
            }
        }
        return false;
    }();
    return is_win11;
}

bool is_safe_adapter_name_for_netsh(const std::wstring& name) {
    if (name.empty()) {
        return false;
    }
    for (wchar_t ch : name) {
        if (ch == L'"' || ch == L'\\' || ch < 32) {
            return false;
        }
    }
    return true;
}

std::wstring canonicalize_ipv4(const std::wstring& ip) {
    if (ip.empty()) {
        return L"";
    }
    ensure_winsock();
    IN_ADDR addr{};
    if (InetPtonW(AF_INET, ip.c_str(), &addr) != 1) {
        return L"";
    }
    wchar_t buf[INET_ADDRSTRLEN] = {};
    if (!InetNtopW(AF_INET, &addr, buf, INET_ADDRSTRLEN)) {
        return L"";
    }
    return std::wstring(buf);
}

std::wstring canonicalize_ipv6(const std::wstring& ip) {
    if (ip.empty()) {
        return L"";
    }
    ensure_winsock();
    IN6_ADDR addr{};
    if (InetPtonW(AF_INET6, ip.c_str(), &addr) != 1) {
        return L"";
    }
    wchar_t buf[INET6_ADDRSTRLEN] = {};
    if (!InetNtopW(AF_INET6, &addr, buf, INET6_ADDRSTRLEN)) {
        return L"";
    }
    std::wstring out(buf);
    for (wchar_t& ch : out) {
        ch = static_cast<wchar_t>(towlower(ch));
    }
    return out;
}

std::wstring canonicalize_ip(const std::wstring& ip) {
    std::wstring v4 = canonicalize_ipv4(ip);
    if (!v4.empty()) {
        return v4;
    }
    return canonicalize_ipv6(ip);
}

std::wstring canonicalize_adapter_guid(const std::wstring& adapter_id, const GUID& fallback_guid = GUID_NULL) {
    GUID parsed = GUID_NULL;
    if (!adapter_id.empty()) {
        std::wstring candidate = adapter_id;
        if (candidate.front() != L'{') {
            candidate.insert(candidate.begin(), L'{');
        }
        if (candidate.back() != L'}') {
            candidate.push_back(L'}');
        }
        parsed = quickdns::DnsManager::string_to_guid(candidate);
    }
    if (parsed == GUID_NULL && fallback_guid != GUID_NULL) {
        parsed = fallback_guid;
    }
    if (parsed == GUID_NULL) {
        return L"";
    }
    std::wstring canonical = quickdns::DnsManager::guid_to_string(parsed);
    for (wchar_t& ch : canonical) {
        ch = static_cast<wchar_t>(towlower(ch));
    }
    return canonical;
}

std::wstring join_servers_comma(const std::vector<std::wstring>& servers) {
    std::wstring out;
    for (size_t i = 0; i < servers.size(); ++i) {
        if (i > 0) {
            out.push_back(L',');
        }
        out += servers[i];
    }
    return out;
}

DWORD apply_interface_dns_settings(PFN_SetInterfaceDnsSettings pfnSet,
                                   const GUID& guid,
                                   bool is_ipv6,
                                   const std::vector<std::wstring>& servers,
                                   const std::wstring& doh_template)
{
    if (!pfnSet || guid == GUID_NULL) {
        return ERROR_INVALID_PARAMETER;
    }

    std::wstring joined = join_servers_comma(servers);

    // 1. Try Windows 11 DNS_INTERFACE_SETTINGS3 (atomically configures NameServer + ProfileNameServer + DoH + DoH Profile)
    QuickDnsDohServerSettings doh_cfg{};
    std::vector<QuickDnsServerProperty> props;
    if (!servers.empty() && !doh_template.empty()) {
        doh_cfg.Template = const_cast<PWSTR>(doh_template.c_str());
        doh_cfg.Flags = QUICKDNS_INTERFACE_DOH_FLAGS;
        props.resize(servers.size());
        for (size_t i = 0; i < servers.size(); ++i) {
            props[i].Version = DNS_SERVER_PROPERTY_VERSION1;
            props[i].ServerIndex = static_cast<ULONG>(i);
            props[i].Type = QuickDnsServerDohProperty;
            props[i].Property.DohSettings = &doh_cfg;
        }
    }

    QuickDnsInterfaceSettings3 settings3{};
    settings3.Version = DNS_INTERFACE_SETTINGS_VERSION3;
    settings3.Flags = (is_ipv6 ? DNS_SETTING_IPV6 : 0ULL) |
                      DNS_SETTING_NAMESERVER |
                      DNS_SETTING_PROFILE_NAMESERVER |
                      DNS_SETTING_DOH |
                      DNS_SETTING_DOH_PROFILE;
    settings3.NameServer = const_cast<PWSTR>(joined.c_str());
    settings3.ProfileNameServer = const_cast<PWSTR>(joined.c_str());
    if (!props.empty()) {
        settings3.cServerProperties = static_cast<ULONG>(props.size());
        settings3.ServerProperties = props.data();
        settings3.cProfileServerProperties = static_cast<ULONG>(props.size());
        settings3.ProfileServerProperties = props.data();
    }

    DWORD status = pfnSet(guid, reinterpret_cast<const DNS_INTERFACE_SETTINGS*>(&settings3));
    if (status == ERROR_SUCCESS || status == ERROR_NOT_FOUND || status == ERROR_ACCESS_DENIED) {
        return status;
    }

    // 2. Fallback to Windows 10 1809+ DNS_INTERFACE_SETTINGS_VERSION1
    DNS_INTERFACE_SETTINGS settings1{};
    settings1.Version = DNS_INTERFACE_SETTINGS_VERSION1;
    settings1.Flags = (is_ipv6 ? DNS_SETTING_IPV6 : 0ULL) |
                      DNS_SETTING_NAMESERVER |
                      DNS_SETTING_PROFILE_NAMESERVER;
    settings1.NameServer = const_cast<PWSTR>(joined.c_str());
    settings1.ProfileNameServer = const_cast<PWSTR>(joined.c_str());
    return pfnSet(guid, &settings1);
}

bool set_reg_dns_values(const std::wstring& subkey, const std::wstring& servers_joined) {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, subkey.c_str(), 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &hKey) != ERROR_SUCCESS) {
        return false;
    }

    const DWORD byte_len = static_cast<DWORD>((servers_joined.length() + 1) * sizeof(wchar_t));
    LSTATUS ret = RegSetValueExW(hKey, L"NameServer", 0, REG_SZ,
                                 reinterpret_cast<const BYTE*>(servers_joined.c_str()),
                                 byte_len);

    // Keep ProfileNameServer synchronized if present (e.g. Wi-Fi profile DNS override)
    DWORD prof_type = 0;
    DWORD prof_len = 0;
    if (RegQueryValueExW(hKey, L"ProfileNameServer", nullptr, &prof_type, nullptr, &prof_len) == ERROR_SUCCESS) {
        RegSetValueExW(hKey, L"ProfileNameServer", 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(servers_joined.c_str()),
                       byte_len);
    }

    RegCloseKey(hKey);
    return (ret == ERROR_SUCCESS);
}

bool clear_reg_dns_values(const std::wstring& subkey, bool& out_unbound) {
    out_unbound = false;
    HKEY hKey = nullptr;
    LSTATUS open_status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, subkey.c_str(), 0, KEY_SET_VALUE, &hKey);
    if (open_status == ERROR_FILE_NOT_FOUND || open_status == ERROR_PATH_NOT_FOUND) {
        out_unbound = true;
        return false;
    }
    if (open_status != ERROR_SUCCESS) {
        return false;
    }

    const wchar_t emptyStr = L'\0';
    LSTATUS ret1 = RegSetValueExW(hKey, L"NameServer", 0, REG_SZ,
                                  reinterpret_cast<const BYTE*>(&emptyStr), sizeof(wchar_t));
    LSTATUS ret2 = RegSetValueExW(hKey, L"ProfileNameServer", 0, REG_SZ,
                                  reinterpret_cast<const BYTE*>(&emptyStr), sizeof(wchar_t));
    RegCloseKey(hKey);
    return (ret1 == ERROR_SUCCESS && ret2 == ERROR_SUCCESS);
}

constexpr const wchar_t* QUICKDNS_REG_PATH = L"SOFTWARE\\QuickDNS";
constexpr const wchar_t* MANAGED_DOH_REG_PATH = L"SOFTWARE\\QuickDNS\\ManagedDohServers";

bool is_doh_server_managed(const std::wstring& server_ip) {
    std::wstring canon = canonicalize_ip(server_ip);
    if (canon.empty()) {
        return false;
    }
    std::wstring path = std::wstring(MANAGED_DOH_REG_PATH) + L"\\" + canon;
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        RegCloseKey(hKey);
        return true;
    }
    return false;
}

void mark_doh_server_managed(const std::wstring& server_ip) {
    std::wstring canon = canonicalize_ip(server_ip);
    if (canon.empty()) {
        return;
    }
    std::wstring path = std::wstring(MANAGED_DOH_REG_PATH) + L"\\" + canon;
    HKEY hKey = nullptr;
    DWORD disp = 0;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE, nullptr, &hKey, &disp) == ERROR_SUCCESS) {
        RegCloseKey(hKey);
    }
}

void unmark_doh_server_managed(const std::wstring& server_ip) {
    std::wstring canon = canonicalize_ip(server_ip);
    if (!canon.empty()) {
        std::wstring path = std::wstring(MANAGED_DOH_REG_PATH) + L"\\" + canon;
        RegDeleteKeyW(HKEY_LOCAL_MACHINE, path.c_str());
    }
}

std::vector<std::wstring> get_managed_doh_servers() {
    std::vector<std::wstring> servers;
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, MANAGED_DOH_REG_PATH, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        wchar_t subkeyName[256] = {0};
        DWORD index = 0;
        DWORD nameLen = 256;
        while (RegEnumKeyExW(hKey, index, subkeyName, &nameLen, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
            std::wstring canon = canonicalize_ip(std::wstring(subkeyName, nameLen));
            if (!canon.empty() && std::find(servers.begin(), servers.end(), canon) == servers.end()) {
                servers.push_back(std::move(canon));
            }
            index++;
            nameLen = 256;
        }
        RegCloseKey(hKey);
    }
    return servers;
}

std::optional<std::wstring> read_reg_string(HKEY root, const std::wstring& path, const wchar_t* name) {
    DWORD bytes = 0;
    LONG status = RegGetValueW(root, path.c_str(), name, RRF_RT_REG_SZ | RRF_RT_REG_MULTI_SZ, nullptr, nullptr, &bytes);
    if (status != ERROR_SUCCESS || bytes == 0) {
        return std::nullopt;
    }

    const size_t wchar_cap = (static_cast<size_t>(bytes) + sizeof(wchar_t) - 1) / sizeof(wchar_t);
    std::wstring result(wchar_cap, L'\0');
    DWORD buf_bytes = static_cast<DWORD>(wchar_cap * sizeof(wchar_t));
    status = RegGetValueW(root, path.c_str(), name, RRF_RT_REG_SZ | RRF_RT_REG_MULTI_SZ, nullptr, result.data(), &buf_bytes);
    if (status != ERROR_SUCCESS) {
        return std::nullopt;
    }

    size_t len = buf_bytes / sizeof(wchar_t);
    while (len > 0 && result[len - 1] == L'\0') {
        len--;
    }
    result.resize(len);

    // Replace embedded REG_MULTI_SZ NUL separators with commas
    for (wchar_t& ch : result) {
        if (ch == L'\0') {
            ch = L',';
        }
    }
    return result;
}

void backup_and_set_enable_autodoh() {
    HKEY hAppKey = nullptr;
    DWORD disp = 0;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, QUICKDNS_REG_PATH, 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE, nullptr, &hAppKey, &disp) == ERROR_SUCCESS) {
        DWORD present = 0;
        DWORD pType = 0;
        DWORD pLen = sizeof(DWORD);
        if (RegQueryValueExW(hAppKey, L"OriginalEnableAutoDohPresent", nullptr, &pType,
                             reinterpret_cast<BYTE*>(&present), &pLen) != ERROR_SUCCESS || pType != REG_DWORD) {
            HKEY hDnsParam = nullptr;
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\Dnscache\\Parameters",
                              0, KEY_READ, &hDnsParam) == ERROR_SUCCESS) {
                DWORD valType = 0;
                DWORD valBytes = 0;
                LSTATUS qStatus = RegQueryValueExW(hDnsParam, L"EnableAutoDoh", nullptr, &valType, nullptr, &valBytes);
                if (qStatus == ERROR_FILE_NOT_FOUND) {
                    DWORD isPresent = 0;
                    RegSetValueExW(hAppKey, L"OriginalEnableAutoDohPresent", 0, REG_DWORD,
                                   reinterpret_cast<const BYTE*>(&isPresent), sizeof(DWORD));
                } else if (qStatus == ERROR_SUCCESS && valBytes <= 1024) {
                    std::vector<BYTE> rawBuf(valBytes > 0 ? valBytes : 1);
                    DWORD readBytes = valBytes;
                    if (RegQueryValueExW(hDnsParam, L"EnableAutoDoh", nullptr, &valType,
                                         valBytes > 0 ? rawBuf.data() : nullptr, &readBytes) == ERROR_SUCCESS) {
                        LSTATUS s1 = RegSetValueExW(hAppKey, L"OriginalEnableAutoDohType", 0, REG_DWORD,
                                                    reinterpret_cast<const BYTE*>(&valType), sizeof(DWORD));
                        LSTATUS s2 = RegSetValueExW(hAppKey, L"OriginalEnableAutoDoh", 0, valType,
                                                    readBytes > 0 ? rawBuf.data() : nullptr, readBytes);
                        if (s1 == ERROR_SUCCESS && s2 == ERROR_SUCCESS) {
                            DWORD isPresent = 1;
                            RegSetValueExW(hAppKey, L"OriginalEnableAutoDohPresent", 0, REG_DWORD,
                                           reinterpret_cast<const BYTE*>(&isPresent), sizeof(DWORD));
                        }
                    }
                }
                RegCloseKey(hDnsParam);
            }
        }
        RegCloseKey(hAppKey);
    }

    HKEY hDnsParam = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\Dnscache\\Parameters",
                      0, KEY_READ | KEY_WRITE, &hDnsParam) == ERROR_SUCCESS) {
        DWORD enableAutoDoh = 2;
        RegSetValueExW(hDnsParam, L"EnableAutoDoh", 0, REG_DWORD,
                       reinterpret_cast<const BYTE*>(&enableAutoDoh), sizeof(DWORD));
        RegCloseKey(hDnsParam);
    }
}

void restore_enable_autodoh() {
    HKEY hAppKey = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, QUICKDNS_REG_PATH, 0, KEY_READ | KEY_WRITE, &hAppKey) == ERROR_SUCCESS) {
        DWORD present = 0;
        DWORD pType = 0;
        DWORD pLen = sizeof(DWORD);
        if (RegQueryValueExW(hAppKey, L"OriginalEnableAutoDohPresent", nullptr, &pType,
                             reinterpret_cast<BYTE*>(&present), &pLen) == ERROR_SUCCESS && pType == REG_DWORD) {
            bool restore_ok = false;
            HKEY hDnsParam = nullptr;
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\Dnscache\\Parameters",
                              0, KEY_READ | KEY_WRITE, &hDnsParam) == ERROR_SUCCESS) {
                if (present == 1) {
                    DWORD origType = 0;
                    DWORD origBytes = 0;
                    if (RegQueryValueExW(hAppKey, L"OriginalEnableAutoDoh", nullptr, &origType, nullptr, &origBytes) == ERROR_SUCCESS &&
                        origBytes <= 1024) {
                        std::vector<BYTE> rawBuf(origBytes > 0 ? origBytes : 1);
                        DWORD readBytes = origBytes;
                        if (RegQueryValueExW(hAppKey, L"OriginalEnableAutoDoh", nullptr, &origType,
                                             origBytes > 0 ? rawBuf.data() : nullptr, &readBytes) == ERROR_SUCCESS) {
                            DWORD savedType = origType;
                            DWORD stType = 0;
                            DWORD stLen = sizeof(DWORD);
                            if (RegQueryValueExW(hAppKey, L"OriginalEnableAutoDohType", nullptr, &stType,
                                                 reinterpret_cast<BYTE*>(&savedType), &stLen) != ERROR_SUCCESS || stType != REG_DWORD) {
                                savedType = origType;
                            }
                            if (RegSetValueExW(hDnsParam, L"EnableAutoDoh", 0, savedType,
                                               readBytes > 0 ? rawBuf.data() : nullptr, readBytes) == ERROR_SUCCESS) {
                                restore_ok = true;
                            }
                        }
                    }
                } else {
                    LSTATUS delStatus = RegDeleteValueW(hDnsParam, L"EnableAutoDoh");
                    if (delStatus == ERROR_SUCCESS || delStatus == ERROR_FILE_NOT_FOUND) {
                        restore_ok = true;
                    }
                }
                RegCloseKey(hDnsParam);
            }

            if (restore_ok) {
                RegDeleteValueW(hAppKey, L"OriginalEnableAutoDohPresent");
                RegDeleteValueW(hAppKey, L"OriginalEnableAutoDoh");
                RegDeleteValueW(hAppKey, L"OriginalEnableAutoDohType");
            }
        }
        RegCloseKey(hAppKey);
    }
}

void append_parsed_ips(const std::wstring& raw, bool want_v6, std::vector<std::wstring>& dest) {
    auto tokens = split_string_by_delimiters(raw, L", ;\t\r\n");
    for (const auto& token : tokens) {
        std::wstring canon = want_v6 ? canonicalize_ipv6(token) : canonicalize_ipv4(token);
        if (canon.empty()) {
            continue;
        }
        if (want_v6 && canon.rfind(L"fec0:", 0) == 0) {
            continue;
        }
        if (std::find(dest.begin(), dest.end(), canon) == dest.end()) {
            dest.push_back(std::move(canon));
        }
    }
}

} // anonymous namespace

namespace quickdns {

DnsManager::DnsManager() {
    ensure_winsock();

    m_hIpHlp = LoadLibraryExW(L"iphlpapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (m_hIpHlp) {
        m_pfnSetInterfaceDnsSettings = reinterpret_cast<void*>(
            GetProcAddress(m_hIpHlp, "SetInterfaceDnsSettings"));
        m_pfnGetInterfaceDnsSettings = reinterpret_cast<void*>(
            GetProcAddress(m_hIpHlp, "GetInterfaceDnsSettings"));
        m_pfnFreeInterfaceDnsSettings = reinterpret_cast<void*>(
            GetProcAddress(m_hIpHlp, "FreeInterfaceDnsSettings"));
    }

    m_hDnsApi = LoadLibraryExW(L"dnsapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (m_hDnsApi) {
        m_pfnDnsFlushResolverCache = reinterpret_cast<void*>(
            GetProcAddress(m_hDnsApi, "DnsFlushResolverCache"));
    }
}

DnsManager::~DnsManager() {
    std::lock_guard<std::mutex> op_lock(op_mutex_);
    if (m_hIpHlp) {
        FreeLibrary(m_hIpHlp);
        m_hIpHlp = nullptr;
    }
    if (m_hDnsApi) {
        FreeLibrary(m_hDnsApi);
        m_hDnsApi = nullptr;
    }
}

DnsManager::DnsManager(DnsManager&& other) noexcept {
    std::scoped_lock lock(other.op_mutex_, other.error_mutex_);
    last_error_ = std::move(other.last_error_);
    m_hIpHlp = other.m_hIpHlp;
    m_hDnsApi = other.m_hDnsApi;
    m_pfnSetInterfaceDnsSettings = other.m_pfnSetInterfaceDnsSettings;
    m_pfnGetInterfaceDnsSettings = other.m_pfnGetInterfaceDnsSettings;
    m_pfnFreeInterfaceDnsSettings = other.m_pfnFreeInterfaceDnsSettings;
    m_pfnDnsFlushResolverCache = other.m_pfnDnsFlushResolverCache;

    other.m_hIpHlp = nullptr;
    other.m_hDnsApi = nullptr;
    other.m_pfnSetInterfaceDnsSettings = nullptr;
    other.m_pfnGetInterfaceDnsSettings = nullptr;
    other.m_pfnFreeInterfaceDnsSettings = nullptr;
    other.m_pfnDnsFlushResolverCache = nullptr;
}

DnsManager& DnsManager::operator=(DnsManager&& other) noexcept {
    if (this != &other) {
        std::scoped_lock lock(op_mutex_, other.op_mutex_, error_mutex_, other.error_mutex_);
        if (m_hIpHlp) FreeLibrary(m_hIpHlp);
        if (m_hDnsApi) FreeLibrary(m_hDnsApi);

        last_error_ = std::move(other.last_error_);
        m_hIpHlp = other.m_hIpHlp;
        m_hDnsApi = other.m_hDnsApi;
        m_pfnSetInterfaceDnsSettings = other.m_pfnSetInterfaceDnsSettings;
        m_pfnGetInterfaceDnsSettings = other.m_pfnGetInterfaceDnsSettings;
        m_pfnFreeInterfaceDnsSettings = other.m_pfnFreeInterfaceDnsSettings;
        m_pfnDnsFlushResolverCache = other.m_pfnDnsFlushResolverCache;

        other.m_hIpHlp = nullptr;
        other.m_hDnsApi = nullptr;
        other.m_pfnSetInterfaceDnsSettings = nullptr;
        other.m_pfnGetInterfaceDnsSettings = nullptr;
        other.m_pfnFreeInterfaceDnsSettings = nullptr;
        other.m_pfnDnsFlushResolverCache = nullptr;
    }
    return *this;
}

void DnsManager::log_info(const std::wstring& msg) const {
    write_log_entry(L"INFO", msg);
}

void DnsManager::log_error(const std::wstring& msg) const {
    write_log_entry(L"ERROR", msg);
}

void DnsManager::set_last_error(const std::wstring& msg) const {
    {
        std::lock_guard<std::mutex> lock(error_mutex_);
        last_error_ = msg;
    }
    log_error(msg);
}

std::wstring DnsManager::get_last_error() const {
    std::lock_guard<std::mutex> lock(error_mutex_);
    return last_error_;
}

bool DnsManager::is_elevated() {
    BOOL isAdmin = FALSE;
    PSID adminGroup = nullptr;
    SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&ntAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                 DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &adminGroup)) {
        CheckTokenMembership(nullptr, adminGroup, &isAdmin);
        FreeSid(adminGroup);
    }
    return isAdmin != FALSE;
}

std::wstring DnsManager::format_win32_error(DWORD error_code) {
    if (error_code == ERROR_SUCCESS) {
        return L"Success";
    }

    LPWSTR buffer = nullptr;
    DWORD size = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error_code,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&buffer),
        0,
        nullptr
    );

    std::wstring result;
    if (size > 0 && buffer) {
        result = buffer;
        LocalFree(buffer);
        while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n' || result.back() == L' ')) {
            result.pop_back();
        }
    } else {
        wchar_t hex_buf[32] = {};
        std::swprintf(hex_buf, sizeof(hex_buf) / sizeof(wchar_t),
                      L"System error 0x%08lX", static_cast<unsigned long>(error_code));
        result = hex_buf;
    }
    return result;
}

std::wstring DnsManager::guid_to_string(const GUID& guid) {
    wchar_t buffer[40] = {0};
    if (StringFromGUID2(guid, buffer, ARRAYSIZE(buffer)) > 0) {
        return std::wstring(buffer);
    }
    return L"";
}

GUID DnsManager::string_to_guid(const std::wstring& str) {
    GUID guid = GUID_NULL;
    if (!str.empty()) {
        if (FAILED(IIDFromString(str.c_str(), &guid))) {
            guid = GUID_NULL;
        }
    }
    return guid;
}

bool DnsManager::is_virtual_adapter(const IP_ADAPTER_ADDRESSES* adapter) {
    if (!adapter) {
        return true;
    }

    // Always filter loopback and tunnel interfaces
    if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK || adapter->IfType == IF_TYPE_TUNNEL) {
        return true;
    }

    std::wstring desc = adapter->Description ? adapter->Description : L"";
    std::wstring name = adapter->FriendlyName ? adapter->FriendlyName : L"";
    std::wstring combined = desc + L" " + name;
    std::transform(combined.begin(), combined.end(), combined.begin(), ::towlower);

    static const std::vector<std::wstring> virtual_signatures = {
        L"virtualbox host-only",
        L"vmware virtual ethernet",
        L"tap-windows",
        L"npcap loopback",
        L"bluetooth device (personal area",
        L"bluetooth network connection",
        L"microsoft wi-fi direct",
        L"microsoft kernel debug",
        L"wintun",
        L"tailscale",
        L"wireguard tunnel",
        L"zerotier"
    };

    for (const auto& sig : virtual_signatures) {
        if (combined.find(sig) != std::wstring::npos) {
            return true;
        }
    }

    return false;
}

bool DnsManager::execute_hidden_process(const std::wstring& exe_path, const std::wstring& args) {
    if (exe_path.empty()) {
        return false;
    }

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi = {};
    std::wstring cmd_mutable = L"\"" + exe_path + L"\"";
    if (!args.empty()) {
        cmd_mutable += L" ";
        cmd_mutable += args;
    }

    DWORD creation_flags = CREATE_NO_WINDOW;
    BOOL success = CreateProcessW(
        exe_path.c_str(),
        cmd_mutable.data(),
        nullptr,
        nullptr,
        FALSE,
        creation_flags,
        nullptr,
        nullptr,
        &si,
        &pi
    );

    if (!success) {
        return false;
    }

    // Wait up to 10 seconds for completion
    DWORD wait_result = WaitForSingleObject(pi.hProcess, 10000);
    DWORD exit_code = 1;
    if (wait_result == WAIT_OBJECT_0) {
        if (!GetExitCodeProcess(pi.hProcess, &exit_code)) {
            exit_code = 1;
        }
    } else {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 1000);
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    return (wait_result == WAIT_OBJECT_0 && exit_code == 0);
}

void DnsManager::query_static_dns_servers(const GUID& guid,
                                          const std::wstring& guid_str,
                                          std::vector<std::wstring>& out_v4,
                                          std::vector<std::wstring>& out_v6,
                                          bool& out_has_static) const
{
    out_v4.clear();
    out_v6.clear();
    out_has_static = false;

    bool api_checked_v4 = false;
    bool api_checked_v6 = false;

    auto pfnGet = reinterpret_cast<PFN_GetInterfaceDnsSettings>(m_pfnGetInterfaceDnsSettings);
    auto pfnFree = reinterpret_cast<PFN_FreeInterfaceDnsSettings>(m_pfnFreeInterfaceDnsSettings);

    // Attempt 1: Modern Windows 10/11 GetInterfaceDnsSettings API (require both pfnGet and pfnFree)
    if (pfnGet && pfnFree && guid != GUID_NULL) {
        DNS_INTERFACE_SETTINGS settings_v4 = {};
        settings_v4.Version = DNS_INTERFACE_SETTINGS_VERSION1;
        settings_v4.Flags = 0;
        if (pfnGet(guid, &settings_v4) == ERROR_SUCCESS) {
            api_checked_v4 = true;
            if (has_non_whitespace(settings_v4.ProfileNameServer)) {
                out_has_static = true;
                append_parsed_ips(settings_v4.ProfileNameServer, false, out_v4);
            }
            if (has_non_whitespace(settings_v4.NameServer)) {
                out_has_static = true;
                append_parsed_ips(settings_v4.NameServer, false, out_v4);
            }
            pfnFree(&settings_v4);
        }

        DNS_INTERFACE_SETTINGS settings_v6 = {};
        settings_v6.Version = DNS_INTERFACE_SETTINGS_VERSION1;
        settings_v6.Flags = DNS_SETTING_IPV6;
        if (pfnGet(guid, &settings_v6) == ERROR_SUCCESS) {
            api_checked_v6 = true;
            if (has_non_whitespace(settings_v6.ProfileNameServer)) {
                out_has_static = true;
                append_parsed_ips(settings_v6.ProfileNameServer, true, out_v6);
            }
            if (has_non_whitespace(settings_v6.NameServer)) {
                out_has_static = true;
                append_parsed_ips(settings_v6.NameServer, true, out_v6);
            }
            pfnFree(&settings_v6);
        }
    }

    // Attempt 2: Direct Registry Query fallback (Tcpip and Tcpip6, NameServer and ProfileNameServer)
    std::wstring canon_guid = canonicalize_adapter_guid(guid_str, guid);
    if (!canon_guid.empty()) {
        if (!api_checked_v4 || out_v4.empty()) {
            std::wstring subkey4 = L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters\\Interfaces\\" + canon_guid;
            for (const wchar_t* valName : {L"ProfileNameServer", L"NameServer"}) {
                auto optVal = read_reg_string(HKEY_LOCAL_MACHINE, subkey4, valName);
                if (optVal.has_value() && has_non_whitespace(*optVal)) {
                    out_has_static = true;
                    append_parsed_ips(*optVal, false, out_v4);
                }
            }
        }

        if (!api_checked_v6 || out_v6.empty()) {
            std::wstring subkey6 = L"SYSTEM\\CurrentControlSet\\Services\\Tcpip6\\Parameters\\Interfaces\\" + canon_guid;
            for (const wchar_t* valName : {L"ProfileNameServer", L"NameServer"}) {
                auto optVal = read_reg_string(HKEY_LOCAL_MACHINE, subkey6, valName);
                if (optVal.has_value() && has_non_whitespace(*optVal)) {
                    out_has_static = true;
                    append_parsed_ips(*optVal, true, out_v6);
                }
            }
        }
    }
}

bool DnsManager::query_is_dhcp_dns(const GUID& guid, const std::wstring& guid_str) const {
    std::vector<std::wstring> v4;
    std::vector<std::wstring> v6;
    bool has_static = false;
    query_static_dns_servers(guid, guid_str, v4, v6, has_static);
    return !has_static;
}

bool DnsManager::is_valid_ipv4(const std::wstring& ip) {
    if (ip.empty()) return false;
    ensure_winsock();
    IN_ADDR addr{};
    return InetPtonW(AF_INET, ip.c_str(), &addr) == 1;
}

bool DnsManager::is_valid_ipv4(std::string_view ip) {
    if (ip.empty()) return false;
    ensure_winsock();
    IN_ADDR addr{};
    std::string ip_str(ip);
    return inet_pton(AF_INET, ip_str.c_str(), &addr) == 1;
}

bool DnsManager::is_valid_ipv6(const std::wstring& ip) {
    if (ip.empty()) return false;
    ensure_winsock();
    IN6_ADDR addr{};
    return InetPtonW(AF_INET6, ip.c_str(), &addr) == 1;
}

bool DnsManager::is_valid_ipv6(std::string_view ip) {
    if (ip.empty()) return false;
    ensure_winsock();
    IN6_ADDR addr{};
    std::string ip_str(ip);
    return inet_pton(AF_INET6, ip_str.c_str(), &addr) == 1;
}

bool DnsManager::is_valid_ip(const std::wstring& ip) {
    return is_valid_ipv4(ip) || is_valid_ipv6(ip);
}

bool DnsManager::is_valid_doh_template(const std::wstring& t) {
    if (t.length() < 12 || t.length() > 2048) {
        return false;
    }

    const std::wstring prefix = L"https://";
    if (t.length() < prefix.length()) return false;
    for (size_t i = 0; i < prefix.length(); ++i) {
        if (towlower(t[i]) != prefix[i]) {
            return false;
        }
    }

    // Host check: after "https://" and before first '/', ':', '?', or '{'
    size_t host_start = prefix.length();
    size_t host_end = t.find_first_of(L"/:?{", host_start);
    if (host_end == std::wstring::npos) {
        host_end = t.length();
    }
    if (host_end <= host_start) {
        return false;
    }

    // Validate host portion is non-trivial and well-formed
    std::wstring_view host(t.data() + host_start, host_end - host_start);
    if (host.front() == L'.' || host.back() == L'.' || host.front() == L'-' || host.back() == L'-') {
        return false;
    }
    bool host_has_alnum = false;
    for (size_t i = 0; i < host.size(); ++i) {
        wchar_t hc = host[i];
        if (hc > 127) return false;
        if ((hc >= L'a' && hc <= L'z') || (hc >= L'A' && hc <= L'Z') || (hc >= L'0' && hc <= L'9')) {
            host_has_alnum = true;
        } else if (hc == L'.') {
            if (i > 0 && host[i - 1] == L'.') {
                return false;
            }
        } else if (hc != L'-') {
            return false;
        }
    }
    if (!host_has_alnum) {
        return false;
    }

    // Allowed characters: ASCII A-Z a-z 0-9 - . _ ~ : / ? = & % { }
    // Strictly forbidden: spaces, quotes, ;, $, |, <, >, ^, (, ), comma, control chars, non-ASCII
    for (wchar_t ch : t) {
        if (ch > 127 || ch <= 32) return false;
        if ((ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') || (ch >= L'0' && ch <= L'9')) {
            continue;
        }
        if (ch == L'-' || ch == L'.' || ch == L'_' || ch == L'~' ||
            ch == L':' || ch == L'/' || ch == L'?' || ch == L'=' ||
            ch == L'&' || ch == L'%' || ch == L'{' || ch == L'}') {
            continue;
        }
        return false;
    }

    return true;
}

std::vector<NetworkAdapterInfo> DnsManager::get_network_adapters() {
    ensure_winsock();
    std::vector<PrioritizedAdapter> adapter_candidates;

    ULONG flags = GAA_FLAG_INCLUDE_PREFIX |
                  GAA_FLAG_SKIP_ANYCAST |
                  GAA_FLAG_SKIP_MULTICAST |
                  GAA_FLAG_INCLUDE_GATEWAYS;

    ULONG buffer_size = 16384;
    std::vector<BYTE> buffer;
    PIP_ADAPTER_ADDRESSES pAddresses = nullptr;
    ULONG result = ERROR_BUFFER_OVERFLOW;

    for (int attempt = 0; attempt < 3; ++attempt) {
        buffer.resize(buffer_size);
        pAddresses = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
        result = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, pAddresses, &buffer_size);
        if (result != ERROR_BUFFER_OVERFLOW) {
            break;
        }
    }

    if (result != NO_ERROR) {
        set_last_error(L"GetAdaptersAddresses failed: " + format_win32_error(result));
        return {};
    }

    for (PIP_ADAPTER_ADDRESSES pCurr = pAddresses; pCurr != nullptr; pCurr = pCurr->Next) {
        // Exclude loopback and tunnel adapters completely
        if (pCurr->IfType == IF_TYPE_SOFTWARE_LOOPBACK || pCurr->IfType == IF_TYPE_TUNNEL) {
            continue;
        }

        bool is_virt = is_virtual_adapter(pCurr);

        PrioritizedAdapter item;
        item.is_virtual = is_virt;
        item.has_gateway = (pCurr->FirstGatewayAddress != nullptr);
        item.is_ethernet_or_wifi = (pCurr->IfType == IF_TYPE_ETHERNET_CSMACD || pCurr->IfType == IF_TYPE_IEEE80211);

        // Convert AdapterName (UTF-8 GUID string) to wide string
        std::wstring adapter_id;
        if (pCurr->AdapterName) {
            adapter_id = utf8_to_wide(pCurr->AdapterName);
        }
        item.info.id = adapter_id;
        item.info.luid = pCurr->Luid;
        item.info.if_index = pCurr->IfIndex;

        // Resolve GUID from LUID or AdapterName
        GUID guid = GUID_NULL;
        if (ConvertInterfaceLuidToGuid(&pCurr->Luid, &guid) != NO_ERROR) {
            if (!adapter_id.empty()) {
                guid = string_to_guid(adapter_id);
            }
        }
        item.info.guid = guid;
        if (item.info.id.empty() && guid != GUID_NULL) {
            item.info.id = guid_to_string(guid);
        }

        item.info.name = pCurr->FriendlyName ? pCurr->FriendlyName : L"";
        item.info.description = pCurr->Description ? pCurr->Description : L"";
        item.info.is_connected = (pCurr->OperStatus == IfOperStatusUp);

        // Extract DNS servers from GetAdaptersAddresses
        for (PIP_ADAPTER_DNS_SERVER_ADDRESS pDns = pCurr->FirstDnsServerAddress; pDns != nullptr; pDns = pDns->Next) {
            if (!pDns->Address.lpSockaddr) continue;

            if (pDns->Address.lpSockaddr->sa_family == AF_INET) {
                auto* sa = reinterpret_cast<sockaddr_in*>(pDns->Address.lpSockaddr);
                wchar_t ipStr[INET_ADDRSTRLEN] = {0};
                if (InetNtopW(AF_INET, &sa->sin_addr, ipStr, INET_ADDRSTRLEN)) {
                    std::wstring ipW(ipStr);
                    if (std::find(item.info.dns_ipv4.begin(), item.info.dns_ipv4.end(), ipW) == item.info.dns_ipv4.end()) {
                        item.info.dns_ipv4.push_back(std::move(ipW));
                    }
                }
            } else if (pDns->Address.lpSockaddr->sa_family == AF_INET6) {
                auto* sa = reinterpret_cast<sockaddr_in6*>(pDns->Address.lpSockaddr);
                wchar_t ipStr[INET6_ADDRSTRLEN] = {0};
                if (InetNtopW(AF_INET6, &sa->sin6_addr, ipStr, INET6_ADDRSTRLEN)) {
                    std::wstring ipW(ipStr);
                    for (wchar_t& ch : ipW) {
                        ch = static_cast<wchar_t>(towlower(ch));
                    }
                    // Filter out legacy site-local fec0: addresses
                    if (ipW.rfind(L"fec0:", 0) != 0) {
                        if (std::find(item.info.dns_ipv6.begin(), item.info.dns_ipv6.end(), ipW) == item.info.dns_ipv6.end()) {
                            item.info.dns_ipv6.push_back(std::move(ipW));
                        }
                    }
                }
            }
        }

        item.info.has_ipv6 = (pCurr->Ipv6IfIndex != 0) || !item.info.dns_ipv6.empty();
        if (!item.info.has_ipv6) {
            for (PIP_ADAPTER_UNICAST_ADDRESS pUni = pCurr->FirstUnicastAddress; pUni != nullptr; pUni = pUni->Next) {
                if (pUni->Address.lpSockaddr && pUni->Address.lpSockaddr->sa_family == AF_INET6) {
                    item.info.has_ipv6 = true;
                    break;
                }
            }
        }

        // Determine DHCP vs Static DNS across both IPv4 and IPv6 (including ProfileNameServer)
        std::vector<std::wstring> static_v4;
        std::vector<std::wstring> static_v6;
        bool has_static = false;
        query_static_dns_servers(item.info.guid, item.info.id, static_v4, static_v6, has_static);
        item.info.is_dhcp_dns = !has_static;

        if (has_static) {
            if (item.info.dns_ipv4.empty() && !static_v4.empty()) {
                item.info.dns_ipv4 = std::move(static_v4);
            }
            if (item.info.dns_ipv6.empty() && !static_v6.empty()) {
                item.info.dns_ipv6 = std::move(static_v6);
                item.info.has_ipv6 = true;
            }
        }

        adapter_candidates.push_back(std::move(item));
    }

    // Check if non-virtual adapters exist; if so, filter out virtual adapters
    bool has_non_virtual = std::any_of(adapter_candidates.begin(), adapter_candidates.end(), [](const PrioritizedAdapter& a) {
        return !a.is_virtual;
    });

    std::vector<PrioritizedAdapter> filtered_candidates;
    for (auto& item : adapter_candidates) {
        if (!has_non_virtual || !item.is_virtual) {
            filtered_candidates.push_back(std::move(item));
        }
    }

    // Sort and prioritize adapters
    std::sort(filtered_candidates.begin(), filtered_candidates.end(), [](const PrioritizedAdapter& a, const PrioritizedAdapter& b) {
        if (a.info.is_connected != b.info.is_connected) {
            return a.info.is_connected > b.info.is_connected;
        }
        if (a.is_ethernet_or_wifi != b.is_ethernet_or_wifi) {
            return a.is_ethernet_or_wifi > b.is_ethernet_or_wifi;
        }
        if (a.has_gateway != b.has_gateway) {
            return a.has_gateway > b.has_gateway;
        }
        bool a_has_dns = !a.info.dns_ipv4.empty() || !a.info.dns_ipv6.empty();
        bool b_has_dns = !b.info.dns_ipv4.empty() || !b.info.dns_ipv6.empty();
        if (a_has_dns != b_has_dns) {
            return a_has_dns > b_has_dns;
        }
        return a.info.name < b.info.name;
    });

    std::vector<NetworkAdapterInfo> result_adapters;
    result_adapters.reserve(filtered_candidates.size());
    for (auto& candidate : filtered_candidates) {
        result_adapters.push_back(std::move(candidate.info));
    }

    return result_adapters;
}

std::optional<NetworkAdapterInfo> DnsManager::get_primary_adapter() {
    auto adapters = get_network_adapters();
    if (adapters.empty()) {
        return std::nullopt;
    }

    ensure_winsock();

    // Method 1: Query Windows IPv4 routing table via GetBestInterfaceEx (8.8.8.8, 1.1.1.1)
    for (const wchar_t* v4_target : {L"8.8.8.8", L"1.1.1.1"}) {
        sockaddr_in sa4{};
        sa4.sin_family = AF_INET;
        if (InetPtonW(AF_INET, v4_target, &sa4.sin_addr) == 1) {
            DWORD best_if_index = 0;
            if (GetBestInterfaceEx(reinterpret_cast<sockaddr*>(&sa4), &best_if_index) == NO_ERROR) {
                NET_LUID best_luid{};
                if (ConvertInterfaceIndexToLuid(best_if_index, &best_luid) == NO_ERROR) {
                    for (const auto& adapter : adapters) {
                        if (adapter.luid.Value == best_luid.Value) {
                            return adapter;
                        }
                    }
                }
            }
        }
    }

    // Method 2: Query Windows IPv6 routing table via GetBestInterfaceEx (2001:4860:4860::8888, 2606:4700:4700::1111)
    for (const wchar_t* v6_target : {L"2001:4860:4860::8888", L"2606:4700:4700::1111"}) {
        sockaddr_in6 sa6{};
        sa6.sin6_family = AF_INET6;
        if (InetPtonW(AF_INET6, v6_target, &sa6.sin6_addr) == 1) {
            DWORD best_if_index = 0;
            if (GetBestInterfaceEx(reinterpret_cast<sockaddr*>(&sa6), &best_if_index) == NO_ERROR) {
                NET_LUID best_luid{};
                if (ConvertInterfaceIndexToLuid(best_if_index, &best_luid) == NO_ERROR) {
                    for (const auto& adapter : adapters) {
                        if (adapter.luid.Value == best_luid.Value) {
                            return adapter;
                        }
                    }
                }
            }
        }
    }

    // Method 3: First connected adapter with active IPv4 or IPv6 DNS
    for (const auto& adapter : adapters) {
        if (adapter.is_connected && (!adapter.dns_ipv4.empty() || !adapter.dns_ipv6.empty())) {
            return adapter;
        }
    }

    // Method 4: First connected adapter
    for (const auto& adapter : adapters) {
        if (adapter.is_connected) {
            return adapter;
        }
    }

    // Method 5: Return highest prioritized adapter
    return adapters.front();
}

bool DnsManager::validate_dns_addresses(const std::wstring& ipv4_primary,
                                       const std::wstring& ipv4_secondary,
                                       const std::wstring& ipv6_primary,
                                       const std::wstring& ipv6_secondary,
                                       std::wstring* out_err)
{
    if (ipv4_primary.empty() && ipv6_primary.empty()) {
        if (out_err) *out_err = L"Не указан ни один адрес DNS сервера.";
        return false;
    }
    if (!ipv4_secondary.empty() && ipv4_primary.empty()) {
        if (out_err) *out_err = L"Дополнительный IPv4 DNS не может быть указан без основного IPv4 DNS.";
        return false;
    }
    if (!ipv6_secondary.empty() && ipv6_primary.empty()) {
        if (out_err) *out_err = L"Дополнительный IPv6 DNS не может быть указан без основного IPv6 DNS.";
        return false;
    }
    if (!ipv4_primary.empty() && !is_valid_ipv4(ipv4_primary)) {
        if (out_err) *out_err = L"Некорректный IPv4-адрес основного DNS: " + ipv4_primary;
        return false;
    }
    if (!ipv4_secondary.empty() && !is_valid_ipv4(ipv4_secondary)) {
        if (out_err) *out_err = L"Некорректный IPv4-адрес дополнительного DNS: " + ipv4_secondary;
        return false;
    }
    if (!ipv6_primary.empty() && !is_valid_ipv6(ipv6_primary)) {
        if (out_err) *out_err = L"Некорректный IPv6-адрес основного DNS: " + ipv6_primary;
        return false;
    }
    if (!ipv6_secondary.empty() && !is_valid_ipv6(ipv6_secondary)) {
        if (out_err) *out_err = L"Некорректный IPv6-адрес дополнительного DNS: " + ipv6_secondary;
        return false;
    }
    return true;
}

bool DnsManager::set_static_dns(const NetworkAdapterInfo& adapter,
                                const std::wstring& ipv4_primary,
                                const std::wstring& ipv4_secondary,
                                const std::wstring& ipv6_primary,
                                const std::wstring& ipv6_secondary,
                                const std::wstring& doh_template)
{
    std::wstring err;
    if (!validate_dns_addresses(ipv4_primary, ipv4_secondary, ipv6_primary, ipv6_secondary, &err)) {
        set_last_error(err);
        return false;
    }

    if (!doh_template.empty() && !is_valid_doh_template(doh_template)) {
        set_last_error(L"Некорректный DoH-шаблон");
        return false;
    }

    if (!is_elevated()) {
        set_last_error(L"Для изменения параметров DNS требуются права Администратора.");
        return false;
    }

    std::lock_guard<std::mutex> op_lock(op_mutex_);
    std::lock_guard<std::mutex> state_lock(g_state_mutex);

    const std::wstring v4_pri = canonicalize_ipv4(ipv4_primary);
    const std::wstring v4_sec = canonicalize_ipv4(ipv4_secondary);
    const std::wstring v6_pri = canonicalize_ipv6(ipv6_primary);
    const std::wstring v6_sec = canonicalize_ipv6(ipv6_secondary);

    std::vector<std::wstring> v4_list;
    if (!v4_pri.empty()) {
        v4_list.push_back(v4_pri);
        if (!v4_sec.empty() && v4_sec != v4_pri) {
            v4_list.push_back(v4_sec);
        }
    }

    std::vector<std::wstring> v6_list;
    if (!v6_pri.empty()) {
        v6_list.push_back(v6_pri);
        if (!v6_sec.empty() && v6_sec != v6_pri) {
            v6_list.push_back(v6_sec);
        }
    }

    const std::wstring v4_servers = join_servers_comma(v4_list);
    const std::wstring v6_servers = join_servers_comma(v6_list);

    const std::wstring canon_guid = canonicalize_adapter_guid(adapter.id, adapter.guid);
    const GUID target_guid = (adapter.guid != GUID_NULL) ? adapter.guid : string_to_guid(canon_guid);
    const std::wstring netsh_path = get_system32_path() + L"\\netsh.exe";
    const bool safe_name = is_safe_adapter_name_for_netsh(adapter.name);

    bool v4_modern_ok = false;
    bool v6_modern_ok = false;
    bool v4_reset_done = false;
    bool v6_reset_done = false;

    // 1. Modern SetInterfaceDnsSettings API (DNS_INTERFACE_SETTINGS3 -> V1 fallback)
    auto pfnSet = reinterpret_cast<PFN_SetInterfaceDnsSettings>(m_pfnSetInterfaceDnsSettings);
    if (pfnSet && target_guid != GUID_NULL) {
        if (!v4_list.empty()) {
            DWORD status = apply_interface_dns_settings(pfnSet, target_guid, false, v4_list, doh_template);
            if (status == ERROR_SUCCESS) {
                v4_modern_ok = true;
                log_info(L"SetInterfaceDnsSettings (IPv4) succeeded on adapter: " + adapter.name);
            } else {
                log_info(L"SetInterfaceDnsSettings (IPv4) returned: " + format_win32_error(status));
            }
        } else {
            // Reset unconfigured IPv4 family to DHCP so stale static IPv4 DNS does not persist
            DWORD status = apply_interface_dns_settings(pfnSet, target_guid, false, {}, L"");
            if (status == ERROR_SUCCESS || status == ERROR_NOT_FOUND) {
                v4_reset_done = true;
            }
        }

        if (!v6_list.empty()) {
            DWORD status = apply_interface_dns_settings(pfnSet, target_guid, true, v6_list, doh_template);
            if (status == ERROR_SUCCESS) {
                v6_modern_ok = true;
                log_info(L"SetInterfaceDnsSettings (IPv6) succeeded on adapter: " + adapter.name);
            } else {
                log_info(L"SetInterfaceDnsSettings (IPv6) returned: " + format_win32_error(status));
            }
        } else {
            // Reset unconfigured IPv6 family to DHCP so stale static IPv6 DNS does not persist
            DWORD status = apply_interface_dns_settings(pfnSet, target_guid, true, {}, L"");
            if (status == ERROR_SUCCESS || status == ERROR_NOT_FOUND) {
                v6_reset_done = true;
            }
        }
    }

    // 2. Netsh fallback for configured and unconfigured families
    bool v4_netsh_ok = false;
    bool v6_netsh_ok = false;

    if (!v4_list.empty() && !v4_modern_ok) {
        log_info(L"Modern SetInterfaceDnsSettings failed or unavailable for IPv4, attempting netsh fallback.");
        if (adapter.if_index > 0) {
            std::wstring args_pri = L"interface ipv4 set dnsservers name=" +
                std::to_wstring(adapter.if_index) +
                L" source=static address=" + v4_list[0] +
                L" register=none validate=no";
            if (execute_hidden_process(netsh_path, args_pri)) {
                v4_netsh_ok = true;
                if (v4_list.size() > 1) {
                    std::wstring args_sec = L"interface ipv4 add dnsservers name=" +
                        std::to_wstring(adapter.if_index) +
                        L" address=" + v4_list[1] +
                        L" index=2 validate=no";
                    if (!execute_hidden_process(netsh_path, args_sec)) {
                        log_error(L"netsh IPv4 secondary DNS add failed by index on " + adapter.name);
                    }
                }
            }
        }

        if (!v4_netsh_ok && safe_name) {
            std::wstring args_pri = L"interface ipv4 set dnsservers name=\"" + adapter.name +
                L"\" source=static address=" + v4_list[0] +
                L" register=none validate=no";
            if (execute_hidden_process(netsh_path, args_pri)) {
                v4_netsh_ok = true;
                if (v4_list.size() > 1) {
                    std::wstring args_sec = L"interface ipv4 add dnsservers name=\"" + adapter.name +
                        L"\" address=" + v4_list[1] +
                        L" index=2 validate=no";
                    if (!execute_hidden_process(netsh_path, args_sec)) {
                        log_error(L"netsh IPv4 secondary DNS add failed by name on " + adapter.name);
                    }
                }
            }
        }
    } else if (v4_list.empty() && !v4_reset_done) {
        bool reset_netsh = false;
        if (adapter.if_index > 0) {
            reset_netsh = execute_hidden_process(netsh_path,
                L"interface ipv4 set dnsservers name=" + std::to_wstring(adapter.if_index) + L" source=dhcp");
        }
        if (!reset_netsh && safe_name) {
            reset_netsh = execute_hidden_process(netsh_path,
                L"interface ipv4 set dnsservers name=\"" + adapter.name + L"\" source=dhcp");
        }
        if (!reset_netsh && !canon_guid.empty()) {
            bool dummy_unbound = false;
            clear_reg_dns_values(L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters\\Interfaces\\" + canon_guid, dummy_unbound);
        }
    }

    if (!v6_list.empty() && !v6_modern_ok) {
        log_info(L"Modern SetInterfaceDnsSettings failed or unavailable for IPv6, attempting netsh fallback.");
        if (adapter.if_index > 0) {
            std::wstring args_pri = L"interface ipv6 set dnsservers name=" +
                std::to_wstring(adapter.if_index) +
                L" source=static address=" + v6_list[0] +
                L" register=none validate=no";
            if (execute_hidden_process(netsh_path, args_pri)) {
                v6_netsh_ok = true;
                if (v6_list.size() > 1) {
                    std::wstring args_sec = L"interface ipv6 add dnsservers name=" +
                        std::to_wstring(adapter.if_index) +
                        L" address=" + v6_list[1] +
                        L" index=2 validate=no";
                    if (!execute_hidden_process(netsh_path, args_sec)) {
                        log_error(L"netsh IPv6 secondary DNS add failed by index on " + adapter.name);
                    }
                }
            }
        }

        if (!v6_netsh_ok && safe_name) {
            std::wstring args_pri = L"interface ipv6 set dnsservers name=\"" + adapter.name +
                L"\" source=static address=" + v6_list[0] +
                L" register=none validate=no";
            if (execute_hidden_process(netsh_path, args_pri)) {
                v6_netsh_ok = true;
                if (v6_list.size() > 1) {
                    std::wstring args_sec = L"interface ipv6 add dnsservers name=\"" + adapter.name +
                        L"\" address=" + v6_list[1] +
                        L" index=2 validate=no";
                    if (!execute_hidden_process(netsh_path, args_sec)) {
                        log_error(L"netsh IPv6 secondary DNS add failed by name on " + adapter.name);
                    }
                }
            }
        }
    } else if (v6_list.empty() && !v6_reset_done) {
        bool reset_netsh = false;
        if (adapter.if_index > 0) {
            reset_netsh = execute_hidden_process(netsh_path,
                L"interface ipv6 set dnsservers name=" + std::to_wstring(adapter.if_index) + L" source=dhcp");
        }
        if (!reset_netsh && safe_name) {
            reset_netsh = execute_hidden_process(netsh_path,
                L"interface ipv6 set dnsservers name=\"" + adapter.name + L"\" source=dhcp");
        }
        if (!reset_netsh && !canon_guid.empty()) {
            bool dummy_unbound = false;
            clear_reg_dns_values(L"SYSTEM\\CurrentControlSet\\Services\\Tcpip6\\Parameters\\Interfaces\\" + canon_guid, dummy_unbound);
        }
    }

    bool v4_ok = v4_list.empty() || v4_modern_ok || v4_netsh_ok;
    bool v6_ok = v6_list.empty() || v6_modern_ok || v6_netsh_ok;

    // 3. Direct Registry synchronization ONLY if both modern API and netsh failed
    if (!v4_ok && !canon_guid.empty()) {
        log_info(L"Both modern API and netsh failed for IPv4, attempting registry fallback.");
        std::wstring subkey4 = L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters\\Interfaces\\" + canon_guid;
        if (set_reg_dns_values(subkey4, v4_servers)) {
            v4_ok = true;
            log_info(L"Applied IPv4 DNS via direct registry fallback.");
        }
    }
    if (!v6_ok && !canon_guid.empty()) {
        log_info(L"Both modern API and netsh failed for IPv6, attempting registry fallback.");
        std::wstring subkey6 = L"SYSTEM\\CurrentControlSet\\Services\\Tcpip6\\Parameters\\Interfaces\\" + canon_guid;
        if (set_reg_dns_values(subkey6, v6_servers)) {
            v6_ok = true;
            log_info(L"Applied IPv6 DNS via direct registry fallback.");
        }
    }

    // Abort BEFORE mutating any DoH state if DNS server configuration failed
    if (!v4_ok || !v6_ok) {
        set_last_error(L"Не удалось применить DNS ни через Windows API, ни через netsh. Проверьте права Администратора.");
        return false;
    }

    // 4. DNS over HTTPS (DoH) encryption & stale DoH state cleanup
    std::vector<std::wstring> server_list;
    server_list.reserve(v4_list.size() + v6_list.size());
    for (const auto& s : v4_list) server_list.push_back(s);
    for (const auto& s : v6_list) server_list.push_back(s);

    // Clean any previously managed DohWellKnownServers not in the new active DoH list
    auto prev_managed = get_managed_doh_servers();
    for (const auto& s : prev_managed) {
        if (!is_valid_ip(s)) {
            continue;
        }
        const bool keep = (!doh_template.empty()) &&
            (std::find(server_list.begin(), server_list.end(), s) != server_list.end());
        if (!keep) {
            std::wstring wellKnownKey = L"SYSTEM\\CurrentControlSet\\Services\\Dnscache\\Parameters\\DohWellKnownServers\\" + s;
            RegDeleteTreeW(HKEY_LOCAL_MACHINE, wellKnownKey.c_str());
            if (is_windows_11_or_greater()) {
                execute_hidden_process(netsh_path, L"dns delete encryption server=" + s);
            }
            unmark_doh_server_managed(s);
            log_info(L"Removed inactive managed DoH server registration for " + s);
        }
    }

    if (!doh_template.empty()) {
        for (const auto& s : server_list) {
            configure_doh_server(s, doh_template);
        }
    } else {
        restore_enable_autodoh();
    }

    // Configure/clean both DohInterfaceSettings and DohProfileSettings before final Dnscache notification
    if (!canon_guid.empty()) {
        configure_interface_doh(canon_guid, server_list, doh_template);
    }

    if (pfnSet && target_guid != GUID_NULL) {
        if (!v4_list.empty()) {
            apply_interface_dns_settings(pfnSet, target_guid, false, v4_list, doh_template);
        }
        if (!v6_list.empty()) {
            apply_interface_dns_settings(pfnSet, target_guid, true, v6_list, doh_template);
        }
    }

    flush_dns_cache_unlocked();
    return true;
}

bool DnsManager::set_static_dns(const NetworkAdapterInfo& adapter,
                                const std::wstring& primary_dns,
                                const std::wstring& secondary_dns,
                                const std::wstring& doh_template) {
    if (is_valid_ipv6(primary_dns) || (primary_dns.empty() && is_valid_ipv6(secondary_dns))) {
        return set_static_dns(adapter, L"", L"", primary_dns, secondary_dns, doh_template);
    }
    return set_static_dns(adapter, primary_dns, secondary_dns, L"", L"", doh_template);
}

void DnsManager::configure_doh_server(const std::wstring& server_ip, const std::wstring& doh_template) {
    if (server_ip.empty() || doh_template.empty()) {
        return;
    }

    const std::wstring canon_ip = canonicalize_ip(server_ip);
    if (canon_ip.empty() || !is_valid_doh_template(doh_template)) {
        log_error(L"configure_doh_server: invalid server_ip or doh_template rejected");
        return;
    }

    log_info(L"Configuring DoH encryption for server: " + canon_ip + L" -> " + doh_template);

    // 1. Direct Windows Registry configuration
    // HKLM\SYSTEM\CurrentControlSet\Services\Dnscache\Parameters\DohWellKnownServers\<canon_ip>
    std::wstring reg_key_path = L"SYSTEM\\CurrentControlSet\\Services\\Dnscache\\Parameters\\DohWellKnownServers\\" + canon_ip;

    HKEY hExisting = nullptr;
    bool alreadyExists = (RegOpenKeyExW(HKEY_LOCAL_MACHINE, reg_key_path.c_str(), 0, KEY_READ, &hExisting) == ERROR_SUCCESS);
    if (alreadyExists) {
        RegCloseKey(hExisting);
    }

    bool isManaged = is_doh_server_managed(canon_ip);
    if (alreadyExists && !isManaged) {
        log_info(L"DoH server " + canon_ip + L" already exists in system DohWellKnownServers and was not created by QuickDNS; preserving system entry");
        backup_and_set_enable_autodoh();
        return;
    }

    HKEY hKey = nullptr;
    DWORD disposition = 0;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, reg_key_path.c_str(), 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE, nullptr, &hKey, &disposition) == ERROR_SUCCESS) {
        RegSetValueExW(hKey, L"Template", 0, REG_SZ,
            reinterpret_cast<const BYTE*>(doh_template.c_str()),
            static_cast<DWORD>((doh_template.length() + 1) * sizeof(wchar_t)));

        // Flags: 0x0c = FallbackToUdp (0x4) | AutoUpgradeServer (0x8)
        ULONGLONG flags64 = DNS_DOH_SERVER_SETTINGS_FALLBACK_TO_UDP | DNS_DOH_AUTO_UPGRADE_SERVER;
        RegSetValueExW(hKey, L"Flags", 0, REG_QWORD, reinterpret_cast<const BYTE*>(&flags64), sizeof(flags64));

        RegCloseKey(hKey);
        mark_doh_server_managed(canon_ip);
        log_info(L"Registered DoH server in Dnscache\\Parameters\\DohWellKnownServers (managed by QuickDNS)");
    }

    backup_and_set_enable_autodoh();

    // 2. Windows 11+ netsh dns encryption notification (not available on Windows 10)
    if (is_windows_11_or_greater()) {
        std::wstring netsh_path = get_system32_path() + L"\\netsh.exe";
        std::wstring args_set = L"dns set encryption server=" + canon_ip +
            L" dohtemplate=\"" + doh_template + L"\" autoupgrade=yes udpfallback=yes";
        if (!execute_hidden_process(netsh_path, args_set)) {
            std::wstring args_add = L"dns add encryption server=" + canon_ip +
                L" dohtemplate=\"" + doh_template + L"\" autoupgrade=yes udpfallback=yes";
            if (execute_hidden_process(netsh_path, args_add)) {
                mark_doh_server_managed(canon_ip);
            }
        } else {
            mark_doh_server_managed(canon_ip);
        }
    }
}

void DnsManager::configure_interface_doh(const std::wstring& adapter_id,
                                         const std::vector<std::wstring>& servers,
                                         const std::wstring& doh_template) {
    const std::wstring guid_str = canonicalize_adapter_guid(adapter_id);
    if (guid_str.empty()) {
        log_error(L"configure_interface_doh: invalid adapter GUID rejected");
        return;
    }

    if (!doh_template.empty() && !is_valid_doh_template(doh_template)) {
        log_error(L"configure_interface_doh: invalid doh_template rejected");
        return;
    }

    const std::wstring iface_root = L"SYSTEM\\CurrentControlSet\\Services\\Dnscache\\InterfaceSpecificParameters\\" + guid_str;
    const std::wstring iface_v4 = iface_root + L"\\DohInterfaceSettings\\Doh";
    const std::wstring iface_v6 = iface_root + L"\\DohInterfaceSettings\\Doh6";
    const std::wstring prof_v4  = iface_root + L"\\DohProfileSettings\\Doh";
    const std::wstring prof_v6  = iface_root + L"\\DohProfileSettings\\Doh6";

    // Clean up existing adapter & Wi-Fi profile DoH keys to prevent stale server templates
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, iface_v4.c_str());
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, iface_v6.c_str());
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, prof_v4.c_str());
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, prof_v6.c_str());

    if (doh_template.empty() || servers.empty()) {
        log_info(L"Cleared interface and profile DoH settings for " + guid_str);
        return;
    }

    const ULONGLONG dohFlags = QUICKDNS_INTERFACE_DOH_FLAGS; // 0x6 = ENABLE (0x2) | FALLBACK_TO_UDP (0x4)
    const DWORD tmplBytes = static_cast<DWORD>((doh_template.length() + 1) * sizeof(wchar_t));

    for (const auto& raw_ip : servers) {
        const std::wstring ip = canonicalize_ip(raw_ip);
        if (ip.empty()) {
            continue;
        }

        const bool is_v6 = is_valid_ipv6(ip);
        const std::wstring targets[2] = {
            (is_v6 ? iface_v6 : iface_v4) + L"\\" + ip,
            (is_v6 ? prof_v6 : prof_v4) + L"\\" + ip
        };

        for (const auto& target_key : targets) {
            HKEY hKey = nullptr;
            DWORD disp = 0;
            if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, target_key.c_str(), 0, nullptr,
                                REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE, nullptr, &hKey, &disp) == ERROR_SUCCESS) {
                RegSetValueExW(hKey, L"DohTemplate", 0, REG_SZ,
                    reinterpret_cast<const BYTE*>(doh_template.c_str()), tmplBytes);
                RegSetValueExW(hKey, L"DohFlags", 0, REG_QWORD,
                    reinterpret_cast<const BYTE*>(&dohFlags), sizeof(dohFlags));
                RegCloseKey(hKey);
            }
        }
        log_info(L"Registered interface & profile DoH (flags=0x6) for adapter " + guid_str + L" server " + ip);
    }
}

bool DnsManager::reset_to_dhcp(const NetworkAdapterInfo& adapter) {
    if (!is_elevated()) {
        set_last_error(L"Для сброса параметров DNS требуются права Администратора.");
        return false;
    }

    std::lock_guard<std::mutex> op_lock(op_mutex_);
    std::lock_guard<std::mutex> state_lock(g_state_mutex);

    const std::wstring canon_guid = canonicalize_adapter_guid(adapter.id, adapter.guid);
    const GUID target_guid = (adapter.guid != GUID_NULL) ? adapter.guid : string_to_guid(canon_guid);
    const std::wstring netsh_path = get_system32_path() + L"\\netsh.exe";
    const bool safe_name = is_safe_adapter_name_for_netsh(adapter.name);

    bool v4_ok = false;
    bool v4_unbound = false;
    bool v6_ok = false;
    bool v6_unbound = false;
    bool reg_ok = false;

    // 1. Try modern SetInterfaceDnsSettings API independently for IPv4 and IPv6
    auto pfnSet = reinterpret_cast<PFN_SetInterfaceDnsSettings>(m_pfnSetInterfaceDnsSettings);
    if (pfnSet && target_guid != GUID_NULL) {
        DWORD status_v4 = apply_interface_dns_settings(pfnSet, target_guid, false, {}, L"");
        if (status_v4 == ERROR_SUCCESS) {
            v4_ok = true;
            log_info(L"Successfully reset IPv4 DNS to DHCP via SetInterfaceDnsSettings on " + adapter.name);
        } else if (status_v4 == ERROR_NOT_FOUND) {
            v4_unbound = true;
        } else {
            log_info(L"SetInterfaceDnsSettings reset IPv4 returned: " + format_win32_error(status_v4));
        }

        DWORD status_v6 = apply_interface_dns_settings(pfnSet, target_guid, true, {}, L"");
        if (status_v6 == ERROR_SUCCESS) {
            v6_ok = true;
            log_info(L"Successfully reset IPv6 DNS to DHCP via SetInterfaceDnsSettings on " + adapter.name);
        } else if (status_v6 == ERROR_NOT_FOUND) {
            v6_unbound = true;
        } else {
            log_info(L"SetInterfaceDnsSettings reset IPv6 returned: " + format_win32_error(status_v6));
        }
    }

    // 2. Netsh fallback independently per family
    if (!v4_ok && !v4_unbound) {
        if (adapter.if_index > 0) {
            if (execute_hidden_process(netsh_path,
                    L"interface ipv4 set dnsservers name=" + std::to_wstring(adapter.if_index) + L" source=dhcp")) {
                v4_ok = true;
            }
        }
        if (!v4_ok && safe_name) {
            if (execute_hidden_process(netsh_path,
                    L"interface ipv4 set dnsservers name=\"" + adapter.name + L"\" source=dhcp")) {
                v4_ok = true;
            }
        }
    }

    if (!v6_ok && !v6_unbound) {
        if (adapter.if_index > 0) {
            if (execute_hidden_process(netsh_path,
                    L"interface ipv6 set dnsservers name=" + std::to_wstring(adapter.if_index) + L" source=dhcp")) {
                v6_ok = true;
            }
        }
        if (!v6_ok && safe_name) {
            if (execute_hidden_process(netsh_path,
                    L"interface ipv6 set dnsservers name=\"" + adapter.name + L"\" source=dhcp")) {
                v6_ok = true;
            }
        }
    }

    // 3. Direct Registry fallback (clearing both NameServer and ProfileNameServer)
    if (!v4_ok && !v4_unbound && !canon_guid.empty()) {
        std::wstring subkey4 = L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters\\Interfaces\\" + canon_guid;
        if (clear_reg_dns_values(subkey4, v4_unbound)) {
            v4_ok = true;
            reg_ok = true;
            log_info(L"Reset IPv4 DNS to DHCP via direct registry fallback on " + adapter.name);
        }
    }

    if (!v6_ok && !v6_unbound && !canon_guid.empty()) {
        std::wstring subkey6 = L"SYSTEM\\CurrentControlSet\\Services\\Tcpip6\\Parameters\\Interfaces\\" + canon_guid;
        if (clear_reg_dns_values(subkey6, v6_unbound)) {
            v6_ok = true;
            reg_ok = true;
            log_info(L"Reset IPv6 DNS to DHCP via direct registry fallback on " + adapter.name);
        }
    }

    const bool any_family_ok = v4_ok || v6_ok || reg_ok;
    const bool all_bound_ok = (v4_ok || v4_unbound) && (v6_ok || v6_unbound);
    if (!any_family_ok || !all_bound_ok) {
        set_last_error(L"Не удалось переключить адаптер на DHCP. Проверьте права Администратора.");
        return false;
    }

    // 4. Clear interface and profile DoH configuration
    if (!canon_guid.empty()) {
        configure_interface_doh(canon_guid, {}, L"");
    }

    // 5. Clean up application-managed DohWellKnownServers and netsh encryption
    auto managed_servers = get_managed_doh_servers();
    for (const auto& s : managed_servers) {
        if (!is_valid_ip(s)) {
            continue;
        }
        std::wstring wellKnownKey = L"SYSTEM\\CurrentControlSet\\Services\\Dnscache\\Parameters\\DohWellKnownServers\\" + s;
        RegDeleteTreeW(HKEY_LOCAL_MACHINE, wellKnownKey.c_str());

        if (is_windows_11_or_greater()) {
            execute_hidden_process(netsh_path, L"dns delete encryption server=" + s);
        }

        unmark_doh_server_managed(s);
        log_info(L"Removed managed DoH server registration for " + s);
    }

    // 6. Restore original EnableAutoDoh setting
    restore_enable_autodoh();

    // Notify Dnscache after DoH cleanup
    if (pfnSet && target_guid != GUID_NULL) {
        if (!v4_unbound) {
            apply_interface_dns_settings(pfnSet, target_guid, false, {}, L"");
        }
        if (!v6_unbound) {
            apply_interface_dns_settings(pfnSet, target_guid, true, {}, L"");
        }
    }

    flush_dns_cache_unlocked();
    return true;
}

bool DnsManager::flush_dns_cache_unlocked() {
    // 1. Direct in-process call to DnsFlushResolverCache in dnsapi.dll
    auto pfnFlush = reinterpret_cast<PFN_DnsFlushResolverCache>(m_pfnDnsFlushResolverCache);
    if (pfnFlush && pfnFlush() != FALSE) {
        log_info(L"DNS resolver cache successfully flushed via in-process dnsapi.dll::DnsFlushResolverCache.");
        return true;
    }

    log_info(L"In-process DnsFlushResolverCache unavailable or failed, falling back to ipconfig.exe /flushdns.");

    // 2. Fallback: invoke ipconfig.exe /flushdns only if in-process call failed
    bool flushed_ipconfig = execute_hidden_process(get_system32_path() + L"\\ipconfig.exe", L"/flushdns");
    if (flushed_ipconfig) {
        log_info(L"DNS resolver cache successfully flushed via fallback ipconfig.exe /flushdns.");
        return true;
    }

    log_error(L"Failed to flush DNS cache via both DnsFlushResolverCache and ipconfig.");
    return false;
}

bool DnsManager::flush_dns_cache() {
    std::lock_guard<std::mutex> op_lock(op_mutex_);
    return flush_dns_cache_unlocked();
}

} // namespace quickdns
