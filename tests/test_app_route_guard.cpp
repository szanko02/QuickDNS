#include "app_route_guard.h"
#include <iostream>
#include <stdexcept>

static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

class FakeWfp final : public quickdns::GuardWfpApi {
public:
    DWORD begin() override { ++begun; return ERROR_SUCCESS; }
    DWORD add_sublayer(const GUID&) override { return ERROR_SUCCESS; }
    DWORD add_filter(const FWPM_FILTER0&) override { return ++added == fail_at ? ERROR_ACCESS_DENIED : ERROR_SUCCESS; }
    DWORD check_filter_owner(const GUID&, const GUID&) override {
        ++checked;
        if (checked == foreign_at) return ERROR_ACCESS_DENIED;
        return (missing_all || checked == missing_at) ? FWP_E_FILTER_NOT_FOUND : ERROR_SUCCESS;
    }
    DWORD check_sublayer_owner(const GUID&) override { return missing_sublayer ? FWP_E_SUBLAYER_NOT_FOUND : ERROR_SUCCESS; }
    DWORD delete_filter(const GUID&) override { return ++deleted == delete_fail_at ? ERROR_ACCESS_DENIED : ERROR_SUCCESS; }
    DWORD delete_sublayer(const GUID&) override { ++sublayers_deleted; return ERROR_SUCCESS; }
    DWORD commit() override { ++committed; return ERROR_SUCCESS; }
    DWORD abort() override { ++aborted; return ERROR_SUCCESS; }
    int begun = 0, added = 0, committed = 0, aborted = 0, fail_at = -1, deleted = 0, sublayers_deleted = 0;
    int checked = 0, foreign_at = -1, delete_fail_at = -1;
    int missing_at = -1;
    bool missing_all = false, missing_sublayer = false;
};

