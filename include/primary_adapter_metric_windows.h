#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include "primary_adapter_metric.h"

namespace quickdns {
class MetricNetioApi {
public:
    virtual ~MetricNetioApi() = default;
    virtual ULONG routes(ADDRESS_FAMILY, MIB_IPFORWARD_TABLE2**) = 0;
    virtual void free_routes(MIB_IPFORWARD_TABLE2*) = 0;
    virtual ULONG get(MIB_IPINTERFACE_ROW*) = 0;
    virtual ULONG set(MIB_IPINTERFACE_ROW*) = 0;
};
class WindowsMetricApi final : public MetricNetioApi {
public:
    ULONG routes(ADDRESS_FAMILY, MIB_IPFORWARD_TABLE2**) override;
    void free_routes(MIB_IPFORWARD_TABLE2*) override;
    ULONG get(MIB_IPINTERFACE_ROW*) override;
    ULONG set(MIB_IPINTERFACE_ROW*) override;
};
class WindowsMetricSystem final : public MetricSystem {
public:
    explicit WindowsMetricSystem(MetricNetioApi& api) : api_(api) {}
    bool default_routes(IpFamily, std::vector<DefaultRouteMetric>&) override;
    bool read(IpFamily, std::uint64_t, InterfaceMetric&) override;
    bool write(IpFamily, std::uint64_t, const InterfaceMetric&) override;
    ULONG last_error() const { return error_; }
private:
    MetricNetioApi& api_;
    ULONG error_ = 0;
};
}
