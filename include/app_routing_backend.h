#pragma once
#include "app_policy_lifecycle.h"

namespace quickdns {
// Must verify feature support, route-policy configuration, and that the
// transaction API has the same application identity and LUID as the guard.
class RoutingBackendPreflight {
public:
    virtual ~RoutingBackendPreflight() = default;
    virtual DWORD validate(const GuardJournalEntry& entry, const GuardIdentity& identity,
                           std::uint64_t interface_luid) = 0;
};

class AppRoutingBackend {
public:
    AppRoutingBackend(GuardJournal& journal, GuardWfpApi& guard, PolicyTransactionApi& policy,
                      RoutingBackendPreflight& preflight);
    GuardEnableResult enable(GuardJournalEntry entry, const GuardIdentity& identity, std::uint64_t interface_luid);
    DWORD disable(const std::string& rule_id);
private:
    GuardJournal& journal_;
    GuardWfpApi& guard_;
    PolicyTransactionApi& policy_;
    RoutingBackendPreflight& preflight_;
};
}
