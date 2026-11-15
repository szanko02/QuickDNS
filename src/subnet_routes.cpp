#include "subnet_routes.h"
#include <ws2tcpip.h>
#include <objbase.h>
#include <algorithm>

namespace quickdns {
namespace {
bool number(const std::wstring& text, ULONG limit, ULONG& value) {
    value = 0;
    if (text.empty()) return false;
    for (wchar_t c : text) {
        if (c < L'0' || c > L'9') return false;
        const auto digit = static_cast<ULONG>(c - L'0');
        if (value > limit / 10 || (value == limit / 10 && digit > limit % 10)) return false;
        value = value * 10 + digit;
    }
    return true;
}

DWORD set_destination(const std::wstring& cidr, IP_ADDRESS_PREFIX& prefix) {
    const auto slash = cidr.find(L'/');
    if (slash == std::wstring::npos) return ERROR_INVALID_PARAMETER;
    const auto address = cidr.substr(0, slash);
    const bool ipv6 = address.find(L':') != std::wstring::npos;
    prefix.Prefix.si_family = ipv6 ? AF_INET6 : AF_INET;
    auto* bytes = ipv6 ? reinterpret_cast<BYTE*>(&prefix.Prefix.Ipv6.sin6_addr)
                       : reinterpret_cast<BYTE*>(&prefix.Prefix.Ipv4.sin_addr);
    const ULONG count = ipv6 ? 16 : 4;
    ULONG length = 0;
    if (InetPtonW(prefix.Prefix.si_family, address.c_str(), bytes) != 1 ||
        !number(cidr.substr(slash + 1), count * 8, length)) return ERROR_INVALID_PARAMETER;
    prefix.PrefixLength = static_cast<UINT8>(length);
    for (ULONG i = 0; i < count; ++i) {
        const ULONG bits = length > i * 8 ? std::min<ULONG>(length - i * 8, 8) : 0;
        bytes[i] &= static_cast<BYTE>(0xff << (8 - bits));
    }
    return NO_ERROR;
}

DWORD set_gateway(const std::wstring& gateway, ULONG index, SOCKADDR_INET& hop) {
    if (gateway.empty()) return NO_ERROR;
    if (hop.si_family == AF_INET)
        return InetPtonW(AF_INET, gateway.c_str(), &hop.Ipv4.sin_addr) == 1 ? NO_ERROR : ERROR_INVALID_PARAMETER;

    const auto percent = gateway.find(L'%');
    if (InetPtonW(AF_INET6, gateway.substr(0, percent).c_str(), &hop.Ipv6.sin6_addr) != 1)
        return ERROR_INVALID_PARAMETER;
    if (percent != std::wstring::npos) {
        ULONG scope = 0;
        if (!number(gateway.substr(percent + 1), MAXDWORD, scope) || scope != index)
            return ERROR_INVALID_PARAMETER;
    }
    if (IN6_IS_ADDR_LINKLOCAL(&hop.Ipv6.sin6_addr)) hop.Ipv6.sin6_scope_id = index;
    else if (percent != std::wstring::npos) return ERROR_INVALID_PARAMETER;
    return NO_ERROR;
}

std::wstring format_address(const SOCKADDR_INET& address) {
    wchar_t buffer[INET6_ADDRSTRLEN]{};
    const bool ipv6 = address.si_family == AF_INET6;
    const void* bytes = ipv6 ? static_cast<const void*>(&address.Ipv6.sin6_addr)
                            : static_cast<const void*>(&address.Ipv4.sin_addr);
    InetNtopW(address.si_family, bytes, buffer, INET6_ADDRSTRLEN);
    return buffer;
}
}

SubnetRoutes::~SubnetRoutes() {
    for (size_t i = routes_.size(); i != 0; --i) remove(i - 1);
}

DWORD SubnetRoutes::add(const std::wstring& cidr, const std::wstring& gateway, const RoutingAdapter& adapter) {
    Route route{{}, adapter.name};
    auto& row = route.row;
    InitializeIpForwardEntry(&row);
    DWORD result = set_destination(cidr, row.DestinationPrefix);
    if (result != NO_ERROR) return result;
    row.NextHop.si_family = row.DestinationPrefix.Prefix.si_family;
    const bool ipv6 = row.NextHop.si_family == AF_INET6;
    GUID guid{};
    if (FAILED(CLSIDFromString(adapter.id.c_str(), &guid))) return ERROR_INVALID_PARAMETER;
    result = ConvertInterfaceGuidToLuid(&guid, &row.InterfaceLuid);
    if (result != NO_ERROR) return result;
    MIB_IF_ROW2 interface_row{};
    interface_row.InterfaceLuid = row.InterfaceLuid;
    result = GetIfEntry2(&interface_row);
    if (result != NO_ERROR) return result;
    if (interface_row.OperStatus != IfOperStatusUp) return ERROR_NETWORK_UNREACHABLE;
    result = set_gateway(gateway, interface_row.InterfaceIndex, row.NextHop);
    if (result != NO_ERROR) return result;
    MIB_IPINTERFACE_ROW ip_interface{};
    InitializeIpInterfaceEntry(&ip_interface);
    ip_interface.InterfaceLuid = row.InterfaceLuid;
    ip_interface.Family = ipv6 ? AF_INET6 : AF_INET;
    result = GetIpInterfaceEntry(&ip_interface);
    if (result != NO_ERROR) return result;
    if (!ip_interface.Connected) return ERROR_NETWORK_UNREACHABLE;

    row.Metric = 0;
    row.Protocol = MIB_IPPROTO_NETMGMT;
    routes_.push_back(std::move(route));
    result = CreateIpForwardEntry2(&routes_.back().row);
    if (result != NO_ERROR) routes_.pop_back();
    return result;
}

DWORD SubnetRoutes::status(size_t index) const {
    if (index >= routes_.size()) return ERROR_INVALID_PARAMETER;
    const auto& installed = routes_[index].row;
    auto current = installed;
    const DWORD result = GetIpForwardEntry2(&current);
    if (result != NO_ERROR) return result;
    return current.Protocol == installed.Protocol && current.Metric == installed.Metric
        ? NO_ERROR : ERROR_REVISION_MISMATCH;
}

DWORD SubnetRoutes::remove(size_t index) {
    DWORD result = status(index);
    if (result == NO_ERROR) result = DeleteIpForwardEntry2(&routes_[index].row);
    if (result == NO_ERROR || result == ERROR_NOT_FOUND || result == ERROR_REVISION_MISMATCH)
        routes_.erase(routes_.begin() + index);
    return result;
}

std::wstring subnet_route_label(const SubnetRoutes::Route& route) {
    const auto& row = route.row;
    const auto& prefix = row.DestinationPrefix;
    return format_address(prefix.Prefix) + L"/" + std::to_wstring(prefix.PrefixLength) +
        L"  →  " + route.adapter_name + L"  (" + format_address(row.NextHop) + L")";
}
}
