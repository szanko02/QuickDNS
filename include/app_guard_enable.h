#pragma once
#include "app_guard_journal.h"

namespace quickdns {
class RoutingPolicyInstaller {
public:
    virtual ~RoutingPolicyInstaller() = default;
    virtual bool matches_keys(const std::string& rule_id, const PolicyKeys& keys) const = 0;
    // Must commit atomically or leave its policy state inspectable by recovery.
    virtual DWORD install(const std::string& rule_id) = 0;
};
struct GuardEnableResult {
    DWORD error = ERROR_SUCCESS;
    bool guard_installed = false;
    bool policy_installed = false;
};
GuardEnableResult enable_guarded_policy(GuardJournal& journal, GuardWfpApi& wfp,
    RoutingPolicyInstaller& policy, GuardJournalEntry entry, const GuardIdentity& identity, std::uint64_t interface_luid);
}
