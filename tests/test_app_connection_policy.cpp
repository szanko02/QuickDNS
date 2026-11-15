#include "app_connection_policy.h"
#include <stdexcept>
#include <iostream>

using namespace quickdns;
static void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
static unsigned calls = 0;
static GUID expected_key = {0x12345678, 0xabcd, 0xef01, {1,2,3,4,5,6,7,8}};
static BYTE app_bytes[] = {1,2,3};
static BYTE sid_bytes[] = {1,1,0,0,0,0,0,15,2,0,0,0};
static bool expect_package = false;
static DWORD WINAPI fake_add(HANDLE engine, const FWPM_PROVIDER_CONTEXT3* context, FWP_IP_VERSION family,
                             UINT64, UINT32 count, const FWPM_FILTER_CONDITION0* conditions, PSECURITY_DESCRIPTOR) {
    ++calls;
    check(engine == reinterpret_cast<HANDLE>(1), "wrong engine");
    check(IsEqualGUID(context->providerContextKey, expected_key), "wrong key");
    check(context->type == FWPM_NETWORK_CONNECTION_POLICY_CONTEXT, "wrong provider context");
    check(family == FWP_IP_VERSION_V6, "wrong address family");
    check(context->networkConnectionPolicy && context->networkConnectionPolicy->numSettings == 1, "missing route setting");
    const auto& setting = context->networkConnectionPolicy->settings[0];
    check(setting.type == FWP_NETWORK_CONNECTION_POLICY_NEXT_HOP_INTERFACE && setting.value.type == FWP_UINT64 &&
          *setting.value.uint64 == 1234, "wrong interface");
    check(count == 1, "wrong condition count");
    if (expect_package) {
        check(IsEqualGUID(conditions[0].fieldKey, FWPM_CONDITION_ALE_PACKAGE_ID) &&
              conditions[0].conditionValue.type == FWP_SID && reinterpret_cast<const void*>(conditions[0].conditionValue.sid) == sid_bytes,
              "wrong package condition");
    } else {
        check(IsEqualGUID(conditions[0].fieldKey, FWPM_CONDITION_ALE_APP_ID) &&
              conditions[0].conditionValue.type == FWP_BYTE_BLOB_TYPE &&
              conditions[0].conditionValue.byteBlob->data == app_bytes, "wrong application condition");
    }
    return ERROR_ACCESS_DENIED;
}
static DWORD WINAPI fake_remove(HANDLE, const GUID* key) {
    check(IsEqualGUID(*key, expected_key), "wrong deletion key");
    return ERROR_SUCCESS;
}
int main() {
    try {
        ConnectionPolicyApi api(fake_add, fake_remove);
        check(api.supported(), "available API rejected");
        FWP_BYTE_BLOB app{3, app_bytes};
        check(api.add(reinterpret_cast<HANDLE>(1), expected_key, FWP_IP_VERSION_V6, 1234, app) == ERROR_ACCESS_DENIED, "native error lost");
        check(calls == 1, "native add not called");
        check(api.add(reinterpret_cast<HANDLE>(1), expected_key, FWP_IP_VERSION_NONE, 1234, app) == ERROR_INVALID_PARAMETER, "invalid family accepted");
        check(api.add(reinterpret_cast<HANDLE>(1), expected_key, FWP_IP_VERSION_V4, 0, app) == ERROR_INVALID_PARAMETER, "invalid LUID accepted");
        expect_package = true;
        check(api.add_package(reinterpret_cast<HANDLE>(1), expected_key, FWP_IP_VERSION_V6, 1234, sid_bytes) == ERROR_ACCESS_DENIED,
              "package policy not added");
        check(api.remove(reinterpret_cast<HANDLE>(1), expected_key) == ERROR_SUCCESS, "delete failed");
        ConnectionPolicyApi unavailable(nullptr, nullptr);
        check(!unavailable.supported(), "missing API reported available");
        check(unavailable.add(reinterpret_cast<HANDLE>(1), expected_key, FWP_IP_VERSION_V4, 1, app) == ERROR_CALL_NOT_IMPLEMENTED,
              "missing API must fail explicitly");
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
