#include "dialog_app_routing.h"
#include <winrt/Windows.Management.Deployment.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <roapi.h>
#include <appmodel.h>
#include <algorithm>

namespace quickdns {
std::vector<InstalledPackage> enumerate_installed_packages() {
    std::vector<InstalledPackage> result;
    const HRESULT initialized = RoInitialize(RO_INIT_SINGLETHREADED);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) return result;
    try {
        winrt::Windows::Management::Deployment::PackageManager manager;
        for (const auto& package : manager.FindPackagesForUser(L"")) {
            if (package.IsFramework() || package.IsResourcePackage() || package.IsBundle()) continue;
            const auto id = package.Id();
            const auto name = id.Name();
            const auto family = id.FamilyName();
            if (family.empty()) continue;
            const auto full_name = id.FullName();
            UINT32 length = 0;
            std::wstring path;
            if (GetPackagePathByFullName(full_name.c_str(), &length, nullptr) == ERROR_INSUFFICIENT_BUFFER) {
                path.resize(length);
                if (GetPackagePathByFullName(full_name.c_str(), &length, path.data()) == ERROR_SUCCESS && length)
                    path.resize(length - 1);
                else path.clear();
            }
            result.push_back({std::wstring(name), std::wstring(family), std::move(path)});
        }
        std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
            return left.family < right.family;
        });
        result.erase(std::unique(result.begin(), result.end(), [](const auto& left, const auto& right) {
            return left.family == right.family;
        }), result.end());
    } catch (const winrt::hresult_error&) {
        result.clear();
    }
    if (SUCCEEDED(initialized)) RoUninitialize();
    return result;
}
}
