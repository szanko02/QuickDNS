#include "dns_manager.h"

#include <iostream>
#include <sstream>
#include <algorithm>
#include <cwctype>
#include <objbase.h>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "dnsapi.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")

// Dynamic function pointer signatures for Windows 10 1809+ / Windows 11 DNS settings API
#ifndef DNS_INTERFACE_SETTINGS_VERSION1
#define DNS_INTERFACE_SETTINGS_VERSION1 1

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

typedef DWORD (WINAPI *PFN_SetInterfaceDnsSettings)(GUID InterfaceGuid, const DNS_INTERFACE_SETTINGS *Settings);
typedef DWORD (WINAPI *PFN_GetInterfaceDnsSettings)(GUID InterfaceGuid, DNS_INTERFACE_SETTINGS *Settings);
typedef VOID (WINAPI *PFN_FreeInterfaceDnsSettings)(DNS_INTERFACE_SETTINGS *Settings);
typedef BOOL (WINAPI *PFN_DnsFlushResolverCache)(VOID);

namespace {

// RAII helper to ensure WSAStartup is called exactly once across the process
struct WinsockInitializer {
    WinsockInitializer() {
        WSADATA wsaData;
        WSAStartup(MAKEWORD(2, 2), &wsaData);
    }
    ~WinsockInitializer() {
        WSACleanup();
    }
};

void ensure_winsock() {
    static WinsockInitializer initializer;
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
            // Trim whitespace
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

std::wstring get_system32_path() {
    wchar_t sys_dir[MAX_PATH] = {};
    UINT len = GetSystemDirectoryW(sys_dir, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        return std::wstring(sys_dir);
    }
    wchar_t win_dir[MAX_PATH] = {};
    if (GetWindowsDirectoryW(win_dir, MAX_PATH) > 0) {
        return std::wstring(win_dir) + L"\\System32";
    }
    return L"C:\\Windows\\System32";
}

constexpr const wchar_t* QUICKDNS_REG_PATH = L"SOFTWARE\\QuickDNS";
constexpr const wchar_t* MANAGED_DOH_REG_PATH = L"SOFTWARE\\QuickDNS\\ManagedDohServers";

bool is_doh_server_managed(const std::wstring& server_ip) {
    std::wstring path = std::wstring(MANAGED_DOH_REG_PATH) + L"\\" + server_ip;
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        RegCloseKey(hKey);
        return true;
    }
    return false;
}

void mark_doh_server_managed(const std::wstring& server_ip) {
    std::wstring path = std::wstring(MANAGED_DOH_REG_PATH) + L"\\" + server_ip;
    HKEY hKey = nullptr;
    DWORD disp = 0;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKey, &disp) == ERROR_SUCCESS) {
        RegCloseKey(hKey);
    }
}

void unmark_doh_server_managed(const std::wstring& server_ip) {
    std::wstring path = std::wstring(MANAGED_DOH_REG_PATH) + L"\\" + server_ip;
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, path.c_str());
}

std::vector<std::wstring> get_managed_doh_servers() {
    std::vector<std::wstring> servers;
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, MANAGED_DOH_REG_PATH, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        wchar_t subkeyName[256] = {0};
        DWORD index = 0;
        DWORD nameLen = 256;
        while (RegEnumKeyExW(hKey, index, subkeyName, &nameLen, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
            servers.emplace_back(subkeyName);
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

    std::wstring result(bytes / sizeof(wchar_t), L'\0');
    status = RegGetValueW(root, path.c_str(), name, RRF_RT_REG_SZ | RRF_RT_REG_MULTI_SZ, nullptr, result.data(), &bytes);
    if (status != ERROR_SUCCESS) {
        return std::nullopt;
    }

    size_t len = bytes / sizeof(wchar_t);
    while (len > 0 && result[len - 1] == L'\0') {
        len--;
    }
    result.resize(len);
    return result;
}

void backup_and_set_enable_autodoh() {
    HKEY hAppKey = nullptr;
    DWORD disp = 0;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, QUICKDNS_REG_PATH, 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, nullptr, &hAppKey, &disp) == ERROR_SUCCESS) {
        DWORD present = 0;
        DWORD pLen = sizeof(DWORD);
        if (RegQueryValueExW(hAppKey, L"OriginalEnableAutoDohPresent", nullptr, nullptr,
                            reinterpret_cast<BYTE*>(&present), &pLen) != ERROR_SUCCESS) {
            HKEY hDnsParam = nullptr;
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\Dnscache\\Parameters", 0, KEY_READ, &hDnsParam) == ERROR_SUCCESS) {
                DWORD origVal = 0;
                DWORD vLen = sizeof(DWORD);
                if (RegQueryValueExW(hDnsParam, L"EnableAutoDoh", nullptr, nullptr,
                                     reinterpret_cast<BYTE*>(&origVal), &vLen) == ERROR_SUCCESS) {
                    DWORD isPresent = 1;
                    RegSetValueExW(hAppKey, L"OriginalEnableAutoDohPresent", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&isPresent), sizeof(DWORD));
                    RegSetValueExW(hAppKey, L"OriginalEnableAutoDoh", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&origVal), sizeof(DWORD));
                } else {
                    DWORD isPresent = 0;
                    RegSetValueExW(hAppKey, L"OriginalEnableAutoDohPresent", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&isPresent), sizeof(DWORD));
                }
                RegCloseKey(hDnsParam);
            }
        }
        RegCloseKey(hAppKey);
    }

    HKEY hDnsParam = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\Dnscache\\Parameters", 0, KEY_SET_VALUE, &hDnsParam) == ERROR_SUCCESS) {
        DWORD enableAutoDoh = 2;
        RegSetValueExW(hDnsParam, L"EnableAutoDoh", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&enableAutoDoh), sizeof(DWORD));
        RegCloseKey(hDnsParam);
    }
}

