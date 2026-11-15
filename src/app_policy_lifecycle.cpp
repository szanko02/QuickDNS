#include "app_policy_lifecycle.h"
#include "app_policy_filter_identity.h"
#include <cwchar>
#include <utility>
namespace quickdns {
namespace {
DWORD verify_policy_identity(HANDLE engine, const GUID& key, FWP_IP_VERSION family, const GuardIdentity& identity) {
    HANDLE enumeration = nullptr;
    DWORD status = FwpmFilterCreateEnumHandle0(engine, nullptr, &enumeration);
    if (status != ERROR_SUCCESS) return status;
    PolicyFilterAudit audit(key, family, identity);
    for (;;) {
        FWPM_FILTER0** filters = nullptr;
        UINT32 count = 0;
        status = FwpmFilterEnum0(engine, enumeration, 128, &filters, &count);
        if (status == ERROR_SUCCESS) {
            for (UINT32 i = 0; i < count; ++i) {
                if (!filters || !filters[i]) { status = ERROR_INVALID_DATA; break; }
                audit.observe(*filters[i]);
            }
        }
        if (filters) FwpmFreeMemory0(reinterpret_cast<void**>(&filters));
        if (status != ERROR_SUCCESS || count == 0) break;
    }
    const DWORD close_status = FwpmFilterDestroyEnumHandle0(engine, enumeration);
    if (status == ERROR_SUCCESS && close_status != ERROR_SUCCESS) status = close_status;
    if (status != ERROR_SUCCESS) return status;
    return audit.valid() ? ERROR_SUCCESS : ERROR_ACCESS_DENIED;
}
}
NativePolicyTransactionApi::NativePolicyTransactionApi(HANDLE engine, const ConnectionPolicyApi& policy,
        const GuardIdentity& identity, std::uint64_t interface_luid)
    : engine_(engine), policy_(policy), identity_(identity), interface_luid_(interface_luid) {}
DWORD NativePolicyTransactionApi::begin() {
    if (!engine_ || engine_ == INVALID_HANDLE_VALUE) return ERROR_INVALID_HANDLE;
    if (!policy_.supported()) return ERROR_CALL_NOT_IMPLEMENTED;
    if (!interface_luid_) return ERROR_INVALID_PARAMETER;
    return FwpmTransactionBegin0(engine_, 0);
}
DWORD NativePolicyTransactionApi::add(const GUID& key, FWP_IP_VERSION family) {
    if (identity_.type == GuardIdentityType::Executable && identity_.executable_id && !identity_.package_sid)
        return policy_.add(engine_, key, family, interface_luid_, *identity_.executable_id);
    if (identity_.type == GuardIdentityType::PackageSid && identity_.package_sid && !identity_.executable_id)
        return policy_.add_package(engine_, key, family, interface_luid_, identity_.package_sid);
    return ERROR_INVALID_PARAMETER;
}
DWORD NativePolicyTransactionApi::check_owned(const GUID& key, FWP_IP_VERSION family) {
    if (family != FWP_IP_VERSION_V4 && family != FWP_IP_VERSION_V6) return ERROR_INVALID_PARAMETER;
    FWPM_PROVIDER_CONTEXT3* context = nullptr;
    const DWORD status = FwpmProviderContextGetByKey3(engine_, &key, &context);
    if (status != ERROR_SUCCESS) return status;
    bool ours = context && context->type == FWPM_NETWORK_CONNECTION_POLICY_CONTEXT &&
        context->displayData.name && std::wcscmp(context->displayData.name, L"QuickDNS application route") == 0 &&
        context->networkConnectionPolicy && context->networkConnectionPolicy->numSettings == 1 &&
        context->networkConnectionPolicy->settings;
    if (ours) {
        const auto& setting = context->networkConnectionPolicy->settings[0];
        ours = setting.type == FWP_NETWORK_CONNECTION_POLICY_NEXT_HOP_INTERFACE &&
            setting.value.type == FWP_UINT64 && setting.value.uint64 && *setting.value.uint64 == interface_luid_;
    }
    if (context) FwpmFreeMemory0(reinterpret_cast<void**>(&context));
    if (!ours) return ERROR_ACCESS_DENIED;
    return verify_policy_identity(engine_, key, family, identity_);
}
DWORD NativePolicyTransactionApi::erase(const GUID& key) { return policy_.remove(engine_, key); }
DWORD NativePolicyTransactionApi::commit() { return FwpmTransactionCommit0(engine_); }
DWORD NativePolicyTransactionApi::abort() { return FwpmTransactionAbort0(engine_); }
AppPolicyLifecycle::AppPolicyLifecycle(PolicyTransactionApi& api, std::string rule_id, PolicyKeys keys)
    : api_(api), rule_id_(std::move(rule_id)), keys_(keys) {}
bool AppPolicyLifecycle::matches_keys(const std::string& rule_id, const PolicyKeys& keys) const {
    const GUID empty{};
    return !rule_id_.empty() && rule_id == rule_id_ && !IsEqualGUID(keys.ipv4, empty) &&
        !IsEqualGUID(keys.ipv6, empty) && !IsEqualGUID(keys.ipv4, keys.ipv6) &&
        IsEqualGUID(keys.ipv4, keys_.ipv4) && IsEqualGUID(keys.ipv6, keys_.ipv6);
}
namespace {
bool valid(const std::string& requested, const std::string& expected, const PolicyKeys& keys) {
    const GUID empty{};
    return !expected.empty() && requested == expected && !IsEqualGUID(keys.ipv4, empty) &&
           !IsEqualGUID(keys.ipv6, empty) && !IsEqualGUID(keys.ipv4, keys.ipv6);
}
DWORD finish(PolicyTransactionApi& api, DWORD status) {
    if (status == ERROR_SUCCESS) status = api.commit();
    if (status != ERROR_SUCCESS) api.abort();
    return status;
}
}
DWORD AppPolicyLifecycle::install(const std::string& rule_id) {
    if (!valid(rule_id, rule_id_, keys_)) return ERROR_INVALID_PARAMETER;
    DWORD status = api_.begin();
    if (status != ERROR_SUCCESS) return status;
    status = api_.add(keys_.ipv4, FWP_IP_VERSION_V4);
    if (status == ERROR_SUCCESS) status = api_.add(keys_.ipv6, FWP_IP_VERSION_V6);
    return finish(api_, status);
}
DWORD AppPolicyLifecycle::is_absent(const std::string& rule_id, bool& absent) {
    absent = false;
    if (!valid(rule_id, rule_id_, keys_)) return ERROR_INVALID_PARAMETER;
    DWORD status = api_.begin();
    if (status != ERROR_SUCCESS) return status;
    DWORD v4 = api_.check_owned(keys_.ipv4, FWP_IP_VERSION_V4);
    if (v4 != ERROR_SUCCESS && v4 != FWP_E_PROVIDER_CONTEXT_NOT_FOUND) return finish(api_, v4);
    DWORD v6 = api_.check_owned(keys_.ipv6, FWP_IP_VERSION_V6);
    if (v6 != ERROR_SUCCESS && v6 != FWP_E_PROVIDER_CONTEXT_NOT_FOUND) return finish(api_, v6);
    status = finish(api_, ERROR_SUCCESS);
    if (status == ERROR_SUCCESS) absent = v4 == FWP_E_PROVIDER_CONTEXT_NOT_FOUND && v6 == FWP_E_PROVIDER_CONTEXT_NOT_FOUND;
    return status;
}
DWORD AppPolicyLifecycle::remove(const std::string& rule_id) {
    if (!valid(rule_id, rule_id_, keys_)) return ERROR_INVALID_PARAMETER;
    DWORD status = api_.begin();
    if (status != ERROR_SUCCESS) return status;
    DWORD v4 = api_.check_owned(keys_.ipv4, FWP_IP_VERSION_V4);
    if (v4 != ERROR_SUCCESS && v4 != FWP_E_PROVIDER_CONTEXT_NOT_FOUND) return finish(api_, v4);
    DWORD v6 = api_.check_owned(keys_.ipv6, FWP_IP_VERSION_V6);
    if (v6 != ERROR_SUCCESS && v6 != FWP_E_PROVIDER_CONTEXT_NOT_FOUND) return finish(api_, v6);
    if (v4 == ERROR_SUCCESS) status = api_.erase(keys_.ipv4);
    if (status == ERROR_SUCCESS && v6 == ERROR_SUCCESS) status = api_.erase(keys_.ipv6);
    return finish(api_, status);
}
}
