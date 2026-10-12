#include <windows.h>
#include <commctrl.h>
#include <objbase.h>
#include <shellapi.h>
#include "ui_main.h"
#include "dns_manager.h"

int WINAPI wWinMain(
    _In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPWSTR lpCmdLine,
    _In_ int nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);

    // Determine initial show state
    LPWSTR fullCmdLine = GetCommandLineW();
    int showCmd = nCmdShow;
    if (fullCmdLine != nullptr && wcsstr(fullCmdLine, L"--minimized") != nullptr) {
        showCmd = SW_HIDE;
    }

    bool noElevate = (fullCmdLine != nullptr && wcsstr(fullCmdLine, L"--no-elevate") != nullptr);

    // Ensure the application runs with elevated Administrator privileges.
    // Changing Windows network adapter IP/DNS settings requires elevation.
    if (!noElevate && !quickdns::DnsManager::is_elevated()) {
        wchar_t szExePath[MAX_PATH];
        if (GetModuleFileNameW(nullptr, szExePath, MAX_PATH)) {
            SHELLEXECUTEINFOW sei = {};
            sei.cbSize = sizeof(sei);
            sei.lpVerb = L"runas";
            sei.lpFile = szExePath;
            sei.lpParameters = lpCmdLine;
            sei.hwnd = nullptr;
            sei.nShow = showCmd;
            if (ShellExecuteExW(&sei)) {
                return 0; // Elevated instance launched successfully; exit this process.
            }
        }
    }

    // Initialize COM library for shell / network APIs
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hr)) {
        return 1;
    }

    // Enable Common Controls v6 (Visual Styles, modern buttons, combos)
    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icex.dwICC = ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icex);

    // Run Application
    MainWindow app;
    int exitCode = 0;
    if (app.initialize(hInstance, showCmd)) {
        exitCode = app.run_message_loop();
    }

    CoUninitialize();
    return exitCode;
}
