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

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "comctl32.lib")

using QuickDns::Encoding::utf8_to_wide;
using QuickDns::Encoding::wide_to_utf8;
using namespace quickdns;

namespace {
    constexpr const wchar_t* WINDOW_CLASS_NAME = L"QuickDNS_MainWindowClass";
    constexpr const wchar_t* GRAPH_CLASS_NAME  = L"QuickDNS_SparklineGraph";
    constexpr const wchar_t* WINDOW_TITLE      = L"QuickDNS v1.2.1 — Быстрая смена DNS в 1 клик";

    constexpr int WINDOW_WIDTH  = 540;
    constexpr int WINDOW_HEIGHT = 650;

    // Modern Deep Slate & Accent Color Palette
    constexpr COLORREF COLOR_BG          = RGB(18, 20, 26);   // Deep Midnight Slate
    constexpr COLORREF COLOR_CARD_BG     = RGB(27, 31, 41);   // Elevated Slate-Blue Card
    constexpr COLORREF COLOR_CARD_BORDER = RGB(45, 53, 70);   // Subtle Card Border

    constexpr COLORREF COLOR_TEXT_PRIMARY   = RGB(248, 250, 252);
    constexpr COLORREF COLOR_TEXT_SECONDARY = RGB(148, 163, 184);
    constexpr COLORREF COLOR_TEXT_MUTED     = RGB(100, 116, 139);

    constexpr COLORREF COLOR_ACCENT_BLUE    = RGB(56, 189, 248);  // Vibrant Sky Cyan
    constexpr COLORREF COLOR_ACCENT_GREEN   = RGB(52, 211, 153);  // Emerald Mint
    constexpr COLORREF COLOR_ACCENT_AMBER   = RGB(251, 191, 36);  // Warm Amber
    constexpr COLORREF COLOR_ACCENT_RED     = RGB(248, 113, 113); // Coral Red
}

