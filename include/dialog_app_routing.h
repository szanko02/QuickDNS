#pragma once

#include "routing_adapters.h"
#include "app_routing_config.h"
#include "ui_palette.h"
#include <windows.h>
#include <filesystem>
#include <optional>
#include <vector>

namespace quickdns {
class SubnetRoutes;
class SplitRoutingSession;
enum RoutingDialogControl { RoutingKind = 8201, RoutingIdentity, RoutingBrowse, RoutingAdapterCombo,
                            RoutingList, RoutingAdd, RoutingRemove, RoutingSave, RoutingStatus, RoutingPackages,
                            SubnetDestination, SubnetGateway, SubnetAdapter, SubnetAdd, SubnetList,
                            SubnetRemove, SubnetStatus, SplitPrimary, SplitEngine, SplitBrowse, SplitStart, SplitStop, SplitStatus,
                            RoutingAppsPage, RoutingSubnetsPage, SplitNotice, SplitPrimaryLabel, SplitPrimaryDns, RoutingAdapterDns,
                            SplitChatGptDns, SplitDomains, SplitPublicWeb, SplitRulesLabel, SplitEngineLabel, SplitSecondaryLabel,
                            SplitHelp, SubnetHelp };
struct InstalledPackage { std::wstring name; std::wstring family; std::wstring install_path{}; };
std::vector<InstalledPackage> enumerate_installed_packages();

class AppRoutingDialog {
public:
    AppRoutingDialog(std::filesystem::path path, std::vector<RoutingAdapter> adapters,
                     std::optional<std::vector<InstalledPackage>> packages = std::nullopt,
                     SubnetRoutes* subnet_routes = nullptr, SplitRoutingSession* split_routing = nullptr,
                     bool dark = false);
    ~AppRoutingDialog();
    AppRoutingDialog(const AppRoutingDialog&) = delete;
    AppRoutingDialog& operator=(const AppRoutingDialog&) = delete;
    bool create(HINSTANCE instance, HWND parent = nullptr);
    HWND window() const { return window_; }
    void show_modal();
private:
    static LRESULT CALLBACK procedure(HWND, UINT, WPARAM, LPARAM);
    LRESULT dispatch(UINT, WPARAM, LPARAM);
    HWND control(HWND parent, const wchar_t* cls, const wchar_t* label, DWORD style,
                 int x, int y, int width, int height, int id = 0);
    void select_page(bool subnets);
    void update_adapter_dns(int adapter_control, int dns_control) const;
    void draw_control(const DRAWITEMSTRUCT& item) const;
    void create_subnet_controls();
    void refresh_subnets();
    void change_subnet(int command);
    void create_primary_control();
    void create_split_controls();
    void start_split();
    void refresh_split_status();
    AppRoutingConfigStore store_;
    std::vector<RoutingAdapter> adapters_;
    std::optional<std::vector<InstalledPackage>> packages_;
    std::vector<AppRoutingRule> drafts_;
    SubnetRoutes* subnet_routes_;
    SplitRoutingSession* split_routing_;
    bool dark_;
    ColorPalette palette_;
    HBRUSH background_ = nullptr;
    HBRUSH field_ = nullptr;
    HFONT font_ = nullptr;
    HFONT heading_ = nullptr;
    std::vector<HWND> app_controls_;
    std::vector<HWND> subnet_controls_;
    bool subnets_selected_ = false;
    bool creating_subnets_ = false;
    HINSTANCE instance_ = nullptr;
    HWND parent_ = nullptr;
    HWND window_ = nullptr;
};
}
