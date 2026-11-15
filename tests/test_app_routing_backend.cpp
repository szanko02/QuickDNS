#include "app_routing_backend.h"
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace quickdns;
static void require(bool v, const char* error) { if (!v) throw std::runtime_error(error); }
struct Guard final : GuardWfpApi {
    DWORD begin() override { events.push_back(1); return ERROR_SUCCESS; }
    DWORD add_sublayer(const GUID&) override { return ERROR_SUCCESS; }
    DWORD add_filter(const FWPM_FILTER0&) override { return ERROR_SUCCESS; }
    DWORD check_filter_owner(const GUID&, const GUID&) override { return ERROR_SUCCESS; }
    DWORD check_sublayer_owner(const GUID&) override { return ERROR_SUCCESS; }
    DWORD delete_filter(const GUID&) override { events.push_back(2); return ERROR_SUCCESS; }
    DWORD delete_sublayer(const GUID&) override { return ERROR_SUCCESS; }
    DWORD commit() override { events.push_back(3); return ERROR_SUCCESS; }
    DWORD abort() override { events.push_back(4); return ERROR_SUCCESS; }
    std::vector<int> events;
};
struct Policy final : PolicyTransactionApi {
    DWORD begin() override { events.push_back(1); return ERROR_SUCCESS; }
    DWORD add(const GUID&, FWP_IP_VERSION) override { events.push_back(2); present = true; return ERROR_SUCCESS; }
    DWORD check_owned(const GUID&, FWP_IP_VERSION) override { return present ? ERROR_SUCCESS : FWP_E_PROVIDER_CONTEXT_NOT_FOUND; }
    DWORD erase(const GUID&) override { events.push_back(3); if (erase_error) return erase_error; present = false; return ERROR_SUCCESS; }
    DWORD commit() override { events.push_back(4); return ERROR_SUCCESS; }
    DWORD abort() override { events.push_back(5); return ERROR_SUCCESS; }
    bool present = false; DWORD erase_error = ERROR_SUCCESS; std::vector<int> events;
};
struct Preflight final : RoutingBackendPreflight {
    DWORD validate(const GuardJournalEntry&, const GuardIdentity&, std::uint64_t) override { return status; }
    DWORD status = ERROR_CALL_NOT_IMPLEMENTED;
};
int main() {
    const auto path = std::filesystem::temp_directory_path() / L"quickdns_backend_test.json";
    std::filesystem::remove(path);
    try {
        GuardJournal journal(path); require(journal.load(), "cannot load test journal");
        Guard guard; Policy policy; Preflight preflight;
        AppRoutingBackend backend(journal, guard, policy, preflight);
        GuardJournalEntry entry{"rule", {}, GuardJournalPhase::Prepared};
        entry.keys.sublayer.Data1 = 1;
        for (size_t i=0; i<6; ++i) entry.keys.filters[i].Data1 = static_cast<unsigned long>(i+2);
        entry.policy_keys = PolicyKeys{}; entry.policy_keys->ipv4.Data1 = 20; entry.policy_keys->ipv6.Data1 = 21;
        BYTE value[] = {1,2}; FWP_BYTE_BLOB app{2,value}; GuardIdentity identity{GuardIdentityType::Executable,&app,nullptr};
        auto result = backend.enable(entry, identity, 55);
        require(result.error == ERROR_CALL_NOT_IMPLEMENTED && guard.events.empty() && policy.events.empty() && journal.entries().empty(), "unsupported API performed changes");
        preflight.status = ERROR_SUCCESS;
        result = backend.enable(entry, identity, 55);
        require(result.error == ERROR_SUCCESS && result.guard_installed && result.policy_installed && journal.entries().size()==1 && journal.entries()[0].phase==GuardJournalPhase::Active, "activation failed");
        policy.erase_error = ERROR_ACCESS_DENIED;
        require(backend.disable("rule") == ERROR_ACCESS_DENIED && journal.entries()[0].phase == GuardJournalPhase::Removing, "failed removal lost journal");
        require(std::find(guard.events.begin(), guard.events.end(), 2) == guard.events.end(), "guard removed while policy remained");
        policy.erase_error = ERROR_SUCCESS;
        require(backend.disable("rule") == ERROR_SUCCESS && journal.entries().empty(), "disable retry failed");
        std::filesystem::remove(path); return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; std::filesystem::remove(path); return 1; }
}
