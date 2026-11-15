#include "routing_adapters.h"
#include <ws2tcpip.h>
#include <stdexcept>
#include <iostream>

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int main() {
    try {
        IP_ADAPTER_ADDRESSES loop{}, adapter{};
        loop.IfType = IF_TYPE_SOFTWARE_LOOPBACK;
        loop.Next = &adapter;
        adapter.IfType = IF_TYPE_TUNNEL;
        adapter.AdapterName = const_cast<char*>("{01234567-89AB-CDEF-0123-456789ABCDEF}");
        adapter.FriendlyName = const_cast<wchar_t*>(L"Disconnected tunnel");
        adapter.OperStatus = IfOperStatusDown;
        adapter.IfIndex = 12;
        adapter.Ipv6IfIndex = 17;
        sockaddr_in address{};
        address.sin_family = AF_INET;
        InetPtonW(AF_INET, L"192.0.2.25", &address.sin_addr);
        IP_ADAPTER_UNICAST_ADDRESS unicast{};
        unicast.Address.lpSockaddr = reinterpret_cast<sockaddr*>(&address);
        unicast.Address.iSockaddrLength = sizeof(address);
        adapter.FirstUnicastAddress = &unicast;
        sockaddr_in6 address6{};
        address6.sin6_family = AF_INET6;
        address6.sin6_scope_id = 17;
        InetPtonW(AF_INET6, L"fe80::42", &address6.sin6_addr);
        IP_ADAPTER_UNICAST_ADDRESS unicast6{};
        unicast6.Address.lpSockaddr = reinterpret_cast<sockaddr*>(&address6);
        unicast6.Address.iSockaddrLength = sizeof(address6);
        unicast.Next = &unicast6;
        sockaddr_in gateway{};
        gateway.sin_family = AF_INET;
        InetPtonW(AF_INET, L"192.0.2.1", &gateway.sin_addr);
        IP_ADAPTER_GATEWAY_ADDRESS gw{};
        gw.Address.lpSockaddr = reinterpret_cast<sockaddr*>(&gateway);
        gw.Address.iSockaddrLength = sizeof(gateway);
        adapter.FirstGatewayAddress = &gw;
        sockaddr_in dns{};
        dns.sin_family = AF_INET;
        InetPtonW(AF_INET, L"192.0.2.53", &dns.sin_addr);
        IP_ADAPTER_DNS_SERVER_ADDRESS dns_entry{};
        dns_entry.Address.lpSockaddr = reinterpret_cast<sockaddr*>(&dns);
        dns_entry.Address.iSockaddrLength = sizeof(dns);
        adapter.FirstDnsServerAddress = &dns_entry;
        const auto result = quickdns::routing_adapters_from_windows(&loop);
        require(result.size() == 1, "tunnel adapter omitted or loopback included");
        require(result[0].id == L"{01234567-89AB-CDEF-0123-456789ABCDEF}", "adapter identity lost");
        require(result[0].name == L"Disconnected tunnel" && !result[0].connected, "adapter status lost");
        require(result[0].ipv4_index == 12 && result[0].ipv6_index == 17, "interface indices lost");
        require(result[0].ipv4_addresses == std::vector<std::wstring>{L"192.0.2.25"}, "IPv4 address lost");
        require(result[0].ipv6_addresses == std::vector<std::wstring>{L"fe80::42%17"}, "IPv6 address or scope lost");
        require(result[0].ipv4_gateways == std::vector<std::wstring>{L"192.0.2.1"}, "IPv4 gateway lost");
        require(result[0].ipv4_dns == std::vector<std::wstring>{L"192.0.2.53"}, "adapter DNS lost");
        require(quickdns::routing_adapters_from_windows(nullptr).empty(), "empty adapter list failed");
        adapter.FriendlyName = const_cast<wchar_t*>(L"");
        const auto unnamed = quickdns::routing_adapters_from_windows(&adapter);
        require(unnamed[0].name == unnamed[0].id, "unnamed adapter has no usable label");
        adapter.AdapterName = const_cast<char*>("");
        require(quickdns::routing_adapters_from_windows(&adapter).empty(), "adapter without stable ID accepted");
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << '\n';
        return 1;
    }
}
