#include "ui_main.h"
#include "dialog_add_profile.h"
#include "dialog_benchmark.h"
#include "dialog_smartdns.h"
#include "smartdns_verifier.h"
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

#include "ui_palette.h"

namespace {
    constexpr const wchar_t* WINDOW_CLASS_NAME = L"QuickDNS_MainWindowClass";
    constexpr const wchar_t* GRAPH_CLASS_NAME  = L"QuickDNS_SparklineGraph";
    constexpr const wchar_t* WINDOW_TITLE      = L"QuickDNS v2.0.0 — Быстрая смена DNS в 1 клик";

    constexpr int WINDOW_CLIENT_WIDTH  = 580;
    constexpr int WINDOW_CLIENT_HEIGHT = 760;

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

    struct ComboAnimationState {
        MainWindow* pMain{nullptr};
        bool isHovered{false};
    };

    LRESULT CALLBACK CustomComboSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
                                             UINT_PTR uIdSubclass, DWORD_PTR dwRefData) {
        auto* state = reinterpret_cast<ComboAnimationState*>(dwRefData);
        if (!state || !state->pMain) {
            return DefSubclassProc(hWnd, uMsg, wParam, lParam);
        }

        switch (uMsg) {
        case WM_SETCURSOR: {
            if (LOWORD(lParam) == HTCLIENT && IsWindowEnabled(hWnd)) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            break;
        }
        case WM_MOUSEMOVE: {
            if (!state->isHovered) {
                state->isHovered = true;
                TRACKMOUSEEVENT tme = { sizeof(TRACKMOUSEEVENT), TME_LEAVE, hWnd, 0 };
                TrackMouseEvent(&tme);
                InvalidateRect(hWnd, nullptr, FALSE);
            }
            break;
        }
        case WM_MOUSELEAVE: {
            state->isHovered = false;
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        }
        case WM_SETFOCUS:
        case WM_KILLFOCUS: {
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        }
        case WM_ERASEBKGND: {
            return 1;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);

            RECT rc;
            GetClientRect(hWnd, &rc);
            int w = rc.right - rc.left;
            int h = rc.bottom - rc.top;

            HDC hdcMem = CreateCompatibleDC(hdc);
            HBITMAP hbmMem = CreateCompatibleBitmap(hdc, w, h);
            HGDIOBJ oldBmp = SelectObject(hdcMem, hbmMem);

            bool isDark = state->pMain->is_dark_mode();
            auto pal = GetPalette(isDark);

            {
                Gdiplus::Graphics g(hdcMem);
                g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

                // Clear memory buffer with parent window background color so rounded edges blend
                g.Clear(Gdiplus::Color(255, GetRValue(pal.bg), GetGValue(pal.bg), GetBValue(pal.bg)));

                float rW = static_cast<float>(w) - 1.0f;
                float rH = static_cast<float>(h) - 1.0f;
                Gdiplus::GraphicsPath boxPath;
                AddRoundedRect(boxPath, Gdiplus::RectF(0.5f, 0.5f, rW, rH), 6.0f);

                Gdiplus::SolidBrush bgBrush(Gdiplus::Color(255, GetRValue(pal.cardBg), GetGValue(pal.cardBg), GetBValue(pal.cardBg)));
                g.FillPath(&bgBrush, &boxPath);

                bool hasFocus = (GetFocus() == hWnd);
                COLORREF borderCol = (state->isHovered || hasFocus) 
                    ? (isDark ? RGB(96, 165, 250) : RGB(37, 99, 235)) 
                    : pal.cardBorder;
                float borderWidth = (state->isHovered || hasFocus) ? 1.5f : 1.0f;
                Gdiplus::Pen borderPen(Gdiplus::Color(255, GetRValue(borderCol), GetGValue(borderCol), GetBValue(borderCol)), borderWidth);
                g.DrawPath(&borderPen, &boxPath);

                // Modern downward chevron arrow
                Gdiplus::Pen arrowPen(Gdiplus::Color(255, GetRValue(pal.textSecondary), GetGValue(pal.textSecondary), GetBValue(pal.textSecondary)), 1.8f);
                arrowPen.SetStartCap(Gdiplus::LineCapRound);
                arrowPen.SetEndCap(Gdiplus::LineCapRound);
                arrowPen.SetLineJoin(Gdiplus::LineJoinRound);

                float ax = static_cast<float>(w) - 18.0f;
                float ay = static_cast<float>(h) / 2.0f - 1.5f;
                Gdiplus::PointF arrowPts[3] = {
                    Gdiplus::PointF(ax - 4.5f, ay),
                    Gdiplus::PointF(ax, ay + 4.5f),
                    Gdiplus::PointF(ax + 4.5f, ay)
                };
                g.DrawLines(&arrowPen, arrowPts, 3);
            }

            // Draw selected item text
            int curSel = (int)SendMessageW(hWnd, CB_GETCURSEL, 0, 0);
            std::wstring itemText;
            if (curSel != CB_ERR) {
                int len = (int)SendMessageW(hWnd, CB_GETLBTEXTLEN, curSel, 0);
                if (len > 0) {
                    itemText.resize(len);
                    SendMessageW(hWnd, CB_GETLBTEXT, curSel, reinterpret_cast<LPARAM>(&itemText[0]));
                }
            }

            SetBkMode(hdcMem, TRANSPARENT);
            SetTextColor(hdcMem, pal.textPrimary);
            HFONT hFont = state->pMain->get_font_regular();
            HGDIOBJ oldFont = SelectObject(hdcMem, hFont);

            RECT rcText = rc;
            rcText.left += 12;
            rcText.right -= 30; // Leave room for chevron arrow
            DrawTextW(hdcMem, itemText.c_str(), -1, &rcText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            SelectObject(hdcMem, oldFont);

            BitBlt(hdc, 0, 0, w, h, hdcMem, 0, 0, SRCCOPY);

            SelectObject(hdcMem, oldBmp);
            DeleteObject(hbmMem);
            DeleteDC(hdcMem);

            EndPaint(hWnd, &ps);
            return 0;
        }
        case WM_NCDESTROY: {
            RemovePropW(hWnd, L"QuickDnsComboState");
            delete state;
            RemoveWindowSubclass(hWnd, CustomComboSubclassProc, uIdSubclass);
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

    DWORD dwStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_THICKFRAME;
    RECT wr = { 0, 0, WINDOW_CLIENT_WIDTH, WINDOW_CLIENT_HEIGHT };
    AdjustWindowRectEx(&wr, dwStyle, FALSE, 0);
    int winWidth = wr.right - wr.left;
    int winHeight = wr.bottom - wr.top;

    // Center window on screen
    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    int posX = (screenWidth - winWidth) / 2;
    int posY = (screenHeight - winHeight) / 2;

    m_hWnd = CreateWindowExW(
        0,
        WINDOW_CLASS_NAME,
        WINDOW_TITLE,
        dwStyle,
        posX, posY,
        winWidth, winHeight,
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

    // Enable inter-process single-instance communication
    m_msgRestoreInstance = RegisterWindowMessageW(L"QuickDNS_RestoreExistingInstanceMessage");
    if (m_msgRestoreInstance != 0) {
        ChangeWindowMessageFilterEx(m_hWnd, m_msgRestoreInstance, MSGFLT_ALLOW, nullptr);
        ChangeWindowMessageFilter(m_msgRestoreInstance, MSGFLT_ADD);
    }

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
    if (uMsg != 0 && uMsg == m_msgRestoreInstance) {
        restore_from_tray();
        return 0;
    }

    switch (uMsg) {
    case WM_GETMINMAXINFO: {
        auto* pMMI = reinterpret_cast<MINMAXINFO*>(lParam);
        RECT rcMin = { 0, 0, 520, 720 };
        AdjustWindowRectEx(&rcMin, GetWindowLongW(hWnd, GWL_STYLE), FALSE, GetWindowLongW(hWnd, GWL_EXSTYLE));
        pMMI->ptMinTrackSize.x = rcMin.right - rcMin.left;
        pMMI->ptMinTrackSize.y = rcMin.bottom - rcMin.top;
        return 0;
    }

    case WM_SIZE: {
        if (wParam != SIZE_MINIMIZED) {
            reposition_controls();
        }
        return 0;
    }

    case WM_APP_RESTORE_INSTANCE: {
        restore_from_tray();
        return 0;
    }

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
        if (wmId == IDC_BTN_SMARTDNS_CATALOG) {
            open_smartdns_catalog_dialog();
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
            restore_from_tray();
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
        if (wmId == IDM_TRAY_VERIFY_SMARTDNS) {
            std::thread([this]() {
                auto profiles = m_configManager->get_all_profiles();
                auto results = quickdns::SmartDnsVerifier::verify_profiles(profiles);
                int updated = quickdns::SmartDnsVerifier::apply_best_ips(*m_configManager, results);
                PostMessageW(m_hWnd, WM_APP_REFRESH_STATUS, 0, static_cast<LPARAM>(updated));
            }).detach();
            show_tray_notification(L"QuickDNS", L"Запущена проверка и обновление серверов SmartDNS...");
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
            restore_from_tray();
        } else if (lParam == WM_RBUTTONUP) {
            show_tray_context_menu();
        }
        return 0;
    }

    case WM_APP_DNS_APPLIED: {
        m_isBusy = false;
        EnableWindow(m_hBtnApply, TRUE);
        EnableWindow(m_hBtnBenchmark, TRUE);
        if (m_hBtnSmartDnsCatalog) EnableWindow(m_hBtnSmartDnsCatalog, TRUE);
        EnableWindow(m_hBtnResetDhcp, TRUE);
        EnableWindow(m_hBtnFlushDns, TRUE);
        if (m_hBtnWinNetwork) EnableWindow(m_hBtnWinNetwork, TRUE);

        refresh_network_adapters();
        update_status_card();
        trigger_immediate_ping();
        return 0;
    }

    case WM_APP_REFRESH_STATUS: {
        m_profiles = m_configManager->get_all_profiles();
        populate_profiles_combobox();
        refresh_network_adapters();
        update_status_card();
        trigger_immediate_ping();
        if (lParam > 0) {
            std::wstring msg = L"Обновлены IP-адреса для " + std::to_wstring(lParam) + L" провайдеров SmartDNS.";
            show_tray_notification(L"QuickDNS", msg);
        }
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

    case WM_MEASUREITEM: {
        auto* pMis = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
        if (pMis && pMis->CtlType == ODT_COMBOBOX) {
            pMis->itemHeight = 24;
            return TRUE;
        }
        break;
    }

    case WM_DRAWITEM: {
        auto* pDis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
        if (pDis) {
            if (pDis->CtlType == ODT_BUTTON) {
                if (pDis->CtlID == IDC_BTN_THEME_TOGGLE) {
                    draw_theme_toggle_button(pDis);
                } else {
                    draw_custom_button(pDis);
                }
                return TRUE;
            } else if (pDis->CtlType == ODT_COMBOBOX) {
                draw_combobox_item(pDis);
                return TRUE;
            }
        }
        break;
    }

    case WM_CTLCOLORLISTBOX: {
        HDC hdcList = (HDC)wParam;
        auto pal = GetPalette(m_isDarkMode);
        SetBkMode(hdcList, OPAQUE);
        SetTextColor(hdcList, pal.textPrimary);
        SetBkColor(hdcList, pal.cardBg);
        return (INT_PTR)m_hbrCard;
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

        RECT rcClient = {};
        GetClientRect(hWnd, &rcClient);
        int clientWidth = rcClient.right - rcClient.left;
        float marginX = 20.0f;
        float cardWidth = static_cast<float>(clientWidth) - (2.0f * marginX);

        // Draw background elevated card for Status area with anti-aliasing
        {
            Gdiplus::Graphics g(hdc);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

            Gdiplus::GraphicsPath cardPath;
            AddRoundedRect(cardPath, Gdiplus::RectF(marginX + 0.5f, 44.5f, cardWidth - 1.0f, 114.0f - 1.0f), 12.0f);

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
        remove_tray_icon();
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

    if (m_hComboAdapter) InvalidateRect(m_hComboAdapter, nullptr, TRUE);
    if (m_hComboProfile) InvalidateRect(m_hComboProfile, nullptr, TRUE);
    if (m_hGraphWnd) InvalidateRect(m_hGraphWnd, nullptr, TRUE);

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

void MainWindow::attach_combobox_effects(HWND hCombo, UINT_PTR subclassId) {
    if (!hCombo) return;
    auto* state = new ComboAnimationState();
    state->pMain = this;
    SetPropW(hCombo, L"QuickDnsComboState", reinterpret_cast<HANDLE>(state));
    SetWindowSubclass(hCombo, CustomComboSubclassProc, subclassId, reinterpret_cast<DWORD_PTR>(state));
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
    RECT rcClient = {};
    GetClientRect(m_hWnd, &rcClient);
    int clientWidth = rcClient.right - rcClient.left;
    int marginX = 20;
    int contentWidth = clientWidth - (marginX * 2);
    int startY = 14;

    // 1. Status Card Header
    m_hTitleLabel = CreateWindowExW(0, L"STATIC", L"QuickDNS v2.0.0 — Менеджер DNS",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        marginX, startY, contentWidth - 44, 24,
        m_hWnd, nullptr, m_hInstance, nullptr);
    SendMessageW(m_hTitleLabel, WM_SETFONT, (WPARAM)m_hFontHeader, TRUE);

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
    // Right side of Card: Minimalist Sparkline Graph Control!
    int graphW = 145;
    int graphH = 94;
    int graphX = marginX + contentWidth - graphW - 10;
    int leftInfoW = graphX - (marginX + 14) - 8;
    int insideCardY = startY + 10;

    m_hStatusAdapterName = CreateWindowExW(0, L"STATIC", L"Подключение: Определение сети...",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS,
        marginX + 14, insideCardY, leftInfoW, 20,
        m_hWnd, (HMENU)IDC_STATUS_TITLE, m_hInstance, nullptr);
    SendMessageW(m_hStatusAdapterName, WM_SETFONT, (WPARAM)m_hFontBold, TRUE);
    insideCardY += 24;

    m_hStatusDnsList = CreateWindowExW(0, L"STATIC", L"Текущий DNS: Загрузка...",
        WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
        marginX + 14, insideCardY, leftInfoW, 20,
        m_hWnd, (HMENU)IDC_STATUS_DNS_VAL, m_hInstance, nullptr);
    SendMessageW(m_hStatusDnsList, WM_SETFONT, (WPARAM)m_hFontRegular, TRUE);
    insideCardY += 24;

    m_hStatusBadge = CreateWindowExW(0, L"STATIC", L"Статус: Проверка...",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS,
        marginX + 14, insideCardY, leftInfoW, 20,
        m_hWnd, nullptr, m_hInstance, nullptr);
    SendMessageW(m_hStatusBadge, WM_SETFONT, (WPARAM)m_hFontRegular, TRUE);
    insideCardY += 24;

    m_hStatusPing = CreateWindowExW(0, L"STATIC", L"Задержка: Ожидание...",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        marginX + 14, insideCardY, leftInfoW, 20,
        m_hWnd, (HMENU)IDC_STATUS_PING, m_hInstance, nullptr);
    SendMessageW(m_hStatusPing, WM_SETFONT, (WPARAM)m_hFontBold, TRUE);

    int graphY = startY + 10;

    m_hGraphWnd = CreateWindowExW(
        0, GRAPH_CLASS_NAME, L"",
        WS_CHILD | WS_VISIBLE,
        graphX, graphY, graphW, graphH,
        m_hWnd, (HMENU)IDC_GRAPH_VIEW, m_hInstance, this
    );

    startY += 114 + 14; // Past Card -> 172

    // Separator Line
    m_hSepLine = CreateWindowExW(0, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
        marginX, startY, contentWidth, 2,
        m_hWnd, nullptr, m_hInstance, nullptr);
    startY += 14;

    // 2. Network Adapter Selector
    m_hLblAdapter = CreateWindowExW(0, L"STATIC", L"Сетевой адаптер:",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        marginX, startY, contentWidth, 18,
        m_hWnd, nullptr, m_hInstance, nullptr);
    SendMessageW(m_hLblAdapter, WM_SETFONT, (WPARAM)m_hFontRegular, TRUE);
    startY += 24;

    m_hComboAdapter = CreateWindowExW(0, WC_COMBOBOXW, L"",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP,
        marginX, startY, contentWidth, 240,
        m_hWnd, (HMENU)IDC_COMBO_ADAPTER, m_hInstance, nullptr);
    SendMessageW(m_hComboAdapter, WM_SETFONT, (WPARAM)m_hFontRegular, TRUE);
    SendMessageW(m_hComboAdapter, CB_SETITEMHEIGHT, (WPARAM)-1, 30);
    SendMessageW(m_hComboAdapter, CB_SETITEMHEIGHT, (WPARAM)0, 24);
    attach_combobox_effects(m_hComboAdapter, 201);
    startY += 46;

    // 3. DNS Profile Selector
    m_hLblProfile = CreateWindowExW(0, L"STATIC", L"Выберите DNS профиль (Comss, GeoHide, Без межаў и др.):",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        marginX, startY, contentWidth, 18,
        m_hWnd, nullptr, m_hInstance, nullptr);
    SendMessageW(m_hLblProfile, WM_SETFONT, (WPARAM)m_hFontRegular, TRUE);
    startY += 24;

    m_hComboProfile = CreateWindowExW(0, WC_COMBOBOXW, L"",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP,
        marginX, startY, contentWidth, 240,
        m_hWnd, (HMENU)IDC_COMBO_PROFILE, m_hInstance, nullptr);
    SendMessageW(m_hComboProfile, WM_SETFONT, (WPARAM)m_hFontRegular, TRUE);
    SendMessageW(m_hComboProfile, CB_SETITEMHEIGHT, (WPARAM)-1, 30);
    SendMessageW(m_hComboProfile, CB_SETITEMHEIGHT, (WPARAM)0, 24);
    attach_combobox_effects(m_hComboProfile, 202);
    startY += 48;

    int editW = contentWidth - 116;
    int btnCopyW = 106;
    int copyX = marginX + editW + 10;

    // IP Address (Copyable)
    m_hLblIp = CreateWindowExW(0, L"STATIC", L"IP-адреса DNS серверов:",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        marginX, startY, contentWidth, 16,
        m_hWnd, nullptr, m_hInstance, nullptr);
    SendMessageW(m_hLblIp, WM_SETFONT, (WPARAM)m_hFontSmall, TRUE);
    startY += 22;

    m_hEditProfileIp = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_READONLY,
        marginX, startY, editW, 26,
        m_hWnd, (HMENU)IDC_EDIT_PROFILE_IP, m_hInstance, nullptr);
    SendMessageW(m_hEditProfileIp, WM_SETFONT, (WPARAM)m_hFontMonospace, TRUE);

    m_hBtnCopyIp = CreateWindowExW(0, WC_BUTTONW, L"Копировать IP",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        copyX, startY, btnCopyW, 26,
        m_hWnd, (HMENU)IDC_BTN_COPY_IP, m_hInstance, nullptr);
    startY += 40;

    // DoH Template (Copyable, Read-Only)
    m_hLblDoh = CreateWindowExW(0, L"STATIC", L"Шаблон DNS over HTTPS (DoH):",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        marginX, startY, contentWidth, 16,
        m_hWnd, nullptr, m_hInstance, nullptr);
    SendMessageW(m_hLblDoh, WM_SETFONT, (WPARAM)m_hFontSmall, TRUE);
    startY += 22;

    m_hEditProfileDoh = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_READONLY,
        marginX, startY, editW, 26,
        m_hWnd, (HMENU)IDC_EDIT_PROFILE_DOH, m_hInstance, nullptr);
    SendMessageW(m_hEditProfileDoh, WM_SETFONT, (WPARAM)m_hFontRegular, TRUE);

    m_hBtnCopyDoh = CreateWindowExW(0, WC_BUTTONW, L"Копировать DoH",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        copyX, startY, btnCopyW, 26,
        m_hWnd, (HMENU)IDC_BTN_COPY_DOH, m_hInstance, nullptr);
    startY += 38;

    // Profile Description
    m_hProfileDetails = CreateWindowExW(0, L"STATIC", L"Описание выбранного профиля...",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        marginX, startY, contentWidth, 24,
        m_hWnd, nullptr, m_hInstance, nullptr);
    SendMessageW(m_hProfileDetails, WM_SETFONT, (WPARAM)m_hFontSmall, TRUE);
    startY += 40;

    // 4. Action Buttons with comfortable neutral spacing
    m_hBtnApply = CreateWindowExW(0, WC_BUTTONW, L"Применить DNS в 1 клик",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        marginX, startY, contentWidth, 38,
        m_hWnd, (HMENU)IDC_BTN_APPLY, m_hInstance, nullptr);
    startY += 38 + 10;

    int halfWidth = (contentWidth - 12) / 2;
    m_hBtnBenchmark = CreateWindowExW(0, WC_BUTTONW, L"Тест скорости DNS (пинг)",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        marginX, startY, halfWidth, 34,
        m_hWnd, (HMENU)IDC_BTN_BENCHMARK, m_hInstance, nullptr);

    m_hBtnSmartDnsCatalog = CreateWindowExW(0, WC_BUTTONW, L"Каталог SmartDNS",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        marginX + halfWidth + 12, startY, halfWidth, 34,
        m_hWnd, (HMENU)IDC_BTN_SMARTDNS_CATALOG, m_hInstance, nullptr);
    startY += 34 + 10;

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
    startY += 30 + 14;

    // 5. Footer Version Badge
    m_hFooterVersion = CreateWindowExW(0, L"STATIC", L"QuickDNS v2.0.0 • Portable",
        WS_CHILD | WS_VISIBLE | SS_CENTER,
        marginX, startY, contentWidth, 18,
        m_hWnd, nullptr, m_hInstance, nullptr);
    SendMessageW(m_hFooterVersion, WM_SETFONT, (WPARAM)m_hFontRegular, TRUE);

    // Attach smooth animations, hover effects and hand cursor (IDC_HAND) to all buttons
    attach_button_effects(m_hBtnApply, 1);
    attach_button_effects(m_hBtnBenchmark, 2);
    attach_button_effects(m_hBtnSmartDnsCatalog, 11);
    attach_button_effects(m_hBtnResetDhcp, 3);
    attach_button_effects(m_hBtnWinNetwork, 4);
    attach_button_effects(m_hBtnFlushDns, 5);
    attach_button_effects(m_hBtnAddProfile, 6);
    attach_button_effects(m_hBtnDeleteProfile, 7);
    attach_button_effects(m_hBtnCopyIp, 8);
    attach_button_effects(m_hBtnCopyDoh, 9);
    attach_button_effects(m_hBtnThemeToggle, 10);
}

void MainWindow::reposition_controls() {
    RECT rcClient = {};
    GetClientRect(m_hWnd, &rcClient);
    int clientWidth = rcClient.right - rcClient.left;
    int clientHeight = rcClient.bottom - rcClient.top;

    int marginX = 20;
    int contentWidth = clientWidth - (marginX * 2);
    if (contentWidth < 200) return;

    int startY = 14;

    // 1. Status Card Header
    if (m_hTitleLabel) {
        SetWindowPos(m_hTitleLabel, nullptr, marginX, startY, contentWidth - 44, 24, SWP_NOZORDER);
    }
    if (m_hBtnThemeToggle) {
        SetWindowPos(m_hBtnThemeToggle, nullptr, marginX + contentWidth - 36, startY - 3, 36, 28, SWP_NOZORDER);
    }
    startY += 30; // 44

    // Inside Status Card:
    int graphW = std::clamp(contentWidth / 3, 140, 260);
    int graphH = 94;
    int graphX = marginX + contentWidth - graphW - 10;
    int leftInfoW = graphX - (marginX + 14) - 8;
    int insideCardY = startY + 10;

    if (m_hStatusAdapterName) SetWindowPos(m_hStatusAdapterName, nullptr, marginX + 14, insideCardY, leftInfoW, 20, SWP_NOZORDER);
    insideCardY += 24;
    if (m_hStatusDnsList) SetWindowPos(m_hStatusDnsList, nullptr, marginX + 14, insideCardY, leftInfoW, 20, SWP_NOZORDER);
    insideCardY += 24;
    if (m_hStatusBadge) SetWindowPos(m_hStatusBadge, nullptr, marginX + 14, insideCardY, leftInfoW, 20, SWP_NOZORDER);
    insideCardY += 24;
    if (m_hStatusPing) SetWindowPos(m_hStatusPing, nullptr, marginX + 14, insideCardY, leftInfoW, 20, SWP_NOZORDER);

    if (m_hGraphWnd) {
        SetWindowPos(m_hGraphWnd, nullptr, graphX, startY + 10, graphW, graphH, SWP_NOZORDER);
        InvalidateRect(m_hGraphWnd, nullptr, TRUE);
    }

    startY += 114 + 14; // 172

    // Separator line
    if (m_hSepLine) SetWindowPos(m_hSepLine, nullptr, marginX, startY, contentWidth, 2, SWP_NOZORDER);
    startY += 14;

    // 2. Network Adapter Selector
    if (m_hLblAdapter) SetWindowPos(m_hLblAdapter, nullptr, marginX, startY, contentWidth, 18, SWP_NOZORDER);
    startY += 24;
    if (m_hComboAdapter) SetWindowPos(m_hComboAdapter, nullptr, marginX, startY, contentWidth, 240, SWP_NOZORDER);
    startY += 46;

    // 3. DNS Profile Selector
    if (m_hLblProfile) SetWindowPos(m_hLblProfile, nullptr, marginX, startY, contentWidth, 18, SWP_NOZORDER);
    startY += 24;
    if (m_hComboProfile) SetWindowPos(m_hComboProfile, nullptr, marginX, startY, contentWidth, 240, SWP_NOZORDER);
    startY += 48;

    int btnCopyW = 120;
    int editW = std::max(100, contentWidth - btnCopyW - 10);
    int copyX = marginX + editW + 10;

    // IP Address (Copyable)
    if (m_hLblIp) SetWindowPos(m_hLblIp, nullptr, marginX, startY, contentWidth, 16, SWP_NOZORDER);
    startY += 22;
    if (m_hEditProfileIp) SetWindowPos(m_hEditProfileIp, nullptr, marginX, startY, editW, 26, SWP_NOZORDER);
    if (m_hBtnCopyIp) SetWindowPos(m_hBtnCopyIp, nullptr, copyX, startY, btnCopyW, 26, SWP_NOZORDER);
    startY += 40;

    // DoH Template (Copyable, Read-Only)
    if (m_hLblDoh) SetWindowPos(m_hLblDoh, nullptr, marginX, startY, contentWidth, 16, SWP_NOZORDER);
    startY += 22;
    if (m_hEditProfileDoh) SetWindowPos(m_hEditProfileDoh, nullptr, marginX, startY, editW, 26, SWP_NOZORDER);
    if (m_hBtnCopyDoh) SetWindowPos(m_hBtnCopyDoh, nullptr, copyX, startY, btnCopyW, 26, SWP_NOZORDER);
    startY += 38;

    // Profile Details
    if (m_hProfileDetails) SetWindowPos(m_hProfileDetails, nullptr, marginX, startY, contentWidth, 24, SWP_NOZORDER);
    startY += 40;

    // 4. Action Buttons
    if (m_hBtnApply) SetWindowPos(m_hBtnApply, nullptr, marginX, startY, contentWidth, 38, SWP_NOZORDER);
    startY += 38 + 10;

    int halfWidth = (contentWidth - 12) / 2;
    int col2X = marginX + halfWidth + 12;

    if (m_hBtnBenchmark) SetWindowPos(m_hBtnBenchmark, nullptr, marginX, startY, halfWidth, 34, SWP_NOZORDER);
    if (m_hBtnSmartDnsCatalog) SetWindowPos(m_hBtnSmartDnsCatalog, nullptr, col2X, startY, halfWidth, 34, SWP_NOZORDER);
    startY += 34 + 10;

    if (m_hBtnResetDhcp) SetWindowPos(m_hBtnResetDhcp, nullptr, marginX, startY, halfWidth, 34, SWP_NOZORDER);
    if (m_hBtnWinNetwork) SetWindowPos(m_hBtnWinNetwork, nullptr, col2X, startY, halfWidth, 34, SWP_NOZORDER);
    startY += 34 + 10;

    if (m_hBtnFlushDns) SetWindowPos(m_hBtnFlushDns, nullptr, marginX, startY, halfWidth, 34, SWP_NOZORDER);
    if (m_hBtnAddProfile) SetWindowPos(m_hBtnAddProfile, nullptr, col2X, startY, halfWidth, 34, SWP_NOZORDER);
    startY += 34 + 10;

    if (m_hBtnDeleteProfile) SetWindowPos(m_hBtnDeleteProfile, nullptr, marginX, startY, contentWidth, 30, SWP_NOZORDER);
    startY += 30 + 14;

    // 5. Footer Version Badge
    if (m_hFooterVersion) SetWindowPos(m_hFooterVersion, nullptr, marginX, startY, contentWidth, 18, SWP_NOZORDER);

    InvalidateRect(m_hWnd, nullptr, TRUE);
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

void MainWindow::draw_combobox_item(DRAWITEMSTRUCT* pDis) {
    if (!pDis || pDis->itemID == static_cast<UINT>(-1)) return;

    HDC hdc = pDis->hDC;
    RECT rc = pDis->rcItem;
    auto pal = GetPalette(m_isDarkMode);

    bool isSelected = (pDis->itemState & ODS_SELECTED) != 0;

    COLORREF bgCol = isSelected 
        ? (m_isDarkMode ? RGB(45, 65, 95) : RGB(219, 234, 254)) 
        : pal.cardBg;
    COLORREF textCol = isSelected 
        ? (m_isDarkMode ? RGB(255, 255, 255) : RGB(30, 58, 138)) 
        : pal.textPrimary;

    HBRUSH hbr = CreateSolidBrush(bgCol);
    FillRect(hdc, &rc, hbr);
    DeleteObject(hbr);

    int textLen = ComboBox_GetLBTextLen(pDis->hwndItem, pDis->itemID);
    std::wstring text;
    if (textLen > 0) {
        text.resize(textLen);
        ComboBox_GetLBText(pDis->hwndItem, pDis->itemID, &text[0]);
    }

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, textCol);
    HGDIOBJ oldFont = SelectObject(hdc, m_hFontRegular);

    RECT rcText = rc;
    rcText.left += 10;
    rcText.right -= 10;

    bool isSmart = (pDis->hwndItem == m_hComboProfile && pDis->itemID < m_profiles.size() && m_profiles[pDis->itemID].is_smart_dns);
    if (isSmart) {
        rcText.right -= 75;
    }

    DrawTextW(hdc, text.c_str(), -1, &rcText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

    if (isSmart) {
        SelectObject(hdc, m_hFontSmall);
        COLORREF smartCol = isSelected 
            ? (m_isDarkMode ? RGB(110, 231, 183) : RGB(22, 101, 52))
            : (m_isDarkMode ? RGB(52, 211, 153) : RGB(21, 128, 61));
        SetTextColor(hdc, smartCol);
        RECT rcBadge = rc;
        rcBadge.right -= 12;
        DrawTextW(hdc, L"SmartDNS", -1, &rcBadge, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }

    SelectObject(hdc, oldFont);
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

        // Pre-fill entire buffer with parent card background color so rounded corners blend seamlessly
        g.Clear(Gdiplus::Color(255, GetRValue(pal.cardBg), GetGValue(pal.cardBg), GetBValue(pal.cardBg)));

        // 1. Draw smooth container background
        Gdiplus::GraphicsPath contPath;
        AddRoundedRect(contPath, Gdiplus::RectF(0.5f, 0.5f, static_cast<float>(width) - 1.0f, static_cast<float>(height) - 1.0f), 10.0f);
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
    wcscpy_s(m_nid.szTip, L"QuickDNS v2.0.0 — Быстрая смена DNS");

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

void MainWindow::restore_from_tray() {
    m_inTray = false;
    ShowWindow(m_hWnd, SW_RESTORE);
    UpdateWindow(m_hWnd);

    HWND hPopup = GetLastActivePopup(m_hWnd);
    if (hPopup && hPopup != m_hWnd && IsWindow(hPopup)) {
        BringWindowToTop(hPopup);
        SetForegroundWindow(hPopup);
    } else {
        BringWindowToTop(m_hWnd);
        SetForegroundWindow(m_hWnd);
    }
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
            std::string adPri = wide_to_utf8(adapter.dns_ipv4[0]);
            std::string adSec = adapter.dns_ipv4.size() > 1 ? wide_to_utf8(adapter.dns_ipv4[1]) : "";

            const DnsProfile* matchedProfile = nullptr;

            // 1. Exact match (primary == adPri and secondary matches or is empty)
            for (const auto& p : m_profiles) {
                if (p.is_dhcp) continue;
                if (p.primary_ipv4 == adPri && (adSec.empty() || p.secondary_ipv4 == adSec || p.secondary_ipv4.empty())) {
                    matchedProfile = &p;
                    break;
                }
            }

            // 2. Primary matches adPri
            if (!matchedProfile) {
                for (const auto& p : m_profiles) {
                    if (p.is_dhcp) continue;
                    if (p.primary_ipv4 == adPri) {
                        matchedProfile = &p;
                        break;
                    }
                }
            }

            // 3. Any IP match in adapter.dns_ipv4
            if (!matchedProfile) {
                for (const auto& adIpWide : adapter.dns_ipv4) {
                    std::string adIp = wide_to_utf8(adIpWide);
                    for (const auto& p : m_profiles) {
                        if (p.is_dhcp) continue;
                        if (p.primary_ipv4 == adIp || (!p.secondary_ipv4.empty() && p.secondary_ipv4 == adIp)) {
                            matchedProfile = &p;
                            break;
                        }
                    }
                    if (matchedProfile) break;
                }
            }

            // 4. Check active_profile_id from config as a fallback if any IP is associated
            if (!matchedProfile && m_configManager) {
                std::string activeId = m_configManager->get_active_profile_id();
                auto optP = m_configManager->get_profile_by_id(activeId);
                if (optP && !optP->is_dhcp) {
                    for (const auto& adIpWide : adapter.dns_ipv4) {
                        std::string adIp = wide_to_utf8(adIpWide);
                        if (optP->primary_ipv4 == adIp || optP->secondary_ipv4 == adIp) {
                            matchedProfile = &(*optP);
                            break;
                        }
                    }
                }
            }

            if (matchedProfile) {
                std::wstring pName = utf8_to_wide(matchedProfile->name);
                badge = L"Статус: Активен " + pName;
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
            if (p.is_smart_dns) {
                descText = L"[SmartDNS] " + utf8_to_wide(p.description);
                if (!p.unblocked_summary.empty()) {
                    descText += L" • Разблокирует: " + utf8_to_wide(p.unblocked_summary);
                }
            } else {
                descText = utf8_to_wide(p.description);
            }
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
    if (m_hBtnSmartDnsCatalog) EnableWindow(m_hBtnSmartDnsCatalog, FALSE);
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
    auto newProfile = AddProfileDialog::show(m_hWnd, m_hInstance, m_isDarkMode);
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
    auto selectedId = BenchmarkDialog::show(m_hWnd, m_hInstance, m_profiles, m_configManager->get_active_profile_id(), m_isDarkMode);
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

void MainWindow::open_smartdns_catalog_dialog() {
    auto selectedId = SmartDnsDialog::show(m_hWnd, m_hInstance, m_profiles, m_configManager->get_active_profile_id(), m_isDarkMode, m_configManager.get());
    m_profiles = m_configManager->get_all_profiles();
    populate_profiles_combobox();
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
    } else {
        if (m_selectedProfileIndex >= 0 && m_selectedProfileIndex < static_cast<int>(m_profiles.size())) {
            on_profile_selected();
        }
        update_status_card();
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
        if (p.is_smart_dns) {
            pName += L" [SmartDNS]";
        }
        UINT flags = MF_BYPOSITION | MF_STRING;
        if (p.id == activeId) {
            flags |= MF_CHECKED;
        }
        InsertMenuW(hMenu, -1, flags, IDM_TRAY_PRESET_FIRST + i, pName.c_str());
    }

    InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
    InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_STRING, IDM_TRAY_RESET_DHCP, L"Сброс на DHCP");
    InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_STRING, IDM_TRAY_FLUSH_DNS, L"Сбросить DNS-кэш");
    InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_STRING, IDM_TRAY_VERIFY_SMARTDNS, L"Проверить и обновить SmartDNS...");
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
