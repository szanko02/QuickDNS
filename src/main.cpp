#include "ui_main.h"
#include "dns_manager.h"
#include "app_updater.h"

#include <windows.h>
#include <objbase.h>
#include <propidl.h>
#include <algorithm>
#include <gdiplus.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <string_view>
#include <cstdint>

#ifdef _MSC_VER
#pragma comment(lib, "gdiplus.lib")
#endif

namespace {
    constexpr const wchar_t* SINGLE_INSTANCE_MUTEX_NAME = L"Local\\QuickDNS_SingleInstance_Mutex";
    constexpr const wchar_t* RESTORE_MESSAGE_NAME       = L"QuickDNS_RestoreExistingInstanceMessage";
    constexpr const wchar_t* MAIN_WINDOW_CLASS_NAME     = L"QuickDNS_MainWindowClass";

    bool is_single_instance_mutex_present() {
        HANDLE hTestMutex = OpenMutexW(SYNCHRONIZE, FALSE, SINGLE_INSTANCE_MUTEX_NAME);
        if (hTestMutex != nullptr) {
            CloseHandle(hTestMutex);
            return true;
        }
        return (GetLastError() == ERROR_ACCESS_DENIED);
    }

    bool parse_pid_argument(std::wstring_view str, DWORD& outPid) {
        if (str.empty()) {
            return false;
        }
        uint64_t val = 0;
        for (wchar_t ch : str) {
            if (ch < L'0' || ch > L'9') {
                return false;
            }
            val = val * 10 + static_cast<uint64_t>(ch - L'0');
            if (val > static_cast<uint64_t>(MAXDWORD)) {
                return false;
            }
        }
        if (val == 0) {
            return false;
        }
        outPid = static_cast<DWORD>(val);
        return true;
    }

