#include "dialog_app_routing.h"
#include "encoding_utils.h"
#include "split_routing.h"
#include <nlohmann/json.hpp>
#include <regex>
#include <shlobj.h>
#include <shellapi.h>
#include <algorithm>
#include <cwchar>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace quickdns;
static void check(bool ok, const char* msg) { if (!ok) throw std::runtime_error(msg); }
int main() {
    if (int count = 0; auto args = CommandLineToArgvW(GetCommandLineW(), &count)) {
        if (count == 3 && std::wstring_view(args[1]) == L"--stop-live-engine") {
            const auto pid = std::wcstoul(args[2], nullptr, 10);
            LocalFree(args);
            if (!pid) return 1;
            FreeConsole();
            if (!AttachConsole(pid)) return 1;
            SetConsoleCtrlHandler(nullptr, TRUE);
            const bool signalled = GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0) != FALSE;
            FreeConsole();
            return signalled ? 0 : 1;
        }
        if ((count == 7 || count == 8) && std::wstring_view(args[1]) == L"--write-live-config") {
            const std::wstring primary_name = args[2], secondary_name = args[3];
            const std::filesystem::path app = args[4], output = args[5], log = args[6];
            const std::wstring package_family = count == 8 ? args[7] : L"";
            LocalFree(args);
            const auto adapters = enumerate_routing_adapters();
            const auto find_adapter = [&](const std::wstring& name) {
                return std::find_if(adapters.begin(), adapters.end(), [&](const RoutingAdapter& adapter) {
                    return adapter.connected && adapter.name == name;
                });
            };
            const auto primary = find_adapter(primary_name), secondary = find_adapter(secondary_name);
            if (primary == adapters.end() || secondary == adapters.end() || primary == secondary) {
                std::cerr << "Specify two distinct connected adapters.\n";
                return 1;
            }
            const auto alias = new_split_routing_tun_alias();
            if (alias.empty()) return 1;
            auto config = nlohmann::json::parse(make_split_routing_config(
                *primary, *secondary, app, !package_family.empty(), log, package_family, alias));
            config["log"]["level"] = "debug";
            std::ofstream stream(output, std::ios::binary);
            stream << config.dump(2);
            stream.close();
            return stream ? 0 : 1;
        }
        LocalFree(args);
    }
    const auto path = std::filesystem::temp_directory_path() / (L"quickdns_dialog_" + std::to_wstring(GetCurrentProcessId()) + L".json");
    std::filesystem::remove(path);
    try {
        const auto alias = new_split_routing_tun_alias();
        check(alias.starts_with(L"QuickDNS TUN ") && alias != new_split_routing_tun_alias(),
              "TUN aliases must be unique across sessions");
        PWSTR local_app_data = nullptr;
        check(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local_app_data)),
              "local AppData is unavailable");
        const auto bin = std::filesystem::path(local_app_data) / "OpenAI" / "Codex" / "bin";
        CoTaskMemFree(local_app_data);
        const auto config = [&](const std::wstring& family) {
            RoutingAdapter primary, secondary;
            primary.ipv4_dns = {L"10.6.1.11"};
            secondary.ipv4_dns = {L"143.20.64.55", L"10.6.1.11"};
            return nlohmann::json::parse(make_split_routing_config(primary, secondary, LR"(C:\WindowsApps\Test)", true, {}, family, alias));
        };
        const auto codex = config(L"OpenAI.Codex_2p2nqsd0c76g0");
        check(codex["inbounds"][0]["interface_name"] == QuickDns::Encoding::wide_to_utf8(alias),
              "TUN config does not use the session alias");
        check(codex["inbounds"][0]["dns_mode"] == "disabled", "TUN replaces adapter DNS");
        const auto& doh = codex["route"]["rules"][1];
        check(doh.contains("ip_cidr") && doh["ip_cidr"] == nlohmann::json::array({"143.20.64.55/32"}) &&
              doh["port"] == nlohmann::json::array({443}) && doh["outbound"] == "secondary",
              "Dedicated DNS endpoint is not routed over its own adapter");
        const auto& dns = codex["route"]["rules"][2];
        check(dns["ip_cidr"] == nlohmann::json::array({"143.20.64.55/32"}) &&
              dns["port"] == nlohmann::json::array({53}) &&
              dns["network"] == nlohmann::json::array({"tcp", "udp"}) &&
              dns["outbound"] == "secondary" && !dns.contains("process_name") &&
              !dns.contains("process_path_regex"),
              "Dedicated DNS requires process attribution or uses the wrong adapter");
        const auto& shared_dns = codex["route"]["rules"][3];
        check(shared_dns["ip_cidr"] == nlohmann::json::array({"10.6.1.11/32"}) &&
              shared_dns["port"] == nlohmann::json::array({53}) && shared_dns["outbound"] == "primary",
              "Shared DNS is not retained on the primary adapter");
        const auto& paths = codex["route"]["rules"][0]["process_path_regex"];
        check(paths.size() == 2, "Codex runtime is missing from the secondary route");
        const std::regex runtime(paths[1].get<std::string>().substr(4), std::regex::icase);
        check(std::regex_search(QuickDns::Encoding::wide_to_utf8((bin / "c38161ad8494f88c" / "codex.exe").native()), runtime),
              "versioned Codex runtime does not match");
        check(!std::regex_search(QuickDns::Encoding::wide_to_utf8((bin / "c38161ad8494f88c" / "other.exe").native()), runtime),
              "unrelated binary matches the Codex route");
        check(config(L"Other.Package_family")["route"]["rules"][0]["process_path_regex"].size() == 1,
              "Codex runtime was added to an unrelated package route");
        RoutingAdapter adapter;
        adapter.id = L"{01234567-89AB-CDEF-0123-456789ABCDEF}";
        adapter.name = L"Disconnected adapter";
        adapter.connected = false;
        AppRoutingDialog dialog(path, {adapter}, std::vector<InstalledPackage>{{L"OpenAI.Codex", L"OpenAI.Codex_2p2nqsd0c76g0"}});
        check(dialog.create(GetModuleHandleW(nullptr)), "dialog failed to initialize");
        const HWND window = dialog.window();
        check(window && GetDlgItem(window, RoutingStatus), "missing inactive status");
        const HWND adapters = GetDlgItem(window, RoutingAdapterCombo);
        check(SendMessageW(adapters, CB_GETCOUNT, 0, 0) == 1, "disconnected adapter missing");
        SendMessageW(adapters, CB_SETCURSEL, 0, 0);
        SetWindowTextW(GetDlgItem(window, RoutingIdentity), L"C:\\Apps\\client.exe");
        SendMessageW(window, WM_COMMAND, MAKEWPARAM(RoutingAdd, BN_CLICKED), 0);
        check(SendMessageW(GetDlgItem(window, RoutingList), LB_GETCOUNT, 0, 0) == 1, "draft not added");
        SendMessageW(window, WM_COMMAND, MAKEWPARAM(RoutingSave, BN_CLICKED), 0);
        AppRoutingConfigStore saved(path);
        check(saved.load(), "failed to reload saved draft");
        check(saved.rules().size() == 1 && !saved.rules()[0].enabled && saved.rules()[0].application_path == "C:\\Apps\\client.exe", "draft changed or marked active");
        SendMessageW(GetDlgItem(window, RoutingKind), CB_SETCURSEL, 1, 0);
        SendMessageW(window, WM_COMMAND, MAKEWPARAM(RoutingKind, CBN_SELCHANGE), 0);
        const HWND packages = GetDlgItem(window, RoutingPackages);
        check(packages && SendMessageW(packages, CB_GETCOUNT, 0, 0) == 1, "installed package missing");
        for (int i = 0; i < 100; ++i)
            SendMessageW(packages, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Extra package"));
        COMBOBOXINFO package_info{};
        package_info.cbSize = sizeof(package_info);
        check(GetComboBoxInfo(packages, &package_info), "MSIX dropdown list handle missing");
        check((GetWindowLongPtrW(package_info.hwndList, GWL_STYLE) & WS_VSCROLL) != 0,
              "MSIX dropdown has no vertical scrollbar for long lists");
        wchar_t package_label[256]{};
        SendMessageW(packages, CB_GETLBTEXT, 0, reinterpret_cast<LPARAM>(package_label));
        check(std::wstring_view(package_label).starts_with(L"ChatGPT.exe"), "ChatGPT executable name missing");
        SendMessageW(packages, CB_SETCURSEL, 0, 0);
        SendMessageW(window, WM_COMMAND, MAKEWPARAM(RoutingPackages, CBN_SELCHANGE), 0);
        SendMessageW(window, WM_COMMAND, MAKEWPARAM(RoutingAdd, BN_CLICKED), 0);
        check(SendMessageW(GetDlgItem(window, RoutingList), LB_GETCOUNT, 0, 0) == 2, "package draft not added");
        SendMessageW(GetDlgItem(window, RoutingList), LB_SETCURSEL, 0, 0);
        SendMessageW(window, WM_COMMAND, MAKEWPARAM(RoutingRemove, BN_CLICKED), 0);
        SendMessageW(window, WM_COMMAND, MAKEWPARAM(RoutingSave, BN_CLICKED), 0);
        check(saved.load() && saved.rules().size() == 1 && !saved.rules()[0].enabled &&
              saved.rules()[0].package_family == "OpenAI.Codex_2p2nqsd0c76g0", "package draft not persisted after delete");
        DestroyWindow(window);
        HWND scope_window = nullptr;
        {
            AppRoutingDialog temporary(path, {adapter});
            check(temporary.create(GetModuleHandleW(nullptr)), "second dialog failed to initialize");
            scope_window = temporary.window();
        }
        check(!IsWindow(scope_window), "dialog window outlived its owner");
        {
            AppRoutingDialog quit_dialog(path, {adapter});
            check(quit_dialog.create(GetModuleHandleW(nullptr)), "quit dialog failed to initialize");
            PostQuitMessage(42);
            quit_dialog.show_modal();
            MSG quit{};
            check(PeekMessageW(&quit, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE) && quit.wParam == 42,
                  "modal loop swallowed WM_QUIT");
        }
        wchar_t expected_family[256]{};
        if (GetEnvironmentVariableW(L"QUICKDNS_EXPECT_PACKAGE_FAMILY", expected_family, 256)) {
            bool found = false;
            for (const auto& package : enumerate_installed_packages())
                if (CompareStringOrdinal(package.family.c_str(), -1, expected_family, -1, TRUE) == CSTR_EQUAL)
                    found = std::filesystem::path(package.install_path).is_absolute();
            check(found, "installed package or install directory is missing");
        }
        std::filesystem::remove(path);
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; std::filesystem::remove(path); return 1; }
}
