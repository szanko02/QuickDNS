#include "dialog_app_routing.h"
#include <winrt/Windows.Management.Deployment.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <roapi.h>
#include <algorithm>

namespace quickdns {
std::vector<InstalledPackage> enumerate_installed_packages() {
    std::vector<InstalledPackage> result;
    const HRESULT initialized = RoInitialize(RO_INIT_SINGLETHREADED);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) return result;
    try {
        winrt::Windows::Management::Deployment::PackageManager manager;
        for (const auto& package : manager.FindPackagesForUser(L"")) {
            const auto id = package.Id();
            const auto name = id.Name();
            const auto family = id.FamilyName();
            if (!family.empty()) result.push_back({std::wstring(name), std::wstring(family)});
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