MainWindow::MainWindow()
    : m_dnsManager(std::make_unique<DnsManager>())
    , m_configManager(std::make_unique<ConfigManager>())
{
    m_hbrBg = CreateSolidBrush(COLOR_BG);
    m_hbrCard = CreateSolidBrush(COLOR_CARD_BG);
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
            draw_custom_button(pDis);
            return TRUE;
        }
        break;
    }

    case WM_CTLCOLORSTATIC: {
        HDC hdcStatic = (HDC)wParam;
        HWND hwndStatic = (HWND)lParam;
        SetBkMode(hdcStatic, TRANSPARENT);

        if (hwndStatic == m_hFooterVersion) {
            SetTextColor(hdcStatic, COLOR_TEXT_MUTED);
        } else if (hwndStatic == m_hStatusDnsList) {
            SetTextColor(hdcStatic, COLOR_ACCENT_BLUE);
        } else if (hwndStatic == m_hStatusBadge) {
            SetTextColor(hdcStatic, COLOR_ACCENT_GREEN);
        } else if (hwndStatic == m_hStatusPing) {
            if (m_currentPingMs > 0 && m_currentPingMs < 60) {
                SetTextColor(hdcStatic, RGB(74, 222, 128)); // Bright Green
            } else if (m_currentPingMs >= 60 && m_currentPingMs <= 150) {
                SetTextColor(hdcStatic, COLOR_ACCENT_AMBER);
            } else if (m_currentPingMs > 150 || m_currentPingMs < 0) {
                SetTextColor(hdcStatic, COLOR_ACCENT_RED);
            } else {
                SetTextColor(hdcStatic, COLOR_TEXT_SECONDARY);
            }
        } else if (hwndStatic == m_hProfileDetails) {
            SetTextColor(hdcStatic, RGB(203, 213, 225));
        } else if (hwndStatic == m_hStatusAdapterName) {
            SetTextColor(hdcStatic, COLOR_TEXT_PRIMARY);
        } else {
            SetTextColor(hdcStatic, COLOR_TEXT_SECONDARY);
        }

        if (hwndStatic == m_hStatusAdapterName || hwndStatic == m_hStatusDnsList || 
            hwndStatic == m_hStatusBadge || hwndStatic == m_hStatusPing) {
            return (INT_PTR)m_hbrCard;
        }

        return (INT_PTR)m_hbrBg;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);

        // Draw background elevated card for Status area
        RECT cardRect = { 20, 44, 20 + (WINDOW_WIDTH - 40), 44 + 114 };
        HGDIOBJ oldBrush = SelectObject(hdc, m_hbrCard);
        HPEN hPenCard = CreatePen(PS_SOLID, 1, COLOR_CARD_BORDER);
        HGDIOBJ oldPen = SelectObject(hdc, hPenCard);

        RoundRect(hdc, cardRect.left, cardRect.top, cardRect.right, cardRect.bottom, 12, 12);

        SelectObject(hdc, oldPen);
        DeleteObject(hPenCard);
        SelectObject(hdc, oldBrush);

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
    BOOL useDarkMode = TRUE;
    DwmSetWindowAttribute(m_hWnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &useDarkMode, sizeof(useDarkMode));

    // Windows 11 Rounded Corners
    DWORD cornerPref = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(m_hWnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &cornerPref, sizeof(cornerPref));
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
        marginX, startY, contentWidth, 24,
        m_hWnd, nullptr, m_hInstance, nullptr);
    SendMessageW(hTitle, WM_SETFONT, (WPARAM)m_hFontHeader, TRUE);
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
    startY += 36;

    // Profile Details Card
    m_hProfileDetails = CreateWindowExW(0, L"STATIC", L"Описание выбранного профиля...",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        marginX, startY, contentWidth, 58,
        m_hWnd, nullptr, m_hInstance, nullptr);
    SendMessageW(m_hProfileDetails, WM_SETFONT, (WPARAM)m_hFontRegular, TRUE);
    startY += 64;

    // 4. Action Buttons (All Owner-Drawn for colorful, modern, rounded appearance)
    m_hBtnApply = CreateWindowExW(0, WC_BUTTONW, L"Применить DNS в 1 клик",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        marginX, startY, contentWidth, 42,
        m_hWnd, (HMENU)IDC_BTN_APPLY, m_hInstance, nullptr);
    startY += 48;

    m_hBtnBenchmark = CreateWindowExW(0, WC_BUTTONW, L"Проверить скорость всех DNS (пинг)",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        marginX, startY, contentWidth, 36,
        m_hWnd, (HMENU)IDC_BTN_BENCHMARK, m_hInstance, nullptr);
    startY += 42;

    int halfWidth = (contentWidth - 12) / 2;
    m_hBtnResetDhcp = CreateWindowExW(0, WC_BUTTONW, L"Сброс на DHCP (Авто)",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        marginX, startY, halfWidth, 34,
        m_hWnd, (HMENU)IDC_BTN_RESET_DHCP, m_hInstance, nullptr);

    m_hBtnFlushDns = CreateWindowExW(0, WC_BUTTONW, L"Сбросить DNS-кэш",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        marginX + halfWidth + 12, startY, halfWidth, 34,
        m_hWnd, (HMENU)IDC_BTN_FLUSH_DNS, m_hInstance, nullptr);
    startY += 40;

    m_hBtnAddProfile = CreateWindowExW(0, WC_BUTTONW, L"Добавить свой DNS...",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        marginX, startY, halfWidth, 34,
        m_hWnd, (HMENU)IDC_BTN_ADD_PROFILE, m_hInstance, nullptr);

    m_hBtnDeleteProfile = CreateWindowExW(0, WC_BUTTONW, L"Удалить профиль",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        marginX + halfWidth + 12, startY, halfWidth, 34,
        m_hWnd, (HMENU)IDC_BTN_DELETE_PROFILE, m_hInstance, nullptr);
    EnableWindow(m_hBtnDeleteProfile, FALSE);

    // 5. Footer Version Badge
    startY += 42;
    m_hFooterVersion = CreateWindowExW(0, L"STATIC", L"QuickDNS v1.2.1 • Portable",
        WS_CHILD | WS_VISIBLE | SS_CENTER,
        marginX, startY, contentWidth, 18,
        m_hWnd, nullptr, m_hInstance, nullptr);
    SendMessageW(m_hFooterVersion, WM_SETFONT, (WPARAM)m_hFontRegular, TRUE);
}

