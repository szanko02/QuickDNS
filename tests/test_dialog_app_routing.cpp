#include "dialog_app_routing.h"
#include "app_updater.h"
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
        const std::wstring_view command = count > 1 ? args[1] : L"";
        const bool split_dns = command == L"--write-live-dns-config";
        const bool multiple = command == L"--write-live-multi-config";
        const bool web = command == L"--write-live-web-config" && count == 8;
        if ((count == 7 || count == 8) && (command == L"--write-live-config" || split_dns || web || (multiple && count == 8))) {
            const std::wstring primary_name = args[2], secondary_name = args[3];
            const std::filesystem::path app = args[4], output = args[5], log = args[6];
            std::vector<SplitRoutingTarget> targets{{app, count == 8 && !multiple && !web ? args[7] : L""}};
            std::vector<std::string> domains;
            if (web) domains.push_back(QuickDns::Encoding::wide_to_utf8(args[7]));
            if (multiple) targets.push_back({args[7], {}});
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
                *primary, *secondary, targets, log, alias, split_dns, domains, web));
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
            const SplitRoutingTarget targets[] = {{LR"(C:\WindowsApps\Test)", family}};
            return nlohmann::json::parse(make_split_routing_config(primary, secondary, targets, {}, alias));
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
        RoutingAdapter corporate, external;
        corporate.name = L"Ethernet 2";
        corporate.ipv4_dns = {L"10.6.1.11"};
        external.name = L"Ethernet 6";
        external.ipv4_dns = {L"143.20.64.55"};
        const SplitRoutingTarget targets[] = {
            {LR"(C:\WindowsApps\ChatGPT)", L"OpenAI.ChatGPT_test"}, {LR"(C:\Auth Browser\browser.exe)", {}}
        };
        const auto split = nlohmann::json::parse(make_split_routing_config(corporate, external, targets, {}, alias, true));
        const auto& dns_policy = split["dns"];
        check(split["inbounds"][0]["dns_mode"] == "hijack" && split["inbounds"][0]["strict_route"] == true,
              "TUN is not handling Windows DNS for split DNS");
        check(dns_policy["final"] == "primary-dns" && dns_policy["rules"][0]["server"] == "secondary-dns" &&
              dns_policy["rules"][0]["domain_suffix"][0] == "chatgpt.com",
              "ChatGPT DNS does not use the second adapter or the default DNS is not corporate");
        const auto& servers = dns_policy["servers"];
        check(servers[0]["server"] == "10.6.1.11" && servers[0]["bind_interface"] == "Ethernet 2" &&
              servers[1]["server"] == "143.20.64.55" && servers[1]["bind_interface"] == "Ethernet 6",
              "Split DNS upstream binding is incorrect");
        check(split["route"]["rules"].size() == 3 && split["route"]["rules"][0]["outbound"] == "secondary" &&
              split["route"]["rules"][1]["outbound"] == "primary",
              "Split DNS changes application routing or keeps the legacy DNS bypass");
        const auto& rules = split["route"]["rules"][0]["process_path_regex"];
        const std::regex package_rule(rules[0].get<std::string>().substr(4), std::regex::icase);
        const std::regex browser_rule(rules[1].get<std::string>().substr(4), std::regex::icase);
        check(rules.size() == 2 && std::regex_search(R"(C:\WindowsApps\ChatGPT\ChatGPT.exe)", package_rule) &&
              std::regex_search(R"(C:\Auth Browser\browser.exe)", browser_rule) &&
              !std::regex_search(R"(C:\Work Browser\browser.exe)", browser_rule),
              "ChatGPT and its authentication browser must both match without including the work browser");
        check(config(L"Other.Package_family")["route"]["rules"][0]["process_path_regex"].size() == 1,
              "Codex runtime was added to an unrelated package route");
        RoutingAdapter adapter;
        adapter.id = L"{01234567-89AB-CDEF-0123-456789ABCDEF}";
        adapter.name = L"Disconnected adapter";
        adapter.connected = false;
        {
            RoutingAdapter primary, secondary;
            primary.name = L"LAN";
            primary.dns_suffix = L"company.example";
            primary.ipv4_dns = {L"10.0.0.53"};
            secondary.name = L"WAN";
            secondary.ipv4_dns = {L"192.0.2.53"};
            const std::array<std::string, 1> domains{"github.com"};
            const auto sites = nlohmann::json::parse(make_split_routing_config(
                primary, secondary, {}, {}, L"Test TUN", false, domains, true));
            const auto& site_rules = sites["route"]["rules"];
            check(site_rules[0]["action"] == "sniff" && site_rules[1]["domain_suffix"][0] == "github.com" &&
                  site_rules[1]["outbound"] == "secondary" && site_rules[2]["outbound"] == "primary" &&
                  site_rules[3]["ip_is_private"] == true && site_rules[4]["outbound"] == "secondary",
                  "domain and external web rules must preserve corporate routing");
            check(sites["dns"]["final"] == "secondary-dns" && sites["dns"]["rules"][2]["server"] == "primary-dns",
                  "public and corporate DNS must use their respective adapters");
            const auto listed_only = nlohmann::json::parse(make_split_routing_config(
                primary, secondary, {}, {}, L"Test TUN", false, domains));
            check(listed_only["dns"]["final"] == "primary-dns" &&
                  listed_only["dns"]["rules"][0]["server"] == "selected-domains" &&
                  listed_only["outbounds"][1]["domain_resolver"] == "secondary-dns",
                  "a listed domain must keep its identity and use secondary DNS without the public web option");
        }
        {
            SplitRoutingSession session;
            AppRoutingDialog split_dialog(path, {adapter}, std::vector<InstalledPackage>{}, nullptr, &session);
            check(split_dialog.create(GetModuleHandleW(nullptr)), "split routing dialog failed to initialize");
            check(!GetDlgItem(split_dialog.window(), SplitPublicWeb) && !GetDlgItem(split_dialog.window(), SplitChatGptDns),
                  "routing options managed automatically must not be shown");
            const auto top = [&](int id) {
                RECT bounds{};
                check(GetWindowRect(GetDlgItem(split_dialog.window(), id), &bounds), "missing routing control");
                return bounds.top;
            };
            check(top(SplitPrimary) == top(RoutingAdapterCombo) && top(SplitPrimary) < top(SplitEngine),
                  "network controls must appear above the engine");
            const HWND engine = GetDlgItem(split_dialog.window(), SplitEngine);
            const HWND browse = GetDlgItem(split_dialog.window(), SplitBrowse);
            const bool embedded = has_embedded_routing_engine();
            wchar_t test_executable[32768]{};
            const auto exe_length = GetModuleFileNameW(nullptr, test_executable, static_cast<DWORD>(std::size(test_executable)));
            const auto edition = embedded ? UpdateEdition::Embedded : UpdateEdition::Classic;
            const auto other_edition = embedded ? UpdateEdition::Classic : UpdateEdition::Embedded;
            check(exe_length > 0 && exe_length < std::size(test_executable) &&
                  AppUpdater::verify_executable_edition(test_executable, edition) &&
                  !AppUpdater::verify_executable_edition(test_executable, other_edition),
                  "update payload must match the current edition by its embedded engine resource");
            const auto engine_style = GetWindowLongPtrW(engine, GWL_STYLE);
            check(engine && browse && (engine_style & WS_VISIBLE) &&
                  (GetWindowLongPtrW(browse, GWL_STYLE) & WS_VISIBLE),
                  "the engine path and Browse button must be visible on the applications page");
            check((!IsWindowEnabled(browse)) == embedded &&
                  ((engine_style & ES_READONLY) != 0) == embedded,
                  "only the external engine build may edit or browse for sing-box.exe");
            wchar_t expected_engine[32768]{};
            const auto expected_length = GetEnvironmentVariableW(
                L"QUICKDNS_EXPECT_COLOCATED_ENGINE", expected_engine, static_cast<DWORD>(std::size(expected_engine)));
            if (!embedded && expected_length && expected_length < std::size(expected_engine)) {
                wchar_t engine_path[32768]{};
                GetWindowTextW(GetDlgItem(split_dialog.window(), SplitEngine),
                               engine_path, static_cast<int>(std::size(engine_path)));
                check(std::filesystem::path(engine_path) == std::filesystem::path(expected_engine),
                      "the EXE did not preselect the adjacent sing-box.exe");
            }
            check(!GetDlgItem(split_dialog.window(), RoutingList) && !GetDlgItem(split_dialog.window(), RoutingAdd),
                  "the two-network dialog must omit application rules");
            SendMessageW(GetDlgItem(split_dialog.window(), SplitPrimary), CB_SETCURSEL, 0, 0);
            SendMessageW(GetDlgItem(split_dialog.window(), RoutingAdapterCombo), CB_SETCURSEL, 0, 0);
            check(!GetDlgItem(split_dialog.window(), SplitDomains), "domain input must be hidden");
            SendMessageW(split_dialog.window(), WM_COMMAND, MAKEWPARAM(RoutingSave, BN_CLICKED), 0);
            AppRoutingConfigStore saved_settings(path);
            const auto id = QuickDns::Encoding::wide_to_utf8(adapter.id);
            check(saved_settings.load() && saved_settings.settings.primary_id == id &&
                  saved_settings.settings.secondary_id == id,
                  "saving the routing configuration must persist both networks");
        }
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
