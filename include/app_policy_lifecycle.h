#pragma once

#include "app_guard_enable.h"
#include "app_guard_recovery.h"
#include "app_connection_policy.h"
#include <array>
#include <string>

namespace quickdns {

class PolicyTransactionApi {
public:
    virtual ~PolicyTransactionApi() = default;
    virtual DWORD begin() = 0;
    virtual DWORD add(const GUID& key, FWP_IP_VERSION family) = 0;
    // FWP_E_PROVIDER_CONTEXT_NOT_FOUND indicates absence. A foreign object
    // must yield ERROR_ACCESS_DENIED, never success or absence.
    virtual DWORD check_owned(const GUID& key, FWP_IP_VERSION family) = 0;
    virtual DWORD erase(const GUID& key) = 0;
    virtual DWORD commit() = 0;
    virtual DWORD abort() = 0;
};

class AppPolicyLifecycle final : public RoutingPolicyInstaller, public RoutingPolicyProbe {
public:
    AppPolicyLifecycle(PolicyTransactionApi& api, std::string rule_id, PolicyKeys keys);
    bool matches_keys(const std::string& rule_id, const PolicyKeys& keys) const override;
    DWORD install(const std::string& rule_id) override;
    DWORD is_absent(const std::string& rule_id, bool& absent) override;
    DWORD remove(const std::string& rule_id);
private:
    PolicyTransactionApi& api_;
    std::string rule_id_;
    PolicyKeys keys_;
};

class NativePolicyTransactionApi final : public PolicyTransactionApi {
public:
    NativePolicyTransactionApi(HANDLE engine, const ConnectionPolicyApi& policy,
                               const GuardIdentity& identity, std::uint64_t interface_luid);
    DWORD begin() override;
    DWORD add(const GUID& key, FWP_IP_VERSION family) override;
    DWORD check_owned(const GUID& key, FWP_IP_VERSION family) override;
    DWORD erase(const GUID& key) override;
    DWORD commit() override;
    DWORD abort() override;
private:
    HANDLE engine_;
    const ConnectionPolicyApi& policy_;
    GuardIdentity identity_;
    std::uint64_t interface_luid_;
};
}
