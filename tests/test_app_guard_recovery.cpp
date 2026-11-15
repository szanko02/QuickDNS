#include "app_guard_recovery.h"
#include <filesystem>
#include <iostream>
#include <stdexcept>

using namespace quickdns;
static void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
class FakeWfp final : public GuardWfpApi {
public:
    DWORD begin() override { return ERROR_SUCCESS; }
    DWORD add_sublayer(const GUID&) override { return ERROR_SUCCESS; }
    DWORD add_filter(const FWPM_FILTER0&) override { return ERROR_SUCCESS; }
    DWORD check_filter_owner(const GUID&, const GUID&) override { return missing ? FWP_E_FILTER_NOT_FOUND : foreign ? ERROR_ACCESS_DENIED : ERROR_SUCCESS; }
    DWORD check_sublayer_owner(const GUID&) override { return missing ? FWP_E_SUBLAYER_NOT_FOUND : ERROR_SUCCESS; }
    DWORD delete_filter(const GUID&) override { ++deleted; return ERROR_SUCCESS; }
    DWORD delete_sublayer(const GUID&) override { return ERROR_SUCCESS; }
    DWORD commit() override { return ERROR_SUCCESS; }
    DWORD abort() override { ++aborted; return ERROR_SUCCESS; }
    int deleted = 0, aborted = 0;
    bool missing = false, foreign = false;
};
class FakePolicy final : public RoutingPolicyProbe {
public:
    DWORD is_absent(const std::string&, bool& value) override { value = absent; return error; }
    bool absent = true;
    DWORD error = ERROR_SUCCESS;
};
int main() {
    const auto path = std::filesystem::temp_directory_path() / L"quickdns_guard_recovery_test.json";
    std::filesystem::remove(path);
    try {
        GuardJournal journal(path);
        require(journal.load(), "journal load failed");
        GuardJournalEntry entry{"r", {}, GuardJournalPhase::Removing};
        entry.keys.sublayer.Data1 = 1;
        for (std::size_t i = 0; i < 6; ++i) entry.keys.filters[i].Data1 = static_cast<unsigned long>(i + 2);
        require(journal.replace({entry}) && journal.save(), "journal setup failed");
        FakeWfp wfp;
        FakePolicy policy;
        policy.absent = false;
        auto result = recover_guard_journal(journal, wfp, policy);
        require(result.error == ERROR_SUCCESS && result.deferred == 1 && wfp.deleted == 0, "active policy must retain guard");
        policy.absent = true;
        wfp.foreign = true;
        result = recover_guard_journal(journal, wfp, policy);
        require(result.error == ERROR_ACCESS_DENIED && journal.entries().size() == 1 && wfp.deleted == 0, "foreign filter must be retained");
        wfp.foreign = false;
        wfp.missing = true;
        result = recover_guard_journal(journal, wfp, policy);
        require(result.error == ERROR_SUCCESS && result.removed == 1 && journal.entries().empty(), "missing guard must be cleaned from journal");
        result = recover_guard_journal(journal, wfp, policy);
        require(result.error == ERROR_SUCCESS && result.removed == 0, "recovery must be idempotent");
        entry.phase = GuardJournalPhase::Prepared;
        require(journal.replace({entry}) && journal.save(), "prepared setup failed");
        result = recover_guard_journal(journal, wfp, policy);
        require(result.deferred == 1 && journal.entries().size() == 1, "prepared guard must be preserved");
        entry.phase = GuardJournalPhase::Active;
        require(journal.replace({entry}) && journal.save(), "active setup failed");
        result = recover_guard_journal(journal, wfp, policy);
        require(result.deferred == 1 && journal.entries().size() == 1, "active guard must be preserved");
        std::filesystem::remove(path);
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << '\n';
        std::filesystem::remove(path);
        return 1;
    }
}
