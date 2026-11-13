#include "app_route_guard.h"
#include <cwchar>
#include <cstring>

namespace quickdns {
namespace {
constexpr wchar_t guard_name[] = L"QuickDNS application routing guard";
bool valid_keys(const GuardKeys& keys) {
    const GUID null_guid{};
    if (IsEqualGUID(keys.sublayer, null_guid)) return false;
    for (std::size_t i = 0; i < keys.filters.size(); ++i) {
        if (IsEqualGUID(keys.filters[i], null_guid) || IsEqualGUID(keys.filters[i], keys.sublayer)) return false;
        for (std::size_t j = 0; j < i; ++j)
            if (IsEqualGUID(keys.filters[i], keys.filters[j])) return false;
    }
    return true;
}
bool same_guard_name(const wchar_t* name) { return name && std::wcscmp(name, guard_name) == 0; }
bool valid_identity(const GuardIdentity& identity) {
    if (identity.type == GuardIdentityType::Executable)
        return identity.executable_id && identity.executable_id->size && identity.executable_id->data && !identity.package_sid;
    if (identity.type != GuardIdentityType::PackageSid || !identity.package_sid || identity.executable_id || !IsValidSid(identity.package_sid)) return false;
    SID_IDENTIFIER_AUTHORITY authority = SECURITY_APP_PACKAGE_AUTHORITY;
    return std::memcmp(GetSidIdentifierAuthority(identity.package_sid), &authority, sizeof(authority)) == 0 &&
        *GetSidSubAuthorityCount(identity.package_sid) >= 8 && *GetSidSubAuthority(identity.package_sid, 0) == SECURITY_APP_PACKAGE_BASE_RID;
}
}

DWORD remove_guard(GuardWfpApi& api, const GuardKeys& keys) {
    if (!valid_keys(keys)) return ERROR_INVALID_PARAMETER;
    DWORD status = api.begin();
    if (status != ERROR_SUCCESS) return status;
    status = api.check_sublayer_owner(keys.sublayer);
    const bool sublayer_missing = status == FWP_E_SUBLAYER_NOT_FOUND;
    if (sublayer_missing) status = ERROR_SUCCESS;
    std::array<bool, 6> present{};
    for (std::size_t i = 0; status == ERROR_SUCCESS && i < keys.filters.size(); ++i) {
        const DWORD checked = api.check_filter_owner(keys.filters[i], keys.sublayer);
        if (checked == FWP_E_FILTER_NOT_FOUND) continue;
        if (checked != ERROR_SUCCESS) { status = checked; break; }
        if (sublayer_missing) { status = ERROR_INVALID_DATA; break; }
        present[i] = true;
    }
    if (status == ERROR_SUCCESS) {
        for (std::size_t i = 0; i < keys.filters.size(); ++i) {
            if (!present[i]) continue;
            status = api.delete_filter(keys.filters[i]);
            if (status != ERROR_SUCCESS) break;
        }
    }
    if (status == ERROR_SUCCESS && !sublayer_missing) status = api.delete_sublayer(keys.sublayer);
    if (status == ERROR_SUCCESS) status = api.commit();
    if (status != ERROR_SUCCESS) api.abort();
    return status;
}
DWORD NativeGuardWfpApi::begin() { return FwpmTransactionBegin0(engine_, 0); }
DWORD NativeGuardWfpApi::add_sublayer(const GUID& key) {
    FWPM_SUBLAYER0 sublayer{};
    sublayer.subLayerKey = key;
    sublayer.displayData.name = const_cast<wchar_t*>(guard_name);
    sublayer.flags = FWPM_SUBLAYER_FLAG_PERSISTENT;
    sublayer.weight = 0xffff;
    return FwpmSubLayerAdd0(engine_, &sublayer, nullptr);
}
DWORD NativeGuardWfpApi::add_filter(const FWPM_FILTER0& filter) {
    return FwpmFilterAdd0(engine_, &filter, nullptr, nullptr);
}
DWORD NativeGuardWfpApi::check_filter_owner(const GUID& key, const GUID& sublayer) {
    FWPM_FILTER0* filter = nullptr;
    DWORD status = FwpmFilterGetByKey0(engine_, &key, &filter);
    if (status != ERROR_SUCCESS) return status;
    const bool owned = IsEqualGUID(filter->subLayerKey, sublayer) && same_guard_name(filter->displayData.name) &&
        (IsEqualGUID(filter->layerKey, FWPM_LAYER_ALE_AUTH_CONNECT_V4) || IsEqualGUID(filter->layerKey, FWPM_LAYER_ALE_AUTH_CONNECT_V6));
    FwpmFreeMemory0(reinterpret_cast<void**>(&filter));
    return owned ? ERROR_SUCCESS : ERROR_ACCESS_DENIED;
}
DWORD NativeGuardWfpApi::check_sublayer_owner(const GUID& key) {
    FWPM_SUBLAYER0* sublayer = nullptr;
    DWORD status = FwpmSubLayerGetByKey0(engine_, &key, &sublayer);
    if (status != ERROR_SUCCESS) return status;
    const bool owned = same_guard_name(sublayer->displayData.name) && (sublayer->flags & FWPM_SUBLAYER_FLAG_PERSISTENT);
    FwpmFreeMemory0(reinterpret_cast<void**>(&sublayer));
    return owned ? ERROR_SUCCESS : ERROR_ACCESS_DENIED;
}
DWORD NativeGuardWfpApi::delete_filter(const GUID& key) { return FwpmFilterDeleteByKey0(engine_, &key); }
DWORD NativeGuardWfpApi::delete_sublayer(const GUID& key) { return FwpmSubLayerDeleteByKey0(engine_, &key); }
DWORD NativeGuardWfpApi::commit() { return FwpmTransactionCommit0(engine_); }
DWORD NativeGuardWfpApi::abort() { return FwpmTransactionAbort0(engine_); }

DWORD install_guard(GuardWfpApi& api, FWP_BYTE_BLOB& app_id, std::uint64_t interface_luid, const GuardKeys& keys) {
    return install_guard(api, GuardIdentity{GuardIdentityType::Executable, &app_id, nullptr}, interface_luid, keys);
}
DWORD install_guard(GuardWfpApi& api, const GuardIdentity& identity, std::uint64_t interface_luid, const GuardKeys& keys) {
    if (!valid_identity(identity) || !interface_luid || !valid_keys(keys)) return ERROR_INVALID_PARAMETER;
    const auto specs = make_guard_filter_specs(L"validated identity", interface_luid);
    DWORD status = api.begin();
    if (status != ERROR_SUCCESS) return status;
    status = api.add_sublayer(keys.sublayer);
    for (std::size_t i = 0; status == ERROR_SUCCESS && i < specs.size(); ++i) {
        GuardNativeFilter filter(specs[i], identity, keys.sublayer, keys.filters[i]);
        status = api.add_filter(filter.value());
    }
    if (status == ERROR_SUCCESS) status = api.commit();
    if (status != ERROR_SUCCESS) api.abort();
    return status;
}
GuardNativeFilter::GuardNativeFilter(const GuardFilterSpec& spec, FWP_BYTE_BLOB& app_id,
                                     const GUID& sublayer, const GUID& filter_key)
    : GuardNativeFilter(spec, GuardIdentity{GuardIdentityType::Executable, &app_id, nullptr}, sublayer, filter_key) {}
GuardNativeFilter::GuardNativeFilter(const GuardFilterSpec& spec, const GuardIdentity& identity,
                                     const GUID& sublayer, const GUID& filter_key) {
    filter_.filterKey = filter_key;
    filter_.displayData.name = const_cast<wchar_t*>(guard_name);
    filter_.layerKey = spec.family == GuardFamily::Ipv4 ? FWPM_LAYER_ALE_AUTH_CONNECT_V4 : FWPM_LAYER_ALE_AUTH_CONNECT_V6;
    filter_.subLayerKey = sublayer;
    filter_.flags = spec.lifetime == GuardLifetime::Boot ? FWPM_FILTER_FLAG_BOOTTIME : FWPM_FILTER_FLAG_PERSISTENT;
    const bool permit = spec.action == GuardAction::PermitSelectedInterface;
    filter_.action.type = permit ? FWP_ACTION_PERMIT : FWP_ACTION_BLOCK;
    weight_ = permit ? UINT64_MAX : 0;
    filter_.weight.type = FWP_UINT64;
    filter_.weight.uint64 = &weight_;
    const bool package = identity.type == GuardIdentityType::PackageSid;
    conditions_[0].fieldKey = package ? FWPM_CONDITION_ALE_PACKAGE_ID : FWPM_CONDITION_ALE_APP_ID;
    conditions_[0].matchType = FWP_MATCH_EQUAL;
    conditions_[0].conditionValue.type = package ? FWP_SID : FWP_BYTE_BLOB_TYPE;
    if (package) conditions_[0].conditionValue.sid = identity.package_sid;
    else conditions_[0].conditionValue.byteBlob = identity.executable_id;
    filter_.numFilterConditions = permit ? 2 : 1;
    filter_.filterCondition = conditions_;
    if (permit) {
        interface_luid_ = spec.interface_luid;
        conditions_[1].fieldKey = FWPM_CONDITION_IP_NEXTHOP_INTERFACE;
        conditions_[1].matchType = FWP_MATCH_EQUAL;
        conditions_[1].conditionValue.type = FWP_UINT64;
        conditions_[1].conditionValue.uint64 = &interface_luid_;
    }
}
std::vector<GuardFilterSpec> make_guard_filter_specs(const std::wstring& executable, std::uint64_t interface_luid) {
    if (executable.empty() || interface_luid == 0) return {};
    std::vector<GuardFilterSpec> result;
    for (const auto family : {GuardFamily::Ipv4, GuardFamily::Ipv6}) {
        result.push_back({family, GuardLifetime::Boot, GuardAction::Block, 0});
        result.push_back({family, GuardLifetime::Persistent, GuardAction::Block, 0});
        result.push_back({family, GuardLifetime::Persistent, GuardAction::PermitSelectedInterface, interface_luid});
    }
    return result;
}
}
