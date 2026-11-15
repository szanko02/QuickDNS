#include "app_policy_filter_identity.h"
#include <cstring>
namespace quickdns {
PolicyFilterAudit::PolicyFilterAudit(const GUID& key, FWP_IP_VERSION family, const GuardIdentity& identity)
    : policy_key_(key), family_(family), identity_(identity) {}
void PolicyFilterAudit::observe(const FWPM_FILTER0& filter) {
    if (!(filter.flags & FWPM_FILTER_FLAG_HAS_PROVIDER_CONTEXT) || !IsEqualGUID(filter.providerContextKey, policy_key_)) return;
    if (policy_filter_matches_identity(filter, policy_key_, family_, identity_)) ++matching_;
    else foreign_ = true;
}
bool PolicyFilterAudit::valid() const { return !foreign_ && matching_ == 1; }
bool policy_filter_matches_identity(const FWPM_FILTER0& filter, const GUID& key, FWP_IP_VERSION family,
                                    const GuardIdentity& identity) {
    if (!(filter.flags & FWPM_FILTER_FLAG_HAS_PROVIDER_CONTEXT) || !IsEqualGUID(filter.providerContextKey, key) ||
        (family != FWP_IP_VERSION_V4 && family != FWP_IP_VERSION_V6) ||
        !IsEqualGUID(filter.layerKey, family == FWP_IP_VERSION_V4 ? FWPM_LAYER_OUTBOUND_NETWORK_CONNECTION_POLICY_V4 :
                                                                 FWPM_LAYER_OUTBOUND_NETWORK_CONNECTION_POLICY_V6) ||
        filter.numFilterConditions != 1 || !filter.filterCondition) return false;
    const auto& condition = filter.filterCondition[0];
    if (condition.matchType != FWP_MATCH_EQUAL) return false;
    if (identity.type == GuardIdentityType::Executable && identity.executable_id && !identity.package_sid &&
        IsEqualGUID(condition.fieldKey, FWPM_CONDITION_ALE_APP_ID) && condition.conditionValue.type == FWP_BYTE_BLOB_TYPE) {
        const auto* expected = identity.executable_id;
        const auto* actual = condition.conditionValue.byteBlob;
        return expected->size && expected->data && actual && actual->data && expected->size == actual->size &&
            std::memcmp(expected->data, actual->data, expected->size) == 0;
    }
    if (identity.type == GuardIdentityType::PackageSid && identity.package_sid && !identity.executable_id &&
        IsEqualGUID(condition.fieldKey, FWPM_CONDITION_ALE_PACKAGE_ID) && condition.conditionValue.type == FWP_SID &&
        condition.conditionValue.sid && IsValidSid(identity.package_sid) && IsValidSid(condition.conditionValue.sid))
        return EqualSid(identity.package_sid, condition.conditionValue.sid) != FALSE;
    return false;
}
}