int main() {
    using namespace quickdns;
    try {
        require(make_guard_filter_specs(L"", 55).empty(), "empty identity must fail closed");
        require(make_guard_filter_specs(L"C:\\App\\a.exe", 0).empty(), "missing interface must fail closed");
        const auto specs = make_guard_filter_specs(L"C:\\App\\a.exe", 55);
        require(specs.size() == 6, "each family needs a boot block and persistent block/permit");
        for (const auto family : {GuardFamily::Ipv4, GuardFamily::Ipv6}) {
            int boot_block = 0, persistent_block = 0, persistent_permit = 0;
            for (const auto& spec : specs) {
                if (spec.family != family) continue;
                if (spec.lifetime == GuardLifetime::Boot && spec.action == GuardAction::Block) ++boot_block;
                if (spec.lifetime == GuardLifetime::Persistent && spec.action == GuardAction::Block) ++persistent_block;
                if (spec.lifetime == GuardLifetime::Persistent && spec.action == GuardAction::PermitSelectedInterface) {
                    require(spec.interface_luid == 55, "permit must target only the selected interface");
                    ++persistent_permit;
                }
            }
            require(boot_block == 1 && persistent_block == 1 && persistent_permit == 1,
                    "missing or duplicate protection filter");
        }
        unsigned char bytes[] = {1, 2};
        FWP_BYTE_BLOB app_id{sizeof(bytes), bytes};
        GUID sublayer{1}, key{2};
        for (const auto& spec : specs) {
            GuardNativeFilter native(spec, app_id, sublayer, key);
            const auto& f = native.value();
            require(f.action.type == static_cast<FWP_ACTION_TYPE>(spec.action == GuardAction::Block ? FWP_ACTION_BLOCK : FWP_ACTION_PERMIT), "incorrect WFP action");
            require(f.flags == static_cast<UINT32>(spec.lifetime == GuardLifetime::Boot ? FWPM_FILTER_FLAG_BOOTTIME : FWPM_FILTER_FLAG_PERSISTENT), "incorrect WFP lifetime");
            require(f.numFilterConditions == (spec.action == GuardAction::Block ? 1u : 2u), "incorrect WFP conditions");
            require(IsEqualGUID(f.filterCondition[0].fieldKey, FWPM_CONDITION_ALE_APP_ID), "application identity missing");
            require(f.filterCondition[0].conditionValue.byteBlob == &app_id, "application identity changed");
            if (spec.action == GuardAction::PermitSelectedInterface) {
                require(IsEqualGUID(f.filterCondition[1].fieldKey, FWPM_CONDITION_IP_NEXTHOP_INTERFACE), "next-hop condition missing");
                require(*f.filterCondition[1].conditionValue.uint64 == 55, "wrong WFP interface");
            }
        }
        GuardKeys keys{};
        keys.sublayer.Data1 = 1;
        for (std::size_t i = 0; i < keys.filters.size(); ++i) keys.filters[i].Data1 = static_cast<unsigned long>(i + 2);
        SID_IDENTIFIER_AUTHORITY authority = SECURITY_APP_PACKAGE_AUTHORITY;
        BYTE sid_buffer[SECURITY_MAX_SID_SIZE]{};
        SID* package_sid = reinterpret_cast<SID*>(sid_buffer);
        require(InitializeSid(package_sid, &authority, 8) != FALSE, "cannot initialize SID");
        *GetSidSubAuthority(package_sid, 0) = SECURITY_APP_PACKAGE_BASE_RID;
        for (DWORD i = 1; i < 8; ++i) *GetSidSubAuthority(package_sid, i) = i;
        GuardIdentity package{GuardIdentityType::PackageSid, nullptr, package_sid};
        GuardNativeFilter package_filter(specs[0], package, sublayer, key);
        require(package_filter.value().numFilterConditions == 1, "package identity condition missing");
        require(IsEqualGUID(package_filter.value().filterCondition[0].fieldKey, FWPM_CONDITION_ALE_PACKAGE_ID), "package rule must use package identity");
        require(package_filter.value().filterCondition[0].conditionValue.sid == package_sid, "package SID changed");
        FakeWfp package_engine;
        require(install_guard(package_engine, package, 55, keys) == ERROR_SUCCESS, "valid package rule failed");
        BYTE generic_buffer[SECURITY_MAX_SID_SIZE]{};
        SID* generic_sid = reinterpret_cast<SID*>(generic_buffer);
        require(InitializeSid(generic_sid, &authority, 1) != FALSE, "cannot initialize generic SID");
        *GetSidSubAuthority(generic_sid, 0) = SECURITY_APP_PACKAGE_BASE_RID;
        GuardIdentity generic{GuardIdentityType::PackageSid, nullptr, generic_sid};
        FakeWfp generic_engine;
        require(install_guard(generic_engine, generic, 55, keys) == ERROR_INVALID_PARAMETER && generic_engine.begun == 0,
                "generic package identity must not protect multiple packages");
        FakeWfp successful;
        require(install_guard(successful, app_id, 55, keys) == ERROR_SUCCESS, "valid transaction failed");
        require(successful.added == 6 && successful.committed == 1 && successful.aborted == 0, "guard was not committed atomically");
        FakeWfp failing;
        failing.fail_at = 4;
        require(install_guard(failing, app_id, 55, keys) == ERROR_ACCESS_DENIED, "WFP error not propagated");
        require(failing.committed == 0 && failing.aborted == 1, "partial guard must be aborted");
        FakeWfp removal;
        require(remove_guard(removal, keys) == ERROR_SUCCESS, "guard cleanup failed");
        require(removal.deleted == 6 && removal.sublayers_deleted == 1 && removal.committed == 1,
                "guard cleanup must delete only the recorded keys atomically");
        FakeWfp foreign;
        foreign.foreign_at = 3;
        require(remove_guard(foreign, keys) == ERROR_ACCESS_DENIED, "foreign filter must be rejected");
        require(foreign.deleted == 0 && foreign.aborted == 1 && foreign.committed == 0,
                "foreign filter detection must abort before deleting anything");
        FakeWfp deletion_failure;
        deletion_failure.delete_fail_at = 4;
        require(remove_guard(deletion_failure, keys) == ERROR_ACCESS_DENIED, "deletion error not propagated");
        require(deletion_failure.aborted == 1 && deletion_failure.committed == 0,
                "partial cleanup must roll back");
        FakeWfp partial;
        partial.missing_at = 3;
        require(remove_guard(partial, keys) == ERROR_SUCCESS && partial.deleted == 5 && partial.committed == 1,
                "partially removed guard must be cleaned idempotently");
        FakeWfp absent;
        absent.missing_sublayer = true;
        absent.missing_all = true;
        require(remove_guard(absent, keys) == ERROR_SUCCESS && absent.deleted == 0 && absent.sublayers_deleted == 0,
                "recovery of uninstalled guard must succeed without deletion");
        FakeWfp inconsistent;
        inconsistent.missing_sublayer = true;
        require(remove_guard(inconsistent, keys) != ERROR_SUCCESS && inconsistent.deleted == 0,
                "filters without recorded sublayer must not be deleted");
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << '\n';
        return 1;
    }
}