void restore_enable_autodoh() {
    HKEY hAppKey = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, QUICKDNS_REG_PATH, 0, KEY_ALL_ACCESS, &hAppKey) == ERROR_SUCCESS) {
        DWORD present = 0;
        DWORD pLen = sizeof(DWORD);
        if (RegQueryValueExW(hAppKey, L"OriginalEnableAutoDohPresent", nullptr, nullptr,
                            reinterpret_cast<BYTE*>(&present), &pLen) == ERROR_SUCCESS) {
            HKEY hDnsParam = nullptr;
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\Dnscache\\Parameters", 0, KEY_SET_VALUE, &hDnsParam) == ERROR_SUCCESS) {
                if (present == 1) {
                    DWORD origVal = 0;
                    DWORD vLen = sizeof(DWORD);
                    if (RegQueryValueExW(hAppKey, L"OriginalEnableAutoDoh", nullptr, nullptr,
                                         reinterpret_cast<BYTE*>(&origVal), &vLen) == ERROR_SUCCESS) {
                        RegSetValueExW(hDnsParam, L"EnableAutoDoh", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&origVal), sizeof(DWORD));
                    }
                } else {
                    RegDeleteValueW(hDnsParam, L"EnableAutoDoh");
                }
                RegCloseKey(hDnsParam);
            }
            RegDeleteValueW(hAppKey, L"OriginalEnableAutoDohPresent");
            RegDeleteValueW(hAppKey, L"OriginalEnableAutoDoh");
        }
        RegCloseKey(hAppKey);
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
    if (m_hIpHlp) {
        FreeLibrary(m_hIpHlp);
        m_hIpHlp = nullptr;
    }
    if (m_hDnsApi) {
        FreeLibrary(m_hDnsApi);
        m_hDnsApi = nullptr;
    }
}

DnsManager::DnsManager(DnsManager&& other) noexcept
    : last_error_(std::move(other.last_error_)),
      m_hIpHlp(other.m_hIpHlp),
      m_hDnsApi(other.m_hDnsApi),
      m_pfnSetInterfaceDnsSettings(other.m_pfnSetInterfaceDnsSettings),
      m_pfnGetInterfaceDnsSettings(other.m_pfnGetInterfaceDnsSettings),
      m_pfnFreeInterfaceDnsSettings(other.m_pfnFreeInterfaceDnsSettings),
      m_pfnDnsFlushResolverCache(other.m_pfnDnsFlushResolverCache)
{
    other.m_hIpHlp = nullptr;
    other.m_hDnsApi = nullptr;
    other.m_pfnSetInterfaceDnsSettings = nullptr;
    other.m_pfnGetInterfaceDnsSettings = nullptr;
    other.m_pfnFreeInterfaceDnsSettings = nullptr;
    other.m_pfnDnsFlushResolverCache = nullptr;
}

