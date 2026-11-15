#pragma once
#include "app_route_guard.h"

namespace quickdns {
// A single, exact condition is required. False means unknown or foreign.
bool policy_filter_matches_identity(const FWPM_FILTER0& filter, const GUID& policy_key,
                                    FWP_IP_VERSION family, const GuardIdentity& identity);

class PolicyFilterAudit {
public:
    PolicyFilterAudit(const GUID& policy_key, FWP_IP_VERSION family, const GuardIdentity& identity);
    void observe(const FWPM_FILTER0& filter);
    bool valid() const;
private:
    GUID policy_key_;
    FWP_IP_VERSION family_;
    GuardIdentity identity_;
    unsigned matching_ = 0;
    bool foreign_ = false;
};
}
