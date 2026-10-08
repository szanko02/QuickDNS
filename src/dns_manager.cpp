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

// Dynamic function pointer signatures for Windows 10 1809+ / Windows 11 DNS settings API
#ifndef DNS_INTERFACE_SETTINGS_VERSION1
#define DNS_INTERFACE_SETTINGS_VERSION1 1
#define DNS_SETTING_NAMESERVER 0x0001

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
    ULONG ProfileNameServer;
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

} // anonymous namespace

namespace quickdns {

DnsManager::DnsManager() {
    ensure_winsock();
}

DnsManager::~DnsManager() = default;

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

bool DnsManager::query_is_dhcp_dns(const GUID& guid, const std::wstring& guid_str) {
    // Attempt 1: Modern Windows 10/11 GetInterfaceDnsSettings API
    HMODULE hIpHlp = GetModuleHandleW(L"iphlpapi.dll");
    if (!hIpHlp) {
        hIpHlp = LoadLibraryW(L"iphlpapi.dll");
    }

    if (hIpHlp) {
        auto pfnGet = reinterpret_cast<PFN_GetInterfaceDnsSettings>(
            GetProcAddress(hIpHlp, "GetInterfaceDnsSettings"));
        auto pfnFree = reinterpret_cast<PFN_FreeInterfaceDnsSettings>(
            GetProcAddress(hIpHlp, "FreeInterfaceDnsSettings"));

        if (pfnGet) {
            DNS_INTERFACE_SETTINGS settings = {};
            settings.Version = DNS_INTERFACE_SETTINGS_VERSION1;
            DWORD err = pfnGet(guid, &settings);
            if (err == ERROR_SUCCESS) {
                bool has_static_nameserver = false;
                if ((settings.Flags & DNS_SETTING_NAMESERVER) && settings.NameServer) {
                    for (const wchar_t* p = settings.NameServer; *p; ++p) {
                        if (!iswspace(*p)) {
                            has_static_nameserver = true;
                            break;
                        }
                    }
                }
                if (pfnFree) {
                    pfnFree(&settings);
                }
                return !has_static_nameserver;
            }
        }
    }

    // Attempt 2: Direct Registry Query fallback
    // HKLM\SYSTEM\CurrentControlSet\Services\Tcpip\Parameters\Interfaces\{GUID}
    std::wstring subkey = L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters\\Interfaces\\" + guid_str;
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, subkey.c_str(), 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        wchar_t nameServer[512] = {0};
        DWORD dataSize = sizeof(nameServer);
        DWORD type = 0;
        LONG r = RegQueryValueExW(hKey, L"NameServer", nullptr, &type, reinterpret_cast<LPBYTE>(nameServer), &dataSize);
        RegCloseKey(hKey);

        if (r == ERROR_SUCCESS && (type == REG_SZ || type == REG_MULTI_SZ)) {
            for (const wchar_t* p = nameServer; *p; ++p) {
                if (!iswspace(*p)) {
                    return false; // Found static DNS in registry
                }
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

        // Extract DNS servers
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

        // Determine DHCP vs Static DNS configuration
        item.info.is_dhcp_dns = query_is_dhcp_dns(item.info.guid, item.info.id);

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

bool DnsManager::set_static_dns(const NetworkAdapterInfo& adapter, 
                                const std::wstring& primary_dns, 
                                const std::wstring& secondary_dns) {
    if (primary_dns.empty()) {
        set_last_error(L"Primary DNS address cannot be empty.");
        return false;
    }

    if (!is_valid_ip(primary_dns)) {
        set_last_error(L"Invalid primary DNS address: " + primary_dns);
        return false;
    }

    if (!secondary_dns.empty() && !is_valid_ip(secondary_dns)) {
        set_last_error(L"Invalid secondary DNS address: " + secondary_dns);
        return false;
    }

    bool modern_api_ok = false;

    // 1. Try modern SetInterfaceDnsSettings API (Windows 10 1809+ / Windows 11)
    HMODULE hIpHlp = LoadLibraryW(L"iphlpapi.dll");
    if (hIpHlp) {
        auto pfnSet = reinterpret_cast<PFN_SetInterfaceDnsSettings>(
            GetProcAddress(hIpHlp, "SetInterfaceDnsSettings"));

        if (pfnSet && adapter.guid != GUID_NULL) {
            std::wstring servers = primary_dns;
            if (!secondary_dns.empty()) {
                servers += L"," + secondary_dns;
            }

            DNS_INTERFACE_SETTINGS settings = {};
            settings.Version = DNS_INTERFACE_SETTINGS_VERSION1;
            settings.Flags = DNS_SETTING_NAMESERVER;
            settings.NameServer = const_cast<PWSTR>(servers.c_str());

            DWORD status = pfnSet(adapter.guid, &settings);
            if (status == ERROR_SUCCESS) {
                modern_api_ok = true;
                log_info(L"Successfully applied static DNS via SetInterfaceDnsSettings on " + adapter.name);
            } else {
                log_info(L"SetInterfaceDnsSettings returned error: " + format_win32_error(status) + L", falling back to netsh.");
            }
        }
        FreeLibrary(hIpHlp);
    }

    // 2. Fallback to netsh if modern API is unavailable or failed
    if (!modern_api_ok) {
        std::wstring ip_family = is_valid_ipv4(primary_dns) ? L"ipv4" : L"ipv6";

        std::wstring cmd_primary = L"netsh.exe interface " + ip_family + 
            L" set dnsservers name=\"" + adapter.name + 
            L"\" source=static address=" + primary_dns + 
            L" register=none validate=no";

        log_info(L"Executing fallback: " + cmd_primary);
        if (!execute_hidden_process(cmd_primary)) {
            set_last_error(L"Netsh failed to set primary DNS on adapter: " + adapter.name);
            return false;
        }

        if (!secondary_dns.empty()) {
            std::wstring sec_family = is_valid_ipv4(secondary_dns) ? L"ipv4" : L"ipv6";
            std::wstring cmd_secondary = L"netsh.exe interface " + sec_family + 
                L" add dnsservers name=\"" + adapter.name + 
                L"\" address=" + secondary_dns + 
                L" index=2 validate=no";

            log_info(L"Executing fallback: " + cmd_secondary);
            execute_hidden_process(cmd_secondary);
        }
    }

    // Flush DNS resolver cache
    flush_dns_cache();
    return true;
}

bool DnsManager::reset_to_dhcp(const NetworkAdapterInfo& adapter) {
    bool modern_api_ok = false;

    // 1. Try modern SetInterfaceDnsSettings API
    HMODULE hIpHlp = LoadLibraryW(L"iphlpapi.dll");
    if (hIpHlp) {
        auto pfnSet = reinterpret_cast<PFN_SetInterfaceDnsSettings>(
            GetProcAddress(hIpHlp, "SetInterfaceDnsSettings"));

        if (pfnSet && adapter.guid != GUID_NULL) {
            DNS_INTERFACE_SETTINGS settings = {};
            settings.Version = DNS_INTERFACE_SETTINGS_VERSION1;
            settings.Flags = DNS_SETTING_NAMESERVER;
            settings.NameServer = nullptr; // Setting NameServer to null clears static DNS and restores DHCP

            DWORD status = pfnSet(adapter.guid, &settings);
            if (status == ERROR_SUCCESS) {
                modern_api_ok = true;
                log_info(L"Successfully reset DNS to DHCP via SetInterfaceDnsSettings on " + adapter.name);
            } else {
                log_info(L"SetInterfaceDnsSettings reset returned: " + format_win32_error(status) + L", falling back to netsh.");
            }
        }
        FreeLibrary(hIpHlp);
    }

    // 2. Fallback to netsh
    if (!modern_api_ok) {
        std::wstring cmd_ipv4 = L"netsh.exe interface ipv4 set dnsservers name=\"" + adapter.name + L"\" source=dhcp";
        log_info(L"Executing fallback: " + cmd_ipv4);
        bool ok4 = execute_hidden_process(cmd_ipv4);

        std::wstring cmd_ipv6 = L"netsh.exe interface ipv6 set dnsservers name=\"" + adapter.name + L"\" source=dhcp";
        execute_hidden_process(cmd_ipv6);

        if (!ok4) {
            set_last_error(L"Netsh failed to reset DNS to DHCP on adapter: " + adapter.name);
            return false;
        }
    }

    // Flush DNS resolver cache
    flush_dns_cache();
    return true;
}

bool DnsManager::flush_dns_cache() {
    bool flushed = false;

    // 1. Invoke DnsFlushResolverCache from dnsapi.dll
    HMODULE hDnsApi = LoadLibraryW(L"dnsapi.dll");
    if (hDnsApi) {
        auto pfnFlush = reinterpret_cast<PFN_DnsFlushResolverCache>(
            GetProcAddress(hDnsApi, "DnsFlushResolverCache"));
        if (pfnFlush) {
            flushed = (pfnFlush() != FALSE);
            if (flushed) {
                log_info(L"DNS resolver cache successfully flushed via dnsapi.dll::DnsFlushResolverCache.");
            }
        }
        FreeLibrary(hDnsApi);
    }

    // 2. Fallback to ipconfig /flushdns if in-process call failed
    if (!flushed) {
        log_info(L"DnsFlushResolverCache unavailable or returned false; invoking ipconfig /flushdns.");
        flushed = execute_hidden_process(L"ipconfig.exe /flushdns");
    }

    return flushed;
}

} // namespace quickdns
