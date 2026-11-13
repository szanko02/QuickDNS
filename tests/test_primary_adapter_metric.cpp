#include "primary_adapter_metric.h"
#include <iostream>
#include <map>
#include <stdexcept>
using namespace quickdns;
static void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
class System final : public MetricSystem {
public:
    std::vector<DefaultRouteMetric> routes;
    std::map<std::uint64_t, InterfaceMetric> interfaces;
    bool fail_write = false;
    bool recalculate_auto = false;
    bool route_changes = false;
    int route_reads = 0;
    int writes = 0;
    bool default_routes(IpFamily, std::vector<DefaultRouteMetric>& out) override {
        out = routes;
        if (route_changes && ++route_reads == 2) out[1].interface_metric.metric = 1;
        return true;
    }
    bool read(IpFamily, std::uint64_t id, InterfaceMetric& out) override {
        if (!interfaces.contains(id)) return false;
        out = interfaces[id]; return true;
    }
    bool write(IpFamily, std::uint64_t id, const InterfaceMetric& value) override {
        ++writes; if (fail_write) return false; interfaces[id] = value;
        if (value.automatic && recalculate_auto) interfaces[id].metric = 65;
        return true;
    }
};
class Journal final : public MetricJournal {
public:
    std::optional<SavedMetricChange> entry;
    bool fail_save = false;
    bool load(std::optional<SavedMetricChange>& out) override { out = entry; return true; }
    bool save(const SavedMetricChange& value) override { if (fail_save) return false; entry = value; return true; }
    bool clear() override { entry.reset(); return true; }
};
int main() {
    try {
        System net;
        Journal journal;
        net.interfaces = {{1, {true, 50}}, {2, {false, 20}}};
        net.routes = {{1, 10, {true, 50}}, {2, 5, {false, 20}}};
        require(prefer_primary(net, journal, IpFamily::V4, 1) == MetricResult::Applied, "selected adapter was not preferred");
        require(net.interfaces.at(1) == InterfaceMetric{false, 1}, "selected metric not applied");
        require(journal.entry && journal.entry->original == InterfaceMetric{true, 50}, "original metric not journalled");
        require(restore_primary(net, journal) == MetricResult::Restored, "original metric not restored");
        require(net.interfaces.at(1) == InterfaceMetric{true, 50} && !journal.entry, "original automatic mode lost");
        require(prefer_primary(net, journal, IpFamily::V4, 1) == MetricResult::Applied, "second application failed");
        net.interfaces[1] = {false, 7};
        require(restore_primary(net, journal) == MetricResult::Conflict, "external change was overwritten");
        require(net.interfaces.at(1) == InterfaceMetric{false, 7} && journal.entry, "conflicting state lost");
        journal.entry.reset(); net.writes = 0; journal.fail_save = true;
        require(prefer_primary(net, journal, IpFamily::V4, 1) == MetricResult::IoError && net.writes == 0, "changed system before saving recovery state");
        journal.fail_save = false; net.routes[0].route_metric = 100;
        require(prefer_primary(net, journal, IpFamily::V4, 1) == MetricResult::CannotPrefer && net.writes == 0, "impossible preference silently accepted");
        net.routes[0].route_metric = 10;
        net.interfaces[1] = {true, 50};
        net.recalculate_auto = true;
        require(prefer_primary(net, journal, IpFamily::V4, 1) == MetricResult::Applied, "automatic interface could not be configured");
        require(restore_primary(net, journal) == MetricResult::Restored && net.interfaces.at(1).automatic && !journal.entry,
                "automatic metric recalculation incorrectly reported as failure");
        net.route_changes = true;
        net.writes = 0;
        require(prefer_primary(net, journal, IpFamily::V4, 1) == MetricResult::Conflict && net.writes == 0,
                "changed competing route was ignored");
        return 0;
    } catch (const std::exception& ex) { std::cerr << ex.what() << '\n'; return 1; }
}
