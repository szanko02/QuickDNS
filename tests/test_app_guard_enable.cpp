#include "app_guard_enable.h"
#include <filesystem>
#include <iostream>
#include <stdexcept>
using namespace quickdns;
static void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
class FakeWfp final : public GuardWfpApi {
public:
    explicit FakeWfp(std::filesystem::path path) : path_(std::move(path)) {}
    DWORD begin() override {
        GuardJournal persisted(path_);
        journal_before_guard = persisted.load() && persisted.entries().size() == 1 && persisted.entries()[0].phase == GuardJournalPhase::Prepared &&
            persisted.entries()[0].policy_keys && persisted.entries()[0].policy_keys->ipv4.Data1 == 20 && persisted.entries()[0].policy_keys->ipv6.Data1 == 21;
        return begin_error;
    }
    DWORD add_sublayer(const GUID&) override { return ERROR_SUCCESS; }
    DWORD add_filter(const FWPM_FILTER0&) override { return ERROR_SUCCESS; }
    DWORD check_filter_owner(const GUID&, const GUID&) override { return ERROR_SUCCESS; }
    DWORD check_sublayer_owner(const GUID&) override { return ERROR_SUCCESS; }
    DWORD delete_filter(const GUID&) override { ++deleted; return ERROR_SUCCESS; }
    DWORD delete_sublayer(const GUID&) override { return ERROR_SUCCESS; }
    DWORD commit() override { ++committed; return ERROR_SUCCESS; }
    DWORD abort() override { ++aborted; return ERROR_SUCCESS; }
    std::filesystem::path path_;
    DWORD begin_error = ERROR_SUCCESS;
    int committed = 0, aborted = 0, deleted = 0;
    bool journal_before_guard = false;
};
class FakePolicy final : public RoutingPolicyInstaller {
public:
    explicit FakePolicy(FakeWfp& wfp) : wfp_(wfp) {}
    bool matches_keys(const std::string& rule, const PolicyKeys& keys) const override {
        return rule == "r" && keys.ipv4.Data1 == 20 && keys.ipv6.Data1 == 21;
    }
    DWORD install(const std::string&) override {
        ++calls; guard_before_policy = wfp_.committed == 1;
        if (break_journal) { std::filesystem::remove(wfp_.path_); std::filesystem::create_directory(wfp_.path_); }
        return error;
    }
    FakeWfp& wfp_;
    DWORD error = ERROR_SUCCESS;
    int calls = 0;
    bool guard_before_policy = false, break_journal = false;
};
int main() {
    const auto path = std::filesystem::temp_directory_path() / L"quickdns_guard_enable_test.json";
    std::filesystem::remove(path);
    try {
        GuardJournalEntry entry{"r", {}, GuardJournalPhase::Prepared};
        entry.keys.sublayer.Data1 = 1;
        for (std::size_t i = 0; i < 6; ++i) entry.keys.filters[i].Data1 = static_cast<unsigned long>(i + 2);
        entry.policy_keys = PolicyKeys{};
        entry.policy_keys->ipv4.Data1 = 20; entry.policy_keys->ipv6.Data1 = 21;
        BYTE bytes[] = {1, 2}; FWP_BYTE_BLOB app_id{sizeof(bytes), bytes};
        GuardIdentity identity{GuardIdentityType::Executable, &app_id, nullptr};
        GuardJournal journal(path); require(journal.load(), "journal load failed");
        FakeWfp wfp(path); FakePolicy policy(wfp);
        auto invalid = entry; invalid.policy_keys.reset();
        auto result = enable_guarded_policy(journal, wfp, policy, invalid, identity, 55);
        require(result.error == ERROR_INVALID_PARAMETER && wfp.committed == 0 && policy.calls == 0 && journal.entries().empty(),
                "missing policy keys must prevent protection and routing changes");
        invalid = entry; invalid.policy_keys->ipv6.Data1 = 22;
        result = enable_guarded_policy(journal, wfp, policy, invalid, identity, 55);
        require(result.error == ERROR_INVALID_PARAMETER && wfp.committed == 0 && policy.calls == 0 && journal.entries().empty(),
                "mismatched policy keys must prevent protection and routing changes");
        result = enable_guarded_policy(journal, wfp, policy, entry, identity, 55);
        require(result.error == ERROR_SUCCESS && result.guard_installed && result.policy_installed, "activation failed");
        require(wfp.journal_before_guard && policy.guard_before_policy, "unsafe activation order");
        GuardJournal restored(path); require(restored.load() && restored.entries()[0].phase == GuardJournalPhase::Active, "active state not durable");
        std::filesystem::remove(path);
        GuardJournal second(path); require(second.load(), "second journal failed");
        FakeWfp failed_guard(path); failed_guard.begin_error = ERROR_ACCESS_DENIED; FakePolicy untouched(failed_guard);
        result = enable_guarded_policy(second, failed_guard, untouched, entry, identity, 55);
        require(result.error == ERROR_ACCESS_DENIED && !result.policy_installed && untouched.calls == 0, "guard failure must prevent routing");
        std::filesystem::remove(path);
        GuardJournal third(path); require(third.load(), "third journal failed");
        FakeWfp installed_guard(path); FakePolicy failed_policy(installed_guard); failed_policy.error = ERROR_ACCESS_DENIED;
        result = enable_guarded_policy(third, installed_guard, failed_policy, entry, identity, 55);
        require(result.error == ERROR_ACCESS_DENIED && result.guard_installed && !result.policy_installed && installed_guard.deleted == 0,
                "policy failure must leave guard in place");
        GuardJournal after(path); require(after.load() && after.entries()[0].phase == GuardJournalPhase::Prepared, "failed activation must remain recoverable");
        std::filesystem::remove(path);
        GuardJournal fourth(path); require(fourth.load(), "fourth journal failed");
        std::filesystem::create_directory(path);
        FakeWfp no_journal(path); FakePolicy no_policy(no_journal);
        result = enable_guarded_policy(fourth, no_journal, no_policy, entry, identity, 55);
        require(result.error == ERROR_WRITE_FAULT && !result.guard_installed && no_policy.calls == 0 && no_journal.committed == 0,
                "journal write failure must prevent guard and policy");
        std::filesystem::remove(path);
        GuardJournal fifth(path); require(fifth.load(), "fifth journal failed");
        FakeWfp durable_guard(path); FakePolicy broken_update(durable_guard); broken_update.break_journal = true;
        result = enable_guarded_policy(fifth, durable_guard, broken_update, entry, identity, 55);
        require(result.error == ERROR_WRITE_FAULT && result.guard_installed && result.policy_installed && durable_guard.deleted == 0,
                "active journal write failure must not remove protection");
        std::filesystem::remove(path); return 0;
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << '\n'; std::filesystem::remove(path); return 1;
    }
}
