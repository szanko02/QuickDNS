#include "ui_main.h"
#include "dialog_add_profile.h"
#include "encoding_utils.h"
#include <dwmapi.h>
#include <uxtheme.h>
#include <windowsx.h>
#include <sstream>
#include <algorithm>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "comctl32.lib")

using QuickDns::Encoding::utf8_to_wide;
using QuickDns::Encoding::wide_to_utf8;

namespace {
    constexpr const wchar_t* WINDOW_CLASS_NAME = L"QuickDNS_MainWindowClass";
    constexpr const wchar_t* WINDOW_TITLE = L"QuickDNS — Быстрая смена DNS в 1 клик";
    constexpr int WINDOW_WIDTH = 520;
    constexpr int WINDOW_HEIGHT = 580;

    // Dark theme colors
    constexpr COLORREF COLOR_BG_DARK = RGB(30, 30, 30);
    constexpr COLORREF COLOR_TEXT_LIGHT = RGB(240, 240, 240);
}

MainWindow::MainWindow()
    : m_dnsManager(std::make_unique<DnsManager>())
    , m_configManager(std::make_unique<ConfigManager>())
{
}

MainWindow::~MainWindow() {
    remove_tray_icon();

    if (m_hFontRegular) DeleteObject(m_hFontRegular);
    if (m_hFontBold) DeleteObject(m_hFontBold);
    if (m_hFontHeader) DeleteObject(m_hFontHeader);
    if (m_hFontMonospace) DeleteObject(m_hFontMonospace);
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

    // Load initial data
    refresh_network_adapters();
    populate_profiles_combobox();
    update_status_card();

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
    wc.hbrBackground = CreateSolidBrush(COLOR_BG_DARK);
    wc.lpszClassName = WINDOW_CLASS_NAME;
    wc.hIconSm = LoadIconW(m_hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
    if (!wc.hIconSm) wc.hIconSm = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);
}

LRESULT CALLBACK MainWindow::window_proc_thunk(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    MainWindow* pThis = nullptr;
    if (uMsg == WM_NCCREATE) {
        auto* pCreate = reinterpret_cast<CREATESTRUCTW*>(lParam);
        pThis = reinterpret_cast<MainWindow*>(pCreate->lpCreateParams);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pThis));
        pThis->m_hWnd = hWnd;
    } else {
        pThis = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    }

    if (pThis) {
        return pThis->handle_message(hWnd, uMsg, wParam, lParam);
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
        EnableWindow(m_hBtnResetDhcp, TRUE);
        EnableWindow(m_hBtnFlushDns, TRUE);

        refresh_network_adapters();
        update_status_card();
        return 0;
    }

    case WM_APP_REFRESH_STATUS: {
        refresh_network_adapters();
        update_status_card();
        return 0;
    }

    case WM_CTLCOLORSTATIC: {
        HDC hdcStatic = (HDC)wParam;
        SetTextColor(hdcStatic, COLOR_TEXT_LIGHT);
        SetBkMode(hdcStatic, TRANSPARENT);
        static HBRUSH hbrBg = CreateSolidBrush(COLOR_BG_DARK);
        return (INT_PTR)hbrBg;
    }

    case WM_SYSCOMMAND: {
        if ((wParam & 0xFFF0) == SC_MINIMIZE) {
            ShowWindow(m_hWnd, SW_HIDE);
            m_inTray = true;
            show_tray_notification(L"QuickDNS свернут в трей", L"Приложение продолжает работать. Нажмите на иконку для быстрого переключения DNS.");
            return 0;
        }
        break;
    }

    case WM_DESTROY: {
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

    m_hFontMonospace = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
}

