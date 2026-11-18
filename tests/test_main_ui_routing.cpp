#include "ui_main.h"
#include "dialog_app_routing.h"
#include <objbase.h>
#include <propidl.h>
#include <gdiplus.h>
#include <windows.h>
#include <commctrl.h>
#include <filesystem>
#include <iostream>

namespace {
constexpr int draft_control_id = 1030;
HWND find_test_window(const wchar_t* name, HWND owner = nullptr) {
    struct Query { const wchar_t* name; HWND owner; HWND window; } query{name, owner, nullptr};
    EnumThreadWindows(GetCurrentThreadId(), [](HWND window, LPARAM value) -> BOOL {
        auto& item = *reinterpret_cast<Query*>(value);
        wchar_t cls[100]{};
        GetClassNameW(window, cls, 100);
        if (wcscmp(cls, item.name) == 0 && (!item.owner || GetWindow(window, GW_OWNER) == item.owner)) {
            item.window = window;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&query));
    return query.window;
}
bool dialog_seen = false;
VOID CALLBACK close_dialog(HWND, UINT, UINT_PTR timer, DWORD) {
    HWND dialog = find_test_window(L"QuickDNS_AppRoutingDraftDialog");
    if (!dialog) return;
    const HWND primary = GetDlgItem(dialog, quickdns::SplitPrimary);
    const HWND secondary = GetDlgItem(dialog, quickdns::RoutingAdapterCombo);
    const HWND split_help = GetDlgItem(dialog, quickdns::SplitHelp);
    const HWND tooltip = find_test_window(TOOLTIPS_CLASSW, dialog);
    TOOLINFOW info{TTTOOLINFOW_V2_SIZE};
    info.uFlags = TTF_IDISHWND;
    wchar_t tooltip_text[512]{};
    info.lpszText = tooltip_text;
    info.hwnd = dialog;
    info.uId = reinterpret_cast<UINT_PTR>(split_help);
    dialog_seen = IsWindowVisible(primary) && IsWindowVisible(secondary) && IsWindowVisible(split_help) &&
        tooltip && SendMessageW(tooltip, TTM_GETTOOLINFOW, 0, reinterpret_cast<LPARAM>(&info)) &&
        (info.uFlags & TTF_SUBCLASS) != 0 && wcsstr(tooltip_text, L"Сеть 1") && wcsstr(tooltip_text, L"Сеть 2") &&
        !GetDlgItem(dialog, quickdns::SplitNotice) &&
        !GetDlgItem(dialog, quickdns::RoutingList) && !GetDlgItem(dialog, quickdns::RoutingSubnetsPage) &&
        !GetDlgItem(dialog, quickdns::SubnetDestination) &&
        GetDlgItem(dialog, quickdns::SplitStart) && !IsWindowEnabled(GetDlgItem(dialog, quickdns::SplitStop));
    DestroyWindow(dialog);
    KillTimer(nullptr, timer);
}
}

int main() {
    const auto temporary = std::filesystem::temp_directory_path() / (L"quickdns_main_ui_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64()));
    if (!std::filesystem::create_directory(temporary)) return 10;
    SetEnvironmentVariableW(L"APPDATA", temporary.c_str());
    SetEnvironmentVariableW(L"TESTING", L"1");
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Gdiplus::GdiplusStartupInput input;
    ULONG_PTR token{};
    Gdiplus::GdiplusStartup(&token, &input, nullptr);
    int result = 0;
    {
        MainWindow app(temporary / L"profiles.json");
        if (!app.initialize(GetModuleHandleW(nullptr), SW_HIDE)) result = 1;
        HWND main = find_test_window(L"QuickDNS_MainWindowClass");
        HWND button = main ? GetDlgItem(main, draft_control_id) : nullptr;
        if (!button) { std::cerr << "draft button missing\n"; result = 2; }
        else {
            wchar_t label[256]{};
            GetWindowTextW(button, label, 256);
            if (wcscmp(label, L"Маршрутизация") != 0) { std::cerr << "wrong routing button label\n"; result = 3; }
            UINT_PTR timer = SetTimer(nullptr, 0, 50, close_dialog);
            SendMessageW(main, WM_COMMAND, MAKEWPARAM(draft_control_id, BN_CLICKED), reinterpret_cast<LPARAM>(button));
            KillTimer(nullptr, timer);
            if (!dialog_seen) { std::cerr << "routing window controls or help incorrect\n"; result = 4; }
        }
        if (main) DestroyWindow(main);
    }
    Gdiplus::GdiplusShutdown(token);
    CoUninitialize();
    std::error_code ignored;
    std::filesystem::remove(temporary / L"profiles.json", ignored);
    std::filesystem::remove(temporary, ignored);
    return result;
}
