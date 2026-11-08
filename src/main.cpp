#include <windows.h>
#include <objbase.h>
#include <propidl.h>
#include <gdiplus.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <string_view>
#include "ui_main.h"
#include "dns_manager.h"

#pragma comment(lib, "gdiplus.lib")

namespace {
    constexpr const wchar_t* SINGLE_INSTANCE_MUTEX_NAME = L"Local\\QuickDNS_SingleInstance_Mutex";
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
    UNREFERENCED_PARAMETER(lpCmdLine);

    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_APPLICATION_DIR);

    // Determine initial show state and flags from command line
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    int showCmd = nCmdShow;
    bool isMinimizedLaunch = false;
    bool noElevate = false;
    if (argv) {
        for (int i = 1; i < argc; ++i) {
            std::wstring_view arg(argv[i]);
            if (arg == L"--minimized" || arg == L"-m" || arg == L"/minimized") {
                showCmd = SW_HIDE;
                isMinimizedLaunch = true;
            } else if (arg == L"--no-elevate") {
                noElevate = true;
            }
        }
        LocalFree(argv);
    }

    // 1. Single Instance check:
    // If an instance of QuickDNS is already running, activate it and exit immediately.
    if (restore_existing_instance(isMinimizedLaunch)) {
        return 0;
    }

    // 2. Ensure elevated Administrator privileges:
    // Changing Windows network adapter IP/DNS settings requires elevation.
    if (!noElevate && !IsUserAnAdmin()) {
        MessageBoxW(nullptr,
                    L"Для изменения параметров DNS требуются права администратора.\nЗапустите приложение от имени администратора.",
                    L"QuickDNS — Ошибка доступа", MB_OK | MB_ICONERROR);
        return 1;
    }

    // 3. Create a named single-instance mutex
    HANDLE hSingleInstanceMutex = CreateMutexW(nullptr, TRUE, SINGLE_INSTANCE_MUTEX_NAME);
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

    // Initialize GDI+ for high-quality anti-aliased rendering
    ULONG_PTR gdiplusToken = 0;
    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    Gdiplus::Status gdiStatus = Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, nullptr);
    if (gdiStatus != Gdiplus::Ok) {
        MessageBoxW(nullptr, L"Не удалось инициализировать графическую подсистему GDI+.",
                    L"QuickDNS — Критическая ошибка", MB_OK | MB_ICONERROR);
        CoUninitialize();
        if (hSingleInstanceMutex != nullptr) {
            ReleaseMutex(hSingleInstanceMutex);
            CloseHandle(hSingleInstanceMutex);
        }
        return 1;
    }

    // Enable Common Controls v6 (Visual Styles, modern buttons, combos)
    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icex.dwICC = ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icex);

    // Run Application within isolated lifetime scope before subsystem shutdowns
    int exitCode = 0;
    {
        MainWindow app;
        if (app.initialize(hInstance, showCmd)) {
            exitCode = app.run_message_loop();
        } else {
            exitCode = 1;
        }
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
