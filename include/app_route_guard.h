#pragma once

#include <cstdint>
#include <array>
#include <string>
#include <vector>
#include <windows.h>
#include <fwpmu.h>

namespace quickdns {

enum class GuardLifetime { Boot, Persistent };
enum class GuardAction { Block, PermitSelectedInterface };
enum class GuardFamily { Ipv4, Ipv6 };
enum class GuardIdentityType { Executable, PackageSid };

struct GuardIdentity {
    GuardIdentityType type;
    FWP_BYTE_BLOB* executable_id = nullptr;
    SID* package_sid = nullptr;
};

struct GuardFilterSpec {
    GuardFamily family;
    GuardLifetime lifetime;
    GuardAction action;
    std::uint64_t interface_luid;
};

// An empty result means that the identity or selected interface is invalid.
std::vector<GuardFilterSpec> make_guard_filter_specs(const std::wstring& executable,
                                                     std::uint64_t interface_luid);

class GuardNativeFilter {
public:
    GuardNativeFilter(const GuardFilterSpec& spec, FWP_BYTE_BLOB& app_id,
                      const GUID& sublayer, const GUID& filter_key);
    GuardNativeFilter(const GuardFilterSpec& spec, const GuardIdentity& identity,
                      const GUID& sublayer, const GUID& filter_key);
    GuardNativeFilter(const GuardNativeFilter&) = delete;
    GuardNativeFilter& operator=(const GuardNativeFilter&) = delete;
    const FWPM_FILTER0& value() const { return filter_; }
private:
    FWPM_FILTER0 filter_{};
    FWPM_FILTER_CONDITION0 conditions_[2]{};
    UINT64 interface_luid_ = 0;
    UINT64 weight_ = 0;
};

struct GuardKeys {
    GUID sublayer{};
    std::array<GUID, 6> filters{};
};

// All operations are performed in one WFP transaction. The caller must durably
// save the keys before install so that a crash cannot orphan persistent filters.
class GuardWfpApi {
public:
    virtual ~GuardWfpApi() = default;
    virtual DWORD begin() = 0;
    virtual DWORD add_sublayer(const GUID&) = 0;
    virtual DWORD add_filter(const FWPM_FILTER0&) = 0;
    virtual DWORD check_filter_owner(const GUID& filter, const GUID& sublayer) = 0;
    virtual DWORD check_sublayer_owner(const GUID& sublayer) = 0;
    virtual DWORD delete_filter(const GUID&) = 0;
    virtual DWORD delete_sublayer(const GUID&) = 0;
    virtual DWORD commit() = 0;
    virtual DWORD abort() = 0;
};

class NativeGuardWfpApi final : public GuardWfpApi {
public:
    explicit NativeGuardWfpApi(HANDLE engine) : engine_(engine) {}
    DWORD begin() override;
    DWORD add_sublayer(const GUID& key) override;
    DWORD add_filter(const FWPM_FILTER0& filter) override;
    DWORD check_filter_owner(const GUID& filter, const GUID& sublayer) override;
    DWORD check_sublayer_owner(const GUID& sublayer) override;
    DWORD delete_filter(const GUID& key) override;
    DWORD delete_sublayer(const GUID& key) override;
    DWORD commit() override;
    DWORD abort() override;
private:
    HANDLE engine_;
};

DWORD install_guard(GuardWfpApi& api, FWP_BYTE_BLOB& app_id, std::uint64_t interface_luid,
                    const GuardKeys& keys);
DWORD install_guard(GuardWfpApi& api, const GuardIdentity& identity, std::uint64_t interface_luid,
                    const GuardKeys& keys);
DWORD remove_guard(GuardWfpApi& api, const GuardKeys& keys);

}
