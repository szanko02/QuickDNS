#pragma once

#include <windows.h>
#include <fwpmu.h>

namespace quickdns {

using ConnectionPolicyAddFn = DWORD (WINAPI *)(HANDLE, const FWPM_PROVIDER_CONTEXT3*, FWP_IP_VERSION,
                                               UINT64, UINT32, const FWPM_FILTER_CONDITION0*, PSECURITY_DESCRIPTOR);
using ConnectionPolicyDeleteFn = DWORD (WINAPI *)(HANDLE, const GUID*);

// Callers must install a fail-closed guard before adding policies. Use a WFP
// transaction when adding both address families and retain the GUIDs for removal.
class ConnectionPolicyApi {
public:
    ConnectionPolicyApi();
    ConnectionPolicyApi(ConnectionPolicyAddFn add, ConnectionPolicyDeleteFn remove);
    ~ConnectionPolicyApi();
    ConnectionPolicyApi(const ConnectionPolicyApi&) = delete;
    ConnectionPolicyApi& operator=(const ConnectionPolicyApi&) = delete;
    bool supported() const;
    DWORD add(HANDLE engine, const GUID& policy_key, FWP_IP_VERSION family,
              UINT64 interface_luid, FWP_BYTE_BLOB& app_id) const;
    DWORD add_package(HANDLE engine, const GUID& policy_key, FWP_IP_VERSION family,
                      UINT64 interface_luid, PSID package_sid) const;
    DWORD remove(HANDLE engine, const GUID& policy_key) const;
private:
    HMODULE module_ = nullptr;
    ConnectionPolicyAddFn add_ = nullptr;
    ConnectionPolicyDeleteFn remove_ = nullptr;
};
}
