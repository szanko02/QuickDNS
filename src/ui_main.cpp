#include "ui_main.h"
#include "dialog_add_profile.h"
#include "dialog_benchmark.h"
#include "encoding_utils.h"
#include <dwmapi.h>
#include <uxtheme.h>
#include <windowsx.h>
#include <sstream>
#include <algorithm>
#include <chrono>
#include <objbase.h>
#include <propidl.h>
#include <gdiplus.h>
#include <initguid.h>
#include <uiautomation.h>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "oleaut32.lib")

using QuickDns::Encoding::utf8_to_wide;
using QuickDns::Encoding::wide_to_utf8;
using namespace quickdns;

namespace {
    static void AddRoundedRect(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& rect, float radius) {
        float diameter = radius * 2.0f;
        path.AddArc(rect.X, rect.Y, diameter, diameter, 180.0f, 90.0f);
        path.AddArc(rect.GetRight() - diameter, rect.Y, diameter, diameter, 270.0f, 90.0f);
        path.AddArc(rect.GetRight() - diameter, rect.GetBottom() - diameter, diameter, diameter, 0.0f, 90.0f);
        path.AddArc(rect.X, rect.GetBottom() - diameter, diameter, diameter, 90.0f, 90.0f);
        path.CloseFigure();
    }

    constexpr const wchar_t* WINDOW_CLASS_NAME = L"QuickDNS_MainWindowClass";
    constexpr const wchar_t* GRAPH_CLASS_NAME  = L"QuickDNS_SparklineGraph";
    constexpr const wchar_t* WINDOW_TITLE      = L"QuickDNS v1.2.1 — Быстрая смена DNS в 1 клик";

    constexpr int WINDOW_WIDTH  = 540;
    constexpr int WINDOW_HEIGHT = 720;

    struct ColorPalette {
        COLORREF bg;
        COLORREF cardBg;
        COLORREF cardBorder;
        COLORREF textPrimary;
        COLORREF textSecondary;
        COLORREF textMuted;
        COLORREF accentBlue;
        COLORREF accentGreen;
        COLORREF accentAmber;
        COLORREF accentRed;
        COLORREF separator;
        COLORREF btnPrimaryBg;
        COLORREF btnPrimaryHover;
        COLORREF btnPrimaryPress;
        COLORREF btnPrimaryBorder;
        COLORREF btnPrimaryText;
        COLORREF btnBenchBg;
        COLORREF btnBenchHover;
        COLORREF btnBenchPress;
        COLORREF btnBenchBorder;
        COLORREF btnBenchText;
        COLORREF btnUtilBg;
        COLORREF btnUtilHover;
        COLORREF btnUtilPress;
        COLORREF btnUtilBorder;
        COLORREF btnUtilText;
        COLORREF graphBg;
        COLORREF graphBorder;
        COLORREF graphGrid;
        COLORREF graphLine;
        COLORREF graphText;
    };

    static ColorPalette GetPalette(bool isDark) {
        if (isDark) {
            return ColorPalette{
                RGB(22, 25, 31),     // bg (refined dark charcoal)
                RGB(30, 34, 43),     // cardBg
                RGB(48, 55, 68),     // cardBorder
                RGB(241, 245, 249),  // textPrimary
                RGB(156, 163, 175),  // textSecondary
                RGB(110, 118, 132),  // textMuted
                RGB(92, 130, 165),   // accentBlue (muted steel)
                RGB(72, 150, 112),   // accentGreen (calm sage)
                RGB(185, 142, 60),   // accentAmber (muted warm amber)
                RGB(192, 82, 82),    // accentRed (muted brick)
                RGB(48, 55, 68),     // separator
                RGB(44, 76, 112),    // btnPrimaryBg (calm slate navy)
                RGB(54, 92, 134),    // btnPrimaryHover
                RGB(36, 62, 92),     // btnPrimaryPress
                RGB(62, 102, 146),   // btnPrimaryBorder
                RGB(255, 255, 255),  // btnPrimaryText
                RGB(50, 64, 94),     // btnBenchBg (subdued steel indigo)
                RGB(62, 79, 115),    // btnBenchHover
                RGB(40, 52, 78),     // btnBenchPress
                RGB(72, 92, 132),    // btnBenchBorder
                RGB(255, 255, 255),  // btnBenchText
                RGB(34, 38, 48),     // btnUtilBg (neutral dark slate)
                RGB(46, 52, 65),     // btnUtilHover
                RGB(28, 31, 40),     // btnUtilPress
                RGB(58, 66, 82),     // btnUtilBorder
                RGB(226, 232, 240),  // btnUtilText
                RGB(25, 29, 37),     // graphBg
                RGB(48, 55, 68),     // graphBorder
                RGB(38, 44, 56),     // graphGrid
                RGB(88, 132, 174),   // graphLine (neutral steel blue)
                RGB(140, 148, 162)   // graphText
            };
        } else {
            return ColorPalette{
                RGB(246, 248, 251),  // bg (soft clean neutral)
                RGB(255, 255, 255),  // cardBg
                RGB(218, 224, 233),  // cardBorder
                RGB(30, 41, 59),     // textPrimary
                RGB(85, 95, 110),    // textSecondary
                RGB(120, 130, 145),  // textMuted
                RGB(52, 98, 150),    // accentBlue
                RGB(46, 128, 78),    // accentGreen
                RGB(165, 108, 24),   // accentAmber
                RGB(180, 52, 52),    // accentRed
                RGB(222, 226, 233),  // separator
                RGB(48, 88, 132),    // btnPrimaryBg (neutral slate navy)
                RGB(40, 74, 112),    // btnPrimaryHover
                RGB(32, 60, 92),     // btnPrimaryPress
                RGB(40, 74, 112),    // btnPrimaryBorder
                RGB(255, 255, 255),  // btnPrimaryText
                RGB(62, 80, 114),    // btnBenchBg (neutral steel)
                RGB(52, 67, 96),     // btnBenchHover
                RGB(42, 54, 78),     // btnBenchPress
                RGB(52, 67, 96),     // btnBenchBorder
                RGB(255, 255, 255),  // btnBenchText
                RGB(243, 245, 249),  // btnUtilBg (light neutral)
                RGB(232, 236, 242),  // btnUtilHover
                RGB(220, 225, 233),  // btnUtilPress
                RGB(206, 213, 224),  // btnUtilBorder
                RGB(44, 52, 65),     // btnUtilText
                RGB(250, 251, 253),  // graphBg
                RGB(218, 224, 233),  // graphBorder
                RGB(235, 239, 245),  // graphGrid
                RGB(62, 108, 158),   // graphLine
                RGB(110, 120, 135)   // graphText
            };
        }
    }

    static COLORREF LerpColor(COLORREF c1, COLORREF c2, float t) {
        if (t <= 0.0f) return c1;
        if (t >= 1.0f) return c2;
        int r = static_cast<int>(GetRValue(c1) + (GetRValue(c2) - GetRValue(c1)) * t);
        int g = static_cast<int>(GetGValue(c1) + (GetGValue(c2) - GetGValue(c1)) * t);
        int b = static_cast<int>(GetBValue(c1) + (GetBValue(c2) - GetBValue(c1)) * t);
        return RGB(std::clamp(r, 0, 255), std::clamp(g, 0, 255), std::clamp(b, 0, 255));
    }

    struct ButtonAnimationState {
        float hover = 0.0f;
        float targetHover = 0.0f;
        bool isTracking = false;
        bool isPressed = false;
    };

    static LRESULT CALLBACK AnimatedButtonSubclassProc(
        HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
        UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
    {
        auto* anim = reinterpret_cast<ButtonAnimationState*>(dwRefData);
        if (!anim) return DefSubclassProc(hWnd, uMsg, wParam, lParam);

        switch (uMsg) {
        case WM_SETCURSOR: {
            if (LOWORD(lParam) == HTCLIENT && IsWindowEnabled(hWnd)) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            break;
        }
        case WM_MOUSEMOVE: {
            if (!anim->isTracking) {
                anim->isTracking = true;
                anim->targetHover = 1.0f;
                TRACKMOUSEEVENT tme = { sizeof(TRACKMOUSEEVENT), TME_LEAVE, hWnd, 0 };
                TrackMouseEvent(&tme);
                SetTimer(hWnd, 2001, 16, nullptr);
            }
            break;
        }
        case WM_MOUSELEAVE: {
            anim->isTracking = false;
            anim->targetHover = 0.0f;
            SetTimer(hWnd, 2001, 16, nullptr);
            break;
        }
        case WM_LBUTTONDOWN: {
            anim->isPressed = true;
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        }
        case WM_LBUTTONUP: {
            anim->isPressed = false;
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        }
        case WM_TIMER: {
            if (wParam == 2001) {
                float diff = anim->targetHover - anim->hover;
                if (std::abs(diff) < 0.02f) {
                    anim->hover = anim->targetHover;
                    KillTimer(hWnd, 2001);
                } else {
                    anim->hover += diff * 0.28f;
                }
                InvalidateRect(hWnd, nullptr, FALSE);
                return 0;
            }
            break;
        }
        case WM_NCDESTROY: {
            KillTimer(hWnd, 2001);
            RemovePropW(hWnd, L"QuickDnsAnim");
            delete anim;
            RemoveWindowSubclass(hWnd, AnimatedButtonSubclassProc, uIdSubclass);
            return 0;
        }
        }

        return DefSubclassProc(hWnd, uMsg, wParam, lParam);
    }
}

MainWindow::MainWindow()
    : m_dnsManager(std::make_unique<DnsManager>())
    , m_configManager(std::make_unique<ConfigManager>())
{
    m_isDarkMode = (m_configManager->get_theme_mode() != "light");
    auto pal = GetPalette(m_isDarkMode);
    m_hbrBg = CreateSolidBrush(pal.bg);
    m_hbrCard = CreateSolidBrush(pal.cardBg);
}

MainWindow::~MainWindow() {
    stop_ping_worker();
    remove_tray_icon();

    if (m_hFontRegular) DeleteObject(m_hFontRegular);
    if (m_hFontBold) DeleteObject(m_hFontBold);
    if (m_hFontHeader) DeleteObject(m_hFontHeader);
    if (m_hFontMonospace) DeleteObject(m_hFontMonospace);
    if (m_hFontSmall) DeleteObject(m_hFontSmall);
    if (m_hFontGraphVal) DeleteObject(m_hFontGraphVal);

    if (m_hbrBg) DeleteObject(m_hbrBg);
    if (m_hbrCard) DeleteObject(m_hbrCard);
    if (m_hToolTip) DestroyWindow(m_hToolTip);
}

bool MainWindow::initialize(HINSTANCE hInstance, int nCmdShow) {
    m_hInstance = hInstance;
    register_window_class();

    // Center window on screen
    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    int posX = (screenWidth - WINDOW_WIDTH) / 2;
    int posY = (screenHeight - WINDOW_HEIGHT) / 2;

    m_hWnd = CreateWindowExW(
        0,
        WINDOW_CLASS_NAME,
        WINDOW_TITLE,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        posX, posY,
        WINDOW_WIDTH, WINDOW_HEIGHT,
        nullptr,
        nullptr,
        m_hInstance,
        this
    );

    if (!m_hWnd) {
        return false;
    }

    apply_modern_theme();
    setup_fonts();
    create_controls();
    setup_tray_icon();

    // Load initial network & profile data
    refresh_network_adapters();
    populate_profiles_combobox();
    update_status_card();

    // Start background ping worker (optimal interval: 3.0s = 60s history window)
    start_ping_worker();

    if (nCmdShow != SW_HIDE) {
        ShowWindow(m_hWnd, nCmdShow);
        UpdateWindow(m_hWnd);
    } else {
        m_inTray = true;
    }

    return true;
}

int MainWindow::run_message_loop() {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

void MainWindow::register_window_class() {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &MainWindow::window_proc_thunk;
    wc.hInstance = m_hInstance;
    wc.hIcon = LoadIconW(m_hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
    if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = m_hbrBg;
    wc.lpszClassName = WINDOW_CLASS_NAME;
    wc.hIconSm = LoadIconW(m_hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
    if (!wc.hIconSm) wc.hIconSm = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);

    // Register Custom Sparkline Graph Window Class
    WNDCLASSEXW gwc = {};
    gwc.cbSize = sizeof(WNDCLASSEXW);
    gwc.style = CS_HREDRAW | CS_VREDRAW;
    gwc.lpfnWndProc = &MainWindow::graph_proc_thunk;
    gwc.hInstance = m_hInstance;
    gwc.hCursor = LoadCursorW(nullptr, IDC_HAND);
    gwc.hbrBackground = nullptr;
    gwc.lpszClassName = GRAPH_CLASS_NAME;
    RegisterClassExW(&gwc);
}

LRESULT CALLBACK MainWindow::window_proc_thunk(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    MainWindow* pThis = nullptr;
    if (uMsg == WM_NCCREATE) {
        auto* pCreate = reinterpret_cast<CREATESTRUCTW*>(lParam);
        pThis = reinterpret_cast<MainWindow*>(pCreate->lpCreateParams);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pThis));
    } else {
        pThis = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    }

    if (pThis) {
        return pThis->handle_message(hWnd, uMsg, wParam, lParam);
    }
    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

LRESULT CALLBACK MainWindow::graph_proc_thunk(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    MainWindow* pThis = nullptr;
    if (uMsg == WM_NCCREATE) {
        auto* pCreate = reinterpret_cast<CREATESTRUCTW*>(lParam);
        pThis = reinterpret_cast<MainWindow*>(pCreate->lpCreateParams);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pThis));
    } else {
        pThis = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    }

