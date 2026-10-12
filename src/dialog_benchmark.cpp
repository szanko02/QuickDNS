#include "dialog_benchmark.h"
#include "encoding_utils.h"
#include "resource.h"
#include "ui_palette.h"
#include <dwmapi.h>
#include <windowsx.h>
#include <commctrl.h>
#include <thread>
#include <future>
#include <algorithm>
#include <chrono>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdiplus.lib")

using QuickDns::Encoding::wide_to_utf8;
using QuickDns::Encoding::utf8_to_wide;

namespace {
    constexpr const wchar_t* DIALOG_CLASS_NAME = L"QuickDNS_BenchmarkDialogClass";
    constexpr const wchar_t* LIST_CLASS_NAME   = L"QuickDNS_BenchmarkListClass";

    constexpr int DIALOG_CLIENT_WIDTH  = 700;
    constexpr int DIALOG_CLIENT_HEIGHT = 640;

    constexpr UINT WM_APP_BENCHMARK_PING_UPDATE = WM_APP + 20;

    constexpr int IDC_BTN_BENCHMARK_PAUSE   = 1206;

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
                SetTimer(hWnd, 3001, 16, nullptr);
            }
            break;
        }
        case WM_MOUSELEAVE: {
            anim->isTracking = false;
            anim->targetHover = 0.0f;
            SetTimer(hWnd, 3001, 16, nullptr);
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
            if (wParam == 3001) {
                float diff = anim->targetHover - anim->hover;
                if (std::abs(diff) < 0.02f) {
                    anim->hover = anim->targetHover;
                    KillTimer(hWnd, 3001);
                } else {
                    anim->hover += diff * 0.28f;
                }
                InvalidateRect(hWnd, nullptr, FALSE);
                return 0;
            }
            break;
        }
        case WM_NCDESTROY: {
            KillTimer(hWnd, 3001);
            RemovePropW(hWnd, L"QuickDnsAnim");
            delete anim;
            RemoveWindowSubclass(hWnd, AnimatedButtonSubclassProc, uIdSubclass);
            return 0;
        }
        }

        return DefSubclassProc(hWnd, uMsg, wParam, lParam);
    }
}

