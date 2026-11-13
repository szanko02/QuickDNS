#pragma once

#include "routing_adapters.h"
#include "app_routing_config.h"
#include <windows.h>
#include <filesystem>
#include <optional>
#include <vector>

namespace quickdns {
enum RoutingDialogControl { RoutingKind = 8201, RoutingIdentity, RoutingBrowse, RoutingAdapterCombo,
                            RoutingList, RoutingAdd, RoutingRemove, RoutingSave, RoutingStatus, RoutingPackages };
struct InstalledPackage { std::wstring name; std::wstring family; };
std::vector<InstalledPackage> enumerate_installed_packages();

// An editor for inactive drafts only. It does not install or activate protection.
class AppRoutingDialog {
public:
    AppRoutingDialog(std::filesystem::path path, std::vector<RoutingAdapter> adapters,
                     std::optional<std::vector<InstalledPackage>> packages = std::nullopt);
    ~AppRoutingDialog();
    AppRoutingDialog(const AppRoutingDialog&) = delete;
    AppRoutingDialog& operator=(const AppRoutingDialog&) = delete;
    bool create(HINSTANCE instance, HWND parent = nullptr);
    HWND window() const { return window_; }
    void show_modal();
private:
    static LRESULT CALLBACK procedure(HWND, UINT, WPARAM, LPARAM);
    LRESULT dispatch(UINT, WPARAM, LPARAM);
    AppRoutingConfigStore store_;
    std::vector<RoutingAdapter> adapters_;
    std::optional<std::vector<InstalledPackage>> packages_;
    std::vector<AppRoutingRule> drafts_;
    HINSTANCE instance_ = nullptr;
    HWND parent_ = nullptr;
    HWND window_ = nullptr;
};
}