    if (pThis) {
        return pThis->handle_graph_message(hWnd, uMsg, wParam, lParam);
    }
    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

LRESULT MainWindow::handle_graph_message(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        draw_sparkline(hWnd, hdc);
        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_LBUTTONUP: {
        // Clicking on the sparkline triggers an instant ping test
        trigger_immediate_ping();
        return 0;
    }
    case WM_ERASEBKGND:
        return 1; // Handled with flicker-free double buffering
    }
    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

LRESULT MainWindow::handle_message(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_COMMAND: {
        int wmId = LOWORD(wParam);
        int wmEvent = HIWORD(wParam);

        if (wmId == IDC_COMBO_ADAPTER && wmEvent == CBN_SELCHANGE) {
            on_adapter_changed();
            return 0;
        }
        if (wmId == IDC_COMBO_PROFILE && wmEvent == CBN_SELCHANGE) {
            on_profile_selected();
            return 0;
        }
        if (wmId == IDC_BTN_APPLY) {
            apply_selected_dns();
            return 0;
        }
        if (wmId == IDC_BTN_BENCHMARK) {
            open_benchmark_dialog();
            return 0;
        }
        if (wmId == IDC_BTN_RESET_DHCP) {
            reset_to_dhcp();
            return 0;
        }
        if (wmId == IDC_BTN_FLUSH_DNS) {
            flush_dns_cache();
            return 0;
        }
        if (wmId == IDC_BTN_ADD_PROFILE) {
            open_add_profile_dialog();
            return 0;
        }
        if (wmId == IDC_BTN_DELETE_PROFILE) {
            if (m_selectedProfileIndex >= 0 && m_selectedProfileIndex < static_cast<int>(m_profiles.size())) {
                const auto& p = m_profiles[m_selectedProfileIndex];
                if (p.is_builtin) {
                    MessageBoxW(m_hWnd, L"Встроенные профили нельзя удалить.", L"Информация", MB_OK | MB_ICONINFORMATION);
                    return 0;
                }
                std::wstring pName = utf8_to_wide(p.name);
                std::wstring confirmMsg = L"Вы уверены, что хотите удалить профиль \"" + pName + L"\"?";
                if (MessageBoxW(m_hWnd, confirmMsg.c_str(), L"Подтверждение удаления", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                    m_configManager->delete_profile(p.id);
                    m_configManager->set_active_profile_id("dhcp");
                    m_configManager->save_to_file();
                    populate_profiles_combobox();
                    update_status_card();
                    show_tray_notification(L"Профиль удален", L"Профиль '" + pName + L"' успешно удален.");
                }
            }
            return 0;
        }
        if (wmId == IDC_BTN_COPY_IP) {
            wchar_t buf[256] = {0};
            if (m_hEditProfileIp) {
                GetWindowTextW(m_hEditProfileIp, buf, 256);
                copy_text_to_clipboard(buf);
            }
            return 0;
        }
        if (wmId == IDC_BTN_COPY_DOH) {
            wchar_t buf[512] = {0};
            if (m_hEditProfileDoh) {
                GetWindowTextW(m_hEditProfileDoh, buf, 512);
                copy_text_to_clipboard(buf);
            }
            return 0;
        }
        if (wmId == IDC_BTN_WIN_NETWORK) {
            open_windows_network_settings();
            return 0;
        }
        if (wmId == IDC_BTN_THEME_TOGGLE) {
            toggle_theme();
            return 0;
        }
        if (wmId == IDM_TRAY_SHOW) {
            ShowWindow(m_hWnd, SW_RESTORE);
            SetForegroundWindow(m_hWnd);
            return 0;
        }
        if (wmId == IDM_TRAY_RESET_DHCP) {
            reset_to_dhcp();
            return 0;
        }
        if (wmId == IDM_TRAY_FLUSH_DNS) {
            flush_dns_cache();
            return 0;
        }
        if (wmId == IDM_TRAY_EXIT) {
            DestroyWindow(m_hWnd);
            return 0;
        }
        if (wmId >= IDM_TRAY_PRESET_FIRST && wmId <= IDM_TRAY_PRESET_LAST) {
            on_tray_preset_selected(wmId);
            return 0;
        }
        break;
    }

    case WM_APP_TRAYMSG: {
        if (lParam == WM_LBUTTONUP || lParam == WM_LBUTTONDBLCLK) {
            ShowWindow(m_hWnd, SW_RESTORE);
            SetForegroundWindow(m_hWnd);
        } else if (lParam == WM_RBUTTONUP) {
            show_tray_context_menu();
        }
        return 0;
    }

    case WM_APP_DNS_APPLIED: {
        m_isBusy = false;
        EnableWindow(m_hBtnApply, TRUE);
        EnableWindow(m_hBtnBenchmark, TRUE);
        EnableWindow(m_hBtnResetDhcp, TRUE);
        EnableWindow(m_hBtnFlushDns, TRUE);
        if (m_hBtnWinNetwork) EnableWindow(m_hBtnWinNetwork, TRUE);

        refresh_network_adapters();
        update_status_card();
        trigger_immediate_ping();
        return 0;
    }

    case WM_APP_REFRESH_STATUS: {
        refresh_network_adapters();
        update_status_card();
        trigger_immediate_ping();
        return 0;
    }

    case WM_APP_PING_UPDATED: {
        if (m_hStatusPing) {
            std::wstring pingText = L"Задержка: ";
            if (m_currentPingMs > 0) {
                pingText += std::to_wstring(m_currentPingMs) + L" мс";
                if (m_currentPingMs < 60) pingText += L" (Отличная)";
                else if (m_currentPingMs <= 150) pingText += L" (Хорошая)";
                else pingText += L" (Высокая)";
            } else if (m_currentPingMs == 0) {
                pingText += L"< 1 мс (Мгновенно)";
            } else {
                pingText += L"Тайм-аут (Недоступен)";
            }
            SetWindowTextW(m_hStatusPing, pingText.c_str());
        }
        if (m_hGraphWnd && IsWindow(m_hGraphWnd)) {
            InvalidateRect(m_hGraphWnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_DRAWITEM: {
        auto* pDis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
        if (pDis && pDis->CtlType == ODT_BUTTON) {
            if (pDis->CtlID == IDC_BTN_THEME_TOGGLE) {
                draw_theme_toggle_button(pDis);
            } else {
                draw_custom_button(pDis);
            }
            return TRUE;
        }
        break;
    }

    case WM_CTLCOLORSTATIC: {
        HDC hdcStatic = (HDC)wParam;
        HWND hwndStatic = (HWND)lParam;
        SetBkMode(hdcStatic, TRANSPARENT);
        auto pal = GetPalette(m_isDarkMode);

        if (hwndStatic == m_hFooterVersion) {
            SetTextColor(hdcStatic, pal.textMuted);
        } else if (hwndStatic == m_hStatusDnsList) {
            SetTextColor(hdcStatic, pal.accentBlue);
        } else if (hwndStatic == m_hStatusBadge) {
            SetTextColor(hdcStatic, pal.accentGreen);
        } else if (hwndStatic == m_hStatusPing) {
            if (m_currentPingMs > 0 && m_currentPingMs < 60) {
                SetTextColor(hdcStatic, pal.accentGreen);
            } else if (m_currentPingMs >= 60 && m_currentPingMs <= 150) {
                SetTextColor(hdcStatic, pal.accentAmber);
            } else if (m_currentPingMs > 150 || m_currentPingMs < 0) {
                SetTextColor(hdcStatic, pal.accentRed);
            } else {
                SetTextColor(hdcStatic, pal.textSecondary);
            }
        } else if (hwndStatic == m_hProfileDetails) {
            SetTextColor(hdcStatic, pal.textSecondary);
        } else if (hwndStatic == m_hStatusAdapterName) {
            SetTextColor(hdcStatic, pal.textPrimary);
        } else if (hwndStatic == m_hEditProfileIp || hwndStatic == m_hEditProfileDoh) {
            SetBkMode(hdcStatic, OPAQUE);
            SetTextColor(hdcStatic, pal.textPrimary);
            SetBkColor(hdcStatic, pal.cardBg);
            return (INT_PTR)m_hbrCard;
        } else {
            SetTextColor(hdcStatic, pal.textSecondary);
        }

        if (hwndStatic == m_hStatusAdapterName || hwndStatic == m_hStatusDnsList || 
            hwndStatic == m_hStatusBadge || hwndStatic == m_hStatusPing) {
            return (INT_PTR)m_hbrCard;
        }

        return (INT_PTR)m_hbrBg;
    }

    case WM_CTLCOLOREDIT: {
        HDC hdcEdit = (HDC)wParam;
        auto pal = GetPalette(m_isDarkMode);
        SetBkMode(hdcEdit, OPAQUE);
        SetTextColor(hdcEdit, pal.textPrimary);
        SetBkColor(hdcEdit, pal.cardBg);
        return (INT_PTR)m_hbrCard;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        auto pal = GetPalette(m_isDarkMode);

        // Draw background elevated card for Status area with anti-aliasing
        {
            Gdiplus::Graphics g(hdc);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

            Gdiplus::GraphicsPath cardPath;
            AddRoundedRect(cardPath, Gdiplus::RectF(20.5f, 44.5f, static_cast<float>(WINDOW_WIDTH - 40) - 1.0f, 114.0f - 1.0f), 12.0f);

            Gdiplus::SolidBrush cardBrush(Gdiplus::Color(255, GetRValue(pal.cardBg), GetGValue(pal.cardBg), GetBValue(pal.cardBg)));
            Gdiplus::Pen cardPen(Gdiplus::Color(255, GetRValue(pal.cardBorder), GetGValue(pal.cardBorder), GetBValue(pal.cardBorder)), 1.0f);

            g.FillPath(&cardBrush, &cardPath);
            g.DrawPath(&cardPen, &cardPath);
        }

        EndPaint(hWnd, &ps);
        return 0;
    }

    case WM_SYSCOMMAND: {
        if ((wParam & 0xFFF0) == SC_MINIMIZE) {
            ShowWindow(m_hWnd, SW_HIDE);
            m_inTray = true;
            show_tray_notification(L"QuickDNS свернут в трей", L"Приложение продолжает работать. Нажмите на значок для быстрого переключения DNS.");
            return 0;
        }
        break;
    }

    case WM_DESTROY: {
        stop_ping_worker();
        PostQuitMessage(0);
        return 0;
    }
    }

    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

void MainWindow::apply_modern_theme() {
    // Windows 10/11 Immersive Dark Mode
    BOOL useDarkMode = m_isDarkMode ? TRUE : FALSE;
    DwmSetWindowAttribute(m_hWnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &useDarkMode, sizeof(useDarkMode));

    // Windows 11 Rounded Corners
    DWORD cornerPref = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(m_hWnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &cornerPref, sizeof(cornerPref));
}

void MainWindow::apply_theme(bool isDark) {
    m_isDarkMode = isDark;
    auto pal = GetPalette(m_isDarkMode);

    if (m_hbrBg) DeleteObject(m_hbrBg);
    m_hbrBg = CreateSolidBrush(pal.bg);

    if (m_hbrCard) DeleteObject(m_hbrCard);
    m_hbrCard = CreateSolidBrush(pal.cardBg);

    // Update Windows 10/11 Immersive Dark Mode for Title Bar
    BOOL useDarkMode = m_isDarkMode ? TRUE : FALSE;
    DwmSetWindowAttribute(m_hWnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &useDarkMode, sizeof(useDarkMode));

    SetClassLongPtrW(m_hWnd, GCLP_HBRBACKGROUND, (LONG_PTR)m_hbrBg);

    // Update Tooltip text for theme toggle button
    if (m_hToolTip && m_hBtnThemeToggle) {
        TOOLINFOW ti = { sizeof(TOOLINFOW) };
        ti.hwnd = m_hWnd;
        ti.uId = (UINT_PTR)m_hBtnThemeToggle;
        std::wstring tip = m_isDarkMode ? L"Переключить на светлую тему" : L"Переключить на темную тему";
        ti.lpszText = const_cast<LPWSTR>(tip.c_str());
        SendMessageW(m_hToolTip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
    }

    // Force full repaint of window and all child controls
    InvalidateRect(m_hWnd, nullptr, TRUE);
    RedrawWindow(m_hWnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

void MainWindow::toggle_theme() {
    apply_theme(!m_isDarkMode);
    m_configManager->set_theme_mode(m_isDarkMode ? "dark" : "light");
    m_configManager->save_to_file();
}

void MainWindow::attach_button_effects(HWND hBtn, UINT_PTR subclassId) {
    if (!hBtn) return;
    auto* anim = new ButtonAnimationState();
    SetPropW(hBtn, L"QuickDnsAnim", reinterpret_cast<HANDLE>(anim));
    SetWindowSubclass(hBtn, AnimatedButtonSubclassProc, subclassId, reinterpret_cast<DWORD_PTR>(anim));
}

void MainWindow::setup_fonts() {
    m_hFontRegular = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    m_hFontBold = CreateFontW(-14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    m_hFontHeader = CreateFontW(-18, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable Display");

    m_hFontMonospace = CreateFontW(-13, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");

    m_hFontSmall = CreateFontW(-11, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    m_hFontGraphVal = CreateFontW(-13, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
}

void MainWindow::create_controls() {
    int startY = 14;
    int marginX = 20;
    int contentWidth = WINDOW_WIDTH - 40;

    // 1. Status Card Header
    HWND hTitle = CreateWindowExW(0, L"STATIC", L"QuickDNS v1.2.1 — Менеджер DNS",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        marginX, startY, contentWidth - 44, 24,
        m_hWnd, nullptr, m_hInstance, nullptr);
    SendMessageW(hTitle, WM_SETFONT, (WPARAM)m_hFontHeader, TRUE);

    // Theme Switcher button in top-right corner
    m_hBtnThemeToggle = CreateWindowExW(0, WC_BUTTONW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        marginX + contentWidth - 36, startY - 3, 36, 28,
        m_hWnd, (HMENU)IDC_BTN_THEME_TOGGLE, m_hInstance, nullptr);

    // Tooltip for Theme Switcher
    m_hToolTip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
        WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        m_hWnd, nullptr, m_hInstance, nullptr);
    SetWindowPos(m_hToolTip, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    TOOLINFOW ti = { sizeof(TOOLINFOW) };
    ti.uFlags = TTF_SUBCLASS | TTF_IDISHWND;
    ti.hwnd = m_hWnd;
    ti.uId = (UINT_PTR)m_hBtnThemeToggle;
    std::wstring tip = m_isDarkMode ? L"Переключить на светлую тему" : L"Переключить на темную тему";
    ti.lpszText = const_cast<LPWSTR>(tip.c_str());
    SendMessageW(m_hToolTip, TTM_ADDTOOLW, 0, (LPARAM)&ti);

    startY += 30; // 44

    // Inside Status Card:
    // Left: connection info, DNS IP, status, live ping (width = 300)
    int leftInfoW = 295;
    int insideCardY = startY + 10;

    m_hStatusAdapterName = CreateWindowExW(0, L"STATIC", L"Подключение: Определение сети...",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        marginX + 14, insideCardY, leftInfoW, 20,
        m_hWnd, (HMENU)IDC_STATUS_TITLE, m_hInstance, nullptr);
    SendMessageW(m_hStatusAdapterName, WM_SETFONT, (WPARAM)m_hFontBold, TRUE);
    insideCardY += 24;

    m_hStatusDnsList = CreateWindowExW(0, L"STATIC", L"Текущий DNS: Загрузка...",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        marginX + 14, insideCardY, leftInfoW, 20,
        m_hWnd, (HMENU)IDC_STATUS_DNS_VAL, m_hInstance, nullptr);
    SendMessageW(m_hStatusDnsList, WM_SETFONT, (WPARAM)m_hFontMonospace, TRUE);
    insideCardY += 24;

    m_hStatusBadge = CreateWindowExW(0, L"STATIC", L"Статус: Проверка...",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        marginX + 14, insideCardY, leftInfoW, 20,
        m_hWnd, nullptr, m_hInstance, nullptr);
    SendMessageW(m_hStatusBadge, WM_SETFONT, (WPARAM)m_hFontRegular, TRUE);
    insideCardY += 24;

    m_hStatusPing = CreateWindowExW(0, L"STATIC", L"Задержка: Ожидание...",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        marginX + 14, insideCardY, leftInfoW, 20,
        m_hWnd, (HMENU)IDC_STATUS_PING, m_hInstance, nullptr);
    SendMessageW(m_hStatusPing, WM_SETFONT, (WPARAM)m_hFontBold, TRUE);

    // Right side of Card: Minimalist Sparkline Graph Control!
    int graphW = 175;
    int graphH = 94;
    int graphX = marginX + contentWidth - graphW - 10;
    int graphY = startY + 10;

    m_hGraphWnd = CreateWindowExW(
        0, GRAPH_CLASS_NAME, L"",
        WS_CHILD | WS_VISIBLE,
        graphX, graphY, graphW, graphH,
        m_hWnd, (HMENU)IDC_GRAPH_VIEW, m_hInstance, this
    );

    startY += 114 + 14; // Past Card -> 172

    // Separator Line
    CreateWindowExW(0, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
        marginX, startY, contentWidth, 2,
        m_hWnd, nullptr, m_hInstance, nullptr);
    startY += 12;

    // 2. Network Adapter Selector
    HWND hLblAdapter = CreateWindowExW(0, L"STATIC", L"Сетевой адаптер:",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        marginX, startY, contentWidth, 18,
        m_hWnd, nullptr, m_hInstance, nullptr);
    SendMessageW(hLblAdapter, WM_SETFONT, (WPARAM)m_hFontRegular, TRUE);
    startY += 20;

    m_hComboAdapter = CreateWindowExW(0, WC_COMBOBOXW, L"",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
        marginX, startY, contentWidth, 200,
        m_hWnd, (HMENU)IDC_COMBO_ADAPTER, m_hInstance, nullptr);
    SendMessageW(m_hComboAdapter, WM_SETFONT, (WPARAM)m_hFontRegular, TRUE);
    startY += 36;

    // 3. DNS Profile Selector
    HWND hLblProfile = CreateWindowExW(0, L"STATIC", L"Выберите DNS профиль (Comss, GeoHide, Без межаў и др.):",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        marginX, startY, contentWidth, 18,
        m_hWnd, nullptr, m_hInstance, nullptr);
    SendMessageW(hLblProfile, WM_SETFONT, (WPARAM)m_hFontRegular, TRUE);
    startY += 20;

    m_hComboProfile = CreateWindowExW(0, WC_COMBOBOXW, L"",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
        marginX, startY, contentWidth, 200,
        m_hWnd, (HMENU)IDC_COMBO_PROFILE, m_hInstance, nullptr);
    SendMessageW(m_hComboProfile, WM_SETFONT, (WPARAM)m_hFontRegular, TRUE);
    startY += 32;

    int editW = contentWidth - 116;
    int btnCopyW = 106;
    int copyX = marginX + editW + 10;

    // IP Address (Copyable)
    HWND hLblIp = CreateWindowExW(0, L"STATIC", L"IP-адреса DNS серверов:",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        marginX, startY, contentWidth, 16,
        m_hWnd, nullptr, m_hInstance, nullptr);
    SendMessageW(hLblIp, WM_SETFONT, (WPARAM)m_hFontSmall, TRUE);
    startY += 18;

    m_hEditProfileIp = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_READONLY,
        marginX, startY, editW, 26,
        m_hWnd, (HMENU)IDC_EDIT_PROFILE_IP, m_hInstance, nullptr);
    SendMessageW(m_hEditProfileIp, WM_SETFONT, (WPARAM)m_hFontMonospace, TRUE);

    m_hBtnCopyIp = CreateWindowExW(0, WC_BUTTONW, L"Копировать IP",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        copyX, startY, btnCopyW, 26,
        m_hWnd, (HMENU)IDC_BTN_COPY_IP, m_hInstance, nullptr);
    startY += 30;

    // DoH Template (Copyable, Read-Only)
    HWND hLblDoh = CreateWindowExW(0, L"STATIC", L"Шаблон DNS over HTTPS (DoH):",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        marginX, startY, contentWidth, 16,
        m_hWnd, nullptr, m_hInstance, nullptr);
    SendMessageW(hLblDoh, WM_SETFONT, (WPARAM)m_hFontSmall, TRUE);
    startY += 18;

    m_hEditProfileDoh = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_READONLY,
        marginX, startY, editW, 26,
        m_hWnd, (HMENU)IDC_EDIT_PROFILE_DOH, m_hInstance, nullptr);
    SendMessageW(m_hEditProfileDoh, WM_SETFONT, (WPARAM)m_hFontRegular, TRUE);

    m_hBtnCopyDoh = CreateWindowExW(0, WC_BUTTONW, L"Копировать DoH",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        copyX, startY, btnCopyW, 26,
        m_hWnd, (HMENU)IDC_BTN_COPY_DOH, m_hInstance, nullptr);
    startY += 30;

    // Profile Description
    m_hProfileDetails = CreateWindowExW(0, L"STATIC", L"Описание выбранного профиля...",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        marginX, startY, contentWidth, 26,
        m_hWnd, nullptr, m_hInstance, nullptr);
    SendMessageW(m_hProfileDetails, WM_SETFONT, (WPARAM)m_hFontSmall, TRUE);
    startY += 30;

    // 4. Action Buttons with comfortable neutral spacing
    m_hBtnApply = CreateWindowExW(0, WC_BUTTONW, L"Применить DNS в 1 клик",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        marginX, startY, contentWidth, 38,
        m_hWnd, (HMENU)IDC_BTN_APPLY, m_hInstance, nullptr);
    startY += 38 + 10;

    m_hBtnBenchmark = CreateWindowExW(0, WC_BUTTONW, L"Проверить скорость всех DNS (пинг)",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        marginX, startY, contentWidth, 34,
        m_hWnd, (HMENU)IDC_BTN_BENCHMARK, m_hInstance, nullptr);
    startY += 34 + 10;

    int halfWidth = (contentWidth - 12) / 2;
    m_hBtnResetDhcp = CreateWindowExW(0, WC_BUTTONW, L"Сброс на DHCP (Авто)",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        marginX, startY, halfWidth, 34,
        m_hWnd, (HMENU)IDC_BTN_RESET_DHCP, m_hInstance, nullptr);

    m_hBtnWinNetwork = CreateWindowExW(0, WC_BUTTONW, L"Параметры Ethernet (DNS)",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        marginX + halfWidth + 12, startY, halfWidth, 34,
        m_hWnd, (HMENU)IDC_BTN_WIN_NETWORK, m_hInstance, nullptr);
    startY += 34 + 10;

    m_hBtnFlushDns = CreateWindowExW(0, WC_BUTTONW, L"Сбросить DNS-кэш",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        marginX, startY, halfWidth, 34,
        m_hWnd, (HMENU)IDC_BTN_FLUSH_DNS, m_hInstance, nullptr);

    m_hBtnAddProfile = CreateWindowExW(0, WC_BUTTONW, L"Добавить свой DNS...",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        marginX + halfWidth + 12, startY, halfWidth, 34,
        m_hWnd, (HMENU)IDC_BTN_ADD_PROFILE, m_hInstance, nullptr);
    startY += 34 + 10;

    m_hBtnDeleteProfile = CreateWindowExW(0, WC_BUTTONW, L"Удалить профиль",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        marginX, startY, contentWidth, 30,
        m_hWnd, (HMENU)IDC_BTN_DELETE_PROFILE, m_hInstance, nullptr);
    EnableWindow(m_hBtnDeleteProfile, FALSE);
    startY += 30 + 12;

    // 5. Footer Version Badge
    m_hFooterVersion = CreateWindowExW(0, L"STATIC", L"QuickDNS v1.2.1 • Portable",
        WS_CHILD | WS_VISIBLE | SS_CENTER,
        marginX, startY, contentWidth, 18,
        m_hWnd, nullptr, m_hInstance, nullptr);
    SendMessageW(m_hFooterVersion, WM_SETFONT, (WPARAM)m_hFontRegular, TRUE);

    // Attach smooth animations, hover effects and hand cursor (IDC_HAND) to all buttons
    attach_button_effects(m_hBtnApply, 1);
    attach_button_effects(m_hBtnBenchmark, 2);
    attach_button_effects(m_hBtnResetDhcp, 3);
    attach_button_effects(m_hBtnWinNetwork, 4);
    attach_button_effects(m_hBtnFlushDns, 5);
    attach_button_effects(m_hBtnAddProfile, 6);
    attach_button_effects(m_hBtnDeleteProfile, 7);
    attach_button_effects(m_hBtnCopyIp, 8);
    attach_button_effects(m_hBtnCopyDoh, 9);
    attach_button_effects(m_hBtnThemeToggle, 10);
}

void MainWindow::draw_theme_toggle_button(DRAWITEMSTRUCT* pDis) {
    if (!pDis) return;

    HDC hdc = pDis->hDC;
    RECT rc = pDis->rcItem;
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;

    auto pal = GetPalette(m_isDarkMode);

    auto* anim = reinterpret_cast<ButtonAnimationState*>(GetPropW(pDis->hwndItem, L"QuickDnsAnim"));
    float hoverVal = anim ? anim->hover : 0.0f;
    bool isPressed = (pDis->itemState & ODS_SELECTED) || (anim && anim->isPressed);

    // Double buffer
    HDC hdcMem = CreateCompatibleDC(hdc);
    HBITMAP hbmMem = CreateCompatibleBitmap(hdc, w, h);
    HGDIOBJ oldBmp = SelectObject(hdcMem, hbmMem);

    RECT rcMem = { 0, 0, w, h };

    // Fill background for smooth corner blend
    HBRUSH hbrWin = CreateSolidBrush(pal.bg);
    FillRect(hdcMem, &rcMem, hbrWin);
    DeleteObject(hbrWin);

    COLORREF baseBg = m_isDarkMode ? RGB(27, 31, 41) : RGB(255, 255, 255);
    COLORREF hoverBg = m_isDarkMode ? RGB(45, 53, 70) : RGB(241, 245, 249);
    COLORREF pressBg = m_isDarkMode ? RGB(20, 24, 32) : RGB(226, 232, 240);

    COLORREF baseBorder = m_isDarkMode ? RGB(45, 53, 70) : RGB(226, 232, 240);
    COLORREF hoverBorder = m_isDarkMode ? RGB(251, 191, 36) : RGB(79, 70, 229);

    COLORREF curBg = isPressed ? pressBg : LerpColor(baseBg, hoverBg, hoverVal);
    COLORREF curBorder = LerpColor(baseBorder, hoverBorder, hoverVal);

    {
        Gdiplus::Graphics g(hdcMem);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

        Gdiplus::GraphicsPath path;
        AddRoundedRect(path, Gdiplus::RectF(0.5f, 0.5f, static_cast<float>(w) - 1.0f, static_cast<float>(h) - 1.0f), 7.0f);

        Gdiplus::SolidBrush br(Gdiplus::Color(255, GetRValue(curBg), GetGValue(curBg), GetBValue(curBg)));
        float penWidth = 1.2f + 0.4f * hoverVal;
        Gdiplus::Pen pen(Gdiplus::Color(255, GetRValue(curBorder), GetGValue(curBorder), GetBValue(curBorder)), penWidth);

        g.FillPath(&br, &path);
        g.DrawPath(&pen, &path);

        // Center coordinates of the vector icon (with 1px depression on press)
        float cx = w * 0.5f + (isPressed ? 1.0f : 0.0f);
        float cy = h * 0.5f + (isPressed ? 1.0f : 0.0f);

        if (m_isDarkMode) {
            // Radiant Sun Icon (Warm Amber / Gold #FBBF24)
            Gdiplus::Color sunColor(255, 251, 191, 36);
            Gdiplus::SolidBrush sunBrush(sunColor);
            Gdiplus::Pen sunPen(sunColor, 1.5f);
            sunPen.SetLineCap(Gdiplus::LineCapRound, Gdiplus::LineCapRound, Gdiplus::DashCapRound);

            // Core sun disc
            float sunRadius = 4.2f;
            g.FillEllipse(&sunBrush, cx - sunRadius, cy - sunRadius, sunRadius * 2.0f, sunRadius * 2.0f);

            // 8 Radial rays
            float rInner = 6.2f;
            float rOuter = 9.2f;
            for (int i = 0; i < 8; ++i) {
                float angle = i * (3.14159265f / 4.0f);
                float x1 = cx + std::cos(angle) * rInner;
                float y1 = cy + std::sin(angle) * rInner;
                float x2 = cx + std::cos(angle) * rOuter;
                float y2 = cy + std::sin(angle) * rOuter;
                g.DrawLine(&sunPen, x1, y1, x2, y2);
            }
        } else {
            // Crescent Moon Icon (Indigo #4F46E5)
            Gdiplus::Color moonColor(255, 79, 70, 229);
            Gdiplus::SolidBrush moonBrush(moonColor);

            Gdiplus::GraphicsPath moonPath;
            float moonR = 6.5f;
            moonPath.AddArc(cx - moonR, cy - moonR, moonR * 2.0f, moonR * 2.0f, -80.0f, 260.0f);
            moonPath.AddArc(cx - moonR * 0.6f, cy - moonR * 0.8f, moonR * 1.5f, moonR * 1.5f, 150.0f, -200.0f);
            moonPath.CloseFigure();

            g.FillPath(&moonBrush, &moonPath);
        }
    }

    BitBlt(hdc, rc.left, rc.top, w, h, hdcMem, 0, 0, SRCCOPY);

    SelectObject(hdcMem, oldBmp);
    DeleteObject(hbmMem);
    DeleteDC(hdcMem);
}

void MainWindow::draw_custom_button(DRAWITEMSTRUCT* pDis) {
    if (!pDis) return;

    HDC hdc = pDis->hDC;
    RECT rc = pDis->rcItem;

    auto pal = GetPalette(m_isDarkMode);

    auto* anim = reinterpret_cast<ButtonAnimationState*>(GetPropW(pDis->hwndItem, L"QuickDnsAnim"));
    float hoverVal = anim ? anim->hover : 0.0f;
    bool isPressed  = (pDis->itemState & ODS_SELECTED) || (anim && anim->isPressed);
    bool isDisabled = (pDis->itemState & ODS_DISABLED) != 0;

    COLORREF baseBg, hoverBg, pressBg, baseBorder, hoverBorder, textCol;

    if (pDis->CtlID == IDC_BTN_APPLY) {
        // Primary Slate Navy Action Button
        if (isDisabled) {
            baseBg = m_isDarkMode ? RGB(30, 42, 60) : RGB(210, 222, 238);
            baseBorder = m_isDarkMode ? RGB(45, 58, 80) : RGB(180, 198, 222);
            textCol = m_isDarkMode ? RGB(110, 128, 155) : RGB(130, 150, 180);
            hoverBg = baseBg; pressBg = baseBg; hoverBorder = baseBorder;
        } else {
            baseBg = pal.btnPrimaryBg;
            hoverBg = pal.btnPrimaryHover;
            pressBg = pal.btnPrimaryPress;
            baseBorder = pal.btnPrimaryBorder;
            hoverBorder = m_isDarkMode ? RGB(85, 125, 175) : RGB(55, 95, 140);
            textCol = pal.btnPrimaryText;
        }
    } else if (pDis->CtlID == IDC_BTN_BENCHMARK) {
        // Subdued Steel Speed Test Button
        if (isDisabled) {
            baseBg = m_isDarkMode ? RGB(32, 40, 58) : RGB(218, 224, 238);
            baseBorder = m_isDarkMode ? RGB(46, 56, 78) : RGB(188, 196, 218);
            textCol = m_isDarkMode ? RGB(112, 125, 150) : RGB(135, 148, 175);
            hoverBg = baseBg; pressBg = baseBg; hoverBorder = baseBorder;
        } else {
            baseBg = pal.btnBenchBg;
            hoverBg = pal.btnBenchHover;
            pressBg = pal.btnBenchPress;
            baseBorder = pal.btnBenchBorder;
            hoverBorder = m_isDarkMode ? RGB(95, 115, 160) : RGB(70, 90, 130);
            textCol = pal.btnBenchText;
        }
    } else {
        // Utility Secondary Buttons (All Neutral)
        if (isDisabled) {
            baseBg = m_isDarkMode ? RGB(24, 27, 36) : RGB(241, 245, 249);
            baseBorder = m_isDarkMode ? RGB(38, 43, 56) : RGB(226, 232, 240);
            textCol = m_isDarkMode ? RGB(85, 95, 115) : RGB(148, 163, 184);
            hoverBg = baseBg; pressBg = baseBg; hoverBorder = baseBorder;
        } else {
            baseBg = pal.btnUtilBg;
            hoverBg = pal.btnUtilHover;
            pressBg = pal.btnUtilPress;
            baseBorder = pal.btnUtilBorder;
            hoverBorder = m_isDarkMode ? RGB(82, 94, 114) : RGB(160, 172, 190);

            if (pDis->CtlID == IDC_BTN_DELETE_PROFILE) {
                hoverBorder = m_isDarkMode ? RGB(160, 75, 75) : RGB(190, 70, 70);
            }
            textCol = pal.btnUtilText;
        }
    }

    COLORREF bgCol = isDisabled ? baseBg : (isPressed ? pressBg : LerpColor(baseBg, hoverBg, hoverVal));
    COLORREF borderCol = isDisabled ? baseBorder : (isPressed ? hoverBorder : LerpColor(baseBorder, hoverBorder, hoverVal));

    // Double buffer for clean flicker-free button paint
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    HDC hdcMem = CreateCompatibleDC(hdc);
    HBITMAP hbmMem = CreateCompatibleBitmap(hdc, w, h);
    HGDIOBJ oldBmp = SelectObject(hdcMem, hbmMem);

    RECT rcMem = { 0, 0, w, h };

    // Fill background for smooth corner blend
    HBRUSH hbrWin = CreateSolidBrush(pal.bg);
    FillRect(hdcMem, &rcMem, hbrWin);
    DeleteObject(hbrWin);

    // Draw rounded button surface with anti-aliasing
    {
        Gdiplus::Graphics g(hdcMem);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

        Gdiplus::GraphicsPath btnPath;
        float radius = (pDis->CtlID == IDC_BTN_COPY_IP || pDis->CtlID == IDC_BTN_COPY_DOH) ? 6.0f : 8.0f;
        AddRoundedRect(btnPath, Gdiplus::RectF(0.5f, 0.5f, static_cast<float>(w) - 1.0f, static_cast<float>(h) - 1.0f), radius);

        Gdiplus::SolidBrush bgBrush(Gdiplus::Color(255, GetRValue(bgCol), GetGValue(bgCol), GetBValue(bgCol)));
        float penWidth = 1.2f + 0.4f * hoverVal;
        Gdiplus::Pen borderPen(Gdiplus::Color(255, GetRValue(borderCol), GetGValue(borderCol), GetBValue(borderCol)), penWidth);

        g.FillPath(&bgBrush, &btnPath);
        g.DrawPath(&borderPen, &btnPath);
    }

    // Get Button text
    wchar_t btnText[128] = {};
    GetWindowTextW(pDis->hwndItem, btnText, _countof(btnText));

    SetBkMode(hdcMem, TRANSPARENT);
    SetTextColor(hdcMem, textCol);
    HFONT textFont = (pDis->CtlID == IDC_BTN_APPLY || pDis->CtlID == IDC_BTN_BENCHMARK) ? m_hFontBold : 
                     ((pDis->CtlID == IDC_BTN_COPY_IP || pDis->CtlID == IDC_BTN_COPY_DOH) ? m_hFontSmall : m_hFontRegular);
    HGDIOBJ oldFont = SelectObject(hdcMem, textFont);

    // Press animation: offset text slightly down & right when pressed!
    RECT rcText = rcMem;
    if (isPressed) {
        rcText.left += 1;
        rcText.top += 1;
        rcText.right += 1;
        rcText.bottom += 1;
    }

    DrawTextW(hdcMem, btnText, -1, &rcText, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    SelectObject(hdcMem, oldFont);

    BitBlt(hdc, rc.left, rc.top, w, h, hdcMem, 0, 0, SRCCOPY);

    SelectObject(hdcMem, oldBmp);
    DeleteObject(hbmMem);
    DeleteDC(hdcMem);
}

void MainWindow::draw_sparkline(HWND hWnd, HDC hdc) {
    RECT rc;
    GetClientRect(hWnd, &rc);
    int width = rc.right - rc.left;
    int height = rc.bottom - rc.top;
    if (width <= 0 || height <= 0) return;

    // Double buffer
    HDC hdcMem = CreateCompatibleDC(hdc);
    HBITMAP hbmMem = CreateCompatibleBitmap(hdc, width, height);
    HGDIOBJ oldBmp = SelectObject(hdcMem, hbmMem);

    // Copy latency history safely under lock
    std::vector<int> history;
    int currentPing = -1;
    {
        std::lock_guard<std::mutex> lock(m_historyMutex);
        history = m_latencyHistory;
        currentPing = m_currentPingMs;
    }

    // Grid coordinates
    float plotTop    = 24.0f;
    float plotBottom = static_cast<float>(height - 12);
    float plotLeft   = 10.0f;
    float plotRight  = static_cast<float>(width - 10);
    float plotHeight = plotBottom - plotTop;
    float plotWidth  = plotRight - plotLeft;

    // Determine scale: max latency, minimum 80ms, max 300ms
    int maxMs = 80;
    for (int v : history) {
        if (v > maxMs) maxMs = std::min(300, v + 20);
    }

    // Determine curve color based on latest valid latency
    auto pal = GetPalette(m_isDarkMode);
    Gdiplus::Color lineColor;
    if (currentPing > 0 && currentPing < 60) {
        lineColor = Gdiplus::Color(255, 34, 197, 94);  // Emerald Green
    } else if (currentPing >= 60 && currentPing <= 150) {
        lineColor = Gdiplus::Color(255, 245, 158, 11); // Amber
    } else if (currentPing > 150) {
        lineColor = Gdiplus::Color(255, 239, 68, 68);  // Red
    } else {
        lineColor = Gdiplus::Color(255, GetRValue(pal.graphLine), GetGValue(pal.graphLine), GetBValue(pal.graphLine));
    }

    // GDI+ Anti-Aliased Rendering
    {
        Gdiplus::Graphics g(hdcMem);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

        // 1. Draw smooth container background
        Gdiplus::GraphicsPath contPath;
        AddRoundedRect(contPath, Gdiplus::RectF(0.5f, 0.5f, static_cast<float>(width) - 1.0f, static_cast<float>(height) - 1.0f), 8.0f);
        Gdiplus::SolidBrush contBrush(Gdiplus::Color(255, GetRValue(pal.graphBg), GetGValue(pal.graphBg), GetBValue(pal.graphBg)));
        Gdiplus::Pen contPen(Gdiplus::Color(255, GetRValue(pal.graphBorder), GetGValue(pal.graphBorder), GetBValue(pal.graphBorder)), 1.0f);
        g.FillPath(&contBrush, &contPath);
        g.DrawPath(&contPen, &contPath);

        // 2. Faint grid guide lines at 50ms and 100ms
        Gdiplus::Pen gridPen(Gdiplus::Color(100, GetRValue(pal.graphGrid), GetGValue(pal.graphGrid), GetBValue(pal.graphGrid)), 1.0f);
        gridPen.SetDashStyle(Gdiplus::DashStyleDash);

        if (maxMs >= 50) {
            float y50 = plotBottom - (50.0f * plotHeight / static_cast<float>(maxMs));
            g.DrawLine(&gridPen, plotLeft, y50, plotRight, y50);
        }
        if (maxMs >= 100) {
            float y100 = plotBottom - (100.0f * plotHeight / static_cast<float>(maxMs));
            g.DrawLine(&gridPen, plotLeft, y100, plotRight, y100);
        }

        // 3. Smooth sparkline curve & translucent area fill
        if (!history.empty() && history.size() >= 2) {
            size_t count = history.size();
            std::vector<Gdiplus::PointF> ptsF;
            ptsF.reserve(count);

            for (size_t i = 0; i < count; ++i) {
                int val = history[i];
                float x = plotLeft + (static_cast<float>(i) * plotWidth / static_cast<float>(count - 1));
                float y = (val <= 0) ? plotBottom : (plotBottom - (static_cast<float>(std::min(val, maxMs)) * plotHeight / static_cast<float>(maxMs)));
                ptsF.push_back(Gdiplus::PointF(x, y));
            }

            // Area polygon under curve with translucent alpha fill
            std::vector<Gdiplus::PointF> polyF = ptsF;
            polyF.push_back(Gdiplus::PointF(ptsF.back().X, plotBottom));
            polyF.push_back(Gdiplus::PointF(ptsF.front().X, plotBottom));

            Gdiplus::SolidBrush fillBrush(Gdiplus::Color(45, lineColor.GetR(), lineColor.GetG(), lineColor.GetB()));
            g.FillPolygon(&fillBrush, polyF.data(), static_cast<int>(polyF.size()));

            // Smooth anti-aliased curve line
            Gdiplus::Pen linePen(lineColor, 2.2f);
            linePen.SetLineJoin(Gdiplus::LineJoinRound);
            linePen.SetStartCap(Gdiplus::LineCapRound);
            linePen.SetEndCap(Gdiplus::LineCapRound);
            g.DrawLines(&linePen, ptsF.data(), static_cast<int>(ptsF.size()));

            // Latest point indicator: outer glow halo + inner solid dot + white ring
            Gdiplus::PointF lastPt = ptsF.back();
            Gdiplus::SolidBrush glowBrush(Gdiplus::Color(80, lineColor.GetR(), lineColor.GetG(), lineColor.GetB()));
            g.FillEllipse(&glowBrush, lastPt.X - 6.0f, lastPt.Y - 6.0f, 12.0f, 12.0f);

            Gdiplus::SolidBrush dotBrush(lineColor);
            Gdiplus::Pen ringPen(Gdiplus::Color(255, 255, 255, 255), 1.5f);
            g.FillEllipse(&dotBrush, lastPt.X - 3.5f, lastPt.Y - 3.5f, 7.0f, 7.0f);
            g.DrawEllipse(&ringPen, lastPt.X - 3.5f, lastPt.Y - 3.5f, 7.0f, 7.0f);
        }
    }

    // Top-left header: "Пинг (60 с)"
    SetBkMode(hdcMem, TRANSPARENT);
    SetTextColor(hdcMem, pal.textMuted);
    HGDIOBJ oldFont = SelectObject(hdcMem, m_hFontSmall);

    RECT rcTitle = { 10, 6, width - 10, 20 };
    DrawTextW(hdcMem, L"Пинг (60 с)", -1, &rcTitle, DT_LEFT | DT_TOP | DT_SINGLELINE);

    // Value badge at top-right of graph
    std::wstring valStr;
    COLORREF valCol;
    if (currentPing > 0) {
        valStr = std::to_wstring(currentPing) + L" мс";
        if (currentPing < 60) valCol = RGB(74, 222, 128);
        else if (currentPing <= 150) valCol = RGB(251, 191, 36);
        else valCol = RGB(248, 113, 113);
    } else if (currentPing == 0) {
        valStr = L"<1 мс";
        valCol = RGB(74, 222, 128);
    } else {
        valStr = L"—";
        valCol = pal.textMuted;
    }

    SetTextColor(hdcMem, valCol);
    SelectObject(hdcMem, m_hFontGraphVal);
    RECT rcVal = { width - 85, 6, width - 10, 22 };
    DrawTextW(hdcMem, valStr.c_str(), -1, &rcVal, DT_RIGHT | DT_TOP | DT_SINGLELINE);

    SelectObject(hdcMem, oldFont);

    BitBlt(hdc, 0, 0, width, height, hdcMem, 0, 0, SRCCOPY);

    SelectObject(hdcMem, oldBmp);
    DeleteObject(hbmMem);
    DeleteDC(hdcMem);
}

void MainWindow::start_ping_worker() {
    m_stopPingWorker = false;
    m_pingWorker = std::thread([this]() {
        while (!m_stopPingWorker) {
            std::string ip_to_ping;
            {
                if (m_selectedAdapterIndex >= 0 && m_selectedAdapterIndex < static_cast<int>(m_adapters.size())) {
                    const auto& adp = m_adapters[m_selectedAdapterIndex];
                    if (!adp.dns_ipv4.empty()) {
                        ip_to_ping = wide_to_utf8(adp.dns_ipv4[0]);
                    }
                }
            }

            int ping_val = -1;
            if (!ip_to_ping.empty()) {
                ping_val = DnsBenchmark::ping_dns_server(ip_to_ping, 800, true);
            }

            {
                std::lock_guard<std::mutex> lock(m_historyMutex);
                m_currentPingMs = ping_val;
                if (m_latencyHistory.size() >= 20) {
                    m_latencyHistory.erase(m_latencyHistory.begin());
                }
                m_latencyHistory.push_back(ping_val);
            }

            if (m_hWnd && IsWindow(m_hWnd)) {
                PostMessageW(m_hWnd, WM_APP_PING_UPDATED, 0, 0);
            }

            // Optimal ping update interval: 3.0 seconds (30 * 100ms)
            // 20 historical points = 60 seconds history window
            for (int i = 0; i < 30 && !m_stopPingWorker; ++i) {
                if (m_pingImmediate.exchange(false)) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    });
}

void MainWindow::stop_ping_worker() {
    m_stopPingWorker = true;
    m_pingImmediate = true;
    if (m_pingWorker.joinable()) {
        m_pingWorker.join();
    }
}

void MainWindow::trigger_immediate_ping() {
    m_pingImmediate = true;
}

void MainWindow::setup_tray_icon() {
    ZeroMemory(&m_nid, sizeof(m_nid));
    m_nid.cbSize = sizeof(NOTIFYICONDATAW);
    m_nid.hWnd = m_hWnd;
    m_nid.uID = 1;
    m_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_INFO;
    m_nid.uCallbackMessage = WM_APP_TRAYMSG;
    m_nid.hIcon = LoadIconW(m_hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
    if (!m_nid.hIcon) m_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(m_nid.szTip, L"QuickDNS v1.2.1 — Быстрая смена DNS");

    Shell_NotifyIconW(NIM_ADD, &m_nid);
}

void MainWindow::remove_tray_icon() {
    Shell_NotifyIconW(NIM_DELETE, &m_nid);
}

void MainWindow::show_tray_notification(const std::wstring& title, const std::wstring& message) {
    wcsncpy_s(m_nid.szInfoTitle, title.c_str(), _countof(m_nid.szInfoTitle) - 1);
    wcsncpy_s(m_nid.szInfo, message.c_str(), _countof(m_nid.szInfo) - 1);
    m_nid.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &m_nid);
}

void MainWindow::refresh_network_adapters() {
    std::wstring prevSelectedId;
    if (m_selectedAdapterIndex >= 0 && m_selectedAdapterIndex < static_cast<int>(m_adapters.size())) {
        prevSelectedId = m_adapters[m_selectedAdapterIndex].id;
    }

    m_adapters = m_dnsManager->get_network_adapters();
    ComboBox_ResetContent(m_hComboAdapter);

    int selectIndex = 0;
    for (size_t i = 0; i < m_adapters.size(); ++i) {
        const auto& adapter = m_adapters[i];
        std::wstring itemText = adapter.name + L" (" + adapter.description + L")";
        if (adapter.is_connected) {
            itemText += L" [Подключено]";
        }
        ComboBox_AddString(m_hComboAdapter, itemText.c_str());

        if (!prevSelectedId.empty() && adapter.id == prevSelectedId) {
            selectIndex = static_cast<int>(i);
        }
    }

    if (!m_adapters.empty()) {
        m_selectedAdapterIndex = selectIndex;
        ComboBox_SetCurSel(m_hComboAdapter, m_selectedAdapterIndex);
    } else {
        m_selectedAdapterIndex = -1;
    }
}

void MainWindow::populate_profiles_combobox() {
    std::string prevSelectedId = m_configManager->get_active_profile_id();
    m_profiles = m_configManager->get_all_profiles();

    ComboBox_ResetContent(m_hComboProfile);

    int selectIndex = 0;
    for (size_t i = 0; i < m_profiles.size(); ++i) {
        const auto& p = m_profiles[i];
        std::wstring pName = utf8_to_wide(p.name);
        ComboBox_AddString(m_hComboProfile, pName.c_str());

        if (p.id == prevSelectedId) {
            selectIndex = static_cast<int>(i);
        }
    }

    if (!m_profiles.empty()) {
        m_selectedProfileIndex = selectIndex;
        ComboBox_SetCurSel(m_hComboProfile, m_selectedProfileIndex);
        on_profile_selected();
    }
}

void MainWindow::update_status_card() {
    if (m_selectedAdapterIndex < 0 || m_selectedAdapterIndex >= static_cast<int>(m_adapters.size())) {
        SetWindowTextW(m_hStatusAdapterName, L"Подключение: Нет активного адаптера");
        SetWindowTextW(m_hStatusDnsList, L"Текущий DNS: —");
        SetWindowTextW(m_hStatusBadge, L"Статус: Сеть недоступна");
        SetWindowTextW(m_hStatusPing, L"Задержка: —");
        return;
    }

    const auto& adapter = m_adapters[m_selectedAdapterIndex];
    std::wstring nameText = L"Подключение: " + adapter.name + (adapter.is_connected ? L" (Подключено)" : L" (Отключено)");
    SetWindowTextW(m_hStatusAdapterName, nameText.c_str());

    std::wstring dnsText = L"Текущий DNS: ";
    if (adapter.dns_ipv4.empty()) {
        dnsText += L"Не настроен / DHCP";
    } else {
        for (size_t i = 0; i < adapter.dns_ipv4.size(); ++i) {
            if (i > 0) dnsText += L", ";
            dnsText += adapter.dns_ipv4[i];
        }
    }
    SetWindowTextW(m_hStatusDnsList, dnsText.c_str());

    if (adapter.is_dhcp_dns) {
        SetWindowTextW(m_hStatusBadge, L"Статус: Автоматический (DHCP от провайдера)");
    } else {
        std::wstring badge = L"Статус: Кастомный DNS";
        if (!adapter.dns_ipv4.empty()) {
            std::string pri = wide_to_utf8(adapter.dns_ipv4[0]);
            for (const auto& p : m_profiles) {
                if (p.primary_ipv4 == pri) {
                    std::wstring pName = utf8_to_wide(p.name);
                    badge = L"Статус: Активен " + pName;
                    break;
                }
            }
        }
        SetWindowTextW(m_hStatusBadge, badge.c_str());
    }

    trigger_immediate_ping();
}

void MainWindow::on_adapter_changed() {
    m_selectedAdapterIndex = ComboBox_GetCurSel(m_hComboAdapter);
    update_status_card();

    if (m_hBtnWinNetwork && m_selectedAdapterIndex >= 0 && m_selectedAdapterIndex < static_cast<int>(m_adapters.size())) {
        const auto& adapter = m_adapters[m_selectedAdapterIndex];
        std::wstring nameLower = adapter.name;
        std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::towlower);
        std::wstring descLower = adapter.description;
        std::transform(descLower.begin(), descLower.end(), descLower.begin(), ::towlower);

        if (nameLower.find(L"wi-fi") != std::wstring::npos || 
            nameLower.find(L"wifi") != std::wstring::npos ||
            nameLower.find(L"беспровод") != std::wstring::npos ||
            descLower.find(L"wireless") != std::wstring::npos ||
            descLower.find(L"wi-fi") != std::wstring::npos) {
            SetWindowTextW(m_hBtnWinNetwork, L"Параметры Wi-Fi (DNS)");
        } else {
            SetWindowTextW(m_hBtnWinNetwork, L"Параметры Ethernet (DNS)");
        }
    }
}

void MainWindow::on_profile_selected() {
    m_selectedProfileIndex = ComboBox_GetCurSel(m_hComboProfile);
    if (m_selectedProfileIndex >= 0 && m_selectedProfileIndex < static_cast<int>(m_profiles.size())) {
        const auto& p = m_profiles[m_selectedProfileIndex];
        std::wstring ipText;
        std::wstring dohText;
        std::wstring descText;

        if (p.is_dhcp) {
            ipText = L"Автоматически (DHCP)";
            dohText = L"";
            descText = L"Автоматическое получение DNS серверов от DHCP роутера/провайдера.";
        } else {
            std::wstring pri  = utf8_to_wide(p.primary_ipv4);
            std::wstring sec  = utf8_to_wide(p.secondary_ipv4);
            descText = utf8_to_wide(p.description);
            dohText  = utf8_to_wide(p.doh_template);
            ipText = pri + (sec.empty() ? L"" : L", " + sec);
        }

        if (m_hEditProfileIp) {
            SetWindowTextW(m_hEditProfileIp, ipText.c_str());
        }
        if (m_hEditProfileDoh) {
            SetWindowTextW(m_hEditProfileDoh, dohText.c_str());
        }
        if (m_hProfileDetails) {
            SetWindowTextW(m_hProfileDetails, descText.c_str());
        }

        if (m_hBtnDeleteProfile) {
            EnableWindow(m_hBtnDeleteProfile, !p.is_builtin);
        }
    }
}

void MainWindow::apply_selected_dns() {
    if (m_isBusy) return;
    if (m_selectedAdapterIndex < 0 || m_selectedAdapterIndex >= static_cast<int>(m_adapters.size())) {
        MessageBoxW(m_hWnd, L"Выберите сетевой адаптер для применения DNS.", L"Предупреждение", MB_OK | MB_ICONWARNING);
        return;
    }
    if (m_selectedProfileIndex < 0 || m_selectedProfileIndex >= static_cast<int>(m_profiles.size())) {
        MessageBoxW(m_hWnd, L"Выберите профиль DNS.", L"Предупреждение", MB_OK | MB_ICONWARNING);
        return;
    }

    m_isBusy = true;
    EnableWindow(m_hBtnApply, FALSE);
    EnableWindow(m_hBtnBenchmark, FALSE);
    EnableWindow(m_hBtnResetDhcp, FALSE);
    EnableWindow(m_hBtnFlushDns, FALSE);
    if (m_hBtnWinNetwork) EnableWindow(m_hBtnWinNetwork, FALSE);

    NetworkAdapterInfo adapter = m_adapters[m_selectedAdapterIndex];
    DnsProfile profile = m_profiles[m_selectedProfileIndex];
    HWND hWnd = m_hWnd;

    // Read manual / custom DoH template from edit control if specified
    std::wstring manualDoh;
    if (m_hEditProfileDoh) {
        wchar_t buf[512] = {0};
        GetWindowTextW(m_hEditProfileDoh, buf, 512);
        manualDoh = buf;
        while (!manualDoh.empty() && (manualDoh.back() == L' ' || manualDoh.back() == L'\r' || manualDoh.back() == L'\n' || manualDoh.back() == L'\t')) {
            manualDoh.pop_back();
        }
        while (!manualDoh.empty() && (manualDoh.front() == L' ' || manualDoh.front() == L'\t')) {
            manualDoh.erase(0, 1);
        }
    }

    std::thread([this, adapter, profile, manualDoh, hWnd]() {
        bool success = false;
        std::wstring msg;

        if (profile.is_dhcp) {
            success = m_dnsManager->reset_to_dhcp(adapter);
            msg = success ? L"Сетевой адаптер успешно переключен на DHCP." 
                          : L"Не удалось сбросить DNS на DHCP: " + m_dnsManager->get_last_error();
        } else {
            std::wstring pri = utf8_to_wide(profile.primary_ipv4);
            std::wstring sec = utf8_to_wide(profile.secondary_ipv4);
            std::wstring doh = manualDoh.empty() ? utf8_to_wide(profile.doh_template) : manualDoh;
            success = m_dnsManager->set_static_dns(adapter, pri, sec, doh);
            msg = success ? L"DNS серверы успешно установлены!" 
                          : L"Не удалось применить DNS: " + m_dnsManager->get_last_error();
        }

        if (success) {
            m_configManager->set_active_profile_id(profile.id);
            m_configManager->save_to_file();
            std::wstring profName = utf8_to_wide(profile.name);
            show_tray_notification(L"DNS обновлен", L"Профиль '" + profName + L"' успешно применен.");
        } else {
            MessageBoxW(hWnd, msg.c_str(), L"Ошибка", MB_OK | MB_ICONERROR);
        }

        PostMessageW(hWnd, WM_APP_DNS_APPLIED, 0, 0);
    }).detach();
}

void MainWindow::reset_to_dhcp() {
    if (m_isBusy) return;
    if (m_selectedAdapterIndex < 0 || m_selectedAdapterIndex >= static_cast<int>(m_adapters.size())) {
        return;
    }

    m_isBusy = true;
    EnableWindow(m_hBtnApply, FALSE);
    EnableWindow(m_hBtnBenchmark, FALSE);
    EnableWindow(m_hBtnResetDhcp, FALSE);
    EnableWindow(m_hBtnFlushDns, FALSE);
    if (m_hBtnWinNetwork) EnableWindow(m_hBtnWinNetwork, FALSE);

    NetworkAdapterInfo adapter = m_adapters[m_selectedAdapterIndex];
    HWND hWnd = m_hWnd;

    std::thread([this, adapter, hWnd]() {
        bool success = m_dnsManager->reset_to_dhcp(adapter);
        if (success) {
            m_configManager->set_active_profile_id("dhcp");
            m_configManager->save_to_file();
            show_tray_notification(L"Сброс DNS", L"Адаптер переключен на автоматический DNS (DHCP).");
        } else {
            std::wstring err = L"Не удалось сбросить DNS: " + m_dnsManager->get_last_error();
            MessageBoxW(hWnd, err.c_str(), L"Ошибка", MB_OK | MB_ICONERROR);
        }

        PostMessageW(hWnd, WM_APP_DNS_APPLIED, 0, 0);
    }).detach();
}

void MainWindow::flush_dns_cache() {
    bool ok = m_dnsManager->flush_dns_cache();
    if (ok) {
        show_tray_notification(L"Очистка кэша", L"Кэш сопоставителя DNS Windows успешно сброшен.");
        MessageBoxW(m_hWnd, L"Кэш DNS успешно очищен!", L"Информация", MB_OK | MB_ICONINFORMATION);
    } else {
        MessageBoxW(m_hWnd, L"Не удалось сбросить DNS кэш.", L"Предупреждение", MB_OK | MB_ICONWARNING);
    }
}

void MainWindow::open_add_profile_dialog() {
    auto newProfile = AddProfileDialog::show(m_hWnd, m_hInstance);
    if (newProfile.has_value()) {
        m_configManager->add_profile(newProfile.value());
        m_configManager->set_active_profile_id(newProfile.value().id);
        m_configManager->save_to_file();
        populate_profiles_combobox();
        update_status_card();
        show_tray_notification(L"Профиль добавлен", L"Пользовательский профиль успешно сохранен.");
    }
}

void MainWindow::open_benchmark_dialog() {
    auto selectedId = BenchmarkDialog::show(m_hWnd, m_hInstance, m_profiles, m_configManager->get_active_profile_id());
    if (selectedId.has_value()) {
        std::string chosenId = selectedId.value();
        for (size_t i = 0; i < m_profiles.size(); ++i) {
            if (m_profiles[i].id == chosenId) {
                m_selectedProfileIndex = static_cast<int>(i);
                ComboBox_SetCurSel(m_hComboProfile, m_selectedProfileIndex);
                on_profile_selected();
                apply_selected_dns();
                break;
            }
        }
    }
}

void MainWindow::open_windows_network_settings() {
    // Determine the precise settings page based on active adapter type
    std::wstring targetUri = L"ms-settings:network-ethernet";

    if (m_selectedAdapterIndex >= 0 && m_selectedAdapterIndex < static_cast<int>(m_adapters.size())) {
        const auto& adapter = m_adapters[m_selectedAdapterIndex];
        std::wstring nameLower = adapter.name;
        std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::towlower);
        std::wstring descLower = adapter.description;
        std::transform(descLower.begin(), descLower.end(), descLower.begin(), ::towlower);

        if (nameLower.find(L"wi-fi") != std::wstring::npos || 
            nameLower.find(L"wifi") != std::wstring::npos ||
            nameLower.find(L"беспровод") != std::wstring::npos ||
            descLower.find(L"wireless") != std::wstring::npos ||
            descLower.find(L"wi-fi") != std::wstring::npos) {
            targetUri = L"ms-settings:network-wifi";
        }
    }

    // 1. Open Windows 11 Modern Settings directly to Ethernet or Wi-Fi settings
    HINSTANCE res = ShellExecuteW(m_hWnd, L"open", targetUri.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(res) <= 32) {
        // Fallback 1: general network settings page
        HINSTANCE resNet = ShellExecuteW(m_hWnd, L"open", L"ms-settings:network", nullptr, nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(resNet) <= 32) {
            // Fallback 2: classic Network Connections control panel
            ShellExecuteW(m_hWnd, L"open", L"control.exe", L"ncpa.cpl", nullptr, SW_SHOWNORMAL);
            return;
        }
    }

    // 2. Automatically click the "Изменить" (Edit) DNS button in Settings via UI Automation,
    //    and if a DoH template is present, configure manual DoH template and save.
    std::wstring dohTemplate;
    if (m_hEditProfileDoh) {
        wchar_t buf[512] = {0};
        GetWindowTextW(m_hEditProfileDoh, buf, 512);
        dohTemplate = buf;
        while (!dohTemplate.empty() && (dohTemplate.back() == L' ' || dohTemplate.back() == L'\r' || dohTemplate.back() == L'\n' || dohTemplate.back() == L'\t')) dohTemplate.pop_back();
        while (!dohTemplate.empty() && (dohTemplate.front() == L' ' || dohTemplate.front() == L'\t')) dohTemplate.erase(0, 1);
    }
    if (dohTemplate.empty() && m_selectedProfileIndex >= 0 && m_selectedProfileIndex < static_cast<int>(m_profiles.size())) {
        dohTemplate = utf8_to_wide(m_profiles[m_selectedProfileIndex].doh_template);
    }

    std::thread([dohTemplate]() {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(hr)) return;

        IUIAutomation* pAutomation = nullptr;
        hr = CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_IUIAutomation, reinterpret_cast<void**>(&pAutomation));
        if (SUCCEEDED(hr) && pAutomation) {
            IUIAutomationElement* pRoot = nullptr;
            if (SUCCEEDED(pAutomation->GetRootElement(&pRoot)) && pRoot) {
                // Poll every 250ms for up to ~4 seconds for the XAML page to finish loading
                for (int attempt = 0; attempt < 16; ++attempt) {
                    Sleep(250);

                    IUIAutomationCondition* pTrueCond = nullptr;
                    pAutomation->CreateTrueCondition(&pTrueCond);
                    IUIAutomationElementArray* pWindows = nullptr;

                    if (SUCCEEDED(pRoot->FindAll(TreeScope_Children, pTrueCond, &pWindows)) && pWindows) {
                        int winCount = 0;
                        pWindows->get_Length(&winCount);

                        bool clicked = false;
                        for (int w = 0; w < winCount && !clicked; ++w) {
                            IUIAutomationElement* pWin = nullptr;
                            pWindows->GetElement(w, &pWin);
                            if (!pWin) continue;

                            BSTR bCls = nullptr;
                            BSTR bTitle = nullptr;
                            pWin->get_CurrentClassName(&bCls);
                            pWin->get_CurrentName(&bTitle);
                            std::wstring cls = bCls ? bCls : L"";
                            std::wstring title = bTitle ? bTitle : L"";
                            if (bCls) SysFreeString(bCls);
                            if (bTitle) SysFreeString(bTitle);

                            if (cls == L"ApplicationFrameWindow" || title.find(L"Настройки") != std::wstring::npos || title.find(L"Settings") != std::wstring::npos) {
                                IUIAutomationTreeWalker* pWalker = nullptr;
                                pAutomation->get_ControlViewWalker(&pWalker);

                                auto to_lower_str = [](const std::wstring& str) {
                                    std::wstring res = str;
                                    for (auto& c : res) {
                                        if (c >= L'A' && c <= L'Z') c = c + (L'a' - L'A');
                                        if (c >= L'А' && c <= L'Я') c = c + (L'а' - L'А');
                                        if (c == L'Ё') c = L'ё';
                                    }
                                    return res;
                                };

                                // Strategy 1: Find the specific DNS setting card / header text ("Назначение DNS-сервера" / "DNS server assignment")
                                VARIANT varText;
                                varText.vt = VT_I4;
                                varText.lVal = UIA_TextControlTypeId;
                                IUIAutomationCondition* pTextCond = nullptr;
                                if (SUCCEEDED(pAutomation->CreatePropertyCondition(UIA_ControlTypePropertyId, varText, &pTextCond)) && pTextCond) {
                                    IUIAutomationElementArray* pTexts = nullptr;
                                    if (SUCCEEDED(pWin->FindAll(TreeScope_Descendants, pTextCond, &pTexts)) && pTexts) {
                                        int tCount = 0;
                                        pTexts->get_Length(&tCount);
                                        for (int t = 0; t < tCount && !clicked; ++t) {
                                            IUIAutomationElement* pText = nullptr;
                                            pTexts->GetElement(t, &pText);
                                            if (!pText) continue;

                                            BSTR bTxt = nullptr;
                                            pText->get_CurrentName(&bTxt);
                                            std::wstring txt = bTxt ? to_lower_str(bTxt) : L"";
                                            if (bTxt) SysFreeString(bTxt);

                                            if (txt.find(L"dns") != std::wstring::npos && 
                                                txt.find(L"auth") == std::wstring::npos && 
                                                txt.find(L"подлинност") == std::wstring::npos) {
                                                
                                                // Walk up to find the enclosing SettingCard / container
                                                if (pWalker) {
                                                    IUIAutomationElement* pCard = pText;
                                                    pCard->AddRef();
                                                    for (int depth = 0; depth < 3 && !clicked; ++depth) {
                                                        IUIAutomationElement* pParent = nullptr;
                                                        if (SUCCEEDED(pWalker->GetParentElement(pCard, &pParent)) && pParent) {
                                                            // Search for button inside this card
                                                            VARIANT varBtn;
                                                            varBtn.vt = VT_I4;
                                                            varBtn.lVal = UIA_ButtonControlTypeId;
                                                            IUIAutomationCondition* pBtnCond = nullptr;
                                                            if (SUCCEEDED(pAutomation->CreatePropertyCondition(UIA_ControlTypePropertyId, varBtn, &pBtnCond)) && pBtnCond) {
                                                                IUIAutomationElementArray* pCardButtons = nullptr;
                                                                if (SUCCEEDED(pParent->FindAll(TreeScope_Descendants, pBtnCond, &pCardButtons)) && pCardButtons) {
                                                                    int bLen = 0;
                                                                    pCardButtons->get_Length(&bLen);
                                                                    for (int b = 0; b < bLen; ++b) {
                                                                        IUIAutomationElement* pBtn = nullptr;
                                                                        pCardButtons->GetElement(b, &pBtn);
                                                                        if (!pBtn) continue;

                                                                        BSTR bName = nullptr;
                                                                        BSTR bId = nullptr;
                                                                        pBtn->get_CurrentName(&bName);
                                                                        pBtn->get_CurrentAutomationId(&bId);
                                                                        std::wstring bname = bName ? to_lower_str(bName) : L"";
                                                                        std::wstring bid = bId ? to_lower_str(bId) : L"";
                                                                        if (bName) SysFreeString(bName);
                                                                        if (bId) SysFreeString(bId);

                                                                        if (bname.find(L"auth") == std::wstring::npos && 
                                                                            bname.find(L"подлинност") == std::wstring::npos &&
                                                                            bname != L"редактировать" &&
                                                                            bid.find(L"ip") == std::wstring::npos) {
                                                                            IUIAutomationInvokePattern* pInvoke = nullptr;
                                                                            if (SUCCEEDED(pBtn->GetCurrentPatternAs(UIA_InvokePatternId, IID_IUIAutomationInvokePattern, reinterpret_cast<void**>(&pInvoke))) && pInvoke) {
                                                                                pInvoke->Invoke();
                                                                                pInvoke->Release();
                                                                                clicked = true;
                                                                                pBtn->Release();
                                                                                break;
                                                                            }
                                                                        }
                                                                        pBtn->Release();
                                                                    }
                                                                    pCardButtons->Release();
                                                                }
                                                                pBtnCond->Release();
                                                            }
                                                            pCard->Release();
                                                            pCard = pParent;
                                                        } else {
                                                            pCard->Release();
                                                            break;
                                                        }
                                                    }
                                                }
                                            }
                                            pText->Release();
                                        }
                                        pTexts->Release();
                                    }
                                    pTextCond->Release();
                                }

                                // Strategy 2: If Strategy 1 didn't trigger, inspect all buttons with strict DNS validation
                                if (!clicked) {
                                    VARIANT varBtn;
                                    varBtn.vt = VT_I4;
                                    varBtn.lVal = UIA_ButtonControlTypeId;
                                    IUIAutomationCondition* pBtnCond = nullptr;
                                    if (SUCCEEDED(pAutomation->CreatePropertyCondition(UIA_ControlTypePropertyId, varBtn, &pBtnCond)) && pBtnCond) {
                                        IUIAutomationElementArray* pButtons = nullptr;
                                        if (SUCCEEDED(pWin->FindAll(TreeScope_Descendants, pBtnCond, &pButtons)) && pButtons) {
                                            int btnCount = 0;
                                            pButtons->get_Length(&btnCount);

                                            for (int b = 0; b < btnCount && !clicked; ++b) {
                                                IUIAutomationElement* pButton = nullptr;
                                                pButtons->GetElement(b, &pButton);
                                                if (!pButton) continue;

                                                BSTR bBtnName = nullptr;
                                                BSTR bAutoId = nullptr;
                                                BSTR bHelp = nullptr;
                                                pButton->get_CurrentName(&bBtnName);
                                                pButton->get_CurrentAutomationId(&bAutoId);
                                                pButton->get_CurrentHelpText(&bHelp);
                                                std::wstring btnName = bBtnName ? to_lower_str(bBtnName) : L"";
                                                std::wstring btnAutoId = bAutoId ? to_lower_str(bAutoId) : L"";
                                                std::wstring btnHelp = bHelp ? to_lower_str(bHelp) : L"";
                                                if (bBtnName) SysFreeString(bBtnName);
                                                if (bAutoId) SysFreeString(bAutoId);
                                                if (bHelp) SysFreeString(bHelp);

                                                bool isReject = (btnName.find(L"подлинност") != std::wstring::npos ||
                                                                 btnName.find(L"auth") != std::wstring::npos ||
                                                                 btnAutoId.find(L"auth") != std::wstring::npos ||
                                                                 btnAutoId.find(L"802") != std::wstring::npos ||
                                                                 btnAutoId.find(L"eap") != std::wstring::npos ||
                                                                 btnName == L"редактировать" ||
                                                                 btnName.find(L"ip") != std::wstring::npos ||
                                                                 btnAutoId.find(L"ip") != std::wstring::npos);

                                                if (!isReject) {
                                                    bool isDns = false;
                                                    if (btnAutoId.find(L"dns") != std::wstring::npos ||
                                                        btnName.find(L"dns") != std::wstring::npos ||
                                                        btnHelp.find(L"dns") != std::wstring::npos) {
                                                        isDns = true;
                                                    } else if (pWalker && (btnName == L"изменить" || btnName == L"edit")) {
                                                        // Check ancestors to confirm it's DNS and NOT Authentication or IP
                                                        IUIAutomationElement* pCurr = pButton;
                                                        pCurr->AddRef();
                                                        for (int depth = 0; depth < 5 && !isDns && !isReject; ++depth) {
                                                            IUIAutomationElement* pParent = nullptr;
                                                            if (SUCCEEDED(pWalker->GetParentElement(pCurr, &pParent)) && pParent) {
                                                                BSTR pName = nullptr;
                                                                BSTR pId = nullptr;
                                                                pParent->get_CurrentName(&pName);
                                                                pParent->get_CurrentAutomationId(&pId);
                                                                std::wstring pn = pName ? to_lower_str(pName) : L"";
                                                                std::wstring pi = pId ? to_lower_str(pId) : L"";
                                                                if (pName) SysFreeString(pName);
                                                                if (pId) SysFreeString(pId);

                                                                if (pn.find(L"подлинност") != std::wstring::npos ||
                                                                    pn.find(L"auth") != std::wstring::npos ||
                                                                    pi.find(L"auth") != std::wstring::npos) {
                                                                    isReject = true;
                                                                } else if (pn.find(L"dns") != std::wstring::npos || pi.find(L"dns") != std::wstring::npos) {
                                                                    isDns = true;
                                                                }
                                                                pCurr->Release();
                                                                pCurr = pParent;
                                                            } else {
                                                                pCurr->Release();
                                                                break;
                                                            }
                                                        }
                                                    }

                                                    if (!isReject && isDns) {
                                                        IUIAutomationInvokePattern* pInvoke = nullptr;
                                                        if (SUCCEEDED(pButton->GetCurrentPatternAs(UIA_InvokePatternId, IID_IUIAutomationInvokePattern, reinterpret_cast<void**>(&pInvoke))) && pInvoke) {
                                                            pInvoke->Invoke();
                                                            pInvoke->Release();
                                                            clicked = true;
                                                            pButton->Release();
                                                            break;
                                                        }
                                                    }
                                                }
                                                pButton->Release();
                                            }
                                            pButtons->Release();
                                        }
                                        pBtnCond->Release();
                                    }
                                }

                                if (pWalker) pWalker->Release();

                                // If the DNS Edit button was clicked, automate the ContentDialog "Изменить настройки DNS"
                                if (clicked && !dohTemplate.empty()) {
                                    bool dialogHandled = false;
                                    for (int dAttempt = 0; dAttempt < 15 && !dialogHandled; ++dAttempt) {
                                        Sleep(250);

                                        VARIANT varCombo;
                                        varCombo.vt = VT_I4;
                                        varCombo.lVal = UIA_ComboBoxControlTypeId;
                                        IUIAutomationCondition* pComboCond = nullptr;
                                        if (SUCCEEDED(pAutomation->CreatePropertyCondition(UIA_ControlTypePropertyId, varCombo, &pComboCond)) && pComboCond) {
                                            IUIAutomationElementArray* pCombos = nullptr;
                                            if (SUCCEEDED(pWin->FindAll(TreeScope_Descendants, pComboCond, &pCombos)) && pCombos) {
                                                int cCount = 0;
                                                pCombos->get_Length(&cCount);

                                                if (cCount > 0) {
                                                    for (int c = 0; c < cCount; ++c) {
                                                        IUIAutomationElement* pCombo = nullptr;
                                                        pCombos->GetElement(c, &pCombo);
                                                        if (!pCombo) continue;

                                                        IUIAutomationExpandCollapsePattern* pExp = nullptr;
                                                        if (SUCCEEDED(pCombo->GetCurrentPatternAs(UIA_ExpandCollapsePatternId, IID_IUIAutomationExpandCollapsePattern, reinterpret_cast<void**>(&pExp))) && pExp) {
                                                            pExp->Expand();
                                                            Sleep(100);

                                                            IUIAutomationCondition* pTrue = nullptr;
                                                            pAutomation->CreateTrueCondition(&pTrue);
                                                            IUIAutomationElementArray* pItems = nullptr;
                                                            if (SUCCEEDED(pCombo->FindAll(TreeScope_Descendants, pTrue, &pItems)) && pItems) {
                                                                int itCount = 0;
                                                                pItems->get_Length(&itCount);
                                                                for (int it = 0; it < itCount; ++it) {
                                                                    IUIAutomationElement* pItem = nullptr;
                                                                    pItems->GetElement(it, &pItem);
                                                                    if (pItem) {
                                                                        BSTR bItemName = nullptr;
                                                                        pItem->get_CurrentName(&bItemName);
                                                                        std::wstring iName = bItemName ? to_lower_str(bItemName) : L"";
                                                                        if (bItemName) SysFreeString(bItemName);

                                                                        if (iName.find(L"ручной") != std::wstring::npos || iName.find(L"manual") != std::wstring::npos) {
                                                                            IUIAutomationSelectionItemPattern* pSel = nullptr;
                                                                            if (SUCCEEDED(pItem->GetCurrentPatternAs(UIA_SelectionItemPatternId, IID_IUIAutomationSelectionItemPattern, reinterpret_cast<void**>(&pSel))) && pSel) {
                                                                                pSel->Select();
                                                                                pSel->Release();
                                                                            }
                                                                            pItem->Release();
                                                                            break;
                                                                        }
                                                                        pItem->Release();
                                                                    }
                                                                }
                                                                pItems->Release();
                                                            }
                                                            if (pTrue) pTrue->Release();
                                                            pExp->Collapse();
                                                            pExp->Release();
                                                        }
                                                        pCombo->Release();
                                                    }

                                                    Sleep(200);

                                                    VARIANT varEdit;
                                                    varEdit.vt = VT_I4;
                                                    varEdit.lVal = UIA_EditControlTypeId;
                                                    IUIAutomationCondition* pEditCond = nullptr;
                                                    if (SUCCEEDED(pAutomation->CreatePropertyCondition(UIA_ControlTypePropertyId, varEdit, &pEditCond)) && pEditCond) {
                                                        IUIAutomationElementArray* pEdits = nullptr;
                                                        if (SUCCEEDED(pWin->FindAll(TreeScope_Descendants, pEditCond, &pEdits)) && pEdits) {
                                                            int eCount = 0;
                                                            pEdits->get_Length(&eCount);
                                                            for (int e = 0; e < eCount; ++e) {
                                                                IUIAutomationElement* pEdit = nullptr;
                                                                pEdits->GetElement(e, &pEdit);
                                                                if (!pEdit) continue;

                                                                BSTR bEdName = nullptr;
                                                                BSTR bEdId = nullptr;
                                                                pEdit->get_CurrentName(&bEdName);
                                                                pEdit->get_CurrentAutomationId(&bEdId);
                                                                std::wstring edName = bEdName ? to_lower_str(bEdName) : L"";
                                                                std::wstring edId = bEdId ? to_lower_str(bEdId) : L"";
                                                                if (bEdName) SysFreeString(bEdName);
                                                                if (bEdId) SysFreeString(bEdId);

                                                                if (edName.find(L"шаблон") != std::wstring::npos || 
                                                                    edName.find(L"template") != std::wstring::npos ||
                                                                    edId.find(L"template") != std::wstring::npos ||
                                                                    edId.find(L"doh") != std::wstring::npos) {
                                                                    IUIAutomationValuePattern* pVal = nullptr;
                                                                    if (SUCCEEDED(pEdit->GetCurrentPatternAs(UIA_ValuePatternId, IID_IUIAutomationValuePattern, reinterpret_cast<void**>(&pVal))) && pVal) {
                                                                        BSTR bDohVal = SysAllocString(dohTemplate.c_str());
                                                                        if (bDohVal) {
                                                                            pVal->SetValue(bDohVal);
                                                                            SysFreeString(bDohVal);
                                                                        }
                                                                        pVal->Release();
                                                                    }
                                                                }
                                                                pEdit->Release();
                                                            }
                                                            pEdits->Release();
                                                        }
                                                        pEditCond->Release();
                                                    }

                                                    Sleep(200);

                                                    VARIANT varBtnSave;
                                                    varBtnSave.vt = VT_I4;
                                                    varBtnSave.lVal = UIA_ButtonControlTypeId;
                                                    IUIAutomationCondition* pSaveCond = nullptr;
                                                    if (SUCCEEDED(pAutomation->CreatePropertyCondition(UIA_ControlTypePropertyId, varBtnSave, &pSaveCond)) && pSaveCond) {
                                                        IUIAutomationElementArray* pSaveBtns = nullptr;
                                                        if (SUCCEEDED(pWin->FindAll(TreeScope_Descendants, pSaveCond, &pSaveBtns)) && pSaveBtns) {
                                                            int sCount = 0;
                                                            pSaveBtns->get_Length(&sCount);
                                                            for (int s = 0; s < sCount; ++s) {
                                                                IUIAutomationElement* pSaveBtn = nullptr;
                                                                pSaveBtns->GetElement(s, &pSaveBtn);
                                                                if (!pSaveBtn) continue;

                                                                BSTR bSName = nullptr;
                                                                pSaveBtn->get_CurrentName(&bSName);
                                                                std::wstring sName = bSName ? to_lower_str(bSName) : L"";
                                                                if (bSName) SysFreeString(bSName);

                                                                if (sName == L"сохранить" || sName == L"save") {
                                                                    IUIAutomationInvokePattern* pSaveInv = nullptr;
                                                                    if (SUCCEEDED(pSaveBtn->GetCurrentPatternAs(UIA_InvokePatternId, IID_IUIAutomationInvokePattern, reinterpret_cast<void**>(&pSaveInv))) && pSaveInv) {
                                                                        pSaveInv->Invoke();
                                                                        pSaveInv->Release();
                                                                        dialogHandled = true;
                                                                        pSaveBtn->Release();
                                                                        break;
                                                                    }
                                                                }
                                                                pSaveBtn->Release();
                                                            }
                                                            pSaveBtns->Release();
                                                        }
                                                        pSaveCond->Release();
                                                    }
                                                }
                                                pCombos->Release();
                                            }
                                            pComboCond->Release();
                                        }
                                    }
                                }
                            }
                            pWin->Release();
                            if (clicked) break;
                        }
                        pWindows->Release();
                        if (clicked) {
                            if (pTrueCond) pTrueCond->Release();
                            break;
                        }
                    }
                    if (pTrueCond) pTrueCond->Release();
                }
                pRoot->Release();
            }
            pAutomation->Release();
        }
        CoUninitialize();
    }).detach();
}

void MainWindow::copy_text_to_clipboard(const std::wstring& text) {
    if (text.empty()) return;
    if (!OpenClipboard(m_hWnd)) return;
    EmptyClipboard();

    size_t bytes = (text.length() + 1) * sizeof(wchar_t);
    HGLOBAL hGlob = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (hGlob) {
        void* pMem = GlobalLock(hGlob);
        if (pMem) {
            memcpy(pMem, text.c_str(), bytes);
            GlobalUnlock(hGlob);
            SetClipboardData(CF_UNICODETEXT, hGlob);
        }
    }
    CloseClipboard();
}

void MainWindow::show_tray_context_menu() {
    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) return;

    InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_STRING, IDM_TRAY_SHOW, L"Открыть QuickDNS");
    InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);

    // List presets
    std::string activeId = m_configManager->get_active_profile_id();
    for (size_t i = 0; i < m_profiles.size() && i < 50; ++i) {
        const auto& p = m_profiles[i];
        std::wstring pName = utf8_to_wide(p.name);
        UINT flags = MF_BYPOSITION | MF_STRING;
        if (p.id == activeId) {
            flags |= MF_CHECKED;
        }
        InsertMenuW(hMenu, -1, flags, IDM_TRAY_PRESET_FIRST + i, pName.c_str());
    }

    InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
    InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_STRING, IDM_TRAY_RESET_DHCP, L"Сброс на DHCP");
    InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_STRING, IDM_TRAY_FLUSH_DNS, L"Сбросить DNS-кэш");
    InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
    InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_STRING, IDM_TRAY_EXIT, L"Выход");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(m_hWnd);
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, m_hWnd, nullptr);
    PostMessageW(m_hWnd, WM_NULL, 0, 0);
    DestroyMenu(hMenu);
}

void MainWindow::on_tray_preset_selected(int commandId) {
    size_t index = commandId - IDM_TRAY_PRESET_FIRST;
    if (index < m_profiles.size()) {
        m_selectedProfileIndex = static_cast<int>(index);
        ComboBox_SetCurSel(m_hComboProfile, m_selectedProfileIndex);
        on_profile_selected();
        apply_selected_dns();
    }
}
