#include "app_policy_lifecycle.h"
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace quickdns;
static void require(bool ok, const char* error) { if (!ok) throw std::runtime_error(error); }
class FakePolicyApi : public PolicyTransactionApi {
public:
    DWORD begin() override { events.push_back(1); return begin_error; }
    DWORD add(const GUID&, FWP_IP_VERSION family) override { events.push_back(family == FWP_IP_VERSION_V4 ? 2 : 3); return family == FWP_IP_VERSION_V6 ? v6_error : ERROR_SUCCESS; }
    DWORD check_owned(const GUID&, FWP_IP_VERSION family) override { events.push_back(family == FWP_IP_VERSION_V4 ? 4 : 5); return family == FWP_IP_VERSION_V4 ? v4_status : v6_status; }
    DWORD erase(const GUID&) override { events.push_back(6); return erase_error; }
    DWORD commit() override { events.push_back(7); return commit_error; }
    DWORD abort() override { events.push_back(8); return ERROR_SUCCESS; }
    std::vector<int> events;
    DWORD begin_error = ERROR_SUCCESS, v6_error = ERROR_SUCCESS, erase_error = ERROR_SUCCESS, commit_error = ERROR_SUCCESS;
    DWORD v4_status = ERROR_SUCCESS, v6_status = ERROR_SUCCESS;
};
int main() {
    try {
        PolicyKeys keys{}; keys.ipv4.Data1 = 1; keys.ipv6.Data1 = 2;
        FakePolicyApi api; AppPolicyLifecycle lifecycle(api, "rule", keys);
        require(lifecycle.install("rule") == ERROR_SUCCESS, "dual-family install failed");
        require(api.events == std::vector<int>({1,2,3,7}), "install was not atomic");
        api.events.clear(); api.v6_error = ERROR_ACCESS_DENIED;
        require(lifecycle.install("rule") == ERROR_ACCESS_DENIED && api.events == std::vector<int>({1,2,3,8}), "IPv6 failure must abort IPv4");
        api.events.clear();
        require(lifecycle.install("another") == ERROR_INVALID_PARAMETER && api.events.empty(), "wrong rule allowed");
        api.events.clear(); api.v4_status = ERROR_SUCCESS; api.v6_status = FWP_E_PROVIDER_CONTEXT_NOT_FOUND;
        bool absent = true;
        require(lifecycle.is_absent("rule", absent) == ERROR_SUCCESS && !absent, "partial installation reported absent");
        api.events.clear(); api.v4_status = ERROR_ACCESS_DENIED; api.v6_status = ERROR_SUCCESS;
        require(lifecycle.remove("rule") == ERROR_ACCESS_DENIED && api.events == std::vector<int>({1,4,8}), "foreign policy removed");
        api.events.clear(); api.v4_status = FWP_E_PROVIDER_CONTEXT_NOT_FOUND; api.v6_status = FWP_E_PROVIDER_CONTEXT_NOT_FOUND;
        absent = false;
        require(lifecycle.is_absent("rule", absent) == ERROR_SUCCESS && absent, "absent policies not detected");
        api.events.clear();
        require(lifecycle.remove("rule") == ERROR_SUCCESS && api.events == std::vector<int>({1,4,5,7}), "removal not idempotent");
        ConnectionPolicyApi unavailable(nullptr, nullptr);
        GuardIdentity identity{};
        NativePolicyTransactionApi native(nullptr, unavailable, identity, 0);
        require(native.begin() == ERROR_INVALID_HANDLE, "invalid native engine accepted");
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