    bool restore_existing_instance(bool isMinimizedLaunch) {
        HWND hExistingWnd = FindWindowW(MAIN_WINDOW_CLASS_NAME, nullptr);
        if (!hExistingWnd && is_single_instance_mutex_present()) {
            // Mutex is active: an instance is starting up, wait briefly for window creation
            for (int i = 0; i < 20; ++i) {
                Sleep(50);
                hExistingWnd = FindWindowW(MAIN_WINDOW_CLASS_NAME, nullptr);
                if (hExistingWnd) break;
            }
        }

        if (hExistingWnd) {
            if (!isMinimizedLaunch) {
                DWORD existingPid = 0;
                GetWindowThreadProcessId(hExistingWnd, &existingPid);
                if (existingPid != 0) {
                    AllowSetForegroundWindow(existingPid);
                }

                UINT msgRestore = RegisterWindowMessageW(RESTORE_MESSAGE_NAME);
                if (msgRestore != 0) {
                    PostMessageW(hExistingWnd, msgRestore, 0, 0);
                }

                if (IsIconic(hExistingWnd)) {
                    ShowWindow(hExistingWnd, SW_RESTORE);
                }

                HWND hPopup = GetLastActivePopup(hExistingWnd);
                if (hPopup && hPopup != hExistingWnd && IsWindow(hPopup)) {
                    BringWindowToTop(hPopup);
                    SetForegroundWindow(hPopup);
                } else if (IsWindowVisible(hExistingWnd)) {
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

    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);

    // Determine initial show state and flags from command line
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    int showCmd = nCmdShow;
    bool isMinimizedLaunch = false;
    bool noElevate = false;
    bool isUpdatedLaunch = false;
    DWORD oldPid = 0;

    if (argv) {
        for (int i = 1; i < argc; ++i) {
            std::wstring_view arg(argv[i]);
            if (arg == L"--minimized" || arg == L"-m" || arg == L"/minimized") {
                showCmd = SW_HIDE;
                isMinimizedLaunch = true;
            } else if (arg == L"--no-elevate") {
                noElevate = true;
            } else if (arg == L"--updated" || arg == L"--restart") {
                isUpdatedLaunch = true;
            } else if (arg == L"--pid" && (i + 1) < argc) {
                DWORD parsedPid = 0;
                if (parse_pid_argument(argv[i + 1], parsedPid)) {
                    oldPid = parsedPid;
                    ++i;
                }
            } else if (arg.rfind(L"--pid=", 0) == 0) {
                DWORD parsedPid = 0;
                if (parse_pid_argument(arg.substr(6), parsedPid)) {
                    oldPid = parsedPid;
                }
            }
        }
        LocalFree(argv);
    }

    // 0. Update hand-off handling:
    // If launched after an in-place auto-update, wait for the previous process instance
    // (kernel process object + main window + single-instance mutex) to exit before cleanup.
    if (isUpdatedLaunch) {
        constexpr DWORD kMaxHandoffWaitMs = 15000;
        const ULONGLONG waitStart = GetTickCount64();

        if (oldPid != 0 && oldPid != GetCurrentProcessId()) {
            HANDLE hOldProc = OpenProcess(SYNCHRONIZE, FALSE, oldPid);
            if (hOldProc != nullptr) {
                WaitForSingleObject(hOldProc, kMaxHandoffWaitMs);
                CloseHandle(hOldProc);
            }
        }

        for (;;) {
            HWND hOldWnd = FindWindowW(MAIN_WINDOW_CLASS_NAME, nullptr);
            bool mutexPresent = is_single_instance_mutex_present();
            if (!hOldWnd && !mutexPresent) {
                break;
            }
            if (GetTickCount64() - waitStart >= kMaxHandoffWaitMs) {
                break;
            }
            Sleep(100);
        }

        // Once previous instance is completely gone, delete the .old binary
        quickdns::AppUpdater::cleanup_previous_update();
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

    // 3. Acquire named single-instance mutex ownership (handling abandoned mutex safely)
    HANDLE hSingleInstanceMutex = CreateMutexW(nullptr, FALSE, SINGLE_INSTANCE_MUTEX_NAME);
    if (hSingleInstanceMutex == nullptr) {
        restore_existing_instance(isMinimizedLaunch);
        return 0;
    }

    DWORD mutexWait = WaitForSingleObject(hSingleInstanceMutex, 0);
    if (mutexWait != WAIT_OBJECT_0 && mutexWait != WAIT_ABANDONED) {
        CloseHandle(hSingleInstanceMutex);
        restore_existing_instance(isMinimizedLaunch);
        return 0;
    }

    // Initialize COM library for shell / network APIs
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    bool comInitialized = SUCCEEDED(hr);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        MessageBoxW(nullptr, L"Не удалось инициализировать системную подсистему COM.",
                    L"QuickDNS — Критическая ошибка", MB_OK | MB_ICONERROR);
        ReleaseMutex(hSingleInstanceMutex);
        CloseHandle(hSingleInstanceMutex);
        return 1;
    }

    // Initialize GDI+ for high-quality anti-aliased rendering
    ULONG_PTR gdiplusToken = 0;
    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    Gdiplus::Status gdiStatus = Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, nullptr);
    if (gdiStatus != Gdiplus::Ok) {
        MessageBoxW(nullptr, L"Не удалось инициализировать графическую подсистему GDI+.",
                    L"QuickDNS — Критическая ошибка", MB_OK | MB_ICONERROR);
        if (comInitialized) {
            CoUninitialize();
        }
        ReleaseMutex(hSingleInstanceMutex);
        CloseHandle(hSingleInstanceMutex);
        return 1;
    }

    // Enable Common Controls v6 (Visual Styles, modern buttons, combos)
    INITCOMMONCONTROLSEX icex = {};
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

    if (comInitialized) {
        CoUninitialize();
    }

    ReleaseMutex(hSingleInstanceMutex);
    CloseHandle(hSingleInstanceMutex);

    return exitCode;
}
