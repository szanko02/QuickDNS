#include "routing_adapters.h"
#include <ws2tcpip.h>
#include <algorithm>
#include <cstddef>
#include <utility>

namespace quickdns {
namespace {
void append_address(const SOCKET_ADDRESS& address, std::vector<std::wstring>& v4,
                    std::vector<std::wstring>& v6) {
    if (!address.lpSockaddr) return;
    wchar_t buffer[INET6_ADDRSTRLEN]{};
    if (address.lpSockaddr->sa_family == AF_INET && address.iSockaddrLength >= sizeof(sockaddr_in)) {
        const auto* a = reinterpret_cast<const sockaddr_in*>(address.lpSockaddr);
        if (InetNtopW(AF_INET, const_cast<IN_ADDR*>(&a->sin_addr), buffer, INET6_ADDRSTRLEN)) {
            if (std::find(v4.begin(), v4.end(), buffer) == v4.end()) v4.emplace_back(buffer);
        }
    } else if (address.lpSockaddr->sa_family == AF_INET6 && address.iSockaddrLength >= sizeof(sockaddr_in6)) {
        const auto* a = reinterpret_cast<const sockaddr_in6*>(address.lpSockaddr);
        if (InetNtopW(AF_INET6, const_cast<IN6_ADDR*>(&a->sin6_addr), buffer, INET6_ADDRSTRLEN)) {
            std::wstring value = buffer;
            if (a->sin6_scope_id) value += L"%" + std::to_wstring(a->sin6_scope_id);
            if (std::find(v6.begin(), v6.end(), value) == v6.end()) v6.push_back(std::move(value));
        }
    }
}
}

std::vector<RoutingAdapter> routing_adapters_from_windows(const IP_ADAPTER_ADDRESSES* first) {
    std::vector<RoutingAdapter> result;
    for (auto* current = first; current; current = current->Next) {
        if (current->IfType == IF_TYPE_SOFTWARE_LOOPBACK || !current->AdapterName || !*current->AdapterName) continue;
        RoutingAdapter adapter;
        for (const char* p = current->AdapterName; *p; ++p) adapter.id += static_cast<unsigned char>(*p);
        adapter.name = current->FriendlyName && *current->FriendlyName ? current->FriendlyName : adapter.id;
        adapter.connected = current->OperStatus == IfOperStatusUp;
        adapter.ipv4_index = current->IfIndex;
        adapter.ipv6_index = current->Ipv6IfIndex;
        for (auto* a = current->FirstUnicastAddress; a; a = a->Next)
            append_address(a->Address, adapter.ipv4_addresses, adapter.ipv6_addresses);
        for (auto* a = current->FirstGatewayAddress; a; a = a->Next)
            append_address(a->Address, adapter.ipv4_gateways, adapter.ipv6_gateways);
        for (auto* a = current->FirstDnsServerAddress; a; a = a->Next)
            append_address(a->Address, adapter.ipv4_dns, adapter.ipv6_dns);
        result.push_back(std::move(adapter));
    }
    return result;
}

std::vector<RoutingAdapter> enumerate_routing_adapters() {
    ULONG size = 16 * 1024;
    const ULONG flags = GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST;
    for (int attempt = 0; attempt < 3; ++attempt) {
        std::vector<std::byte> buffer(size);
        auto* first = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        const ULONG result = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, first, &size);
        if (result == NO_ERROR) return routing_adapters_from_windows(first);
        if (result != ERROR_BUFFER_OVERFLOW) { SetLastError(result); return {}; }
    }
    SetLastError(ERROR_BUFFER_OVERFLOW);
    return {};
}
}
