#include <windows.h>
#include <commctrl.h>
#include <objbase.h>
#include "ui_main.h"

int WINAPI wWinMain(
    _In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPWSTR lpCmdLine,
    _In_ int nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);

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

    // Determine initial show state
    int showCmd = nCmdShow;
    if (lpCmdLine != nullptr && wcsstr(lpCmdLine, L"--minimized") != nullptr) {
        showCmd = SW_HIDE;
    }

    // Run Application
    MainWindow app;
    int exitCode = 0;
    if (app.initialize(hInstance, showCmd)) {
        exitCode = app.run_message_loop();
    }

    CoUninitialize();
    return exitCode;
}