void MainWindow::create_controls() {
    int startY = 16;
    int marginX = 24;
    int contentWidth = WINDOW_WIDTH - 64;

    // 1. Status Card Header
    HWND hTitle = CreateWindowExW(0, L"STATIC", L"QuickDNS — Менеджер DNS",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        marginX, startY, contentWidth, 24,
        m_hWnd, nullptr, m_hInstance, nullptr);
    SendMessageW(hTitle, WM_SETFONT, (WPARAM)m_hFontHeader, TRUE);
    startY += 32;

    // Status Area / Card
    m_hStatusAdapterName = CreateWindowExW(0, L"STATIC", L"Подключение: Определение сети...",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        marginX, startY, contentWidth, 20,
        m_hWnd, (HMENU)IDC_STATUS_TITLE, m_hInstance, nullptr);
    SendMessageW(m_hStatusAdapterName, WM_SETFONT, (WPARAM)m_hFontBold, TRUE);
    startY += 24;

    m_hStatusDnsList = CreateWindowExW(0, L"STATIC", L"Текущий DNS: Загрузка...",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        marginX, startY, contentWidth, 20,
        m_hWnd, (HMENU)IDC_STATUS_DNS_VAL, m_hInstance, nullptr);
    SendMessageW(m_hStatusDnsList, WM_SETFONT, (WPARAM)m_hFontMonospace, TRUE);
    startY += 24;

    m_hStatusBadge = CreateWindowExW(0, L"STATIC", L"Статус: Проверка...",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        marginX, startY, contentWidth, 20,
        m_hWnd, nullptr, m_hInstance, nullptr);
    SendMessageW(m_hStatusBadge, WM_SETFONT, (WPARAM)m_hFontRegular, TRUE);
    startY += 36;

    // Separator line
    CreateWindowExW(0, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
        marginX, startY, contentWidth, 2,
        m_hWnd, nullptr, m_hInstance, nullptr);
    startY += 16;

    // 2. Network Adapter Selector
    HWND hLblAdapter = CreateWindowExW(0, L"STATIC", L"Сетевой адаптер:",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        marginX, startY, contentWidth, 18,
        m_hWnd, nullptr, m_hInstance, nullptr);
    SendMessageW(hLblAdapter, WM_SETFONT, (WPARAM)m_hFontRegular, TRUE);
    startY += 22;

    m_hComboAdapter = CreateWindowExW(0, WC_COMBOBOXW, L"",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
        marginX, startY, contentWidth, 200,
        m_hWnd, (HMENU)IDC_COMBO_ADAPTER, m_hInstance, nullptr);
    SendMessageW(m_hComboAdapter, WM_SETFONT, (WPARAM)m_hFontRegular, TRUE);
    startY += 38;

    // 3. DNS Profile Selector
    HWND hLblProfile = CreateWindowExW(0, L"STATIC", L"Выберите DNS профиль (Comss, GeoHide, Без межаў и др.):",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        marginX, startY, contentWidth, 18,
        m_hWnd, nullptr, m_hInstance, nullptr);
    SendMessageW(hLblProfile, WM_SETFONT, (WPARAM)m_hFontRegular, TRUE);
    startY += 22;

    m_hComboProfile = CreateWindowExW(0, WC_COMBOBOXW, L"",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
        marginX, startY, contentWidth, 200,
        m_hWnd, (HMENU)IDC_COMBO_PROFILE, m_hInstance, nullptr);
    SendMessageW(m_hComboProfile, WM_SETFONT, (WPARAM)m_hFontRegular, TRUE);
    startY += 38;

    // Profile Details Card
    m_hProfileDetails = CreateWindowExW(0, L"STATIC", L"Описание выбранного профиля...",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        marginX, startY, contentWidth, 36,
        m_hWnd, nullptr, m_hInstance, nullptr);
    SendMessageW(m_hProfileDetails, WM_SETFONT, (WPARAM)m_hFontRegular, TRUE);
    startY += 46;

    // 4. Action Buttons
    m_hBtnApply = CreateWindowExW(0, WC_BUTTONW, L"⚡ Применить DNS в 1 клик",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_DEFPUSHBUTTON | WS_TABSTOP,
        marginX, startY, contentWidth, 42,
        m_hWnd, (HMENU)IDC_BTN_APPLY, m_hInstance, nullptr);
    SendMessageW(m_hBtnApply, WM_SETFONT, (WPARAM)m_hFontBold, TRUE);
    startY += 52;

    int halfWidth = (contentWidth - 12) / 2;
    m_hBtnResetDhcp = CreateWindowExW(0, WC_BUTTONW, L"🔄 Сброс на DHCP (Авто)",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
        marginX, startY, halfWidth, 34,
        m_hWnd, (HMENU)IDC_BTN_RESET_DHCP, m_hInstance, nullptr);
    SendMessageW(m_hBtnResetDhcp, WM_SETFONT, (WPARAM)m_hFontRegular, TRUE);

    m_hBtnFlushDns = CreateWindowExW(0, WC_BUTTONW, L"🧹 Сбросить DNS-кэш",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
        marginX + halfWidth + 12, startY, halfWidth, 34,
        m_hWnd, (HMENU)IDC_BTN_FLUSH_DNS, m_hInstance, nullptr);
    SendMessageW(m_hBtnFlushDns, WM_SETFONT, (WPARAM)m_hFontRegular, TRUE);
    startY += 46;

    // 5. Custom Profile Actions (Add Profile + Delete Profile buttons)
    m_hBtnAddProfile = CreateWindowExW(0, WC_BUTTONW, L"➕ Добавить свой DNS...",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
        marginX, startY, halfWidth, 32,
        m_hWnd, (HMENU)IDC_BTN_ADD_PROFILE, m_hInstance, nullptr);
    SendMessageW(m_hBtnAddProfile, WM_SETFONT, (WPARAM)m_hFontRegular, TRUE);

    m_hBtnDeleteProfile = CreateWindowExW(0, WC_BUTTONW, L"🗑️ Удалить профиль",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
        marginX + halfWidth + 12, startY, halfWidth, 32,
        m_hWnd, (HMENU)IDC_BTN_DELETE_PROFILE, m_hInstance, nullptr);
    SendMessageW(m_hBtnDeleteProfile, WM_SETFONT, (WPARAM)m_hFontRegular, TRUE);
    EnableWindow(m_hBtnDeleteProfile, FALSE);
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
    wcscpy_s(m_nid.szTip, L"QuickDNS — Быстрая смена DNS");

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

    m_selectedAdapterIndex = -1;
    auto primaryAdapter = m_dnsManager->get_primary_adapter();

    for (size_t i = 0; i < m_adapters.size(); ++i) {
        std::wstring itemText = m_adapters[i].name + L" (" + m_adapters[i].description + L")";
        ComboBox_AddString(m_hComboAdapter, itemText.c_str());

        if (!prevSelectedId.empty() && m_adapters[i].id == prevSelectedId) {
            m_selectedAdapterIndex = static_cast<int>(i);
        }
    }

    if (m_selectedAdapterIndex == -1 && primaryAdapter.has_value()) {
        for (size_t i = 0; i < m_adapters.size(); ++i) {
            if (m_adapters[i].id == primaryAdapter->id) {
                m_selectedAdapterIndex = static_cast<int>(i);
                break;
            }
        }
    }

    if (m_selectedAdapterIndex == -1 && !m_adapters.empty()) {
        m_selectedAdapterIndex = 0;
    }

    if (m_selectedAdapterIndex >= 0) {
        ComboBox_SetCurSel(m_hComboAdapter, m_selectedAdapterIndex);
    }
}

