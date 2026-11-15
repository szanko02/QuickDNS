#pragma once

#include "routing_adapters.h"
#include <netioapi.h>

namespace quickdns {
// Routes installed in the active Windows routing table for this QuickDNS session.
class SubnetRoutes {
public:
    struct Route {
        MIB_IPFORWARD_ROW2 row;
        std::wstring adapter_name;
    };

    SubnetRoutes() = default;
    ~SubnetRoutes();
    SubnetRoutes(const SubnetRoutes&) = delete;
    SubnetRoutes& operator=(const SubnetRoutes&) = delete;

    DWORD add(const std::wstring& cidr, const std::wstring& gateway, const RoutingAdapter& adapter);
    DWORD remove(size_t index);
    DWORD status(size_t index) const;
    const std::vector<Route>& routes() const { return routes_; }

private:
    std::vector<Route> routes_;
};

std::wstring subnet_route_label(const SubnetRoutes::Route& route);
}
