#include "primary_adapter_metric_windows.h"
#include <iostream>
#include <stdexcept>
using namespace quickdns;
static void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
class FakeApi final : public MetricNetioApi {
public:
    MIB_IPFORWARD_TABLE2 table{};
    MIB_IPINTERFACE_ROW settings{};
    bool freed = false;
    ULONG status = NO_ERROR;
    ULONG routes(ADDRESS_FAMILY family, MIB_IPFORWARD_TABLE2** out) override { require(family == AF_INET, "wrong family"); *out = &table; return status; }
    void free_routes(MIB_IPFORWARD_TABLE2* value) override { require(value == &table, "wrong table freed"); freed = true; }
    ULONG get(MIB_IPINTERFACE_ROW* row) override {
        if (status != NO_ERROR) return status;
        require(row->Family == AF_INET && row->InterfaceLuid.Value == 42, "wrong interface selected");
        *row = settings; return NO_ERROR;
    }
    ULONG set(MIB_IPINTERFACE_ROW* row) override { settings = *row; return status; }
};
int main() {
    try {
        FakeApi api;
        api.table.NumEntries = 1;
        api.table.Table[0].InterfaceLuid.Value = 42;
        api.table.Table[0].DestinationPrefix.Prefix.si_family = AF_INET;
        api.table.Table[0].DestinationPrefix.PrefixLength = 0;
        api.table.Table[0].Metric = 7;
        api.table.Table[0].ValidLifetime = 100;
        api.settings.Family = AF_INET; api.settings.InterfaceLuid.Value = 42;
        api.settings.Connected = TRUE; api.settings.UseAutomaticMetric = TRUE;
        api.settings.Metric = 25; api.settings.NlMtu = 1400;
        WindowsMetricSystem system(api);
        std::vector<DefaultRouteMetric> routes;
        require(system.default_routes(IpFamily::V4, routes), "could not read routes");
        require(api.freed && routes.size() == 1 && routes[0].luid == 42 && routes[0].route_metric == 7 &&
                routes[0].interface_metric == InterfaceMetric{true, 25}, "wrong route metric snapshot");
        InterfaceMetric metric;
        require(system.read(IpFamily::V4, 42, metric) && metric == InterfaceMetric{true,25}, "wrong interface metric");
        require(system.write(IpFamily::V4, 42, {false, 1}), "failed to set interface metric");
        require(api.settings.Metric == 1 && !api.settings.UseAutomaticMetric && api.settings.NlMtu == 1400,
                "metric write damaged other settings");
        api.status = ERROR_ACCESS_DENIED;
        require(!system.read(IpFamily::V4, 42, metric) && system.last_error() == ERROR_ACCESS_DENIED,
                "Windows error was hidden");
        return 0;
    } catch (const std::exception& ex) { std::cerr << ex.what() << '\n'; return 1; }
}