DnsManager& DnsManager::operator=(DnsManager&& other) noexcept {
    if (this != &other) {
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
    std::wclog << L"[QuickDNS::DnsManager] INFO: " << msg << std::endl;
}

void DnsManager::log_error(const std::wstring& msg) const {
    std::wcerr << L"[QuickDNS::DnsManager] ERROR: " << msg << std::endl;
}

void DnsManager::set_last_error(const std::wstring& msg) const {
    last_error_ = msg;
    log_error(msg);
}

std::wstring DnsManager::get_last_error() const {
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
        result = L"System error 0x" + std::to_wstring(error_code);
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
        IIDFromString(str.c_str(), &guid);
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

bool DnsManager::execute_hidden_process(const std::wstring& command) {
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi = {};
    std::wstring cmd_mutable = command;

    DWORD creation_flags = CREATE_NO_WINDOW;
    BOOL success = CreateProcessW(
        nullptr,
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
        GetExitCodeProcess(pi.hProcess, &exit_code);
    } else {
        TerminateProcess(pi.hProcess, 1);
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    return (wait_result == WAIT_OBJECT_0 && exit_code == 0);
}

bool DnsManager::query_is_dhcp_dns(const GUID& guid, const std::wstring& guid_str) const {
    // Attempt 1: Modern Windows 10/11 GetInterfaceDnsSettings API
    auto pfnGet = reinterpret_cast<PFN_GetInterfaceDnsSettings>(m_pfnGetInterfaceDnsSettings);
    auto pfnFree = reinterpret_cast<PFN_FreeInterfaceDnsSettings>(m_pfnFreeInterfaceDnsSettings);

    if (pfnGet && guid != GUID_NULL) {
        DNS_INTERFACE_SETTINGS settings = {};
        settings.Version = DNS_INTERFACE_SETTINGS_VERSION1;
        DWORD err = pfnGet(guid, &settings);
        if (err == ERROR_SUCCESS) {
            bool has_static_nameserver = false;
            if (settings.NameServer && settings.NameServer[0] != L'\0') {
                for (const wchar_t* p = settings.NameServer; *p; ++p) {
                    if (!iswspace(*p)) {
                        has_static_nameserver = true;
                        break;
                    }
                }
            }
            if (settings.ProfileNameServer && settings.ProfileNameServer[0] != L'\0') {
                for (const wchar_t* p = settings.ProfileNameServer; *p; ++p) {
                    if (!iswspace(*p)) {
                        has_static_nameserver = true;
                        break;
                    }
                }
            }
            if (pfnFree) {
                pfnFree(&settings);
            }
            if (has_static_nameserver) {
                return false; // Static DNS configured
            }
        }
    }

    // Attempt 2: Direct Registry Query fallback
    // HKLM\SYSTEM\CurrentControlSet\Services\Tcpip\Parameters\Interfaces\{GUID}
    std::wstring subkey = L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters\\Interfaces\\" + guid_str;
    auto optNameServer = read_reg_string(HKEY_LOCAL_MACHINE, subkey, L"NameServer");
    if (optNameServer.has_value()) {
        for (wchar_t c : *optNameServer) {
            if (!iswspace(c)) {
                return false; // Found static DNS in registry
            }
        }
    }

    return true; // Default to DHCP DNS
}

bool DnsManager::is_valid_ipv4(const std::wstring& ip) {
    if (ip.empty()) return false;
    ensure_winsock();
    IN_ADDR addr;
    return InetPtonW(AF_INET, ip.c_str(), &addr) == 1;
}

bool DnsManager::is_valid_ipv4(std::string_view ip) {
    if (ip.empty()) return false;
    ensure_winsock();
    IN_ADDR addr;
    std::string ip_str(ip);
    return inet_pton(AF_INET, ip_str.c_str(), &addr) == 1;
}

bool DnsManager::is_valid_ipv6(const std::wstring& ip) {
    if (ip.empty()) return false;
    ensure_winsock();
    IN6_ADDR addr;
    return InetPtonW(AF_INET6, ip.c_str(), &addr) == 1;
}

bool DnsManager::is_valid_ipv6(std::string_view ip) {
    if (ip.empty()) return false;
    ensure_winsock();
    IN6_ADDR addr;
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

    // Host check: after "https://" and before first '/', ':', or '{'
    size_t host_start = prefix.length();
    size_t host_end = t.find_first_of(L"/:{", host_start);
    if (host_end == std::wstring::npos) {
        host_end = t.length();
    }
    if (host_end <= host_start) {
        return false;
    }

    // Allowed characters: A-Z a-z 0-9 - . _ ~ : / ? = & % { }
    // Strictly forbidden: spaces, quotes, ;, $, |, <, >, ^, (, ), comma, control chars, non-ASCII
    for (wchar_t ch : t) {
        if (ch > 127 || ch < 32) return false;
        if (iswalnum(ch)) continue;
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
    std::vector<PrioritizedAdapter> adapter_candidates;

    ULONG flags = GAA_FLAG_INCLUDE_PREFIX | 
                  GAA_FLAG_SKIP_ANYCAST | 
                  GAA_FLAG_SKIP_MULTICAST | 
                  GAA_FLAG_INCLUDE_GATEWAYS;

    ULONG buffer_size = 16384;
    std::vector<BYTE> buffer(buffer_size);
    PIP_ADAPTER_ADDRESSES pAddresses = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());

    ULONG result = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, pAddresses, &buffer_size);
    if (result == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(buffer_size);
        pAddresses = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
        result = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, pAddresses, &buffer_size);
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

        // Convert AdapterName (ASCII GUID) to wide string
        std::wstring adapter_id;
        if (pCurr->AdapterName) {
            int len = MultiByteToWideChar(CP_ACP, 0, pCurr->AdapterName, -1, nullptr, 0);
            if (len > 0) {
                adapter_id.resize(len - 1);
                MultiByteToWideChar(CP_ACP, 0, pCurr->AdapterName, -1, &adapter_id[0], len);
            }
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
                    if (std::find(item.info.dns_ipv4.begin(), item.info.dns_ipv4.end(), ipStr) == item.info.dns_ipv4.end()) {
                        item.info.dns_ipv4.push_back(ipStr);
                    }
                }
            } else if (pDns->Address.lpSockaddr->sa_family == AF_INET6) {
                auto* sa = reinterpret_cast<sockaddr_in6*>(pDns->Address.lpSockaddr);
                wchar_t ipStr[INET6_ADDRSTRLEN] = {0};
                if (InetNtopW(AF_INET6, &sa->sin6_addr, ipStr, INET6_ADDRSTRLEN)) {
                    std::wstring ipW(ipStr);
                    // Filter out legacy site-local fec0: addresses
                    if (ipW.rfind(L"fec0:", 0) != 0) {
                        if (std::find(item.info.dns_ipv6.begin(), item.info.dns_ipv6.end(), ipW) == item.info.dns_ipv6.end()) {
                            item.info.dns_ipv6.push_back(ipW);
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

        // Determine DHCP vs Static DNS configuration
        item.info.is_dhcp_dns = query_is_dhcp_dns(item.info.guid, item.info.id);

        // If static DNS is detected but dns_ipv4 list is empty, query registry / GetInterfaceDnsSettings to populate it
        if (!item.info.is_dhcp_dns && item.info.dns_ipv4.empty()) {
            std::wstring subkey = L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters\\Interfaces\\" + item.info.id;
            auto optNameServer = read_reg_string(HKEY_LOCAL_MACHINE, subkey, L"NameServer");
            if (optNameServer.has_value()) {
                auto tokens = split_string_by_delimiters(*optNameServer, L", ");
                for (const auto& token : tokens) {
                    if (is_valid_ipv4(token)) {
                        item.info.dns_ipv4.push_back(token);
                    }
                }
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
        // 1. Connected adapters first
        if (a.info.is_connected != b.info.is_connected) {
            return a.info.is_connected > b.info.is_connected;
        }
        // 2. Physical Ethernet and Wi-Fi adapters first
        if (a.is_ethernet_or_wifi != b.is_ethernet_or_wifi) {
            return a.is_ethernet_or_wifi > b.is_ethernet_or_wifi;
        }
        // 3. Adapters with default gateway
        if (a.has_gateway != b.has_gateway) {
            return a.has_gateway > b.has_gateway;
        }
        // 4. Adapters with DNS populated
        bool a_has_dns = !a.info.dns_ipv4.empty() || !a.info.dns_ipv6.empty();
        bool b_has_dns = !b.info.dns_ipv4.empty() || !b.info.dns_ipv6.empty();
        if (a_has_dns != b_has_dns) {
            return a_has_dns > b_has_dns;
        }
        // 5. Friendly name alphabetical
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

    // Method 1: Query Windows IP routing table for the best interface to public DNS (8.8.8.8)
    DWORD best_if_index = 0;
    IPAddr target_ip = inet_addr("8.8.8.8");
    if (GetBestInterface(target_ip, &best_if_index) == NO_ERROR) {
        NET_LUID best_luid{};
        if (ConvertInterfaceIndexToLuid(best_if_index, &best_luid) == NO_ERROR) {
            for (const auto& adapter : adapters) {
                if (adapter.luid.Value == best_luid.Value) {
                    return adapter;
                }
            }
        }
    }

    // Method 2: Query best route to 1.1.1.1
    target_ip = inet_addr("1.1.1.1");
    if (GetBestInterface(target_ip, &best_if_index) == NO_ERROR) {
        NET_LUID best_luid{};
        if (ConvertInterfaceIndexToLuid(best_if_index, &best_luid) == NO_ERROR) {
            for (const auto& adapter : adapters) {
                if (adapter.luid.Value == best_luid.Value) {
                    return adapter;
                }
            }
        }
    }

    // Method 3: First connected adapter with active IPv4 DNS
    for (const auto& adapter : adapters) {
        if (adapter.is_connected && !adapter.dns_ipv4.empty()) {
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

    std::wstring v4_servers = ipv4_primary;
    if (!ipv4_secondary.empty()) {
        v4_servers += L"," + ipv4_secondary;
    }

    std::wstring v6_servers = ipv6_primary;
    if (!ipv6_secondary.empty()) {
        v6_servers += L"," + ipv6_secondary;
    }

    bool v4_modern_ok = false;
    bool v6_modern_ok = false;

    // 1. Modern SetInterfaceDnsSettings API (Windows 10 1809+ / Windows 11)
    auto pfnSet = reinterpret_cast<PFN_SetInterfaceDnsSettings>(m_pfnSetInterfaceDnsSettings);
    if (pfnSet && adapter.guid != GUID_NULL) {
        if (!ipv4_primary.empty()) {
            DNS_INTERFACE_SETTINGS settings_v4 = {};
            settings_v4.Version = DNS_INTERFACE_SETTINGS_VERSION1;
            settings_v4.Flags = DNS_SETTING_NAMESERVER | DNS_SETTING_PROFILE_NAMESERVER;
            settings_v4.NameServer = const_cast<PWSTR>(v4_servers.c_str());
            settings_v4.ProfileNameServer = const_cast<PWSTR>(v4_servers.c_str());

            DWORD status = pfnSet(adapter.guid, &settings_v4);
            if (status == ERROR_SUCCESS) {
                v4_modern_ok = true;
                log_info(L"SetInterfaceDnsSettings (IPv4) succeeded on adapter: " + adapter.name);
            } else {
                log_info(L"SetInterfaceDnsSettings (IPv4) returned code: " + std::to_wstring(status) + L" (" + format_win32_error(status) + L")");
            }
        }

        if (!ipv6_primary.empty()) {
            DNS_INTERFACE_SETTINGS settings_v6 = {};
            settings_v6.Version = DNS_INTERFACE_SETTINGS_VERSION1;
            settings_v6.Flags = DNS_SETTING_IPV6 | DNS_SETTING_NAMESERVER | DNS_SETTING_PROFILE_NAMESERVER;
            settings_v6.NameServer = const_cast<PWSTR>(v6_servers.c_str());
            settings_v6.ProfileNameServer = const_cast<PWSTR>(v6_servers.c_str());

            DWORD status = pfnSet(adapter.guid, &settings_v6);
            if (status == ERROR_SUCCESS) {
                v6_modern_ok = true;
                log_info(L"SetInterfaceDnsSettings (IPv6) succeeded on adapter: " + adapter.name);
            } else {
                log_info(L"SetInterfaceDnsSettings (IPv6) returned code: " + std::to_wstring(status) + L" (" + format_win32_error(status) + L")");
            }
        }
    }

    // 2. Comprehensive netsh application
    bool v4_netsh_ok = false;
    bool v6_netsh_ok = false;
    std::wstring netsh_path = get_system32_path() + L"\\netsh.exe";

    if (!ipv4_primary.empty()) {
        if (adapter.if_index > 0) {
            std::wstring cmd_pri = netsh_path + L" interface ipv4 set dnsservers name=" + 
                std::to_wstring(adapter.if_index) + 
                L" source=static address=" + ipv4_primary + 
                L" register=none validate=no";

            log_info(L"Executing netsh IPv4 by index: " + cmd_pri);
            if (execute_hidden_process(cmd_pri)) {
                v4_netsh_ok = true;
                if (!ipv4_secondary.empty()) {
                    std::wstring cmd_sec = netsh_path + L" interface ipv4 add dnsservers name=" + 
                        std::to_wstring(adapter.if_index) + 
                        L" address=" + ipv4_secondary + 
                        L" index=2 validate=no";
                    execute_hidden_process(cmd_sec);
                }
            }
        }

        if (!v4_netsh_ok && !adapter.name.empty() && adapter.name.find(L'"') == std::wstring::npos) {
            std::wstring cmd_pri = netsh_path + L" interface ipv4 set dnsservers name=\"" + adapter.name + 
                L"\" source=static address=" + ipv4_primary + 
                L" register=none validate=no";

            log_info(L"Executing netsh IPv4 by name: " + cmd_pri);
            if (execute_hidden_process(cmd_pri)) {
                v4_netsh_ok = true;
                if (!ipv4_secondary.empty()) {
                    std::wstring cmd_sec = netsh_path + L" interface ipv4 add dnsservers name=\"" + adapter.name + 
                        L"\" address=" + ipv4_secondary + 
                        L" index=2 validate=no";
                    execute_hidden_process(cmd_sec);
                }
            }
        }
    }

    if (!ipv6_primary.empty()) {
        if (adapter.if_index > 0) {
            std::wstring cmd_pri = netsh_path + L" interface ipv6 set dnsservers name=" + 
                std::to_wstring(adapter.if_index) + 
                L" source=static address=" + ipv6_primary + 
                L" register=none validate=no";

            log_info(L"Executing netsh IPv6 by index: " + cmd_pri);
            if (execute_hidden_process(cmd_pri)) {
                v6_netsh_ok = true;
                if (!ipv6_secondary.empty()) {
                    std::wstring cmd_sec = netsh_path + L" interface ipv6 add dnsservers name=" + 
                        std::to_wstring(adapter.if_index) + 
                        L" address=" + ipv6_secondary + 
                        L" index=2 validate=no";
                    execute_hidden_process(cmd_sec);
                }
            }
        }

        if (!v6_netsh_ok && !adapter.name.empty() && adapter.name.find(L'"') == std::wstring::npos) {
            std::wstring cmd_pri = netsh_path + L" interface ipv6 set dnsservers name=\"" + adapter.name + 
                L"\" source=static address=" + ipv6_primary + 
                L" register=none validate=no";

            log_info(L"Executing netsh IPv6 by name: " + cmd_pri);
            if (execute_hidden_process(cmd_pri)) {
                v6_netsh_ok = true;
                if (!ipv6_secondary.empty()) {
                    std::wstring cmd_sec = netsh_path + L" interface ipv6 add dnsservers name=\"" + adapter.name + 
                        L"\" address=" + ipv6_secondary + 
                        L" index=2 validate=no";
                    execute_hidden_process(cmd_sec);
                }
            }
        }
    }

    // 3. Direct Registry synchronization as an additional reliability layer
    if (!adapter.id.empty() && !v4_servers.empty()) {
        std::wstring subkey = L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters\\Interfaces\\" + adapter.id;
        HKEY hKey = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, subkey.c_str(), 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
            RegSetValueExW(hKey, L"NameServer", 0, REG_SZ,
                reinterpret_cast<const BYTE*>(v4_servers.c_str()),
                static_cast<DWORD>((v4_servers.length() + 1) * sizeof(wchar_t)));
            RegCloseKey(hKey);
        }
    }
    if (!adapter.id.empty() && !v6_servers.empty()) {
        std::wstring subkey6 = L"SYSTEM\\CurrentControlSet\\Services\\Tcpip6\\Parameters\\Interfaces\\" + adapter.id;
        HKEY hKey6 = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, subkey6.c_str(), 0, KEY_SET_VALUE, &hKey6) == ERROR_SUCCESS) {
            RegSetValueExW(hKey6, L"NameServer", 0, REG_SZ,
                reinterpret_cast<const BYTE*>(v6_servers.c_str()),
                static_cast<DWORD>((v6_servers.length() + 1) * sizeof(wchar_t)));
            RegCloseKey(hKey6);
        }
    }

    // 4. DNS over HTTPS (DoH) encryption
    std::vector<std::wstring> server_list;
    if (!ipv4_primary.empty()) server_list.push_back(ipv4_primary);
    if (!ipv4_secondary.empty()) server_list.push_back(ipv4_secondary);
    if (!ipv6_primary.empty()) server_list.push_back(ipv6_primary);
    if (!ipv6_secondary.empty()) server_list.push_back(ipv6_secondary);

    if (!doh_template.empty()) {
        for (const auto& s : server_list) {
            configure_doh_server(s, doh_template);
        }
    }

    std::wstring guid_str = adapter.id;
    if (guid_str.empty() && adapter.guid != GUID_NULL) {
        guid_str = guid_to_string(adapter.guid);
    }
    if (!guid_str.empty()) {
        configure_interface_doh(guid_str, server_list, doh_template);
    }

    bool v4_ok = ipv4_primary.empty() || v4_modern_ok || v4_netsh_ok;
    bool v6_ok = ipv6_primary.empty() || v6_modern_ok || v6_netsh_ok;
    if (!v4_ok && !v6_ok) {
        set_last_error(L"Не удалось применить DNS ни через Windows API, ни через netsh. Проверьте права Администратора.");
        return false;
    }

    // Flush DNS resolver cache
    flush_dns_cache();
    return true;
}

bool DnsManager::set_static_dns(const NetworkAdapterInfo& adapter, 
                                const std::wstring& primary_dns, 
                                const std::wstring& secondary_dns,
                                const std::wstring& doh_template) {
    if (is_valid_ipv6(primary_dns)) {
        return set_static_dns(adapter, L"", L"", primary_dns, secondary_dns, doh_template);
    } else {
        return set_static_dns(adapter, primary_dns, secondary_dns, L"", L"", doh_template);
    }
}

void DnsManager::configure_doh_server(const std::wstring& server_ip, const std::wstring& doh_template) {
    if (server_ip.empty() || doh_template.empty()) {
        return;
    }

    if (!is_valid_ip(server_ip) || !is_valid_doh_template(doh_template)) {
        log_error(L"configure_doh_server: invalid server_ip or doh_template rejected");
        return;
    }

    log_info(L"Configuring DoH encryption for server: " + server_ip + L" -> " + doh_template);

    // 1. Direct Windows Registry configuration
    // HKLM\SYSTEM\CurrentControlSet\Services\Dnscache\Parameters\DohWellKnownServers\<server_ip>
    std::wstring reg_key_path = L"SYSTEM\\CurrentControlSet\\Services\\Dnscache\\Parameters\\DohWellKnownServers\\" + server_ip;

    // Check if key already exists in Windows
    HKEY hExisting = nullptr;
    bool alreadyExists = (RegOpenKeyExW(HKEY_LOCAL_MACHINE, reg_key_path.c_str(), 0, KEY_READ, &hExisting) == ERROR_SUCCESS);
    if (alreadyExists) {
        RegCloseKey(hExisting);
    }

    bool isManaged = is_doh_server_managed(server_ip);
    if (alreadyExists && !isManaged) {
        log_info(L"DoH server " + server_ip + L" already exists in system DohWellKnownServers and was not created by QuickDNS; preserving system entry");
    } else {
        HKEY hKey = nullptr;
        DWORD disposition = 0;
        if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, reg_key_path.c_str(), 0, nullptr,
                            REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, nullptr, &hKey, &disposition) == ERROR_SUCCESS) {
            RegSetValueExW(hKey, L"Template", 0, REG_SZ,
                reinterpret_cast<const BYTE*>(doh_template.c_str()),
                static_cast<DWORD>((doh_template.length() + 1) * sizeof(wchar_t)));

            // Flags: 0x0c = AutoUpgrade (0x4) | UdpFallback (0x8)
            ULONGLONG flags64 = 0x0c;
            RegSetValueExW(hKey, L"Flags", 0, REG_QWORD, reinterpret_cast<const BYTE*>(&flags64), sizeof(flags64));

            RegCloseKey(hKey);
            mark_doh_server_managed(server_ip);
            log_info(L"Registered DoH server in Dnscache\\Parameters\\DohWellKnownServers (managed by QuickDNS)");
        }
    }

    // Backup original EnableAutoDoh setting and set EnableAutoDoh = 2
    backup_and_set_enable_autodoh();

    // 2. Windows netsh encryption (tries 'set encryption' first; if not found, tries 'add encryption')
    std::wstring netsh_path = get_system32_path() + L"\\netsh.exe";
    std::wstring cmd_set = netsh_path + L" dns set encryption server=" + server_ip +
        L" dohtemplate=\"" + doh_template + L"\" autoupgrade=yes udpfallback=yes";
    log_info(L"Executing netsh set encryption: " + cmd_set);
    if (!execute_hidden_process(cmd_set)) {
        std::wstring cmd_add = netsh_path + L" dns add encryption server=" + server_ip +
            L" dohtemplate=\"" + doh_template + L"\" autoupgrade=yes udpfallback=yes";
        log_info(L"Executing netsh add encryption fallback: " + cmd_add);
        execute_hidden_process(cmd_add);
    }
}

void DnsManager::configure_interface_doh(const std::wstring& adapter_id, 
                                         const std::vector<std::wstring>& servers, 
                                         const std::wstring& doh_template) {
    if (adapter_id.empty()) {
        return;
    }

    if (!doh_template.empty() && !is_valid_doh_template(doh_template)) {
        log_error(L"configure_interface_doh: invalid doh_template rejected");
        return;
    }

    std::wstring guid_str = adapter_id;
    if (guid_str.front() != L'{') guid_str = L"{" + guid_str;
    if (guid_str.back() != L'}') guid_str += L"}";

    std::wstring base_v4 = L"SYSTEM\\CurrentControlSet\\Services\\Dnscache\\InterfaceSpecificParameters\\" + guid_str + L"\\DohInterfaceSettings\\Doh";
    std::wstring base_v6 = L"SYSTEM\\CurrentControlSet\\Services\\Dnscache\\InterfaceSpecificParameters\\" + guid_str + L"\\DohInterfaceSettings\\Doh6";

    // Clean up existing adapter DoH keys to prevent stale server templates
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, base_v4.c_str());
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, base_v6.c_str());

    if (doh_template.empty() || servers.empty()) {
        log_info(L"Cleared interface-specific DoH settings for " + guid_str);
        return;
    }

    for (const auto& ip : servers) {
        if (ip.empty()) continue;

        bool is_v6 = is_valid_ipv6(ip);
        std::wstring target_key = (is_v6 ? base_v6 : base_v4) + L"\\" + ip;

        HKEY hKey = nullptr;
        DWORD disp = 0;
        if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, target_key.c_str(), 0, nullptr,
                            REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, nullptr, &hKey, &disp) == ERROR_SUCCESS) {
            RegSetValueExW(hKey, L"DohTemplate", 0, REG_SZ,
                reinterpret_cast<const BYTE*>(doh_template.c_str()),
                static_cast<DWORD>((doh_template.length() + 1) * sizeof(wchar_t)));

            // DohFlags: 0x1 = Encrypted preferred with UDP fallback (matching netsh udpfallback=yes)
            ULONGLONG dohFlags = 0x1;
            RegSetValueExW(hKey, L"DohFlags", 0, REG_QWORD,
                reinterpret_cast<const BYTE*>(&dohFlags), sizeof(dohFlags));

            RegCloseKey(hKey);
            log_info(L"Registered interface DoH for adapter " + guid_str + L" server " + ip + L" -> " + doh_template);
        }
    }
}

