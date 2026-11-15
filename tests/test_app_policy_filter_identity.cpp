#include "app_policy_filter_identity.h"
#include <iostream>
#include <stdexcept>

using namespace quickdns;
static void require(bool v, const char* message) { if (!v) throw std::runtime_error(message); }
int main() {
    try {
        GUID key{}; key.Data1 = 42;
        BYTE bytes[] = {1,2,3}; FWP_BYTE_BLOB app{sizeof(bytes), bytes};
        GuardIdentity identity{GuardIdentityType::Executable, &app, nullptr};
        FWPM_FILTER_CONDITION0 condition{};
        condition.fieldKey = FWPM_CONDITION_ALE_APP_ID; condition.matchType = FWP_MATCH_EQUAL;
        condition.conditionValue.type = FWP_BYTE_BLOB_TYPE; condition.conditionValue.byteBlob = &app;
        FWPM_FILTER0 filter{};
        filter.flags = FWPM_FILTER_FLAG_HAS_PROVIDER_CONTEXT; filter.providerContextKey = key;
        filter.layerKey = FWPM_LAYER_OUTBOUND_NETWORK_CONNECTION_POLICY_V4;
        filter.numFilterConditions = 1; filter.filterCondition = &condition;
        require(policy_filter_matches_identity(filter, key, FWP_IP_VERSION_V4, identity), "matching app identity rejected");
        PolicyFilterAudit one(key, FWP_IP_VERSION_V4, identity); one.observe(filter);
        require(one.valid(), "single associated policy filter rejected");
        PolicyFilterAudit duplicate(key, FWP_IP_VERSION_V4, identity); duplicate.observe(filter); duplicate.observe(filter);
        require(!duplicate.valid(), "multiple associated filters accepted");
        PolicyFilterAudit missing(key, FWP_IP_VERSION_V4, identity);
        require(!missing.valid(), "missing associated filter accepted");
        BYTE other_bytes[] = {1,2,4}; FWP_BYTE_BLOB other{sizeof(other_bytes), other_bytes};
        condition.conditionValue.byteBlob = &other;
        require(!policy_filter_matches_identity(filter, key, FWP_IP_VERSION_V4, identity), "foreign app accepted");
        PolicyFilterAudit wrong(key, FWP_IP_VERSION_V4, identity); wrong.observe(filter);
        require(!wrong.valid(), "associated foreign filter accepted");
        condition.conditionValue.byteBlob = &app;
        require(!policy_filter_matches_identity(filter, key, FWP_IP_VERSION_V6, identity), "wrong address family accepted");
        GUID other_key{}; other_key.Data1 = 43;
        require(!policy_filter_matches_identity(filter, other_key, FWP_IP_VERSION_V4, identity), "foreign context accepted");
        filter.numFilterConditions = 0;
        require(!policy_filter_matches_identity(filter, key, FWP_IP_VERSION_V4, identity), "unconditional filter accepted");
        filter.numFilterConditions = 1;
        SID_IDENTIFIER_AUTHORITY authority = SECURITY_APP_PACKAGE_AUTHORITY;
        PSID a = nullptr, b = nullptr;
        require(AllocateAndInitializeSid(&authority, 2, SECURITY_APP_PACKAGE_BASE_RID, 123, 0,0,0,0,0,0, &a) &&
                AllocateAndInitializeSid(&authority, 2, SECURITY_APP_PACKAGE_BASE_RID, 124, 0,0,0,0,0,0, &b), "could not create test SIDs");
        GuardIdentity package{GuardIdentityType::PackageSid, nullptr, static_cast<SID*>(a)};
        condition.fieldKey = FWPM_CONDITION_ALE_PACKAGE_ID; condition.conditionValue.type = FWP_SID;
        condition.conditionValue.sid = static_cast<SID*>(a);
        const bool package_ok = policy_filter_matches_identity(filter, key, FWP_IP_VERSION_V4, package);
        condition.conditionValue.sid = static_cast<SID*>(b);
        const bool foreign_package = policy_filter_matches_identity(filter, key, FWP_IP_VERSION_V4, package);
        FreeSid(a); FreeSid(b);
        require(package_ok && !foreign_package, "package SID validation failed");
        return 0;
    } catch (const std::exception& ex) { std::cerr << ex.what() << '\n'; return 1; }
}