void MainWindow::draw_custom_button(DRAWITEMSTRUCT* pDis) {
    if (!pDis) return;

    HDC hdc = pDis->hDC;
    RECT rc = pDis->rcItem;
    bool isPressed  = (pDis->itemState & ODS_SELECTED) != 0;
    bool isDisabled = (pDis->itemState & ODS_DISABLED) != 0;

    COLORREF bgCol, borderCol, textCol;

    if (pDis->CtlID == IDC_BTN_APPLY) {
        // Primary Blue Action Button
        if (isDisabled) {
            bgCol = RGB(30, 45, 75);
            borderCol = RGB(45, 60, 95);
            textCol = RGB(120, 140, 175);
        } else if (isPressed) {
            bgCol = RGB(29, 78, 216);    // Blue 700
            borderCol = RGB(37, 99, 235);
            textCol = RGB(255, 255, 255);
        } else {
            bgCol = RGB(37, 99, 235);    // Blue 600
            borderCol = RGB(96, 165, 250); // Blue 400
            textCol = RGB(255, 255, 255);
        }
    } else if (pDis->CtlID == IDC_BTN_BENCHMARK) {
        // Indigo / Violet Speed Test Button
        if (isDisabled) {
            bgCol = RGB(45, 35, 75);
            borderCol = RGB(65, 50, 105);
            textCol = RGB(130, 120, 160);
        } else if (isPressed) {
            bgCol = RGB(67, 56, 202);    // Indigo 700
            borderCol = RGB(79, 70, 229);
            textCol = RGB(255, 255, 255);
        } else {
            bgCol = RGB(79, 70, 229);    // Indigo 600
            borderCol = RGB(129, 140, 248); // Indigo 400
            textCol = RGB(255, 255, 255);
        }
    } else {
        // Utility Secondary Buttons
        if (isDisabled) {
            bgCol = RGB(24, 27, 36);
            borderCol = RGB(38, 43, 56);
            textCol = RGB(85, 95, 115);
        } else if (isPressed) {
            bgCol = RGB(42, 48, 64);
            borderCol = RGB(70, 80, 105);
            textCol = RGB(255, 255, 255);
        } else {
            bgCol = RGB(32, 36, 48);
            if (pDis->CtlID == IDC_BTN_RESET_DHCP) {
                borderCol = RGB(217, 119, 6);   // Amber accent
            } else if (pDis->CtlID == IDC_BTN_FLUSH_DNS) {
                borderCol = RGB(14, 165, 233);  // Sky Blue accent
            } else if (pDis->CtlID == IDC_BTN_ADD_PROFILE) {
                borderCol = RGB(16, 185, 129);  // Emerald Green accent
            } else if (pDis->CtlID == IDC_BTN_DELETE_PROFILE) {
                borderCol = RGB(239, 68, 68);   // Rose Red accent
            } else {
                borderCol = RGB(55, 65, 85);
            }
            textCol = RGB(241, 245, 249);
        }
    }

    // Double buffer for clean flicker-free button paint
    HDC hdcMem = CreateCompatibleDC(hdc);
    HBITMAP hbmMem = CreateCompatibleBitmap(hdc, rc.right - rc.left, rc.bottom - rc.top);
    HGDIOBJ oldBmp = SelectObject(hdcMem, hbmMem);

    RECT rcMem = { 0, 0, rc.right - rc.left, rc.bottom - rc.top };

    // Fill background for smooth corner blend
    HBRUSH hbrWin = CreateSolidBrush(COLOR_BG);
    FillRect(hdcMem, &rcMem, hbrWin);
    DeleteObject(hbrWin);

    // Draw rounded button surface
    HBRUSH hbrBtn = CreateSolidBrush(bgCol);
    HPEN hPenBtn  = CreatePen(PS_SOLID, 1, borderCol);
    HGDIOBJ oldBrush = SelectObject(hdcMem, hbrBtn);
    HGDIOBJ oldPen   = SelectObject(hdcMem, hPenBtn);

    RoundRect(hdcMem, rcMem.left, rcMem.top, rcMem.right, rcMem.bottom, 8, 8);

    SelectObject(hdcMem, oldBrush);
    SelectObject(hdcMem, oldPen);
    DeleteObject(hbrBtn);
    DeleteObject(hPenBtn);

    // Get Button text
    wchar_t btnText[128] = {};
    GetWindowTextW(pDis->hwndItem, btnText, _countof(btnText));

    SetBkMode(hdcMem, TRANSPARENT);
    SetTextColor(hdcMem, textCol);
    HGDIOBJ oldFont = SelectObject(hdcMem, (pDis->CtlID == IDC_BTN_APPLY || pDis->CtlID == IDC_BTN_BENCHMARK) ? m_hFontBold : m_hFontRegular);

    DrawTextW(hdcMem, btnText, -1, &rcMem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    SelectObject(hdcMem, oldFont);

    BitBlt(hdc, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, hdcMem, 0, 0, SRCCOPY);

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

    // Background sparkline container
    HBRUSH hbrContainer = CreateSolidBrush(RGB(20, 24, 32));
    HPEN hPenBorder     = CreatePen(PS_SOLID, 1, RGB(45, 54, 72));
    HGDIOBJ oldBrush    = SelectObject(hdcMem, hbrContainer);
    HGDIOBJ oldPen      = SelectObject(hdcMem, hPenBorder);

    RoundRect(hdcMem, 0, 0, width, height, 8, 8);

    SelectObject(hdcMem, oldBrush);
    SelectObject(hdcMem, oldPen);
    DeleteObject(hbrContainer);
    DeleteObject(hPenBorder);

    // Top-left header: "Пинг (60 с)"
    SetBkMode(hdcMem, TRANSPARENT);
    SetTextColor(hdcMem, RGB(148, 163, 184));
    HGDIOBJ oldFont = SelectObject(hdcMem, m_hFontSmall);

    RECT rcTitle = { 10, 6, width - 10, 20 };
    DrawTextW(hdcMem, L"Пинг (60 с)", -1, &rcTitle, DT_LEFT | DT_TOP | DT_SINGLELINE);

    // Copy latency history safely under lock
    std::vector<int> history;
    int currentPing = -1;
    {
        std::lock_guard<std::mutex> lock(m_historyMutex);
        history = m_latencyHistory;
        currentPing = m_currentPingMs;
    }

    // Grid coordinates
    int plotTop    = 24;
    int plotBottom = height - 12;
    int plotLeft   = 10;
    int plotRight  = width - 10;
    int plotHeight = plotBottom - plotTop;
    int plotWidth  = plotRight - plotLeft;

    // Determine scale: max latency, minimum 80ms, max 300ms
    int maxMs = 80;
    for (int v : history) {
        if (v > maxMs) maxMs = std::min(300, v + 20);
    }

    // Faint grid guide lines at 50ms and 100ms
    HPEN hPenGrid = CreatePen(PS_DOT, 1, RGB(35, 42, 58));
    HGDIOBJ oldGridPen = SelectObject(hdcMem, hPenGrid);

    if (maxMs >= 50) {
        int y50 = plotBottom - (50 * plotHeight / maxMs);
        MoveToEx(hdcMem, plotLeft, y50, nullptr);
        LineTo(hdcMem, plotRight, y50);
    }
    if (maxMs >= 100) {
        int y100 = plotBottom - (100 * plotHeight / maxMs);
        MoveToEx(hdcMem, plotLeft, y100, nullptr);
        LineTo(hdcMem, plotRight, y100);
    }

    SelectObject(hdcMem, oldGridPen);
    DeleteObject(hPenGrid);

    // Plot latency line & area fill if points exist
    if (!history.empty() && history.size() >= 2) {
        size_t count = history.size();
        std::vector<POINT> pts;
        pts.reserve(count);

        for (size_t i = 0; i < count; ++i) {
            int val = history[i];
            int x = plotLeft + static_cast<int>(i * plotWidth / (count - 1));
            int y;
            if (val <= 0) {
                y = plotBottom;
            } else {
                int clampedVal = std::min(val, maxMs);
                y = plotBottom - (clampedVal * plotHeight / maxMs);
            }
            pts.push_back({ x, y });
        }

        // Color based on latest valid latency
        COLORREF lineColor, fillColor;
        if (currentPing > 0 && currentPing < 60) {
            lineColor = RGB(34, 197, 94);  // Emerald Green
            fillColor = RGB(16, 50, 32);   // Dark Emerald fill
        } else if (currentPing >= 60 && currentPing <= 150) {
            lineColor = RGB(245, 158, 11); // Amber
            fillColor = RGB(55, 40, 18);   // Dark Amber fill
        } else if (currentPing > 150) {
            lineColor = RGB(239, 68, 68);  // Red
            fillColor = RGB(55, 20, 20);   // Dark Red fill
        } else {
            lineColor = RGB(56, 189, 248); // Cyan
            fillColor = RGB(20, 45, 60);
        }

        // Area polygon below curve
        std::vector<POINT> poly = pts;
        poly.push_back({ pts.back().x, plotBottom });
        poly.push_back({ pts.front().x, plotBottom });

        HBRUSH hbrFill = CreateSolidBrush(fillColor);
        HPEN hPenNull  = CreatePen(PS_NULL, 0, 0);
        HGDIOBJ oldPolyBrush = SelectObject(hdcMem, hbrFill);
        HGDIOBJ oldPolyPen   = SelectObject(hdcMem, hPenNull);

        Polygon(hdcMem, poly.data(), static_cast<int>(poly.size()));

        SelectObject(hdcMem, oldPolyBrush);
        SelectObject(hdcMem, oldPolyPen);
        DeleteObject(hbrFill);
        DeleteObject(hPenNull);

        // Draw sparkline curve
        HPEN hPenLine = CreatePen(PS_SOLID, 2, lineColor);
        HGDIOBJ oldLinePen = SelectObject(hdcMem, hPenLine);

        Polyline(hdcMem, pts.data(), static_cast<int>(pts.size()));

        SelectObject(hdcMem, oldLinePen);
        DeleteObject(hPenLine);

        // Draw glowing point on latest sample
        POINT lastPt = pts.back();
        HBRUSH hbrDot = CreateSolidBrush(lineColor);
        HGDIOBJ oldDotBrush = SelectObject(hdcMem, hbrDot);
        HPEN hPenDot = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
        HGDIOBJ oldDotPen = SelectObject(hdcMem, hPenDot);

        Ellipse(hdcMem, lastPt.x - 3, lastPt.y - 3, lastPt.x + 4, lastPt.y + 4);

        SelectObject(hdcMem, oldDotBrush);
        SelectObject(hdcMem, oldDotPen);
        DeleteObject(hbrDot);
        DeleteObject(hPenDot);
    }

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
        valCol = RGB(148, 163, 184);
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
}

void MainWindow::on_profile_selected() {
    m_selectedProfileIndex = ComboBox_GetCurSel(m_hComboProfile);
    if (m_selectedProfileIndex >= 0 && m_selectedProfileIndex < static_cast<int>(m_profiles.size())) {
        const auto& p = m_profiles[m_selectedProfileIndex];
        std::wstring details;
        if (p.is_dhcp) {
            details = L"Автоматическое получение DNS серверов от DHCP роутера/провайдера.";
        } else {
            std::wstring pri  = utf8_to_wide(p.primary_ipv4);
            std::wstring sec  = utf8_to_wide(p.secondary_ipv4);
            std::wstring desc = utf8_to_wide(p.description);
            std::wstring doh  = utf8_to_wide(p.doh_template);
            details = L"IP: " + pri + (sec.empty() ? L"" : L", " + sec);
            if (!doh.empty()) {
                details += L"\nDoH: " + doh;
            }
            details += L"\n" + desc;
        }
        SetWindowTextW(m_hProfileDetails, details.c_str());

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

    NetworkAdapterInfo adapter = m_adapters[m_selectedAdapterIndex];
    DnsProfile profile = m_profiles[m_selectedProfileIndex];
    HWND hWnd = m_hWnd;

    std::thread([this, adapter, profile, hWnd]() {
        bool success = false;
        std::wstring msg;

        if (profile.is_dhcp) {
            success = m_dnsManager->reset_to_dhcp(adapter);
            msg = success ? L"Сетевой адаптер успешно переключен на DHCP." 
                          : L"Не удалось сбросить DNS на DHCP: " + m_dnsManager->get_last_error();
        } else {
            std::wstring pri = utf8_to_wide(profile.primary_ipv4);
            std::wstring sec = utf8_to_wide(profile.secondary_ipv4);
            success = m_dnsManager->set_static_dns(adapter, pri, sec);
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
