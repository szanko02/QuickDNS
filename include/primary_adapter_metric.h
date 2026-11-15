#pragma once
#include <cstdint>
#include <optional>
#include <vector>

namespace quickdns {
enum class IpFamily { V4, V6 };
struct InterfaceMetric {
    bool automatic = true;
    std::uint32_t metric = 0;
    bool operator==(const InterfaceMetric&) const = default;
};
struct DefaultRouteMetric {
    std::uint64_t luid = 0;
    std::uint32_t route_metric = 0;
    InterfaceMetric interface_metric;
};
struct SavedMetricChange {
    IpFamily family;
    std::uint64_t luid;
    InterfaceMetric original;
    InterfaceMetric applied;
};
enum class MetricResult { Applied, Unchanged, Restored, NoDefaultRoute, CannotPrefer, Conflict, Pending, IoError };
class MetricSystem {
public:
    virtual ~MetricSystem() = default;
    virtual bool default_routes(IpFamily, std::vector<DefaultRouteMetric>&) = 0;
    virtual bool read(IpFamily, std::uint64_t, InterfaceMetric&) = 0;
    virtual bool write(IpFamily, std::uint64_t, const InterfaceMetric&) = 0;
};
class MetricJournal {
public:
    virtual ~MetricJournal() = default;
    virtual bool load(std::optional<SavedMetricChange>&) = 0;
    virtual bool save(const SavedMetricChange&) = 0;
    virtual bool clear() = 0;
};
MetricResult prefer_primary(MetricSystem&, MetricJournal&, IpFamily, std::uint64_t);
MetricResult restore_primary(MetricSystem&, MetricJournal&);
}
