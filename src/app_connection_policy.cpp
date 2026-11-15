#include "app_connection_policy.h"

namespace quickdns {
ConnectionPolicyApi::ConnectionPolicyApi() {
    module_ = LoadLibraryExW(L"fwpuclnt.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (module_) {
        add_ = reinterpret_cast<ConnectionPolicyAddFn>(GetProcAddress(module_, "FwpmConnectionPolicyAdd0"));
        remove_ = reinterpret_cast<ConnectionPolicyDeleteFn>(GetProcAddress(module_, "FwpmConnectionPolicyDeleteByKey0"));
    }
}
ConnectionPolicyApi::ConnectionPolicyApi(ConnectionPolicyAddFn add, ConnectionPolicyDeleteFn remove)
    : add_(add), remove_(remove) {}
ConnectionPolicyApi::~ConnectionPolicyApi() { if (module_) FreeLibrary(module_); }
bool ConnectionPolicyApi::supported() const { return add_ && remove_; }
static DWORD add_policy(ConnectionPolicyAddFn add, HANDLE engine, const GUID& key, FWP_IP_VERSION family,
                        UINT64 interface_luid, FWPM_FILTER_CONDITION0& condition) {
    const GUID empty{};
    if (!engine || IsEqualGUID(key, empty) || !interface_luid ||
        (family != FWP_IP_VERSION_V4 && family != FWP_IP_VERSION_V6)) return ERROR_INVALID_PARAMETER;
    FWPM_NETWORK_CONNECTION_POLICY_SETTING0 setting{};
    setting.type = FWP_NETWORK_CONNECTION_POLICY_NEXT_HOP_INTERFACE;
    setting.value.type = FWP_UINT64;
    setting.value.uint64 = &interface_luid;
    FWPM_NETWORK_CONNECTION_POLICY_SETTINGS0 settings{1, &setting};
    FWPM_PROVIDER_CONTEXT3 policy{};
    policy.providerContextKey = key;
    policy.displayData.name = const_cast<wchar_t*>(L"QuickDNS application route");
    policy.type = FWPM_NETWORK_CONNECTION_POLICY_CONTEXT;
    policy.networkConnectionPolicy = &settings;
    return add(engine, &policy, family, 0x100, 1, &condition, nullptr);
}
DWORD ConnectionPolicyApi::add(HANDLE engine, const GUID& key, FWP_IP_VERSION family,
                               UINT64 interface_luid, FWP_BYTE_BLOB& app_id) const {
    if (!supported()) return ERROR_CALL_NOT_IMPLEMENTED;
    if (!app_id.size || !app_id.data) return ERROR_INVALID_PARAMETER;
    FWPM_FILTER_CONDITION0 condition{};
    condition.fieldKey = FWPM_CONDITION_ALE_APP_ID;
    condition.matchType = FWP_MATCH_EQUAL;
    condition.conditionValue.type = FWP_BYTE_BLOB_TYPE;
    condition.conditionValue.byteBlob = &app_id;
    return add_policy(add_, engine, key, family, interface_luid, condition);
}
DWORD ConnectionPolicyApi::remove(HANDLE engine, const GUID& key) const {
    if (!supported()) return ERROR_CALL_NOT_IMPLEMENTED;
    const GUID empty{};
    if (!engine || IsEqualGUID(key, empty)) return ERROR_INVALID_PARAMETER;
    return remove_(engine, &key);
}
DWORD ConnectionPolicyApi::add_package(HANDLE engine, const GUID& key, FWP_IP_VERSION family,
                                       UINT64 interface_luid, PSID package_sid) const {
    if (!supported()) return ERROR_CALL_NOT_IMPLEMENTED;
    if (!package_sid || !IsValidSid(package_sid)) return ERROR_INVALID_PARAMETER;
    FWPM_FILTER_CONDITION0 condition{};
    condition.fieldKey = FWPM_CONDITION_ALE_PACKAGE_ID;
    condition.matchType = FWP_MATCH_EQUAL;
    condition.conditionValue.type = FWP_SID;
    condition.conditionValue.sid = static_cast<SID*>(package_sid);
    return add_policy(add_, engine, key, family, interface_luid, condition);
}
}
