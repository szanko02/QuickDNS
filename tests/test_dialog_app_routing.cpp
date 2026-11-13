#include "dialog_app_routing.h"
#include <iostream>
#include <stdexcept>
using namespace quickdns;
static void check(bool ok, const char* msg) { if (!ok) throw std::runtime_error(msg); }
int main() {
    const auto path = std::filesystem::temp_directory_path() / (L"quickdns_dialog_" + std::to_wstring(GetCurrentProcessId()) + L".json");
    std::filesystem::remove(path);
    try {
        RoutingAdapter adapter;
        adapter.id = L"{01234567-89AB-CDEF-0123-456789ABCDEF}";
        adapter.name = L"Disconnected adapter";
        adapter.connected = false;
        AppRoutingDialog dialog(path, {adapter}, std::vector<InstalledPackage>{{L"ChatGPT", L"OpenAI.Codex_2p2nqsd0c76g0"}});
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
                if (CompareStringOrdinal(package.family.c_str(), -1, expected_family, -1, TRUE) == CSTR_EQUAL) found = true;
            check(found, "installed package enumeration missed expected family");
        }
        std::filesystem::remove(path);
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; std::filesystem::remove(path); return 1; }
}