void MainWindow::populate_profiles_combobox() {
    m_profiles = m_configManager->get_all_profiles();
    ComboBox_ResetContent(m_hComboProfile);

    std::string activeId = m_configManager->get_active_profile_id();
    m_selectedProfileIndex = 0;

    for (size_t i = 0; i < m_profiles.size(); ++i) {
        std::wstring wName = utf8_to_wide(m_profiles[i].name);
        ComboBox_AddString(m_hComboProfile, wName.c_str());

        if (m_profiles[i].id == activeId) {
            m_selectedProfileIndex = static_cast<int>(i);
        }
    }

    ComboBox_SetCurSel(m_hComboProfile, m_selectedProfileIndex);
    on_profile_selected();
}

void MainWindow::update_status_card() {
    if (m_selectedAdapterIndex < 0 || m_selectedAdapterIndex >= static_cast<int>(m_adapters.size())) {
        SetWindowTextW(m_hStatusAdapterName, L"Подключение: Нет активного адаптера");
        SetWindowTextW(m_hStatusDnsList, L"Текущий DNS: —");
        SetWindowTextW(m_hStatusBadge, L"Статус: Сеть недоступна");
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
            std::wstring pri = utf8_to_wide(p.primary_ipv4);
            std::wstring sec = utf8_to_wide(p.secondary_ipv4);
            std::wstring desc = utf8_to_wide(p.description);
            details = L"IP: " + pri + (sec.empty() ? L"" : L", " + sec) + L"\n" + desc;
        }
        SetWindowTextW(m_hProfileDetails, details.c_str());

        // Update delete button state: only allow deleting non-builtin profiles
        if (m_hBtnDeleteProfile) {
            EnableWindow(m_hBtnDeleteProfile, !p.is_builtin);
        }
    }
}