bool DnsManager::reset_to_dhcp(const NetworkAdapterInfo& adapter) {
    if (!is_elevated()) {
        set_last_error(L"Для сброса параметров DNS требуются права Администратора.");
        return false;
    }

    bool modern_api_ok = false;

    // 1. Try modern SetInterfaceDnsSettings API for IPv4 and IPv6
    auto pfnSet = reinterpret_cast<PFN_SetInterfaceDnsSettings>(m_pfnSetInterfaceDnsSettings);
    if (pfnSet && adapter.guid != GUID_NULL) {
        // Reset IPv4
        DNS_INTERFACE_SETTINGS settings = {};
        settings.Version = DNS_INTERFACE_SETTINGS_VERSION1;
        settings.Flags = DNS_SETTING_NAMESERVER | DNS_SETTING_PROFILE_NAMESERVER;
        settings.NameServer = const_cast<PWSTR>(L"");
        settings.ProfileNameServer = const_cast<PWSTR>(L"");

        DWORD status = pfnSet(adapter.guid, &settings);
        if (status == ERROR_SUCCESS) {
            modern_api_ok = true;
            log_info(L"Successfully reset IPv4 DNS to DHCP via SetInterfaceDnsSettings on " + adapter.name);
        } else {
            log_info(L"SetInterfaceDnsSettings reset IPv4 returned: " + format_win32_error(status));
        }

        // Reset IPv6
        DNS_INTERFACE_SETTINGS settings_v6 = {};
        settings_v6.Version = DNS_INTERFACE_SETTINGS_VERSION1;
        settings_v6.Flags = DNS_SETTING_IPV6 | DNS_SETTING_NAMESERVER | DNS_SETTING_PROFILE_NAMESERVER;
        settings_v6.NameServer = const_cast<PWSTR>(L"");
        settings_v6.ProfileNameServer = const_cast<PWSTR>(L"");

        DWORD status_v6 = pfnSet(adapter.guid, &settings_v6);
        if (status_v6 == ERROR_SUCCESS) {
            log_info(L"Successfully reset IPv6 DNS to DHCP via SetInterfaceDnsSettings on " + adapter.name);
        }
    }

    // 2. Netsh reset for IPv4 and IPv6
    bool netsh_ok = false;
    std::wstring netsh_path = get_system32_path() + L"\\netsh.exe";

    if (adapter.if_index > 0) {
        std::wstring cmd_ipv4 = netsh_path + L" interface ipv4 set dnsservers name=" + 
            std::to_wstring(adapter.if_index) + L" source=dhcp";
        if (execute_hidden_process(cmd_ipv4)) {
            netsh_ok = true;
        }
        std::wstring cmd_ipv6 = netsh_path + L" interface ipv6 set dnsservers name=" + 
            std::to_wstring(adapter.if_index) + L" source=dhcp";
        execute_hidden_process(cmd_ipv6);
    }

    if (!netsh_ok && !adapter.name.empty() && adapter.name.find(L'"') == std::wstring::npos) {
        std::wstring cmd_ipv4 = netsh_path + L" interface ipv4 set dnsservers name=\"" + 
            adapter.name + L"\" source=dhcp";
        if (execute_hidden_process(cmd_ipv4)) {
            netsh_ok = true;
        }
        std::wstring cmd_ipv6 = netsh_path + L" interface ipv6 set dnsservers name=\"" + 
            adapter.name + L"\" source=dhcp";
        execute_hidden_process(cmd_ipv6);
    }

    // 3. Clear Registry static NameServer entries for IPv4 and IPv6
    if (!adapter.id.empty()) {
        std::wstring subkey = L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters\\Interfaces\\" + adapter.id;
        HKEY hKey = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, subkey.c_str(), 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
            const wchar_t emptyStr = L'\0';
            RegSetValueExW(hKey, L"NameServer", 0, REG_SZ,
                reinterpret_cast<const BYTE*>(&emptyStr), sizeof(wchar_t));
            RegCloseKey(hKey);
        }

        std::wstring subkey6 = L"SYSTEM\\CurrentControlSet\\Services\\Tcpip6\\Parameters\\Interfaces\\" + adapter.id;
        HKEY hKey6 = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, subkey6.c_str(), 0, KEY_SET_VALUE, &hKey6) == ERROR_SUCCESS) {
            const wchar_t emptyStr = L'\0';
            RegSetValueExW(hKey6, L"NameServer", 0, REG_SZ,
                reinterpret_cast<const BYTE*>(&emptyStr), sizeof(wchar_t));
            RegCloseKey(hKey6);
        }
    }

    // 4. Clear interface-specific DoH configuration
    std::wstring guid_str = adapter.id;
    if (guid_str.empty() && adapter.guid != GUID_NULL) {
        guid_str = guid_to_string(adapter.guid);
    }
    if (!guid_str.empty()) {
        configure_interface_doh(guid_str, {}, L"");
    }

    // 5. Clean up application-managed DohWellKnownServers and netsh encryption
    auto managed_servers = get_managed_doh_servers();
    for (const auto& s : managed_servers) {
        std::wstring wellKnownKey = L"SYSTEM\\CurrentControlSet\\Services\\Dnscache\\Parameters\\DohWellKnownServers\\" + s;
        RegDeleteTreeW(HKEY_LOCAL_MACHINE, wellKnownKey.c_str());

        std::wstring cmd_del = netsh_path + L" dns delete encryption server=" + s;
        execute_hidden_process(cmd_del);

        unmark_doh_server_managed(s);
        log_info(L"Removed managed DoH server registration for " + s);
    }

    // 6. Restore original EnableAutoDoh setting
    restore_enable_autodoh();

    if (!modern_api_ok && !netsh_ok) {
        set_last_error(L"Не удалось переключить адаптер на DHCP. Проверьте права Администратора.");
        return false;
    }

    // Flush DNS resolver cache
    flush_dns_cache();
    return true;
}

bool DnsManager::flush_dns_cache() {
    bool flushed_api = false;

    // 1. Direct in-process call to DnsFlushResolverCache in dnsapi.dll
    auto pfnFlush = reinterpret_cast<PFN_DnsFlushResolverCache>(m_pfnDnsFlushResolverCache);
    if (pfnFlush) {
        flushed_api = (pfnFlush() != FALSE);
        if (flushed_api) {
            log_info(L"DNS resolver cache successfully flushed via dnsapi.dll::DnsFlushResolverCache.");
        }
    }

    // 2. Also invoke ipconfig.exe /flushdns to clear system-wide DNS client cache
    bool flushed_ipconfig = execute_hidden_process(get_system32_path() + L"\\ipconfig.exe /flushdns");
    if (flushed_ipconfig) {
        log_info(L"ipconfig.exe /flushdns completed successfully.");
    }

    return flushed_api || flushed_ipconfig;
}

} // namespace quickdns
