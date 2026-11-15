#pragma once
#include "app_guard_journal.h"

namespace quickdns {
class RoutingPolicyProbe {
public:
    virtual ~RoutingPolicyProbe() = default;
    // Unknown/error must be returned as an error, never as absent.
    virtual DWORD is_absent(const std::string& rule_id, bool& absent) = 0;
};
struct GuardRecoveryResult {
    DWORD error = ERROR_SUCCESS;
    std::size_t removed = 0;
    std::size_t deferred = 0;
};
// PREPARED and ACTIVE are preserved for explicit reconciliation. REMOVING is
// cleaned only if the matching routing policy is verified absent.
GuardRecoveryResult recover_guard_journal(GuardJournal& journal, GuardWfpApi& wfp, RoutingPolicyProbe& policies);
}