void MainWindow::apply_selected_dns() {
    if (m_isBusy) return;
    if (m_selectedAdapterIndex < 0 || m_selectedAdapterIndex >= static_cast<int>(m_adapters.size())) return;
    if (m_selectedProfileIndex < 0 || m_selectedProfileIndex >= static_cast<int>(m_profiles.size())) return;

    m_isBusy = true;
    EnableWindow(m_hBtnApply, FALSE);
    EnableWindow(m_hBtnResetDhcp, FALSE);
    EnableWindow(m_hBtnFlushDns, FALSE);

    const auto adapter = m_adapters[m_selectedAdapterIndex];
    const auto profile = m_profiles[m_selectedProfileIndex];

    std::thread([this, adapter, profile]() {
        bool success = false;
        std::wstring errorDetail;

        if (profile.is_dhcp) {
            success = m_dnsManager->reset_to_dhcp(adapter);
            if (!success) errorDetail = m_dnsManager->get_last_error();
        } else {
            std::wstring pri = utf8_to_wide(profile.primary_ipv4);
            std::wstring sec = utf8_to_wide(profile.secondary_ipv4);
            success = m_dnsManager->set_static_dns(adapter, pri, sec);
            if (!success) errorDetail = m_dnsManager->get_last_error();
        }

        m_dnsManager->flush_dns_cache();

        // Brief delay to allow Windows kernel TCP/IP stack to commit adapter state
        Sleep(250);

        if (success) {
            m_configManager->set_active_profile_id(profile.id);
            m_configManager->save_to_file();

            std::wstring pName = utf8_to_wide(profile.name);
            show_tray_notification(L"DNS успешно обновлен!", L"Установлен профиль: " + pName);
        } else {
            std::wstring msg = L"Не удалось применить настройки DNS.";
            if (!errorDetail.empty()) {
                msg += L"\n\nПричина: " + errorDetail;
            } else {
                msg += L"\n\nУбедитесь, что приложение запущено от имени Администратора.";
            }
            show_tray_notification(L"Ошибка установки DNS", msg);
            MessageBoxW(m_hWnd, msg.c_str(), L"Ошибка смены DNS", MB_OK | MB_ICONWARNING);
        }

        PostMessageW(m_hWnd, WM_APP_DNS_APPLIED, success ? 1 : 0, 0);
    }).detach();
}