namespace quickdns {

std::optional<std::string> BenchmarkDialog::show(
    HWND hParent,
    HINSTANCE hInstance,
    const std::vector<DnsProfile>& profiles,
    const std::string& currentActiveId,
    bool isDarkMode)
{
    BenchmarkDialog dlg(hParent, hInstance, profiles, currentActiveId, isDarkMode);
    if (!dlg.create_dialog_window()) {
        return std::nullopt;
    }

    EnableWindow(hParent, FALSE);

    dlg.start_ping_worker();

    MSG msg;
    while (!dlg.m_finished && GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessageW(dlg.m_hWnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    dlg.stop_ping_worker();

    EnableWindow(hParent, TRUE);
    SetForegroundWindow(hParent);

    if (dlg.m_hWnd) {
        DestroyWindow(dlg.m_hWnd);
        dlg.m_hWnd = nullptr;
    }

    return dlg.m_selectedProfileId;
}

BenchmarkDialog::BenchmarkDialog(
    HWND hParent,
    HINSTANCE hInstance,
    const std::vector<DnsProfile>& profiles,
    const std::string& currentActiveId,
    bool isDarkMode)
    : m_hParent(hParent)
    , m_hInstance(hInstance)
    , m_currentActiveId(currentActiveId)
    , m_isDarkMode(isDarkMode)
{
    auto pal = GetPalette(m_isDarkMode);
    m_hbrBg = CreateSolidBrush(pal.bg);
    m_hbrCard = CreateSolidBrush(pal.cardBg);

    // Initialize items
    for (const auto& p : profiles) {
        BenchmarkItem item;
        item.profile = p;
        if (p.is_dhcp) {
            item.status_text = L"DHCP (Авто)";
            item.quality_label = L"Провайдер";
            item.quality = DnsQuality::Fast;
        } else {
            item.status_text = L"Опрос...";
            item.quality_label = L"Ожидание";
            item.quality = DnsQuality::Moderate;
        }
        m_items.push_back(std::move(item));
    }

    // Default select active profile or first item
    for (size_t i = 0; i < m_items.size(); ++i) {
        if (m_items[i].profile.id == m_currentActiveId) {
            m_selectedIndex = static_cast<int>(i);
            break;
        }
    }

    setup_fonts();
}

BenchmarkDialog::~BenchmarkDialog() {
    stop_ping_worker();

    if (m_hFontRegular) DeleteObject(m_hFontRegular);
    if (m_hFontBold) DeleteObject(m_hFontBold);
    if (m_hFontHeader) DeleteObject(m_hFontHeader);
    if (m_hFontMonospace) DeleteObject(m_hFontMonospace);
    if (m_hFontSmall) DeleteObject(m_hFontSmall);

    if (m_hbrBg) DeleteObject(m_hbrBg);
    if (m_hbrCard) DeleteObject(m_hbrCard);
}

void BenchmarkDialog::setup_fonts() {
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
}

bool BenchmarkDialog::create_dialog_window() {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &BenchmarkDialog::dialog_proc_thunk;
    wc.hInstance = m_hInstance;
    wc.hIcon = LoadIconW(m_hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
    if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = m_hbrBg;
    wc.lpszClassName = DIALOG_CLASS_NAME;
    RegisterClassExW(&wc);

    // Register Custom List Class
    WNDCLASSEXW lwc = {};
    lwc.cbSize = sizeof(WNDCLASSEXW);
    lwc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    lwc.lpfnWndProc = &BenchmarkDialog::list_proc_thunk;
    lwc.hInstance = m_hInstance;
    lwc.hCursor = LoadCursorW(nullptr, IDC_HAND);
    lwc.hbrBackground = nullptr;
    lwc.lpszClassName = LIST_CLASS_NAME;
    RegisterClassExW(&lwc);

    DWORD dwStyle = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE;
    RECT wr = { 0, 0, DIALOG_CLIENT_WIDTH, DIALOG_CLIENT_HEIGHT };
    AdjustWindowRectEx(&wr, dwStyle, FALSE, 0);
    int winWidth = wr.right - wr.left;
    int winHeight = wr.bottom - wr.top;

    RECT parentRect = {};
    GetWindowRect(m_hParent, &parentRect);
    int posX = parentRect.left + ((parentRect.right - parentRect.left) - winWidth) / 2;
    int posY = parentRect.top + ((parentRect.bottom - parentRect.top) - winHeight) / 2;

    m_hWnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        DIALOG_CLASS_NAME,
        L"Тест скорости и мониторинг DNS (Пинг)",
        dwStyle,
        posX, posY,
        winWidth, winHeight,
        m_hParent,
        nullptr,
        m_hInstance,
        this
    );

    if (!m_hWnd) {
        return false;
    }

    apply_theme();
    create_controls();
    return true;
}

void BenchmarkDialog::apply_theme() {
    BOOL useDarkMode = m_isDarkMode ? TRUE : FALSE;
    DwmSetWindowAttribute(m_hWnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &useDarkMode, sizeof(useDarkMode));
    DWORD cornerPref = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(m_hWnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &cornerPref, sizeof(cornerPref));
}

void BenchmarkDialog::create_controls() {
    RECT rcClient = {};
    GetClientRect(m_hWnd, &rcClient);
    int clientWidth = rcClient.right - rcClient.left;
    int clientHeight = rcClient.bottom - rcClient.top;

    int marginX = 20;
    int contentWidth = clientWidth - (marginX * 2);
    int curY = 16;

    HWND hTitle = CreateWindowExW(0, L"STATIC", L"Тест скорости и отклика DNS",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        marginX, curY, contentWidth, 24,
        m_hWnd, nullptr, m_hInstance, nullptr);
    SendMessageW(hTitle, WM_SETFONT, (WPARAM)m_hFontHeader, TRUE);
    curY += 28;

    HWND hSub = CreateWindowExW(0, L"STATIC", L"Мониторинг задержки (пинг) серверов в реальном времени с графиком отклика.",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        marginX, curY, contentWidth, 18,
        m_hWnd, nullptr, m_hInstance, nullptr);
    SendMessageW(hSub, WM_SETFONT, (WPARAM)m_hFontRegular, TRUE);
    curY += 22;

    m_hStatusText = CreateWindowExW(0, L"STATIC", L"Автомониторинг активен (опрос каждые 2 сек) • Нажмите на DNS для выбора",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        marginX, curY, contentWidth, 18,
        m_hWnd, (HMENU)IDC_STATUS_BENCHMARK, m_hInstance, nullptr);
    SendMessageW(m_hStatusText, WM_SETFONT, (WPARAM)m_hFontBold, TRUE);
    curY += 24;

    // Custom Sparkline Benchmark List
    int bottomButtonsH = 38;
    int listBottomMargin = 16;
    int listHeight = clientHeight - curY - bottomButtonsH - listBottomMargin - 16;

    m_hListWnd = CreateWindowExW(
        0,
        LIST_CLASS_NAME,
        L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP,
        marginX, curY, contentWidth, listHeight,
        m_hWnd, (HMENU)IDC_LIST_BENCHMARK, m_hInstance, this
    );

    curY += listHeight + 16;

    // Bottom Action Buttons with modern styling
    int btnApplyW = 210;
    int btnPauseW = 180;
    int btnCloseW = 110;

    m_hBtnApply = CreateWindowExW(0, WC_BUTTONW, L"Применить этот DNS",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        marginX, curY, btnApplyW, bottomButtonsH,
        m_hWnd, (HMENU)IDC_BTN_BENCHMARK_APPLY, m_hInstance, nullptr);

    m_hBtnPauseResume = CreateWindowExW(0, WC_BUTTONW, L"Пауза мониторинга",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        marginX + btnApplyW + 12, curY, btnPauseW, bottomButtonsH,
        m_hWnd, (HMENU)IDC_BTN_BENCHMARK_PAUSE, m_hInstance, nullptr);

    m_hBtnClose = CreateWindowExW(0, WC_BUTTONW, L"Закрыть",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        marginX + contentWidth - btnCloseW, curY, btnCloseW, bottomButtonsH,
        m_hWnd, (HMENU)IDC_BTN_BENCHMARK_CLOSE, m_hInstance, nullptr);

    attach_button_effects(m_hBtnApply, 101);
    attach_button_effects(m_hBtnPauseResume, 102);
    attach_button_effects(m_hBtnClose, 103);
}

void BenchmarkDialog::start_ping_worker() {
    m_workerRunning = true;
    m_isPaused = false;

    m_workerThread = std::thread([this]() {
        while (m_workerRunning) {
            if (m_isPaused) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                continue;
            }

            size_t count = 0;
            {
                std::lock_guard<std::mutex> lock(m_itemsMutex);
                count = m_items.size();
            }

            // Ping each profile concurrently for snappy real-time responsiveness
            std::vector<std::future<std::pair<size_t, int>>> futures;
            futures.reserve(count);

            for (size_t i = 0; i < count; ++i) {
                std::string ip;
                bool isDhcp = false;
                {
                    std::lock_guard<std::mutex> lock(m_itemsMutex);
                    if (i < m_items.size()) {
                        ip = m_items[i].profile.primary_ipv4;
                        isDhcp = m_items[i].profile.is_dhcp;
                    }
                }

                futures.push_back(std::async(std::launch::async, [i, ip, isDhcp]() -> std::pair<size_t, int> {
                    if (isDhcp || ip.empty()) return { i, -2 }; // -2 for DHCP
                    int ping = DnsBenchmark::ping_dns_server(ip, 700, true);
                    return { i, ping };
                }));
            }

            for (auto& fut : futures) {
                if (!m_workerRunning) break;
                auto [idx, ping] = fut.get();
                {
                    std::lock_guard<std::mutex> lock(m_itemsMutex);
                    if (idx < m_items.size()) {
                        auto& it = m_items[idx];
                        if (ping == -2) {
                            it.current_ping = 0;
                            it.status_text = L"DHCP (Авто)";
                            it.quality_label = L"Провайдер";
                            it.quality = DnsQuality::Fast;
                        } else {
                            it.current_ping = ping;
                            if (it.latency_history.size() >= 16) {
                                it.latency_history.erase(it.latency_history.begin());
                            }
                            it.latency_history.push_back(ping);

                            if (ping > 0 && ping < 60) {
                                it.status_text = std::to_wstring(ping) + L" ms";
                                it.quality = DnsQuality::Fast;
                                it.quality_label = L"Отличный";
                            } else if (ping >= 60 && ping <= 150) {
                                it.status_text = std::to_wstring(ping) + L" ms";
                                it.quality = DnsQuality::Moderate;
                                it.quality_label = L"Хороший";
                            } else if (ping > 150) {
                                it.status_text = std::to_wstring(ping) + L" ms";
                                it.quality = DnsQuality::SlowOrDown;
                                it.quality_label = L"Медленный";
                            } else {
                                it.status_text = L"Таймаут";
                                it.quality = DnsQuality::SlowOrDown;
                                it.quality_label = L"Недоступен";
                            }
                        }
                    }
                }
            }

            if (m_hWnd && IsWindow(m_hWnd)) {
                PostMessageW(m_hWnd, WM_APP_BENCHMARK_PING_UPDATE, 0, 0);
            }

            // Sleep 2.0 seconds between testing cycles
            for (int s = 0; s < 20 && m_workerRunning && !m_isPaused; ++s) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    });
}

void BenchmarkDialog::stop_ping_worker() {
    m_workerRunning = false;
    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }
}

void BenchmarkDialog::toggle_pause() {
    m_isPaused = !m_isPaused;
    if (m_hBtnPauseResume) {
        SetWindowTextW(m_hBtnPauseResume, m_isPaused ? L"Возобновить" : L"Пауза мониторинга");
    }
    if (m_hStatusText) {
        SetWindowTextW(m_hStatusText, m_isPaused ? L"Мониторинг приостановлен" : L"Автомониторинг активен (опрос каждые 2 сек)");
    }
}

void BenchmarkDialog::on_apply() {
    std::lock_guard<std::mutex> lock(m_itemsMutex);
    if (m_selectedIndex >= 0 && m_selectedIndex < static_cast<int>(m_items.size())) {
        m_selectedProfileId = m_items[m_selectedIndex].profile.id;
        m_finished = true;
    }
}

void BenchmarkDialog::on_close() {
    m_finished = true;
}

void BenchmarkDialog::attach_button_effects(HWND hBtn, UINT_PTR subclassId) {
    if (!hBtn) return;
    auto* anim = new ButtonAnimationState();
    SetPropW(hBtn, L"QuickDnsAnim", reinterpret_cast<HANDLE>(anim));
    SetWindowSubclass(hBtn, AnimatedButtonSubclassProc, subclassId, reinterpret_cast<DWORD_PTR>(anim));
}

void BenchmarkDialog::draw_custom_button(DRAWITEMSTRUCT* pDis) {
    if (!pDis) return;

    HDC hdc = pDis->hDC;
    RECT rc = pDis->rcItem;
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;

    auto pal = GetPalette(m_isDarkMode);
    bool isPrimary = (pDis->CtlID == IDC_BTN_BENCHMARK_APPLY);

    auto* anim = reinterpret_cast<ButtonAnimationState*>(GetPropW(pDis->hwndItem, L"QuickDnsAnim"));
    float hoverVal = anim ? anim->hover : 0.0f;
    bool isPressed = (pDis->itemState & ODS_SELECTED) || (anim && anim->isPressed);

    COLORREF baseBg = isPrimary ? pal.btnPrimaryBg : pal.btnUtilBg;
    COLORREF hoverBg = isPrimary ? pal.btnPrimaryHover : pal.btnUtilHover;
    COLORREF pressBg = isPrimary ? pal.btnPrimaryPress : pal.btnUtilPress;

    COLORREF baseBorder = isPrimary ? pal.btnPrimaryBorder : pal.btnUtilBorder;
    COLORREF hoverBorder = isPrimary ? pal.btnPrimaryHover : (m_isDarkMode ? RGB(70, 85, 110) : RGB(180, 195, 220));

    COLORREF curBg = isPressed ? pressBg : LerpColor(baseBg, hoverBg, hoverVal);
    COLORREF curBorder = LerpColor(baseBorder, hoverBorder, hoverVal);
    COLORREF textColor = isPrimary ? pal.btnPrimaryText : pal.btnUtilText;

    // Double buffer
    HDC hdcMem = CreateCompatibleDC(hdc);
    HBITMAP hbmMem = CreateCompatibleBitmap(hdc, w, h);
    HGDIOBJ oldBmp = SelectObject(hdcMem, hbmMem);

    // Pre-fill memory buffer with window background for perfect rounded blending
    HBRUSH hbrWin = CreateSolidBrush(pal.bg);
    FillRect(hdcMem, &rc, hbrWin);
    DeleteObject(hbrWin);

    {
        Gdiplus::Graphics g(hdcMem);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

        Gdiplus::GraphicsPath path;
        AddRoundedRect(path, Gdiplus::RectF(0.5f, 0.5f, static_cast<float>(w) - 1.0f, static_cast<float>(h) - 1.0f), 7.0f);

        Gdiplus::SolidBrush br(Gdiplus::Color(255, GetRValue(curBg), GetGValue(curBg), GetBValue(curBg)));
        g.FillPath(&br, &path);

        Gdiplus::Pen pen(Gdiplus::Color(255, GetRValue(curBorder), GetGValue(curBorder), GetBValue(curBorder)), 1.0f);
        g.DrawPath(&pen, &path);
    }

    wchar_t btnText[128] = {};
    GetWindowTextW(pDis->hwndItem, btnText, 128);

    SetBkMode(hdcMem, TRANSPARENT);
    SetTextColor(hdcMem, textColor);
    HFONT hFont = isPrimary ? m_hFontBold : m_hFontRegular;
    HGDIOBJ oldFont = SelectObject(hdcMem, hFont);

    RECT rcText = rc;
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

void BenchmarkDialog::draw_list_item(Gdiplus::Graphics& g, int index, int y, int width, int height, bool isSelected, bool isHovered) {
    if (index < 0 || index >= static_cast<int>(m_items.size())) return;
    const auto& it = m_items[index];
    auto pal = GetPalette(m_isDarkMode);

    // 1. Container card with smooth rounded corners
    Gdiplus::RectF rcCard(0.5f, static_cast<float>(y) + 0.5f, static_cast<float>(width) - 1.0f, static_cast<float>(height) - 1.0f);
    Gdiplus::GraphicsPath cardPath;
    AddRoundedRect(cardPath, rcCard, 8.0f);

    COLORREF bgCol = isSelected 
        ? (m_isDarkMode ? RGB(38, 48, 66) : RGB(228, 238, 255))
        : pal.cardBg;
    COLORREF borderCol = isSelected
        ? (m_isDarkMode ? RGB(96, 165, 250) : RGB(37, 99, 235))
        : (isHovered ? (m_isDarkMode ? RGB(75, 90, 115) : RGB(180, 195, 220)) : pal.cardBorder);
    float borderWidth = isSelected ? 1.5f : 1.0f;

    Gdiplus::SolidBrush cardBr(Gdiplus::Color(255, GetRValue(bgCol), GetGValue(bgCol), GetBValue(bgCol)));
    Gdiplus::Pen cardPen(Gdiplus::Color(255, GetRValue(borderCol), GetGValue(borderCol), GetBValue(borderCol)), borderWidth);
    g.FillPath(&cardBr, &cardPath);
    g.DrawPath(&cardPen, &cardPath);

    // Selected indicator accent bar on the left
    if (isSelected) {
        Gdiplus::GraphicsPath selBarPath;
        AddRoundedRect(selBarPath, Gdiplus::RectF(2.0f, static_cast<float>(y) + 6.0f, 3.5f, static_cast<float>(height) - 12.0f), 2.0f);
        Gdiplus::SolidBrush selBarBr(Gdiplus::Color(255, GetRValue(borderCol), GetGValue(borderCol), GetBValue(borderCol)));
        g.FillPath(&selBarBr, &selBarPath);
    }

    // 2. Status Dot
    float dotX = 18.0f;
    float dotY = static_cast<float>(y) + static_cast<float>(height) / 2.0f;
    Gdiplus::Color dotColor;
    if (it.profile.is_dhcp) {
        dotColor = Gdiplus::Color(255, 59, 130, 246); // Blue
    } else if (it.current_ping > 0 && it.current_ping < 60) {
        dotColor = Gdiplus::Color(255, 34, 197, 94);  // Emerald Green
    } else if (it.current_ping >= 60 && it.current_ping <= 150) {
        dotColor = Gdiplus::Color(255, 245, 158, 11); // Amber
    } else if (it.current_ping > 150) {
        dotColor = Gdiplus::Color(255, 239, 68, 68);  // Red
    } else {
        dotColor = Gdiplus::Color(255, 156, 163, 175); // Gray
    }

    // Outer aura
    Gdiplus::SolidBrush auraBr(Gdiplus::Color(60, dotColor.GetR(), dotColor.GetG(), dotColor.GetB()));
    g.FillEllipse(&auraBr, dotX - 7.0f, dotY - 7.0f, 14.0f, 14.0f);
    // Inner solid dot
    Gdiplus::SolidBrush dotBr(dotColor);
    g.FillEllipse(&dotBr, dotX - 4.0f, dotY - 4.0f, 8.0f, 8.0f);

    // 3. Profile Name and Tags
    Gdiplus::FontFamily fontFamily(L"Segoe UI");
    Gdiplus::Font fontTitle(&fontFamily, 10.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);
    Gdiplus::Font fontSub(&fontFamily, 8.5f, Gdiplus::FontStyleRegular, Gdiplus::UnitPoint);
    Gdiplus::Font fontBadge(&fontFamily, 8.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);

    Gdiplus::SolidBrush textPriBr(Gdiplus::Color(255, GetRValue(pal.textPrimary), GetGValue(pal.textPrimary), GetBValue(pal.textPrimary)));
    Gdiplus::SolidBrush textSecBr(Gdiplus::Color(255, GetRValue(pal.textSecondary), GetGValue(pal.textSecondary), GetBValue(pal.textSecondary)));

    std::wstring pName = utf8_to_wide(it.profile.name);
    g.DrawString(pName.c_str(), -1, &fontTitle, Gdiplus::PointF(34.0f, static_cast<float>(y) + 6.0f), &textPriBr);

    // Active profile badge [Текущий]
    if (it.profile.id == m_currentActiveId) {
        Gdiplus::SolidBrush activeBadgeBr(Gdiplus::Color(255, GetRValue(pal.accentBlue), GetGValue(pal.accentBlue), GetBValue(pal.accentBlue)));
        g.DrawString(L"[Текущий DNS]", -1, &fontBadge, Gdiplus::PointF(34.0f + static_cast<float>(pName.length() * 8.0f) + 12.0f, static_cast<float>(y) + 8.0f), &activeBadgeBr);
    }

    // Subtitle: Primary IP & DoH badge
    std::wstring subInfo;
    if (it.profile.is_dhcp) {
        subInfo = L"DHCP роутера/провайдера";
    } else {
        subInfo = L"IP: " + utf8_to_wide(it.profile.primary_ipv4);
        if (!it.profile.secondary_ipv4.empty()) {
            subInfo += L", " + utf8_to_wide(it.profile.secondary_ipv4);
        }
        if (!it.profile.doh_template.empty()) {
            subInfo += L"  •  DoH";
        }
    }
    g.DrawString(subInfo.c_str(), -1, &fontSub, Gdiplus::PointF(34.0f, static_cast<float>(y) + 26.0f), &textSecBr);

    // 4. Ping & Rating Column (X ~ width - 265)
    float pingColX = static_cast<float>(width) - 265.0f;
    std::wstring pingValStr;
    Gdiplus::Color pingTextColor;
    if (it.profile.is_dhcp) {
        pingValStr = L"Авто";
        pingTextColor = Gdiplus::Color(255, GetRValue(pal.accentBlue), GetGValue(pal.accentBlue), GetBValue(pal.accentBlue));
    } else if (it.current_ping > 0) {
        pingValStr = std::to_wstring(it.current_ping) + L" ms";
        pingTextColor = dotColor;
    } else if (it.current_ping == -1 && it.latency_history.empty()) {
        pingValStr = L"Опрос...";
        pingTextColor = Gdiplus::Color(255, GetRValue(pal.textMuted), GetGValue(pal.textMuted), GetBValue(pal.textMuted));
    } else {
        pingValStr = L"Таймаут";
        pingTextColor = Gdiplus::Color(255, 239, 68, 68);
    }

    Gdiplus::FontFamily monoFamily(L"Consolas");
    Gdiplus::Font fontPing(&monoFamily, 10.5f, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);
    Gdiplus::SolidBrush pingTextBr(pingTextColor);
    g.DrawString(pingValStr.c_str(), -1, &fontPing, Gdiplus::PointF(pingColX, static_cast<float>(y) + 6.0f), &pingTextBr);

    std::wstring qualStr = it.quality_label;
    g.DrawString(qualStr.c_str(), -1, &fontSub, Gdiplus::PointF(pingColX, static_cast<float>(y) + 26.0f), &textSecBr);

    // 5. Mini Sparkline Graph Column (X: width - 146, Width: 136, Height: 34)
    float graphX = static_cast<float>(width) - 146.0f;
    float graphY = static_cast<float>(y) + 7.0f;
    float graphW = 136.0f;
    float graphH = 34.0f;

    Gdiplus::GraphicsPath graphCont;
    AddRoundedRect(graphCont, Gdiplus::RectF(graphX, graphY, graphW, graphH), 6.0f);
    Gdiplus::SolidBrush graphBgBr(Gdiplus::Color(255, GetRValue(pal.graphBg), GetGValue(pal.graphBg), GetBValue(pal.graphBg)));
    Gdiplus::Pen graphBorderPen(Gdiplus::Color(255, GetRValue(pal.graphBorder), GetGValue(pal.graphBorder), GetBValue(pal.graphBorder)), 1.0f);
    g.FillPath(&graphBgBr, &graphCont);
    g.DrawPath(&graphBorderPen, &graphCont);

    // Faint grid guide line at center
    Gdiplus::Pen gridPen(Gdiplus::Color(80, GetRValue(pal.graphGrid), GetGValue(pal.graphGrid), GetBValue(pal.graphGrid)), 1.0f);
    gridPen.SetDashStyle(Gdiplus::DashStyleDash);
    g.DrawLine(&gridPen, graphX + 4.0f, graphY + graphH / 2.0f, graphX + graphW - 4.0f, graphY + graphH / 2.0f);

    // Draw sparkline curve if points exist
    const auto& hist = it.latency_history;
    if (!hist.empty()) {
        float pLeft = graphX + 6.0f;
        float pRight = graphX + graphW - 6.0f;
        float pTop = graphY + 5.0f;
        float pBottom = graphY + graphH - 5.0f;
        float pWidth = pRight - pLeft;
        float pHeight = pBottom - pTop;

        int maxMs = 80;
        for (int v : hist) {
            if (v > maxMs) maxMs = std::min(300, v + 20);
        }

        std::vector<Gdiplus::PointF> pts;
        pts.reserve(hist.size());
        size_t n = hist.size();

        for (size_t k = 0; k < n; ++k) {
            int val = hist[k];
            float px = (n == 1) ? (pLeft + pWidth / 2.0f) : (pLeft + static_cast<float>(k) * pWidth / static_cast<float>(n - 1));
            float py = (val <= 0) ? pBottom : (pBottom - (static_cast<float>(std::min(val, maxMs)) * pHeight / static_cast<float>(maxMs)));
            pts.push_back(Gdiplus::PointF(px, py));
        }

        if (pts.size() >= 2) {
            // Area polygon under curve
            std::vector<Gdiplus::PointF> poly = pts;
            poly.push_back(Gdiplus::PointF(pts.back().X, pBottom));
            poly.push_back(Gdiplus::PointF(pts.front().X, pBottom));
            Gdiplus::SolidBrush fillBr(Gdiplus::Color(45, dotColor.GetR(), dotColor.GetG(), dotColor.GetB()));
            g.FillPolygon(&fillBr, poly.data(), static_cast<int>(poly.size()));

            // Anti-aliased line
            Gdiplus::Pen curvePen(dotColor, 1.8f);
            curvePen.SetLineJoin(Gdiplus::LineJoinRound);
            curvePen.SetStartCap(Gdiplus::LineCapRound);
            curvePen.SetEndCap(Gdiplus::LineCapRound);
            g.DrawLines(&curvePen, pts.data(), static_cast<int>(pts.size()));
        }

        // Indicator dot on last point
        Gdiplus::PointF lastPt = pts.back();
        Gdiplus::SolidBrush dotHalo(Gdiplus::Color(70, dotColor.GetR(), dotColor.GetG(), dotColor.GetB()));
        g.FillEllipse(&dotHalo, lastPt.X - 4.5f, lastPt.Y - 4.5f, 9.0f, 9.0f);
        Gdiplus::SolidBrush dotSolid(dotColor);
        g.FillEllipse(&dotSolid, lastPt.X - 2.5f, lastPt.Y - 2.5f, 5.0f, 5.0f);
        Gdiplus::Pen ringPen(Gdiplus::Color(255, 255, 255, 255), 1.0f);
        g.DrawEllipse(&ringPen, lastPt.X - 2.5f, lastPt.Y - 2.5f, 5.0f, 5.0f);
    }
}

LRESULT CALLBACK BenchmarkDialog::dialog_proc_thunk(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    BenchmarkDialog* pThis = nullptr;
    if (uMsg == WM_NCCREATE) {
        auto* pCreate = reinterpret_cast<CREATESTRUCTW*>(lParam);
        pThis = reinterpret_cast<BenchmarkDialog*>(pCreate->lpCreateParams);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pThis));
        pThis->m_hWnd = hWnd;
    } else {
        pThis = reinterpret_cast<BenchmarkDialog*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    }

    if (pThis) {
        return pThis->handle_message(hWnd, uMsg, wParam, lParam);
    }
    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

LRESULT BenchmarkDialog::handle_message(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_APP_BENCHMARK_PING_UPDATE: {
        if (m_hListWnd && IsWindow(m_hListWnd)) {
            InvalidateRect(m_hListWnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (id == IDC_BTN_BENCHMARK_APPLY) {
            on_apply();
            return 0;
        }
        if (id == IDC_BTN_BENCHMARK_PAUSE) {
            toggle_pause();
            return 0;
        }
        if (id == IDC_BTN_BENCHMARK_CLOSE || id == IDCANCEL) {
            on_close();
            return 0;
        }
        break;
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
        HDC hdc = (HDC)wParam;
        auto pal = GetPalette(m_isDarkMode);
        SetTextColor(hdc, pal.textPrimary);
        SetBkMode(hdc, TRANSPARENT);
        return (INT_PTR)m_hbrBg;
    }

    case WM_CLOSE: {
        on_close();
        return 0;
    }
    }

    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

LRESULT CALLBACK BenchmarkDialog::list_proc_thunk(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    BenchmarkDialog* pThis = nullptr;
    if (uMsg == WM_NCCREATE) {
        auto* pCreate = reinterpret_cast<CREATESTRUCTW*>(lParam);
        pThis = reinterpret_cast<BenchmarkDialog*>(pCreate->lpCreateParams);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pThis));
    } else {
        pThis = reinterpret_cast<BenchmarkDialog*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    }

    if (pThis) {
        return pThis->handle_list_message(hWnd, uMsg, wParam, lParam);
    }
    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

LRESULT BenchmarkDialog::handle_list_message(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_ERASEBKGND:
        return 1; // Handled in WM_PAINT double buffer

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

        auto pal = GetPalette(m_isDarkMode);

        // Fill background
        HBRUSH hbrBg = CreateSolidBrush(pal.bg);
        FillRect(hdcMem, &rc, hbrBg);
        DeleteObject(hbrBg);

        {
            Gdiplus::Graphics g(hdcMem);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
            g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

            std::lock_guard<std::mutex> lock(m_itemsMutex);
            int rowHeight = 48;
            int rowGap = 6;
            int rowStep = rowHeight + rowGap;
            int totalItems = static_cast<int>(m_items.size());
            int totalContentHeight = totalItems * rowStep;

            // Configure scrollbar
            SCROLLINFO si = { sizeof(SCROLLINFO) };
            si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
            si.nMin = 0;
            si.nMax = totalContentHeight;
            si.nPage = h;
            si.nPos = m_scrollOffset;
            SetScrollInfo(hWnd, SB_VERT, &si, TRUE);

            int itemW = w;
            if (totalContentHeight > h) {
                itemW = w - 4;
            }

            for (int i = 0; i < totalItems; ++i) {
                int y = i * rowStep - m_scrollOffset;
                if (y + rowHeight < 0 || y > h) continue;

                bool isSelected = (i == m_selectedIndex);
                bool isHovered = (i == m_hoveredIndex);
                draw_list_item(g, i, y, itemW, rowHeight, isSelected, isHovered);
            }
        }

        BitBlt(hdc, 0, 0, w, h, hdcMem, 0, 0, SRCCOPY);
        SelectObject(hdcMem, oldBmp);
        DeleteObject(hbmMem);
        DeleteDC(hdcMem);
        EndPaint(hWnd, &ps);
        return 0;
    }

    case WM_MOUSEMOVE: {
        int my = GET_Y_LPARAM(lParam) + m_scrollOffset;
        int idx = my / 54;
        if (idx >= 0 && idx < static_cast<int>(m_items.size())) {
            if (idx != m_hoveredIndex) {
                m_hoveredIndex = idx;
                InvalidateRect(hWnd, nullptr, FALSE);
            }
        } else if (m_hoveredIndex != -1) {
            m_hoveredIndex = -1;
            InvalidateRect(hWnd, nullptr, FALSE);
        }

        TRACKMOUSEEVENT tme = { sizeof(TRACKMOUSEEVENT), TME_LEAVE, hWnd, 0 };
        TrackMouseEvent(&tme);
        break;
    }

    case WM_MOUSELEAVE: {
        m_hoveredIndex = -1;
        InvalidateRect(hWnd, nullptr, FALSE);
        break;
    }

    case WM_LBUTTONDOWN: {
        SetFocus(hWnd);
        int my = GET_Y_LPARAM(lParam) + m_scrollOffset;
        int idx = my / 54;
        if (idx >= 0 && idx < static_cast<int>(m_items.size())) {
            m_selectedIndex = idx;
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        break;
    }

    case WM_LBUTTONDBLCLK: {
        int my = GET_Y_LPARAM(lParam) + m_scrollOffset;
        int idx = my / 54;
        if (idx >= 0 && idx < static_cast<int>(m_items.size())) {
            m_selectedIndex = idx;
            on_apply();
        }
        break;
    }

    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        int step = 54;
        RECT rc;
        GetClientRect(hWnd, &rc);
        int totalH = static_cast<int>(m_items.size()) * 54;
        int maxScroll = std::max<int>(0, totalH - static_cast<int>(rc.bottom - rc.top));
        m_scrollOffset -= (delta / WHEEL_DELTA) * step;
        m_scrollOffset = std::clamp(m_scrollOffset, 0, maxScroll);
        InvalidateRect(hWnd, nullptr, FALSE);
        return 0;
    }

    case WM_VSCROLL: {
        RECT rc;
        GetClientRect(hWnd, &rc);
        int totalH = static_cast<int>(m_items.size()) * 54;
        int maxScroll = std::max<int>(0, totalH - static_cast<int>(rc.bottom - rc.top));

        SCROLLINFO si = { sizeof(SCROLLINFO) };
        si.fMask = SIF_ALL;
        GetScrollInfo(hWnd, SB_VERT, &si);

        int pos = m_scrollOffset;
        switch (LOWORD(wParam)) {
        case SB_LINEUP: pos -= 54; break;
        case SB_LINEDOWN: pos += 54; break;
        case SB_PAGEUP: pos -= (rc.bottom - rc.top); break;
        case SB_PAGEDOWN: pos += (rc.bottom - rc.top); break;
        case SB_THUMBTRACK: pos = HIWORD(wParam); break;
        }

        m_scrollOffset = std::clamp(pos, 0, maxScroll);
        InvalidateRect(hWnd, nullptr, FALSE);
        return 0;
    }

    case WM_KEYDOWN: {
        if (wParam == VK_UP) {
            m_selectedIndex = std::max(0, m_selectedIndex - 1);
            int targetY = m_selectedIndex * 54;
            if (targetY < m_scrollOffset) m_scrollOffset = targetY;
            InvalidateRect(hWnd, nullptr, FALSE);
            return 0;
        }
        if (wParam == VK_DOWN) {
            m_selectedIndex = std::min(static_cast<int>(m_items.size()) - 1, m_selectedIndex + 1);
            RECT rc; GetClientRect(hWnd, &rc);
            int targetY = (m_selectedIndex + 1) * 54;
            if (targetY > m_scrollOffset + (rc.bottom - rc.top)) {
                m_scrollOffset = targetY - (rc.bottom - rc.top);
            }
            InvalidateRect(hWnd, nullptr, FALSE);
            return 0;
        }
        if (wParam == VK_RETURN) {
            on_apply();
            return 0;
        }
        if (wParam == VK_ESCAPE) {
            on_close();
            return 0;
        }
        break;
    }

    case WM_SETCURSOR: {
        SetCursor(LoadCursorW(nullptr, IDC_HAND));
        return TRUE;
    }
    }

    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

} // namespace quickdns
