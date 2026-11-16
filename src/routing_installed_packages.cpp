#include "dialog_app_routing.h"
#include <winrt/Windows.Management.Deployment.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <roapi.h>
#include <appmodel.h>
#include <tlhelp32.h>
#include <algorithm>

namespace quickdns {
namespace {
template<class Query>
std::wstring package_string(Query query, HANDLE process) {
    UINT32 size = 0;
    if (query(process, &size, nullptr) != ERROR_INSUFFICIENT_BUFFER || size == 0) return {};
    std::wstring value(size, L'\0');
    if (query(process, &size, value.data()) != ERROR_SUCCESS || size == 0) return {};
    value.resize(size - 1);
    return value;
}

void append_running_packages(std::vector<InstalledPackage>& packages) {
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL next = Process32FirstW(snapshot, &entry); next; next = Process32NextW(snapshot, &entry)) {
        const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
        if (!process) continue;
        const auto family = package_string(GetPackageFamilyName, process);
        const bool known = std::any_of(packages.begin(), packages.end(), [&](const auto& package) {
            return package.family == family && !package.install_path.empty();
        });
        if (family.empty() || known) { CloseHandle(process); continue; }
        const auto full_name = package_string(GetPackageFullName, process);
        std::wstring image(32768, L'\0');
        DWORD size = static_cast<DWORD>(image.size());
        const BOOL has_image = QueryFullProcessImageNameW(process, 0, image.data(), &size);
        CloseHandle(process);
        if (!has_image || full_name.empty()) continue;
        image.resize(size);
        auto folder = std::filesystem::path(image).parent_path();
        while (!folder.empty() && folder != folder.root_path()) {
            if (CompareStringOrdinal(folder.filename().c_str(), -1, full_name.c_str(), -1, TRUE) == CSTR_EQUAL) {
                auto existing = std::find_if(packages.begin(), packages.end(), [&](const auto& package) {
                    return package.family == family;
                });
                if (existing != packages.end()) existing->install_path = folder.native();
                else packages.push_back({family.substr(0, family.rfind(L'_')), family, folder.native()});
                break;
            }
            folder = folder.parent_path();
        }
    }
    CloseHandle(snapshot);
}
}

std::vector<InstalledPackage> enumerate_installed_packages() {
    std::vector<InstalledPackage> result;
    const HRESULT initialized = RoInitialize(RO_INIT_SINGLETHREADED);
    if (SUCCEEDED(initialized) || initialized == RPC_E_CHANGED_MODE) try {
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
    } catch (const winrt::hresult_error&) {
        // Running package identities are still available if deployment enumeration fails.
    }
    if (SUCCEEDED(initialized)) RoUninitialize();
    append_running_packages(result);
    std::stable_sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        if (left.family != right.family) return left.family < right.family;
        return !left.install_path.empty() && right.install_path.empty();
    });
    result.erase(std::unique(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.family == right.family;
    }), result.end());
    return result;
}
}
