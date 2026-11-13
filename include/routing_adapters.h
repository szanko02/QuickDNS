#pragma once
#include <winsock2.h>
#include <iphlpapi.h>
#include <string>
#include <vector>

namespace quickdns {
struct RoutingAdapter {
    std::wstring id;
    std::wstring name;
    bool connected = false;
    unsigned long ipv4_index = 0;
    unsigned long ipv6_index = 0;
    std::vector<std::wstring> ipv4_addresses;
    std::vector<std::wstring> ipv6_addresses;
    std::vector<std::wstring> ipv4_gateways;
    std::vector<std::wstring> ipv6_gateways;
    std::vector<std::wstring> ipv4_dns;
    std::vector<std::wstring> ipv6_dns;
};

std::vector<RoutingAdapter> routing_adapters_from_windows(const IP_ADAPTER_ADDRESSES* first);
std::vector<RoutingAdapter> enumerate_routing_adapters();
}
