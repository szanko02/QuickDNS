#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <guiddef.h>

#include <string>
#include <vector>
#include <optional>
#include <string_view>

// Auto-link required Windows networking libraries under MSVC/Clang
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "dnsapi.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "ole32.lib")

namespace quickdns {

/**
 * @brief Represents network adapter configuration and DNS state.
 */
struct NetworkAdapterInfo {
    std::wstring id;                     // Adapter GUID/Interface Luid string or friendly ID
    NET_LUID luid{};                     // Network Interface LUID
    GUID guid{};                         // Adapter Interface GUID
    DWORD if_index{0};                   // Numeric interface index (e.g. 7)
    std::wstring name;                   // Friendly name (e.g. L"Ethernet", L"Wi-Fi")
    std::wstring description;            // Hardware description (e.g. L"Intel(R) Wi-Fi 6 AX200")
    bool is_connected{false};            // Operational status (true if connected/up)
    bool is_dhcp_dns{false};             // True if DNS is configured via DHCP, false if static
    std::vector<std::wstring> dns_ipv4;  // Configured IPv4 DNS server addresses
    std::vector<std::wstring> dns_ipv6;  // Configured IPv6 DNS server addresses
};

/**
 * @brief Manages Windows network adapters and DNS configuration.
 */
class DnsManager {
public:
    DnsManager();
    ~DnsManager();

    // Disable copy for clean resource management (move is allowed)
    DnsManager(const DnsManager&) = delete;
    DnsManager& operator=(const DnsManager&) = delete;
    DnsManager(DnsManager&&) noexcept = default;
    DnsManager& operator=(DnsManager&&) noexcept = default;

    /**
     * @brief Enumerates physical and active network adapters.
     * Filters out loopback, tunnel, and virtual interfaces where possible.
     * Prioritizes adapters with type IF_TYPE_ETHERNET_CSMACD or IF_TYPE_IEEE80211.
     * 
     * @return List of NetworkAdapterInfo structures.
     */
    std::vector<NetworkAdapterInfo> get_network_adapters();

    /**
     * @brief Identifies the active internet connection adapter.
     * Evaluates route metrics to public internet hosts (e.g. 8.8.8.8) and falls back
     * to connected interfaces with default gateways.
     * 
     * @return The active NetworkAdapterInfo if available, or std::nullopt.
     */
    std::optional<NetworkAdapterInfo> get_primary_adapter();

    /**
     * @brief Configures static DNS servers on the specified adapter.
     * Uses SetInterfaceDnsSettings (Windows 10 1809+ / Windows 11) with fallback to netsh.
     * Automatically flushes the DNS resolver cache upon successful application.
     * 
     * @param adapter Target network adapter.
     * @param primary_dns Primary DNS IP address (IPv4 or IPv6).
     * @param secondary_dns Optional secondary DNS IP address.
     * @return True on success, false on error.
     */
    bool set_static_dns(const NetworkAdapterInfo& adapter, 
                        const std::wstring& primary_dns, 
                        const std::wstring& secondary_dns = L"");

    /**
     * @brief Resets the DNS configuration of the specified adapter back to DHCP.
     * Uses SetInterfaceDnsSettings with fallback to netsh, then flushes the DNS cache.
     * 
     * @param adapter Target network adapter.
     * @return True on success, false on error.
     */
    bool reset_to_dhcp(const NetworkAdapterInfo& adapter);

    /**
     * @brief Flushes the Windows DNS resolver cache.
     * Dynamically invokes DnsFlushResolverCache from dnsapi.dll with fallback to ipconfig /flushdns.
     * 
     * @return True on success, false on failure.
     */
    bool flush_dns_cache();

    /**
     * @brief Validates if an IP string is a valid IPv4 address in dotted-decimal format.
     */
    static bool is_valid_ipv4(const std::wstring& ip);
    static bool is_valid_ipv4(std::string_view ip);

    /**
     * @brief Validates if an IP string is a valid IPv6 address in standard hex colon notation.
     */
    static bool is_valid_ipv6(const std::wstring& ip);
    static bool is_valid_ipv6(std::string_view ip);

    /**
     * @brief Validates if an IP string is either a valid IPv4 or valid IPv6 address.
     */
    static bool is_valid_ip(const std::wstring& ip);

    /**
     * @brief Converts a GUID to its standard string representation: {XXXXXXXX-XXXX-...}.
     */
    static std::wstring guid_to_string(const GUID& guid);

    /**
     * @brief Parses a GUID string representation into a GUID structure.
     */
    static GUID string_to_guid(const std::wstring& str);

    /**
     * @brief Checks whether the current process has elevated Administrator privileges.
     */
    static bool is_elevated();

    /**
     * @brief Returns the last recorded error message or Win32 translation.
     */
    std::wstring get_last_error() const;

private:
    void log_info(const std::wstring& msg) const;
    void log_error(const std::wstring& msg) const;
    void set_last_error(const std::wstring& msg) const;

    static bool is_virtual_adapter(const IP_ADAPTER_ADDRESSES* adapter);
    static bool execute_hidden_process(const std::wstring& command);
    static std::wstring format_win32_error(DWORD error_code);
    static bool query_is_dhcp_dns(const GUID& guid, const std::wstring& guid_str);

    mutable std::wstring last_error_;
};

} // namespace quickdns

// Global namespace aliases for compatibility
using quickdns::NetworkAdapterInfo;
using quickdns::DnsManager;
