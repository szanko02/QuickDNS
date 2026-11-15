#include "primary_adapter_metric_windows.h"
#include <memory>
namespace quickdns {
ULONG WindowsMetricApi::routes(ADDRESS_FAMILY f, MIB_IPFORWARD_TABLE2** p) { return GetIpForwardTable2(f,p); }
void WindowsMetricApi::free_routes(MIB_IPFORWARD_TABLE2* p) { FreeMibTable(p); }
ULONG WindowsMetricApi::get(MIB_IPINTERFACE_ROW* row) { return GetIpInterfaceEntry(row); }
ULONG WindowsMetricApi::set(MIB_IPINTERFACE_ROW* row) { return SetIpInterfaceEntry(row); }
namespace {
ADDRESS_FAMILY address_family(IpFamily family) { return family == IpFamily::V4 ? AF_INET : AF_INET6; }
MIB_IPINTERFACE_ROW make_interface(IpFamily family, std::uint64_t luid) {
    MIB_IPINTERFACE_ROW row{};
    InitializeIpInterfaceEntry(&row);
    row.Family = address_family(family);
    row.InterfaceLuid.Value = luid;
    return row;
}
}
bool WindowsMetricSystem::default_routes(IpFamily family, std::vector<DefaultRouteMetric>& out) {
    MIB_IPFORWARD_TABLE2* raw = nullptr;
    error_ = api_.routes(address_family(family), &raw);
    if (error_ != NO_ERROR) return false;
    if (!raw) { error_ = ERROR_INVALID_DATA; return false; }
    auto deleter = [this](MIB_IPFORWARD_TABLE2* table) { api_.free_routes(table); };
    std::unique_ptr<MIB_IPFORWARD_TABLE2, decltype(deleter)> table(raw, deleter);
    std::vector<DefaultRouteMetric> result;
    for (ULONG i = 0; i < table->NumEntries; ++i) {
        const auto& route = table->Table[i];
        if (route.DestinationPrefix.PrefixLength != 0 ||
            route.DestinationPrefix.Prefix.si_family != address_family(family) || route.ValidLifetime == 0) continue;
        auto row = make_interface(family, route.InterfaceLuid.Value);
        error_ = api_.get(&row);
        if (error_ != NO_ERROR) return false;
        if (!row.Connected || row.DisableDefaultRoutes) continue;
        result.push_back({route.InterfaceLuid.Value, route.Metric,
                          {row.UseAutomaticMetric != FALSE, row.Metric}});
    }
    out = std::move(result);
    error_ = NO_ERROR;
    return true;
}
bool WindowsMetricSystem::read(IpFamily family, std::uint64_t luid, InterfaceMetric& out) {
    auto row = make_interface(family, luid);
    error_ = api_.get(&row);
    if (error_ != NO_ERROR) return false;
    out = {row.UseAutomaticMetric != FALSE, row.Metric};
    return true;
}
bool WindowsMetricSystem::write(IpFamily family, std::uint64_t luid, const InterfaceMetric& value) {
    auto row = make_interface(family, luid);
    error_ = api_.get(&row);
    if (error_ != NO_ERROR) return false;
    row.UseAutomaticMetric = value.automatic ? TRUE : FALSE;
    row.Metric = value.metric;
    error_ = api_.set(&row);
    return error_ == NO_ERROR;
}
}
