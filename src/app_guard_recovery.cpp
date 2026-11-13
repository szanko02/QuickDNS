#include "app_guard_recovery.h"
namespace quickdns {
GuardRecoveryResult recover_guard_journal(GuardJournal& journal, GuardWfpApi& wfp, RoutingPolicyProbe& policies) {
    GuardRecoveryResult result;
    auto entries = journal.entries();
    std::size_t index = 0;
    while (index < entries.size()) {
        if (entries[index].phase != GuardJournalPhase::Removing) {
            ++result.deferred;
            ++index;
            continue;
        }
        bool absent = false;
        result.error = policies.is_absent(entries[index].rule_id, absent);
        if (result.error != ERROR_SUCCESS) return result;
        if (!absent) {
            ++result.deferred;
            ++index;
            continue;
        }
        result.error = remove_guard(wfp, entries[index].keys);
        if (result.error != ERROR_SUCCESS) return result;
        auto remaining = entries;
        remaining.erase(remaining.begin() + index);
        if (!journal.replace(remaining) || !journal.save()) {
            journal.replace(entries);
            result.error = ERROR_WRITE_FAULT;
            return result;
        }
        entries = std::move(remaining);
        ++result.removed;
    }
    return result;
}
}
