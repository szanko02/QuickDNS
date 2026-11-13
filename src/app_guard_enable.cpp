#include "app_guard_enable.h"
namespace quickdns {
GuardEnableResult enable_guarded_policy(GuardJournal& journal, GuardWfpApi& wfp,
    RoutingPolicyInstaller& policy, GuardJournalEntry entry, const GuardIdentity& identity, std::uint64_t interface_luid) {
    GuardEnableResult result;
    if (!entry.policy_keys || !policy.matches_keys(entry.rule_id, *entry.policy_keys)) {
        result.error = ERROR_INVALID_PARAMETER;
        return result;
    }
    const auto previous = journal.entries();
    auto pending = previous;
    entry.phase = GuardJournalPhase::Prepared;
    pending.push_back(entry);
    if (!journal.replace(pending)) { result.error = ERROR_INVALID_DATA; return result; }
    if (!journal.save()) {
        journal.replace(previous);
        result.error = ERROR_WRITE_FAULT;
        return result;
    }
    result.error = install_guard(wfp, identity, interface_luid, entry.keys);
    if (result.error != ERROR_SUCCESS) return result;
    result.guard_installed = true;
    result.error = policy.install(entry.rule_id);
    if (result.error != ERROR_SUCCESS) return result;
    result.policy_installed = true;
    auto active = pending;
    active.back().phase = GuardJournalPhase::Active;
    if (!journal.replace(active) || !journal.save()) {
        journal.replace(pending);
        result.error = ERROR_WRITE_FAULT;
    }
    return result;
}
}
