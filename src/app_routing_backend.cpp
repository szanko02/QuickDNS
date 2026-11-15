#include "app_routing_backend.h"

#include <algorithm>
#include <utility>

namespace quickdns {
AppRoutingBackend::AppRoutingBackend(GuardJournal& j, GuardWfpApi& g, PolicyTransactionApi& p, RoutingBackendPreflight& f)
    : journal_(j), guard_(g), policy_(p), preflight_(f) {}

GuardEnableResult AppRoutingBackend::enable(GuardJournalEntry entry, const GuardIdentity& identity,
                                            std::uint64_t interface_luid) {
    if (!entry.policy_keys) return {ERROR_INVALID_PARAMETER};
    const DWORD status = preflight_.validate(entry, identity, interface_luid);
    if (status != ERROR_SUCCESS) return {status};
    AppPolicyLifecycle policy(policy_, entry.rule_id, *entry.policy_keys);
    return enable_guarded_policy(journal_, guard_, policy, std::move(entry), identity, interface_luid);
}

DWORD AppRoutingBackend::disable(const std::string& rule_id) {
    if (rule_id.empty()) return ERROR_INVALID_PARAMETER;
    auto entries = journal_.entries();
    const auto entry = std::find_if(entries.begin(), entries.end(), [&](const auto& candidate) {
        return candidate.rule_id == rule_id;
    });
    if (entry == entries.end()) return ERROR_NOT_FOUND;
    if (!entry->policy_keys) return ERROR_INVALID_DATA;

    if (entry->phase != GuardJournalPhase::Removing) {
        entry->phase = GuardJournalPhase::Removing;
        const auto previous = journal_.entries();
        if (!journal_.replace(entries)) return ERROR_INVALID_DATA;
        if (!journal_.save()) {
            journal_.replace(previous);
            return ERROR_WRITE_FAULT;
        }
    }

    AppPolicyLifecycle policy(policy_, entry->rule_id, *entry->policy_keys);
    DWORD status = policy.remove(entry->rule_id);
    if (status != ERROR_SUCCESS) return status;
    status = remove_guard(guard_, entry->keys);
    if (status != ERROR_SUCCESS) return status;

    entries.erase(entry);
    const auto pending = journal_.entries();
    if (!journal_.replace(std::move(entries))) return ERROR_INVALID_DATA;
    if (!journal_.save()) {
        journal_.replace(pending);
        return ERROR_WRITE_FAULT;
    }
    return ERROR_SUCCESS;
}
}