void MainWindow::reset_to_dhcp() {
    if (m_isBusy) return;
    if (m_selectedAdapterIndex < 0 || m_selectedAdapterIndex >= static_cast<int>(m_adapters.size())) return;

    m_isBusy = true;
    EnableWindow(m_hBtnApply, FALSE);
    EnableWindow(m_hBtnResetDhcp, FALSE);
    EnableWindow(m_hBtnFlushDns, FALSE);

    const auto adapter = m_adapters[m_selectedAdapterIndex];

    std::thread([this, adapter]() {
        bool success = m_dnsManager->reset_to_dhcp(adapter);
        std::wstring errorDetail;
        if (!success) {
            errorDetail = m_dnsManager->get_last_error();
        }

        m_dnsManager->flush_dns_cache();

        Sleep(250);

        if (success) {
            m_configManager->set_active_profile_id("dhcp");
            m_configManager->save_to_file();
            show_tray_notification(L"DNS сброшен", L"Установлено автоматическое получение DNS (DHCP).");
        } else {
            std::wstring msg = L"Не удалось сбросить DNS на DHCP.";
            if (!errorDetail.empty()) {
                msg += L"\n\nПричина: " + errorDetail;
            }
            show_tray_notification(L"Ошибка сброса DNS", msg);
            MessageBoxW(m_hWnd, msg.c_str(), L"Ошибка сброса DNS", MB_OK | MB_ICONWARNING);
        }

        PostMessageW(m_hWnd, WM_APP_DNS_APPLIED, success ? 1 : 0, 0);
    }).detach();
}

void MainWindow::flush_dns_cache() {
    bool ok = m_dnsManager->flush_dns_cache();
    if (ok) {
        show_tray_notification(L"DNS-кэш очищен", L"Системный кэш сопоставителя DNS успешно сброшен.");
        MessageBoxW(m_hWnd, 
            L"Системный кэш сопоставителя DNS успешно очищен!\n\nВыполнен сброс через DnsFlushResolverCache и ipconfig /flushdns.", 
            L"Очистка DNS-кэша", 
            MB_OK | MB_ICONINFORMATION);
    } else {
        show_tray_notification(L"DNS-кэш", L"Попытка сброса кэша завершена.");
    }
}

void MainWindow::open_add_profile_dialog() {
    auto newProfileOpt = AddProfileDialog::show(m_hWnd, m_hInstance);
    if (!newProfileOpt.has_value()) {
        return;
    }

    const auto& newProfile = *newProfileOpt;
    bool added = m_configManager->add_profile(newProfile);
    if (added) {
        m_configManager->set_active_profile_id(newProfile.id);
        m_configManager->save_to_file();
        populate_profiles_combobox();

        std::wstring pName = utf8_to_wide(newProfile.name);
        show_tray_notification(L"Профиль добавлен", L"Профиль '" + pName + L"' успешно сохранен и выбран.");
    } else {
        MessageBoxW(m_hWnd, L"Не удалось добавить профиль (возможно, профиль с таким именем уже существует).", L"Ошибка", MB_OK | MB_ICONERROR);
    }
}

void MainWindow::show_tray_context_menu() {
    HMENU hMenu = CreatePopupMenu();
    InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_STRING, IDM_TRAY_SHOW, L"Открыть QuickDNS");
    InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);

    // Dynamic Presets in Context Menu
    std::string activeId = m_configManager->get_active_profile_id();
    for (size_t i = 0; i < m_profiles.size() && i < (IDM_TRAY_PRESET_LAST - IDM_TRAY_PRESET_FIRST); ++i) {
        std::wstring itemText = utf8_to_wide(m_profiles[i].name);
        UINT flags = MF_BYPOSITION | MF_STRING;
        if (m_profiles[i].id == activeId) {
            flags |= MF_CHECKED;
        }
        InsertMenuW(hMenu, -1, flags, IDM_TRAY_PRESET_FIRST + static_cast<UINT>(i), itemText.c_str());
    }

    InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
    InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_STRING, IDM_TRAY_FLUSH_DNS, L"Очистить DNS-кэш");
    InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
    InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_STRING, IDM_TRAY_EXIT, L"Выход");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(m_hWnd);
    TrackPopupMenuEx(hMenu, TPM_LEFTALIGN | TPM_BOTTOMALIGN, pt.x, pt.y, m_hWnd, nullptr);
    DestroyMenu(hMenu);
}

void MainWindow::on_tray_preset_selected(int commandId) {
    size_t index = static_cast<size_t>(commandId - IDM_TRAY_PRESET_FIRST);
    if (index < m_profiles.size()) {
        m_selectedProfileIndex = static_cast<int>(index);
        ComboBox_SetCurSel(m_hComboProfile, m_selectedProfileIndex);
        on_profile_selected();
        apply_selected_dns();
    }
}
