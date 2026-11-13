#include "primary_adapter_metric.h"
#include <algorithm>
#include <array>
#include <limits>
namespace quickdns {
namespace {
auto route_values(const std::vector<DefaultRouteMetric>& routes) {
    std::vector<std::array<std::uint64_t, 4>> result;
    for (const auto& route : routes)
        result.push_back({route.luid, route.route_metric, route.interface_metric.metric, route.interface_metric.automatic});
    std::sort(result.begin(), result.end());
    return result;
}
}
MetricResult prefer_primary(MetricSystem& system, MetricJournal& journal, IpFamily family, std::uint64_t luid) {
    if (!luid) return MetricResult::NoDefaultRoute;
    std::optional<SavedMetricChange> pending;
    if (!journal.load(pending)) return MetricResult::IoError;
    if (pending) return MetricResult::Pending;
    std::vector<DefaultRouteMetric> routes;
    if (!system.default_routes(family, routes)) return MetricResult::IoError;
    auto target = std::numeric_limits<std::uint64_t>::max();
    auto rival = std::numeric_limits<std::uint64_t>::max();
    for (const auto& route : routes) {
        if (route.luid == luid) target = std::min(target, std::uint64_t(route.route_metric));
        else rival = std::min(rival, std::uint64_t(route.route_metric) + route.interface_metric.metric);
    }
    if (target == std::numeric_limits<std::uint64_t>::max()) return MetricResult::NoDefaultRoute;
    InterfaceMetric original;
    if (!system.read(family, luid, original)) return MetricResult::IoError;
    if (target + 1 >= rival) return MetricResult::CannotPrefer;
    if (!original.automatic && original.metric <= 1 && target + original.metric < rival) return MetricResult::Unchanged;
    SavedMetricChange change{family, luid, original, {false, 1}};
    if (!journal.save(change)) return MetricResult::IoError;
    InterfaceMetric current;
    if (!system.read(family, luid, current)) return MetricResult::IoError;
    if (current != original) return MetricResult::Conflict;
    std::vector<DefaultRouteMetric> latest;
    if (!system.default_routes(family, latest)) return MetricResult::IoError;
    if (route_values(routes) != route_values(latest)) return MetricResult::Conflict;
    if (!system.write(family, luid, change.applied)) return MetricResult::IoError;
    if (!system.read(family, luid, current) || current != change.applied) return MetricResult::IoError;
    return MetricResult::Applied;
}
MetricResult restore_primary(MetricSystem& system, MetricJournal& journal) {
    std::optional<SavedMetricChange> pending;
    if (!journal.load(pending)) return MetricResult::IoError;
    if (!pending) return MetricResult::Unchanged;
    InterfaceMetric current;
    if (!system.read(pending->family, pending->luid, current)) return MetricResult::IoError;
    if (current == pending->original || (pending->original.automatic && current.automatic))
        return journal.clear() ? MetricResult::Restored : MetricResult::IoError;
    if (current != pending->applied) return MetricResult::Conflict;
    if (!system.write(pending->family, pending->luid, pending->original)) return MetricResult::IoError;
    if (!system.read(pending->family, pending->luid, current)) return MetricResult::IoError;
    if (current != pending->original && !(pending->original.automatic && current.automatic)) return MetricResult::Conflict;
    return journal.clear() ? MetricResult::Restored : MetricResult::IoError;
}
}
