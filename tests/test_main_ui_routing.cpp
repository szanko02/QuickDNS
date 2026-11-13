#include "ui_main.h"
#include <objbase.h>
#include <propidl.h>
#include <gdiplus.h>
#include <windows.h>
#include <filesystem>
#include <iostream>

namespace {
constexpr int draft_control_id = 1030;
HWND find_test_window(const wchar_t* name) {
    struct Query { const wchar_t* name; HWND window; } query{name, nullptr};
    EnumThreadWindows(GetCurrentThreadId(), [](HWND window, LPARAM value) -> BOOL {
        auto& item = *reinterpret_cast<Query*>(value);
        wchar_t cls[100]{};
        GetClassNameW(window, cls, 100);
        if (wcscmp(cls, item.name) == 0) { item.window = window; return FALSE; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&query));
    return query.window;
}
bool dialog_seen = false;
VOID CALLBACK close_dialog(HWND, UINT, UINT_PTR timer, DWORD) {
    HWND dialog = find_test_window(L"QuickDNS_AppRoutingDraftDialog");
    if (!dialog) return;
    wchar_t warning[256]{};
    GetWindowTextW(GetDlgItem(dialog, 8209), warning, 256);
    dialog_seen = wcsstr(warning, L"не активны") && wcsstr(warning, L"не применяются");
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
            if (!wcsstr(label, L"черновик")) { std::cerr << "draft button is misleading\n"; result = 3; }
            UINT_PTR timer = SetTimer(nullptr, 0, 50, close_dialog);
            SendMessageW(main, WM_COMMAND, MAKEWPARAM(draft_control_id, BN_CLICKED), reinterpret_cast<LPARAM>(button));
            KillTimer(nullptr, timer);
            if (!dialog_seen) { std::cerr << "inactive draft dialog was not opened\n"; result = 4; }
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
