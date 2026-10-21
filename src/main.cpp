#include <windows.h>
#include <objbase.h>
#include <propidl.h>
#include <gdiplus.h>
#include <commctrl.h>
#include <shellapi.h>
#include "ui_main.h"
#include "dns_manager.h"

#pragma comment(lib, "gdiplus.lib")

namespace {
    constexpr const wchar_t* SINGLE_INSTANCE_MUTEX_NAME = L"Local\\QuickDNS_SingleInstance_Mutex_szanko02";
    constexpr const wchar_t* RESTORE_MESSAGE_NAME        = L"QuickDNS_RestoreExistingInstanceMessage";
    constexpr const wchar_t* MAIN_WINDOW_CLASS_NAME     = L"QuickDNS_MainWindowClass";

    bool restore_existing_instance(bool isMinimizedLaunch) {
        HWND hExistingWnd = FindWindowW(MAIN_WINDOW_CLASS_NAME, nullptr);
        if (!hExistingWnd) {
            HANDLE hTestMutex = OpenMutexW(SYNCHRONIZE, FALSE, SINGLE_INSTANCE_MUTEX_NAME);
            if (hTestMutex != nullptr) {
                CloseHandle(hTestMutex);
                // Mutex is active: an instance is starting up, wait briefly for window to be created
                for (int i = 0; i < 20; ++i) {
                    Sleep(50);
                    hExistingWnd = FindWindowW(MAIN_WINDOW_CLASS_NAME, nullptr);
                    if (hExistingWnd) break;
                }
            }
        }

        if (hExistingWnd) {
            if (!isMinimizedLaunch) {
                AllowSetForegroundWindow(ASFW_ANY);
                UINT msgRestore = RegisterWindowMessageW(RESTORE_MESSAGE_NAME);
                if (msgRestore != 0) {
                    PostMessageW(hExistingWnd, msgRestore, 0, 0);
                }
                ShowWindow(hExistingWnd, SW_RESTORE);
                HWND hPopup = GetLastActivePopup(hExistingWnd);
                if (hPopup && hPopup != hExistingWnd && IsWindow(hPopup)) {
                    BringWindowToTop(hPopup);
                    SetForegroundWindow(hPopup);
                } else {
                    BringWindowToTop(hExistingWnd);
                    SetForegroundWindow(hExistingWnd);
                }
            }
            return true;
        }

        return false;
    }
}

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
    bool isMinimizedLaunch = false;
    if (fullCmdLine != nullptr && wcsstr(fullCmdLine, L"--minimized") != nullptr) {
        showCmd = SW_HIDE;
        isMinimizedLaunch = true;
    }

    bool noElevate = (fullCmdLine != nullptr && wcsstr(fullCmdLine, L"--no-elevate") != nullptr);

    // 1. Single Instance check:
    // If an instance of QuickDNS is already running, activate it and exit immediately.
    // Doing this before UAC elevation avoids prompting the user when the app is already in the tray.
    if (restore_existing_instance(isMinimizedLaunch)) {
        return 0;
    }

    // 2. Ensure elevated Administrator privileges.
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

    // 3. Create a named single-instance mutex with a permissive DACL
    // so any subsequent non-elevated launch can detect it immediately.
    SECURITY_DESCRIPTOR sd;
    InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
    SetSecurityDescriptorDacl(&sd, TRUE, nullptr, FALSE);
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = &sd;
    sa.bInheritHandle = FALSE;

    HANDLE hSingleInstanceMutex = CreateMutexW(&sa, TRUE, SINGLE_INSTANCE_MUTEX_NAME);
    if (hSingleInstanceMutex == nullptr || GetLastError() == ERROR_ALREADY_EXISTS) {
        restore_existing_instance(isMinimizedLaunch);
        if (hSingleInstanceMutex != nullptr) {
            CloseHandle(hSingleInstanceMutex);
        }
        return 0;
    }

    // Initialize COM library for shell / network APIs
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hr)) {
        if (hSingleInstanceMutex != nullptr) {
            ReleaseMutex(hSingleInstanceMutex);
            CloseHandle(hSingleInstanceMutex);
        }
        return 1;
    }

    // Initialize GDI+ for high-quality anti-aliased rendering (smooth buttons, curves and cards)
    ULONG_PTR gdiplusToken = 0;
    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, nullptr);

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

    if (gdiplusToken) {
        Gdiplus::GdiplusShutdown(gdiplusToken);
    }

    CoUninitialize();

    if (hSingleInstanceMutex != nullptr) {
        ReleaseMutex(hSingleInstanceMutex);
        CloseHandle(hSingleInstanceMutex);
    }

    return exitCode;
}
