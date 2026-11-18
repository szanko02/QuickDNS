#include "dialog_benchmark.h"
#include "encoding_utils.h"
#include "resource.h"
#include "ui_palette.h"
#include <dwmapi.h>
#include <windowsx.h>
#include <commctrl.h>
#include <thread>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <sstream>

#ifdef _MSC_VER
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdiplus.lib")
#endif

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

using QuickDns::Encoding::wide_to_utf8;
using QuickDns::Encoding::utf8_to_wide;

namespace {
    constexpr const wchar_t* DIALOG_CLASS_NAME = L"QuickDNS_BenchmarkDialogClass";
    constexpr const wchar_t* LIST_CLASS_NAME   = L"QuickDNS_BenchmarkListClass";

    constexpr int DIALOG_CLIENT_WIDTH  = 900;
    constexpr int DIALOG_CLIENT_HEIGHT = 720;

    constexpr int ROW_HEIGHT_BASE = 48;
    constexpr int ROW_GAP_BASE    = 6;

    constexpr UINT WM_APP_BENCHMARK_PING_UPDATE  = WM_APP + 20;
    constexpr UINT WM_APP_BENCHMARK_TIMED_FINISH = WM_APP + 21;
    constexpr UINT_PTR ID_TIMER_BENCHMARK_CLOCK  = 5001;

    constexpr int IDC_EDIT_BENCHMARK_DOMAIN       = 1210;
    constexpr int IDC_BTN_BENCHMARK_AI_PRESETS    = 1211;
    constexpr int IDC_BTN_FILTER_ALL              = 1212;
    constexpr int IDC_BTN_FILTER_SMART            = 1213;
    constexpr int IDC_BTN_DUR_10                  = 1214;
    constexpr int IDC_BTN_DUR_30                  = 1215;
    constexpr int IDC_BTN_DUR_60                  = 1216;
    constexpr int IDC_BTN_DUR_CONT                = 1217;
    constexpr int IDC_BTN_BENCHMARK_START_STOP    = 1218;
    constexpr int IDC_BTN_BENCHMARK_RESET         = 1219;
    constexpr int IDC_BTN_BENCHMARK_APPLY_SMART   = 1220;
    constexpr int IDC_BTN_BENCHMARK_PAUSE         = 1221;

    constexpr int IDM_PRESET_BASE = 3100;

    struct PresetEntry {
        int id;
        const wchar_t* label;
        const char* domain;
    };

    const PresetEntry AI_PRESETS[] = {
        { IDM_PRESET_BASE + 0, L"ChatGPT (chatgpt.com)", "chatgpt.com" },
        { IDM_PRESET_BASE + 1, L"Claude (claude.ai)", "claude.ai" },
        { IDM_PRESET_BASE + 2, L"Gemini (gemini.google.com)", "gemini.google.com" },
        { IDM_PRESET_BASE + 3, L"Microsoft Copilot (copilot.microsoft.com)", "copilot.microsoft.com" },
        { IDM_PRESET_BASE + 4, L"Perplexity AI (perplexity.ai)", "perplexity.ai" },
        { IDM_PRESET_BASE + 5, L"DeepSeek (deepseek.com)", "deepseek.com" },
        { IDM_PRESET_BASE + 6, L"Grok (grok.com)", "grok.com" },
        { IDM_PRESET_BASE + 7, L"Midjourney (midjourney.com)", "midjourney.com" },
        { IDM_PRESET_BASE + 8, L"Poe (poe.com)", "poe.com" },
        { IDM_PRESET_BASE + 9, L"Sora (sora.com)", "sora.com" },
        { IDM_PRESET_BASE + 10, L"NotebookLM (notebooklm.google.com)", "notebooklm.google.com" },
        { 0, nullptr, nullptr },
        { IDM_PRESET_BASE + 20, L"YouTube (youtube.com)", "youtube.com" },
        { IDM_PRESET_BASE + 21, L"Discord (discord.com)", "discord.com" },
        { IDM_PRESET_BASE + 22, L"Steam (steamcommunity.com)", "steamcommunity.com" },
        { IDM_PRESET_BASE + 23, L"Twitch (twitch.tv)", "twitch.tv" },
        { IDM_PRESET_BASE + 24, L"Google (google.com)", "google.com" },
        { IDM_PRESET_BASE + 25, L"GitHub (github.com)", "github.com" },
        { IDM_PRESET_BASE + 26, L"Roblox (roblox.com)", "roblox.com" },
        { IDM_PRESET_BASE + 27, L"Yandex (ya.ru)", "ya.ru" }
    };

    std::string select_profile_target_ip(const DnsProfile& p) {
        if (!p.primary_ipv4.empty()) return p.primary_ipv4;
        if (!p.primary_ipv6.empty()) return p.primary_ipv6;
        if (!p.secondary_ipv4.empty()) return p.secondary_ipv4;
        if (!p.secondary_ipv6.empty()) return p.secondary_ipv6;
        return {};
    }

    UINT query_window_dpi(HWND hWnd) {
        using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
        using GetDpiForSystemFn = UINT(WINAPI*)(void);
        static HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"
#endif
        static auto pfnGetDpiForWindow = hUser32
            ? reinterpret_cast<GetDpiForWindowFn>(GetProcAddress(hUser32, "GetDpiForWindow"))
            : nullptr;
        static auto pfnGetDpiForSystem = hUser32
            ? reinterpret_cast<GetDpiForSystemFn>(GetProcAddress(hUser32, "GetDpiForSystem"))
            : nullptr;
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
        if (hWnd && IsWindow(hWnd) && pfnGetDpiForWindow) {
            UINT dpi = pfnGetDpiForWindow(hWnd);
            if (dpi > 0) return dpi;
        }
        if (pfnGetDpiForSystem) {
            UINT dpi = pfnGetDpiForSystem();
            if (dpi > 0) return dpi;
        }
        HDC hdc = GetDC(hWnd);
        UINT dpi = hdc ? static_cast<UINT>(GetDeviceCaps(hdc, LOGPIXELSY)) : 96u;
        if (hdc) ReleaseDC(hWnd, hdc);
        return dpi > 0 ? dpi : 96u;
    }

    BOOL adjust_window_rect_for_dpi(LPRECT lpRect, DWORD dwStyle, BOOL bMenu, DWORD dwExStyle, UINT dpi) {
        using AdjustWindowRectExForDpiFn = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
        static HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"
#endif
        static auto pfnAdjustForDpi = hUser32
            ? reinterpret_cast<AdjustWindowRectExForDpiFn>(GetProcAddress(hUser32, "AdjustWindowRectExForDpi"))
            : nullptr;
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
        if (pfnAdjustForDpi) {
            return pfnAdjustForDpi(lpRect, dwStyle, bMenu, dwExStyle, dpi);
        }
        return AdjustWindowRectEx(lpRect, dwStyle, bMenu, dwExStyle);
    }

    RECT get_monitor_work_area(HWND hRefWnd) {
        HMONITOR hMon = (hRefWnd && IsWindow(hRefWnd))
            ? MonitorFromWindow(hRefWnd, MONITOR_DEFAULTTONEAREST)
            : MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (hMon && GetMonitorInfoW(hMon, &mi)) {
            return mi.rcWork;
        }
        RECT rcFallback = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
        return rcFallback;
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
        case WM_ENABLE: {
            anim->hover = 0.0f;
            anim->targetHover = 0.0f;
            anim->isTracking = false;
            anim->isPressed = false;
            KillTimer(hWnd, 3001);
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        }
        case WM_MOUSEMOVE: {
            if (!IsWindowEnabled(hWnd)) break;
            if (!anim->isTracking) {
                anim->isTracking = true;
                anim->targetHover = 1.0f;
                TRACKMOUSEEVENT tme{};
                tme.cbSize = sizeof(TRACKMOUSEEVENT);
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = hWnd;
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
            if (!IsWindowEnabled(hWnd)) break;
            anim->isPressed = true;
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        }
        case WM_LBUTTONUP:
        case WM_CAPTURECHANGED: {
            if (anim->isPressed) {
                anim->isPressed = false;
                InvalidateRect(hWnd, nullptr, FALSE);
            }
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

int BenchmarkDialog::dp(int px) const {
    return MulDiv(px, static_cast<int>(m_dpi), 96);
}

float BenchmarkDialog::dpf(float px) const {
    return px * (static_cast<float>(m_dpi) / 96.0f);
}

int BenchmarkDialog::get_row_height() const {
    return dp(ROW_HEIGHT_BASE);
}

int BenchmarkDialog::get_row_gap() const {
    return dp(ROW_GAP_BASE);
}

int BenchmarkDialog::get_row_step() const {
    return get_row_height() + get_row_gap();
}

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

    if (hParent && IsWindow(hParent)) {
        EnableWindow(hParent, FALSE);
    }

    dlg.start_ping_worker();

    MSG msg{};
    BOOL bRet = 1;
    while (!dlg.m_finished && (bRet = GetMessageW(&msg, nullptr, 0, 0)) != 0) {
        if (bRet == -1) {
            break;
        }
        if (!dlg.m_hWnd || !IsDialogMessageW(dlg.m_hWnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (!dlg.m_finished && bRet == 0) {
        PostQuitMessage(static_cast<int>(msg.wParam));
    }

    if (dlg.m_hWnd && IsWindow(dlg.m_hWnd)) {
        ShowWindow(dlg.m_hWnd, SW_HIDE);
    }

    if (hParent && IsWindow(hParent)) {
        EnableWindow(hParent, TRUE);
        if (IsWindowVisible(hParent)) {
            SetForegroundWindow(hParent);
        }
    }

    dlg.stop_ping_worker();

    if (dlg.m_hWnd && IsWindow(dlg.m_hWnd)) {
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
    , m_isDarkMode(isDarkMode)
    , m_currentActiveId(currentActiveId)
{
    m_dpi = query_window_dpi(m_hParent);
    auto pal = GetPalette(m_isDarkMode);
    m_hbrBg = CreateSolidBrush(pal.bg);
    m_hbrCard = CreateSolidBrush(pal.cardBg);

    for (const auto& p : profiles) {
        BenchmarkItem item;
        item.profile = p;
        item.doh_template = p.doh_template;
        if (p.is_dhcp) {
            item.current_ping = 0;
            item.status_text = L"DHCP (Авто)";
            item.quality_label = L"Провайдер";
            item.quality = DnsQuality::Fast;
        } else {
            item.status_text = L"Ожидание...";
            item.quality_label = L"Ожидание";
            item.quality = DnsQuality::Moderate;
        }
        m_items.push_back(std::move(item));
    }

    update_filtered_indices_locked();

    for (size_t i = 0; i < m_items.size(); ++i) {
        if (m_items[i].profile.id == m_currentActiveId) {
            for (size_t f = 0; f < m_filteredIndices.size(); ++f) {
                if (m_filteredIndices[f] == i) {
                    m_selectedIndex = static_cast<int>(f);
                    break;
                }
            }
            break;
        }
    }

    setup_fonts();
}

BenchmarkDialog::~BenchmarkDialog() {
    stop_ping_worker();

    m_gpFontPing.reset();
    m_gpFontBadge.reset();
    m_gpFontSub.reset();
    m_gpFontTitle.reset();
    m_gpHeaderBold.reset();
    m_gpHeaderSub.reset();
    m_gpHeaderTitle.reset();
    m_gpMonoFamily.reset();
    m_gpDisplayFamily.reset();
    m_gpFontFamily.reset();

    if (m_hFontRegular) DeleteObject(m_hFontRegular);
    if (m_hFontBold) DeleteObject(m_hFontBold);
    if (m_hFontHeader) DeleteObject(m_hFontHeader);
    if (m_hFontMonospace) DeleteObject(m_hFontMonospace);
    if (m_hFontSmall) DeleteObject(m_hFontSmall);
    if (m_hFontSmallBold) DeleteObject(m_hFontSmallBold);

    if (m_hbrBg) DeleteObject(m_hbrBg);
    if (m_hbrCard) DeleteObject(m_hbrCard);

    UnregisterClassW(DIALOG_CLASS_NAME, m_hInstance);
    UnregisterClassW(LIST_CLASS_NAME, m_hInstance);
}

void BenchmarkDialog::setup_fonts() {
    m_gpFontPing.reset();
    m_gpFontBadge.reset();
    m_gpFontSub.reset();
    m_gpFontTitle.reset();
    m_gpHeaderBold.reset();
    m_gpHeaderSub.reset();
    m_gpHeaderTitle.reset();
    m_gpMonoFamily.reset();
    m_gpDisplayFamily.reset();
    m_gpFontFamily.reset();

    if (m_hFontRegular) { DeleteObject(m_hFontRegular); m_hFontRegular = nullptr; }
    if (m_hFontBold) { DeleteObject(m_hFontBold); m_hFontBold = nullptr; }
    if (m_hFontHeader) { DeleteObject(m_hFontHeader); m_hFontHeader = nullptr; }
    if (m_hFontMonospace) { DeleteObject(m_hFontMonospace); m_hFontMonospace = nullptr; }
    if (m_hFontSmall) { DeleteObject(m_hFontSmall); m_hFontSmall = nullptr; }
    if (m_hFontSmallBold) { DeleteObject(m_hFontSmallBold); m_hFontSmallBold = nullptr; }

    m_gpFontFamily = std::make_unique<Gdiplus::FontFamily>(L"Segoe UI");
    m_gpDisplayFamily = std::make_unique<Gdiplus::FontFamily>(L"Segoe UI Variable Display");
    m_gpMonoFamily = std::make_unique<Gdiplus::FontFamily>(L"Consolas");

    const Gdiplus::FontFamily* pRegular = m_gpFontFamily->IsAvailable()
        ? m_gpFontFamily.get()
        : Gdiplus::FontFamily::GenericSansSerif();
    const Gdiplus::FontFamily* pDisplay = m_gpDisplayFamily->IsAvailable()
        ? m_gpDisplayFamily.get()
        : pRegular;
    const Gdiplus::FontFamily* pMono = m_gpMonoFamily->IsAvailable()
        ? m_gpMonoFamily.get()
        : pRegular;
    const wchar_t* headerFace = m_gpDisplayFamily->IsAvailable()
        ? L"Segoe UI Variable Display"
        : L"Segoe UI";

    m_hFontRegular = CreateFontW(-dp(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    m_hFontBold = CreateFontW(-dp(14), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    m_hFontHeader = CreateFontW(-dp(18), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, headerFace);

    m_hFontMonospace = CreateFontW(-dp(13), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");

    m_hFontSmall = CreateFontW(-dp(11), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    m_hFontSmallBold = CreateFontW(-dp(11), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    m_gpHeaderTitle = std::make_unique<Gdiplus::Font>(pDisplay, dpf(18.0f), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    m_gpHeaderSub   = std::make_unique<Gdiplus::Font>(pRegular, dpf(12.0f), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    m_gpHeaderBold  = std::make_unique<Gdiplus::Font>(pRegular, dpf(12.0f), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);

    m_gpFontTitle = std::make_unique<Gdiplus::Font>(pRegular, dpf(13.3f), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    m_gpFontSub   = std::make_unique<Gdiplus::Font>(pRegular, dpf(11.3f), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    m_gpFontBadge = std::make_unique<Gdiplus::Font>(pRegular, dpf(10.7f), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    m_gpFontPing  = std::make_unique<Gdiplus::Font>(pMono,    dpf(14.0f), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
}

void BenchmarkDialog::apply_fonts_to_controls() {
    if (m_hEditDomain && IsWindow(m_hEditDomain)) {
        SendMessageW(m_hEditDomain, WM_SETFONT, reinterpret_cast<WPARAM>(m_hFontRegular), TRUE);
    }
    HWND btns[] = {
        m_hBtnAiPresets, m_hBtnFilterAll, m_hBtnFilterSmart,
        m_hBtnDur10, m_hBtnDur30, m_hBtnDur60, m_hBtnDurCont,
        m_hBtnStartStop, m_hBtnReset, m_hBtnApply, m_hBtnApplySmart,
        m_hBtnPauseResume, m_hBtnClose
    };
    for (HWND hBtn : btns) {
        if (hBtn && IsWindow(hBtn)) {
            InvalidateRect(hBtn, nullptr, FALSE);
        }
    }
}

void BenchmarkDialog::update_filtered_indices_locked() {
    m_filteredIndices.clear();
    for (size_t i = 0; i < m_items.size(); ++i) {
        if (!m_filterOnlySmartDns || m_items[i].profile.is_smart_dns) {
            m_filteredIndices.push_back(i);
        }
    }
    if (m_filteredIndices.empty()) {
        m_selectedIndex = -1;
    } else if (m_selectedIndex < 0 || m_selectedIndex >= static_cast<int>(m_filteredIndices.size())) {
        m_selectedIndex = 0;
    }
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
    wc.hbrBackground = nullptr;
    wc.lpszClassName = DIALOG_CLASS_NAME;
    RegisterClassExW(&wc);

    WNDCLASSEXW lwc = {};
    lwc.cbSize = sizeof(WNDCLASSEXW);
    lwc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    lwc.lpfnWndProc = &BenchmarkDialog::list_proc_thunk;
    lwc.hInstance = m_hInstance;
    lwc.hCursor = LoadCursorW(nullptr, IDC_HAND);
    lwc.hbrBackground = nullptr;
    lwc.lpszClassName = LIST_CLASS_NAME;
    RegisterClassExW(&lwc);

    DWORD dwStyle = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MAXIMIZEBOX | WS_VISIBLE;
    DWORD dwExStyle = WS_EX_DLGMODALFRAME;
    RECT wr = { 0, 0, dp(DIALOG_CLIENT_WIDTH), dp(DIALOG_CLIENT_HEIGHT) };
    adjust_window_rect_for_dpi(&wr, dwStyle, FALSE, dwExStyle, m_dpi);
    int winWidth = wr.right - wr.left;
    int winHeight = wr.bottom - wr.top;

    RECT rcWork = get_monitor_work_area(m_hParent);
    int workW = std::max(320, static_cast<int>(rcWork.right - rcWork.left));
    int workH = std::max(280, static_cast<int>(rcWork.bottom - rcWork.top));
    winWidth = std::min(winWidth, workW);
    winHeight = std::min(winHeight, workH);

    int posX = rcWork.left + (workW - winWidth) / 2;
    int posY = rcWork.top + (workH - winHeight) / 2;
    RECT parentRect = {};
    if (m_hParent && IsWindow(m_hParent) && !IsIconic(m_hParent) && IsWindowVisible(m_hParent) &&
        GetWindowRect(m_hParent, &parentRect)) {
        posX = parentRect.left + ((parentRect.right - parentRect.left) - winWidth) / 2;
        posY = parentRect.top + ((parentRect.bottom - parentRect.top) - winHeight) / 2;
    }
    posX = std::clamp(posX, static_cast<int>(rcWork.left), std::max(static_cast<int>(rcWork.left), static_cast<int>(rcWork.right) - winWidth));
    posY = std::clamp(posY, static_cast<int>(rcWork.top), std::max(static_cast<int>(rcWork.top), static_cast<int>(rcWork.bottom) - winHeight));

    m_hWnd = CreateWindowExW(
        dwExStyle,
        DIALOG_CLASS_NAME,
        L"Тест скорости и стабильности DNS",
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

    UINT actualDpi = query_window_dpi(m_hWnd);
    if (actualDpi != m_dpi) {
        m_dpi = actualDpi;
        setup_fonts();
    }

    apply_theme();
    create_controls();
    return true;
}

void BenchmarkDialog::apply_theme() {
    BOOL useDarkMode = m_isDarkMode ? TRUE : FALSE;
    DwmSetWindowAttribute(m_hWnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &useDarkMode, sizeof(useDarkMode));
    DwmSetWindowAttribute(m_hWnd, 19, &useDarkMode, sizeof(useDarkMode));
    DWORD cornerPref = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(m_hWnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &cornerPref, sizeof(cornerPref));
}

LRESULT CALLBACK BenchmarkDialog::domain_edit_subclass(
    HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
    UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
    auto* pDlg = reinterpret_cast<BenchmarkDialog*>(dwRefData);
    if (!pDlg) return DefSubclassProc(hWnd, uMsg, wParam, lParam);

    auto invalidateDomBox = [pDlg]() {
        if (pDlg->m_hWnd && IsWindow(pDlg->m_hWnd)) {
            RECT rcInv = pDlg->m_domainBoxRect;
            InflateRect(&rcInv, 2, 2);
            InvalidateRect(pDlg->m_hWnd, &rcInv, FALSE);
        }
    };

    switch (uMsg) {
    case WM_SETFOCUS: {
        pDlg->m_domainEditFocused = true;
        invalidateDomBox();
        break;
    }
    case WM_KILLFOCUS: {
        pDlg->m_domainEditFocused = false;
        invalidateDomBox();
        break;
    }
    case WM_MOUSEMOVE: {
        if (!pDlg->m_domainEditHovered) {
            pDlg->m_domainEditHovered = true;
            TRACKMOUSEEVENT tme{};
            tme.cbSize = sizeof(TRACKMOUSEEVENT);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hWnd;
            TrackMouseEvent(&tme);
            invalidateDomBox();
        }
        break;
    }
    case WM_MOUSELEAVE: {
        pDlg->m_domainEditHovered = false;
        invalidateDomBox();
        break;
    }
    case WM_CHAR: {
        if (wParam == VK_RETURN) {
            pDlg->start_timed_test();
            return 0;
        }
        break;
    }
    case WM_NCDESTROY: {
        RemoveWindowSubclass(hWnd, domain_edit_subclass, uIdSubclass);
        return 0;
    }
    }
    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

void BenchmarkDialog::create_controls() {
    RECT rcClient = {};
    GetClientRect(m_hWnd, &rcClient);
    int clientWidth = rcClient.right - rcClient.left;
    int clientHeight = rcClient.bottom - rcClient.top;

    int marginX = dp(20);
    int contentWidth = std::max(dp(300), clientWidth - (marginX * 2));

    int row1Y = dp(64);
    int editDomX = marginX + dp(46);
    int editDomW = dp(190);
    m_domainBoxRect = { editDomX, row1Y - dp(1), editDomX + editDomW, row1Y - dp(1) + dp(28) };

    m_hEditDomain = CreateWindowExW(0, WC_EDITW, L"google.com",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        editDomX + dp(8), row1Y + dp(3), editDomW - dp(16), dp(20),
        m_hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EDIT_BENCHMARK_DOMAIN)), m_hInstance, nullptr);
    SendMessageW(m_hEditDomain, WM_SETFONT, reinterpret_cast<WPARAM>(m_hFontRegular), TRUE);
    SendMessageW(m_hEditDomain, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Введите домен..."));
    SetWindowSubclass(m_hEditDomain, domain_edit_subclass, 210, reinterpret_cast<DWORD_PTR>(this));

    int btnAiX = editDomX + editDomW + dp(6);
    int btnAiW = dp(150);
    m_hBtnAiPresets = CreateWindowExW(0, WC_BUTTONW, L"Нейросети и сайты \u25BE",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        btnAiX, row1Y - dp(1), btnAiW, dp(28),
        m_hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_BENCHMARK_AI_PRESETS)), m_hInstance, nullptr);
    attach_button_effects(m_hBtnAiPresets, 211);

    int chipFilterSmartW = dp(125);
    int chipFilterAllW = dp(85);
    int chipFilterSmartX = marginX + contentWidth - chipFilterSmartW;
    int chipFilterAllX = chipFilterSmartX - chipFilterAllW - dp(6);

    m_hBtnFilterAll = CreateWindowExW(0, WC_BUTTONW, L"Все DNS",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        chipFilterAllX, row1Y - dp(1), chipFilterAllW, dp(28),
        m_hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_FILTER_ALL)), m_hInstance, nullptr);
    attach_button_effects(m_hBtnFilterAll, 212);

    m_hBtnFilterSmart = CreateWindowExW(0, WC_BUTTONW, L"Только SmartDNS",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        chipFilterSmartX, row1Y - dp(1), chipFilterSmartW, dp(28),
        m_hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_FILTER_SMART)), m_hInstance, nullptr);
    attach_button_effects(m_hBtnFilterSmart, 213);

    int row2Y = dp(108);
    int chipDur10W   = dp(55);
    int chipDur30W   = dp(55);
    int chipDur60W   = dp(55);
    int chipDurContW = dp(95);
    int durGap       = dp(4);

    int dur10X   = marginX + dp(46);
    int dur30X   = dur10X + chipDur10W + durGap;
    int dur60X   = dur30X + chipDur30W + durGap;
    int durContX = dur60X + chipDur60W + durGap;

    m_hBtnDur10 = CreateWindowExW(0, WC_BUTTONW, L"10 сек",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        dur10X, row2Y, chipDur10W, dp(28),
        m_hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_DUR_10)), m_hInstance, nullptr);
    attach_button_effects(m_hBtnDur10, 214);

    m_hBtnDur30 = CreateWindowExW(0, WC_BUTTONW, L"30 сек",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        dur30X, row2Y, chipDur30W, dp(28),
        m_hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_DUR_30)), m_hInstance, nullptr);
    attach_button_effects(m_hBtnDur30, 215);

    m_hBtnDur60 = CreateWindowExW(0, WC_BUTTONW, L"60 сек",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        dur60X, row2Y, chipDur60W, dp(28),
        m_hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_DUR_60)), m_hInstance, nullptr);
    attach_button_effects(m_hBtnDur60, 216);

    m_hBtnDurCont = CreateWindowExW(0, WC_BUTTONW, L"Непрерывно",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        durContX, row2Y, chipDurContW, dp(28),
        m_hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_DUR_CONT)), m_hInstance, nullptr);
    attach_button_effects(m_hBtnDurCont, 217);

    int btnStartW = dp(145);
    int btnResetW = dp(105);
    int btnResetX = marginX + contentWidth - btnResetW;
    int btnStartX = btnResetX - btnStartW - dp(6);

    m_hBtnStartStop = CreateWindowExW(0, WC_BUTTONW, L"Старт (30 сек)",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        btnStartX, row2Y - dp(1), btnStartW, dp(30),
        m_hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_BENCHMARK_START_STOP)), m_hInstance, nullptr);
    attach_button_effects(m_hBtnStartStop, 218);

    m_hBtnReset = CreateWindowExW(0, WC_BUTTONW, L"Сбросить",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        btnResetX, row2Y - dp(1), btnResetW, dp(30),
        m_hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_BENCHMARK_RESET)), m_hInstance, nullptr);
    attach_button_effects(m_hBtnReset, 219);

    int listTop = dp(222);
    int bottomButtonsH = dp(38);
    int listBottomMargin = dp(16);
    int gapBelowList = dp(14);
    int listHeight = std::max(dp(100), clientHeight - listTop - bottomButtonsH - listBottomMargin - gapBelowList);

    m_hListWnd = CreateWindowExW(
        0,
        LIST_CLASS_NAME,
        L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        marginX, listTop, contentWidth, listHeight,
        m_hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LIST_BENCHMARK)), m_hInstance, this
    );

    int curY = listTop + listHeight + gapBelowList;

    int btnApplyW      = dp(210);
    int btnApplySmartW = dp(230);
    int btnPauseW      = dp(120);
    int btnCloseW      = dp(110);

    m_hBtnApply = CreateWindowExW(0, WC_BUTTONW, L"Применить выбранный DNS",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        marginX, curY, btnApplyW, bottomButtonsH,
        m_hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_BENCHMARK_APPLY)), m_hInstance, nullptr);

    m_hBtnApplySmart = CreateWindowExW(0, WC_BUTTONW, L"Применить лучший SmartDNS",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        marginX + btnApplyW + dp(8), curY, btnApplySmartW, bottomButtonsH,
        m_hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_BENCHMARK_APPLY_SMART)), m_hInstance, nullptr);

    m_hBtnPauseResume = CreateWindowExW(0, WC_BUTTONW, L"Пауза",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        marginX + btnApplyW + btnApplySmartW + dp(16), curY, btnPauseW, bottomButtonsH,
        m_hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_BENCHMARK_PAUSE)), m_hInstance, nullptr);

    m_hBtnClose = CreateWindowExW(0, WC_BUTTONW, L"Закрыть",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        marginX + contentWidth - btnCloseW, curY, btnCloseW, bottomButtonsH,
        m_hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_BENCHMARK_CLOSE)), m_hInstance, nullptr);

    attach_button_effects(m_hBtnApply, 101);
    attach_button_effects(m_hBtnApplySmart, 102);
    attach_button_effects(m_hBtnPauseResume, 103);
    attach_button_effects(m_hBtnClose, 104);

    reposition_controls();
    start_timed_test();
}

void BenchmarkDialog::reposition_controls() {
    if (!m_hWnd || !IsWindow(m_hWnd)) return;
    RECT rcClient = {};
    GetClientRect(m_hWnd, &rcClient);
    int clientWidth = rcClient.right - rcClient.left;
    int clientHeight = rcClient.bottom - rcClient.top;

    int marginX = dp(20);
    int contentWidth = clientWidth - (marginX * 2);
    if (contentWidth < dp(240) || clientHeight < dp(200)) return;

    int row1Y = dp(64);
    int chipFilterSmartW = dp(125);
    int chipFilterAllW   = dp(85);
    int chipFilterSmartX = marginX + contentWidth - chipFilterSmartW;
    int chipFilterAllX   = chipFilterSmartX - chipFilterAllW - dp(6);

    int editDomX = marginX + dp(46);
    int btnAiW   = dp(150);
    int availableEditW = (chipFilterAllX - dp(12)) - (editDomX + btnAiW + dp(6));
    int editDomW = std::clamp(availableEditW, dp(110), dp(260));
    m_domainBoxRect = { editDomX, row1Y - dp(1), editDomX + editDomW, row1Y - dp(1) + dp(28) };

    if (m_hEditDomain) {
        SetWindowPos(m_hEditDomain, nullptr,
            editDomX + dp(8), row1Y + dp(3), std::max(dp(60), editDomW - dp(16)), dp(20), SWP_NOZORDER);
    }
    int btnAiX = editDomX + editDomW + dp(6);
    if (m_hBtnAiPresets) {
        SetWindowPos(m_hBtnAiPresets, nullptr, btnAiX, row1Y - dp(1), btnAiW, dp(28), SWP_NOZORDER);
    }
    if (m_hBtnFilterAll) {
        SetWindowPos(m_hBtnFilterAll, nullptr, chipFilterAllX, row1Y - dp(1), chipFilterAllW, dp(28), SWP_NOZORDER);
    }
    if (m_hBtnFilterSmart) {
        SetWindowPos(m_hBtnFilterSmart, nullptr, chipFilterSmartX, row1Y - dp(1), chipFilterSmartW, dp(28), SWP_NOZORDER);
    }

    int row2Y        = dp(108);
    int chipDur10W   = dp(55);
    int chipDur30W   = dp(55);
    int chipDur60W   = dp(55);
    int chipDurContW = dp(95);
    int durGap       = dp(4);

    int dur10X   = marginX + dp(46);
    int dur30X   = dur10X + chipDur10W + durGap;
    int dur60X   = dur30X + chipDur30W + durGap;
    int durContX = dur60X + chipDur60W + durGap;

    if (m_hBtnDur10)   SetWindowPos(m_hBtnDur10, nullptr, dur10X, row2Y, chipDur10W, dp(28), SWP_NOZORDER);
    if (m_hBtnDur30)   SetWindowPos(m_hBtnDur30, nullptr, dur30X, row2Y, chipDur30W, dp(28), SWP_NOZORDER);
    if (m_hBtnDur60)   SetWindowPos(m_hBtnDur60, nullptr, dur60X, row2Y, chipDur60W, dp(28), SWP_NOZORDER);
    if (m_hBtnDurCont) SetWindowPos(m_hBtnDurCont, nullptr, durContX, row2Y, chipDurContW, dp(28), SWP_NOZORDER);

    int btnResetW = dp(105);
    int btnStartW = dp(145);
    int btnResetX = marginX + contentWidth - btnResetW;
    int btnStartX = btnResetX - btnStartW - dp(6);

    if (m_hBtnStartStop) SetWindowPos(m_hBtnStartStop, nullptr, btnStartX, row2Y - dp(1), btnStartW, dp(30), SWP_NOZORDER);
    if (m_hBtnReset)     SetWindowPos(m_hBtnReset, nullptr, btnResetX, row2Y - dp(1), btnResetW, dp(30), SWP_NOZORDER);

    int listTop          = dp(222);
    int bottomButtonsH   = dp(38);
    int listBottomMargin = dp(16);
    int gapBelowList     = dp(14);
    int listHeight       = std::max(dp(80), clientHeight - listTop - bottomButtonsH - listBottomMargin - gapBelowList);

    if (m_hListWnd) {
        SetWindowPos(m_hListWnd, nullptr, marginX, listTop, contentWidth, listHeight, SWP_NOZORDER);
    }

    int curY = listTop + listHeight + gapBelowList;

    int btnCloseW = dp(95);
    int btnPauseW = dp(105);
    int gap       = dp(6);
    int remainingActionW = std::max(dp(160), contentWidth - btnCloseW - btnPauseW - (3 * gap));
    int btnApplyW = remainingActionW / 2;
    int btnApplySmartW = remainingActionW - btnApplyW;

    if (m_hBtnApply)       SetWindowPos(m_hBtnApply, nullptr, marginX, curY, btnApplyW, bottomButtonsH, SWP_NOZORDER);
    if (m_hBtnApplySmart)  SetWindowPos(m_hBtnApplySmart, nullptr, marginX + btnApplyW + gap, curY, btnApplySmartW, bottomButtonsH, SWP_NOZORDER);
    if (m_hBtnPauseResume) SetWindowPos(m_hBtnPauseResume, nullptr, marginX + btnApplyW + gap + btnApplySmartW + gap, curY, btnPauseW, bottomButtonsH, SWP_NOZORDER);
    if (m_hBtnClose)       SetWindowPos(m_hBtnClose, nullptr, marginX + contentWidth - btnCloseW, curY, btnCloseW, bottomButtonsH, SWP_NOZORDER);

    InvalidateRect(m_hWnd, nullptr, FALSE);
    if (m_hListWnd) InvalidateRect(m_hListWnd, nullptr, TRUE);
}

void BenchmarkDialog::reset_all_stats() {
    {
        std::lock_guard<std::mutex> lock(m_itemsMutex);
        for (auto& it : m_items) {
            it.reset_stats();
        }
        m_fastestIndex = -1;
        m_mostStableIndex = -1;
        m_fastestSmartIndex = -1;
        m_mostStableSmartIndex = -1;
        m_verdictFastest.clear();
        m_verdictStable.clear();
        m_verdictSmartFastest.clear();
        m_verdictSmartStable.clear();
        m_timedFinished = false;
    }
    {
        std::lock_guard<std::mutex> tlock(m_timerMutex);
        auto now = std::chrono::steady_clock::now();
        m_timedStartTime = now;
        m_pauseStartTime = now;
    }
    m_elapsedSec.store(0, std::memory_order_release);

    if (m_hWnd && IsWindow(m_hWnd)) {
        InvalidateRect(m_hWnd, nullptr, FALSE);
    }
    if (m_hListWnd && IsWindow(m_hListWnd)) {
        InvalidateRect(m_hListWnd, nullptr, FALSE);
    }
}

void BenchmarkDialog::set_duration_mode(BenchmarkDurationMode mode) {
    m_durationMode = mode;
    int newTarget = 30;
    switch (mode) {
    case BenchmarkDurationMode::Timed10s:   newTarget = 10; break;
    case BenchmarkDurationMode::Timed60s:   newTarget = 60; break;
    case BenchmarkDurationMode::Continuous: newTarget = 0;  break;
    case BenchmarkDurationMode::Timed30s:
    default:
        newTarget = 30;
        break;
    }
    m_targetDurationSec.store(newTarget, std::memory_order_release);

    if (mode == BenchmarkDurationMode::Continuous && m_isTimedRunning.load(std::memory_order_acquire)) {
        m_isTimedRunning.store(false, std::memory_order_release);
        if (m_hWnd && IsWindow(m_hWnd)) {
            KillTimer(m_hWnd, ID_TIMER_BENCHMARK_CLOCK);
        }
    }

    if (!m_isTimedRunning.load(std::memory_order_acquire) && m_hBtnStartStop && IsWindow(m_hBtnStartStop)) {
        if (m_durationMode == BenchmarkDurationMode::Continuous) {
            SetWindowTextW(m_hBtnStartStop, L"Тест стабильности");
        } else {
            std::wstring label = L"Старт (" + std::to_wstring(newTarget) + L" сек)";
            SetWindowTextW(m_hBtnStartStop, label.c_str());
        }
    } else if (m_isTimedRunning.load(std::memory_order_acquire)) {
        update_timed_elapsed();
    }

    if (m_hWnd && IsWindow(m_hWnd)) {
        InvalidateRect(m_hWnd, nullptr, FALSE);
    }
}

void BenchmarkDialog::set_filter_smart_only(bool smartOnly) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(m_itemsMutex);
        if (m_filterOnlySmartDns != smartOnly) {
            m_filterOnlySmartDns = smartOnly;
            update_filtered_indices_locked();
            m_scrollOffset = 0;
            m_wheelAccum = 0;
            changed = true;
        }
    }
    if (changed) {
        if (m_hWnd && IsWindow(m_hWnd)) {
            InvalidateRect(m_hWnd, nullptr, FALSE);
        }
        if (m_hListWnd && IsWindow(m_hListWnd)) {
            InvalidateRect(m_hListWnd, nullptr, FALSE);
        }
    }
}

void BenchmarkDialog::show_ai_presets_menu() {
    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) return;

    for (const auto& entry : AI_PRESETS) {
        if (entry.id == 0) {
            AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
        } else {
            AppendMenuW(hMenu, MF_STRING, static_cast<UINT_PTR>(entry.id), entry.label);
        }
    }

    RECT rcBtn = {};
    GetWindowRect(m_hBtnAiPresets, &rcBtn);

    int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_TOPALIGN | TPM_LEFTALIGN,
        rcBtn.left, rcBtn.bottom + dp(2), 0, m_hWnd, nullptr);
    DestroyMenu(hMenu);

    if (cmd >= IDM_PRESET_BASE) {
        for (const auto& entry : AI_PRESETS) {
            if (entry.id == cmd && entry.domain) {
                SetWindowTextW(m_hEditDomain, utf8_to_wide(entry.domain).c_str());
                {
                    std::lock_guard<std::mutex> lock(m_domainMutex);
                    m_targetDomain = entry.domain;
                }
                start_timed_test();
                break;
            }
        }
    }
}

void BenchmarkDialog::start_timed_test() {
    if (m_hEditDomain && IsWindow(m_hEditDomain)) {
        wchar_t domBuf[256] = {};
        GetWindowTextW(m_hEditDomain, domBuf, _countof(domBuf));
        std::string rawDomain = wide_to_utf8(domBuf);
        {
            std::lock_guard<std::mutex> lock(m_domainMutex);
            m_targetDomain = DnsBenchmark::sanitize_domain(rawDomain);
        }
    }

    if (m_isPaused.load(std::memory_order_acquire)) {
        m_isPaused.store(false, std::memory_order_release);
        if (m_hBtnPauseResume && IsWindow(m_hBtnPauseResume)) {
            SetWindowTextW(m_hBtnPauseResume, L"Пауза");
        }
    }

    reset_all_stats();

    if (m_durationMode != BenchmarkDurationMode::Continuous) {
        m_isTimedRunning.store(true, std::memory_order_release);
        if (m_hWnd && IsWindow(m_hWnd)) {
            SetTimer(m_hWnd, ID_TIMER_BENCHMARK_CLOCK, 250, nullptr);
        }
        if (m_hBtnStartStop && IsWindow(m_hBtnStartStop)) {
            SetWindowTextW(m_hBtnStartStop, L"Остановить");
        }
    } else {
        m_isTimedRunning.store(false, std::memory_order_release);
        if (m_hWnd && IsWindow(m_hWnd)) {
            KillTimer(m_hWnd, ID_TIMER_BENCHMARK_CLOCK);
        }
        if (m_hBtnStartStop && IsWindow(m_hBtnStartStop)) {
            SetWindowTextW(m_hBtnStartStop, L"Тест стабильности");
        }
    }

    if (m_hWnd && IsWindow(m_hWnd)) {
        InvalidateRect(m_hWnd, nullptr, FALSE);
    }
}

void BenchmarkDialog::stop_timed_test(bool finishedNaturally) {
    (void)finishedNaturally;
    m_isTimedRunning.store(false, std::memory_order_release);
    if (m_hWnd && IsWindow(m_hWnd)) {
        KillTimer(m_hWnd, ID_TIMER_BENCHMARK_CLOCK);
    }
    {
        std::lock_guard<std::mutex> lock(m_itemsMutex);
        m_timedFinished = true;
    }

    int targetSec = m_targetDurationSec.load(std::memory_order_acquire);
    if (m_hBtnStartStop && IsWindow(m_hBtnStartStop)) {
        if (m_durationMode == BenchmarkDurationMode::Continuous) {
            SetWindowTextW(m_hBtnStartStop, L"Тест стабильности");
        } else {
            std::wstring btnLabel = L"Старт (" + std::to_wstring(targetSec > 0 ? targetSec : 30) + L" сек)";
            SetWindowTextW(m_hBtnStartStop, btnLabel.c_str());
        }
    }

    update_verdict();

    if (m_hWnd && IsWindow(m_hWnd)) {
        InvalidateRect(m_hWnd, nullptr, FALSE);
    }
}

void BenchmarkDialog::update_timed_elapsed() {
    if (!m_isTimedRunning.load(std::memory_order_acquire)) return;
    if (m_isPaused.load(std::memory_order_acquire)) return;

    int targetSec = m_targetDurationSec.load(std::memory_order_acquire);
    if (targetSec <= 0) return;

    int elapsed = 0;
    {
        std::lock_guard<std::mutex> tlock(m_timerMutex);
        auto now = std::chrono::steady_clock::now();
        elapsed = static_cast<int>(
            std::chrono::duration_cast<std::chrono::seconds>(now - m_timedStartTime).count()
        );
    }
    elapsed = std::max(0, elapsed);
    int prevElapsed = m_elapsedSec.exchange(std::min(elapsed, targetSec), std::memory_order_acq_rel);

    if (elapsed >= targetSec) {
        if (m_isTimedRunning.exchange(false, std::memory_order_acq_rel)) {
            if (m_hWnd && IsWindow(m_hWnd)) {
                PostMessageW(m_hWnd, WM_APP_BENCHMARK_TIMED_FINISH, 0, 0);
            }
        }
    } else if (elapsed != prevElapsed && m_hWnd && IsWindow(m_hWnd)) {
        RECT rcClient = {};
        GetClientRect(m_hWnd, &rcClient);
        RECT rcVerdict = { dp(20), dp(148), rcClient.right - dp(20), dp(216) };
        InvalidateRect(m_hWnd, &rcVerdict, FALSE);
    }
}

void BenchmarkDialog::update_verdict() {
    std::lock_guard<std::mutex> lock(m_itemsMutex);

    int bestFastIdx = -1;
    int bestFastPing = 999999;

    int bestStableIdx = -1;
    float bestJitter = 999999.0f;

    int bestSmartFastIdx = -1;
    int bestSmartFastPing = 999999;

    int bestSmartStableIdx = -1;
    float bestSmartJitter = 999999.0f;

    for (size_t i = 0; i < m_items.size(); ++i) {
        auto& it = m_items[i];
        it.is_fastest = false;
        it.is_most_stable = false;
        it.is_fastest_smart = false;
        it.is_most_stable_smart = false;

        if (it.profile.is_dhcp) continue;
        if (it.avg_ping <= 0 || it.reliability < 50.0f) continue;

        if (it.reliability >= 70.0f && it.avg_ping < bestFastPing) {
            bestFastPing = it.avg_ping;
            bestFastIdx = static_cast<int>(i);
        }
        if (it.reliability >= 85.0f && it.jitter < bestJitter) {
            bestJitter = it.jitter;
            bestStableIdx = static_cast<int>(i);
        }

        if (it.profile.is_smart_dns) {
            if (it.reliability >= 70.0f && it.avg_ping < bestSmartFastPing) {
                bestSmartFastPing = it.avg_ping;
                bestSmartFastIdx = static_cast<int>(i);
            }
            if (it.reliability >= 85.0f && it.jitter < bestSmartJitter) {
                bestSmartJitter = it.jitter;
                bestSmartStableIdx = static_cast<int>(i);
            }
        }
    }

    m_fastestIndex = bestFastIdx;
    m_mostStableIndex = bestStableIdx;
    m_fastestSmartIndex = bestSmartFastIdx;
    m_mostStableSmartIndex = bestSmartStableIdx;

    if (bestFastIdx >= 0) {
        m_items[static_cast<size_t>(bestFastIdx)].is_fastest = true;
        std::wstringstream ss;
        ss << utf8_to_wide(m_items[static_cast<size_t>(bestFastIdx)].profile.name) << L" (ср. "
           << m_items[static_cast<size_t>(bestFastIdx)].avg_ping << L" ms, мин. "
           << m_items[static_cast<size_t>(bestFastIdx)].min_ping << L" ms)";
        m_verdictFastest = ss.str();
    } else {
        m_verdictFastest = L"Нет стабильных ответов";
    }

    if (bestStableIdx >= 0) {
        m_items[static_cast<size_t>(bestStableIdx)].is_most_stable = true;
        std::wstringstream ss;
        ss << std::fixed << std::setprecision(1);
        ss << utf8_to_wide(m_items[static_cast<size_t>(bestStableIdx)].profile.name) << L" (джиттер \u00B1"
           << m_items[static_cast<size_t>(bestStableIdx)].jitter << L" ms, "
           << static_cast<int>(m_items[static_cast<size_t>(bestStableIdx)].reliability) << L"% стабильность)";
        m_verdictStable = ss.str();
    } else {
        m_verdictStable = L"Недостаточно данных";
    }

    if (bestSmartFastIdx >= 0) {
        m_items[static_cast<size_t>(bestSmartFastIdx)].is_fastest_smart = true;
        std::wstringstream ss;
        ss << utf8_to_wide(m_items[static_cast<size_t>(bestSmartFastIdx)].profile.name) << L" (ср. "
           << m_items[static_cast<size_t>(bestSmartFastIdx)].avg_ping << L" ms, мин. "
           << m_items[static_cast<size_t>(bestSmartFastIdx)].min_ping << L" ms)";
        m_verdictSmartFastest = ss.str();
    } else {
        m_verdictSmartFastest = L"SmartDNS не ответил";
    }

    if (bestSmartStableIdx >= 0) {
        m_items[static_cast<size_t>(bestSmartStableIdx)].is_most_stable_smart = true;
        std::wstringstream ss;
        ss << std::fixed << std::setprecision(1);
        ss << utf8_to_wide(m_items[static_cast<size_t>(bestSmartStableIdx)].profile.name) << L" (джиттер \u00B1"
           << m_items[static_cast<size_t>(bestSmartStableIdx)].jitter << L" ms, "
           << static_cast<int>(m_items[static_cast<size_t>(bestSmartStableIdx)].reliability) << L"%)";
        m_verdictSmartStable = ss.str();
    } else {
        m_verdictSmartStable = L"SmartDNS не ответил";
    }
}

void BenchmarkDialog::start_ping_worker() {
    m_workerRunning.store(true, std::memory_order_release);
    m_isPaused.store(false, std::memory_order_release);

    m_workerThread = std::thread([this]() {
        struct ProbeTarget {
            size_t idx = 0;
            std::string ip;
            std::string doh_template;
            bool isDhcp = false;
        };

        while (m_workerRunning.load(std::memory_order_acquire)) {
            if (m_isPaused.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
                continue;
            }

            std::string currentDomain;
            {
                std::lock_guard<std::mutex> lock(m_domainMutex);
                currentDomain = m_targetDomain;
            }

            std::vector<ProbeTarget> targets;
            {
                std::lock_guard<std::mutex> lock(m_itemsMutex);
                targets.reserve(m_items.size());
                for (size_t i = 0; i < m_items.size(); ++i) {
                    ProbeTarget pt;
                    pt.idx = i;
                    pt.isDhcp = m_items[i].profile.is_dhcp;
                    pt.ip = select_profile_target_ip(m_items[i].profile);
                    pt.doh_template = m_items[i].doh_template;
                    targets.push_back(std::move(pt));
                }
            }

            if (!targets.empty()) {
                std::vector<int> pings(targets.size(), -1);
                std::atomic<size_t> nextIdx{0};
                size_t workerCount = std::min<size_t>(6, targets.size());
                std::vector<std::thread> pool;
                pool.reserve(workerCount);

                for (size_t w = 0; w < workerCount; ++w) {
                    pool.emplace_back([&]() {
                        while (m_workerRunning.load(std::memory_order_acquire)) {
                            size_t tIdx = nextIdx.fetch_add(1, std::memory_order_relaxed);
                            if (tIdx >= targets.size()) break;
                            const auto& t = targets[tIdx];
                            if (t.isDhcp) {
                                pings[tIdx] = -2;
                            } else {
                                int pingVal = -1;
                                if (!t.ip.empty()) {
                                    pingVal = DnsBenchmark::ping_dns_server_domain(t.ip, currentDomain, 450);
                                }
                                if (pingVal < 0 && !t.doh_template.empty() && m_workerRunning.load(std::memory_order_acquire)) {
                                    pingVal = DnsBenchmark::ping_doh_server_domain(t.doh_template, currentDomain, 2000);
                                    if (pingVal == 0) {
                                        pingVal = 1;
                                    }
                                }
                                pings[tIdx] = pingVal;
                            }
                        }
                    });
                }

                for (auto& th : pool) {
                    if (th.joinable()) th.join();
                }

                if (!m_workerRunning.load(std::memory_order_acquire)) {
                    break;
                }

                {
                    std::lock_guard<std::mutex> lock(m_itemsMutex);
                    for (size_t tIdx = 0; tIdx < targets.size(); ++tIdx) {
                        size_t idx = targets[tIdx].idx;
                        if (idx >= m_items.size()) continue;
                        int ping = pings[tIdx];
                        auto& it = m_items[idx];
                        it.total_queries++;

                        if (ping == -2) {
                            it.current_ping = 0;
                            it.status_text = L"DHCP (Авто)";
                            it.quality_label = L"Провайдер";
                            it.quality = DnsQuality::Fast;
                        } else {
                            it.current_ping = ping;
                            if (ping > 0) {
                                it.success_queries++;
                            }
                            if (it.latency_history.size() >= 32) {
                                it.latency_history.erase(it.latency_history.begin());
                            }
                            it.latency_history.push_back(ping);
                            it.recalculate_stats();

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

            update_timed_elapsed();

            if (m_workerRunning.load(std::memory_order_acquire) && m_hWnd && IsWindow(m_hWnd)) {
                PostMessageW(m_hWnd, WM_APP_BENCHMARK_PING_UPDATE, 0, 0);
            }

            for (int s = 0; s < 10 && m_workerRunning.load(std::memory_order_acquire) && !m_isPaused.load(std::memory_order_acquire); ++s) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    });
}

void BenchmarkDialog::stop_ping_worker() {
    m_workerRunning.store(false, std::memory_order_release);
    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }
}

void BenchmarkDialog::toggle_pause() {
    bool nowPaused = !m_isPaused.load(std::memory_order_acquire);
    {
        std::lock_guard<std::mutex> tlock(m_timerMutex);
        auto now = std::chrono::steady_clock::now();
        if (nowPaused) {
            m_pauseStartTime = now;
        } else {
            m_timedStartTime += (now - m_pauseStartTime);
        }
    }
    m_isPaused.store(nowPaused, std::memory_order_release);

    if (m_hBtnPauseResume && IsWindow(m_hBtnPauseResume)) {
        SetWindowTextW(m_hBtnPauseResume, nowPaused ? L"Возобновить" : L"Пауза");
    }
    if (m_hWnd && IsWindow(m_hWnd)) {
        InvalidateRect(m_hWnd, nullptr, FALSE);
    }
}

void BenchmarkDialog::on_apply() {
    bool applied = false;
    {
        std::lock_guard<std::mutex> lock(m_itemsMutex);
        if (m_selectedIndex >= 0 && m_selectedIndex < static_cast<int>(m_filteredIndices.size())) {
            size_t realIdx = m_filteredIndices[static_cast<size_t>(m_selectedIndex)];
            if (realIdx < m_items.size()) {
                m_selectedProfileId = m_items[realIdx].profile.id;
                m_finished = true;
                applied = true;
            }
        }
    }
    if (applied) {
        m_workerRunning.store(false, std::memory_order_release);
        if (m_hWnd && IsWindow(m_hWnd)) {
            ShowWindow(m_hWnd, SW_HIDE);
        }
    }
}

void BenchmarkDialog::on_close() {
    m_workerRunning.store(false, std::memory_order_release);
    if (m_hWnd && IsWindow(m_hWnd)) {
        ShowWindow(m_hWnd, SW_HIDE);
    }
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
    if (w <= 0 || h <= 0) return;

    auto pal = GetPalette(m_isDarkMode);
    int ctlId = pDis->CtlID;

    bool isPrimary = (ctlId == IDC_BTN_BENCHMARK_APPLY || ctlId == IDC_BTN_BENCHMARK_START_STOP);
    bool isSmartGreen = (ctlId == IDC_BTN_BENCHMARK_APPLY_SMART);
    bool isDisabled = (pDis->itemState & ODS_DISABLED) != 0;

    bool isFilterActive = (ctlId == IDC_BTN_FILTER_ALL && !m_filterOnlySmartDns) ||
                          (ctlId == IDC_BTN_FILTER_SMART && m_filterOnlySmartDns);
    bool isDurActive = (ctlId == IDC_BTN_DUR_10 && m_durationMode == BenchmarkDurationMode::Timed10s) ||
                       (ctlId == IDC_BTN_DUR_30 && m_durationMode == BenchmarkDurationMode::Timed30s) ||
                       (ctlId == IDC_BTN_DUR_60 && m_durationMode == BenchmarkDurationMode::Timed60s) ||
                       (ctlId == IDC_BTN_DUR_CONT && m_durationMode == BenchmarkDurationMode::Continuous);
    bool isChipActive = isFilterActive || isDurActive;
    bool isInsideBar = (ctlId == IDC_BTN_BENCHMARK_AI_PRESETS ||
                        ctlId == IDC_BTN_FILTER_ALL || ctlId == IDC_BTN_FILTER_SMART ||
                        ctlId == IDC_BTN_DUR_10 || ctlId == IDC_BTN_DUR_30 ||
                        ctlId == IDC_BTN_DUR_60 || ctlId == IDC_BTN_DUR_CONT ||
                        ctlId == IDC_BTN_BENCHMARK_START_STOP || ctlId == IDC_BTN_BENCHMARK_RESET);

    auto* anim = reinterpret_cast<ButtonAnimationState*>(GetPropW(pDis->hwndItem, L"QuickDnsAnim"));
    float hoverVal = anim ? anim->hover : 0.0f;
    bool isPressed = (pDis->itemState & ODS_SELECTED) || (anim && anim->isPressed);

    COLORREF baseBg, hoverBg, pressBg, baseBorder, hoverBorder, textColor;

    if (isDisabled) {
        if (isPrimary || isSmartGreen) {
            baseBg = m_isDarkMode ? RGB(30, 42, 60) : RGB(210, 222, 238);
            baseBorder = m_isDarkMode ? RGB(45, 58, 80) : RGB(180, 198, 222);
            textColor = m_isDarkMode ? RGB(110, 128, 155) : RGB(130, 150, 180);
        } else {
            baseBg = m_isDarkMode ? RGB(24, 27, 36) : RGB(241, 245, 249);
            baseBorder = m_isDarkMode ? RGB(38, 43, 56) : RGB(226, 232, 240);
            textColor = m_isDarkMode ? RGB(85, 95, 115) : RGB(148, 163, 184);
        }
        hoverBg = baseBg;
        pressBg = baseBg;
        hoverBorder = baseBorder;
    } else if (isPrimary) {
        baseBg = pal.btnPrimaryBg;
        hoverBg = pal.btnPrimaryHover;
        pressBg = pal.btnPrimaryPress;
        baseBorder = pal.btnPrimaryBorder;
        hoverBorder = pal.btnPrimaryHover;
        textColor = pal.btnPrimaryText;
    } else if (isSmartGreen) {
        baseBg = m_isDarkMode ? RGB(16, 88, 54) : RGB(22, 140, 78);
        hoverBg = m_isDarkMode ? RGB(20, 110, 68) : RGB(28, 165, 92);
        pressBg = m_isDarkMode ? RGB(12, 70, 42) : RGB(18, 120, 66);
        baseBorder = m_isDarkMode ? RGB(34, 197, 94) : RGB(22, 140, 78);
        hoverBorder = RGB(52, 211, 153);
        textColor = RGB(255, 255, 255);
    } else if (isChipActive) {
        baseBg = m_isDarkMode ? RGB(28, 52, 90) : RGB(218, 232, 255);
        hoverBg = m_isDarkMode ? RGB(36, 68, 115) : RGB(200, 222, 255);
        pressBg = m_isDarkMode ? RGB(22, 42, 75) : RGB(185, 210, 250);
        baseBorder = m_isDarkMode ? RGB(96, 165, 250) : RGB(37, 99, 235);
        hoverBorder = RGB(96, 165, 250);
        textColor = m_isDarkMode ? RGB(220, 238, 255) : RGB(20, 70, 160);
    } else {
        baseBg = pal.btnUtilBg;
        hoverBg = pal.btnUtilHover;
        pressBg = pal.btnUtilPress;
        baseBorder = pal.btnUtilBorder;
        hoverBorder = m_isDarkMode ? RGB(70, 85, 110) : RGB(180, 195, 220);
        textColor = pal.btnUtilText;
    }

    COLORREF curBg = isDisabled ? baseBg : (isPressed ? pressBg : LerpColor(baseBg, hoverBg, hoverVal));
    COLORREF curBorder = isDisabled ? baseBorder : LerpColor(baseBorder, hoverBorder, hoverVal);

    HDC hdcMem = CreateCompatibleDC(hdc);
    HBITMAP hbmMem = CreateCompatibleBitmap(hdc, w, h);
    if (!hbmMem) {
        DeleteDC(hdcMem);
        return;
    }
    HGDIOBJ oldBmp = SelectObject(hdcMem, hbmMem);

    RECT rcLocal = { 0, 0, w, h };
    HBRUSH hbrParentBg = CreateSolidBrush(isInsideBar ? pal.cardBg : pal.bg);
    FillRect(hdcMem, &rcLocal, hbrParentBg);
    DeleteObject(hbrParentBg);

    {
        Gdiplus::Graphics g(hdcMem);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

        Gdiplus::GraphicsPath path;
        AddRoundedRect(path, Gdiplus::RectF(0.5f, 0.5f, static_cast<float>(w) - 1.0f, static_cast<float>(h) - 1.0f), dpf(6.0f));

        Gdiplus::SolidBrush br(Gdiplus::Color(255, GetRValue(curBg), GetGValue(curBg), GetBValue(curBg)));
        g.FillPath(&br, &path);

        float penW = (isChipActive || isPrimary || isSmartGreen) ? 1.5f : 1.0f;
        Gdiplus::Pen pen(Gdiplus::Color(255, GetRValue(curBorder), GetGValue(curBorder), GetBValue(curBorder)), penW);
        g.DrawPath(&pen, &path);
    }

    wchar_t btnText[128] = {};
    GetWindowTextW(pDis->hwndItem, btnText, 128);

    SetBkMode(hdcMem, TRANSPARENT);
    SetTextColor(hdcMem, textColor);
    HFONT hFont = (isPrimary || isSmartGreen || isChipActive) ? m_hFontBold : m_hFontRegular;
    HGDIOBJ oldFont = SelectObject(hdcMem, hFont);

    RECT rcText = rcLocal;
    if (isPressed && !isDisabled) {
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

void BenchmarkDialog::draw_list_item(Gdiplus::Graphics& g, size_t filteredIdx, int y, int width, int height, bool isSelected, bool isHovered) {
    if (filteredIdx >= m_filteredIndices.size()) return;
    if (!m_gpFontTitle || !m_gpFontSub || !m_gpFontBadge || !m_gpFontPing) return;

    size_t realIdx = m_filteredIndices[filteredIdx];
    if (realIdx >= m_items.size()) return;

    const auto& it = m_items[realIdx];
    auto pal = GetPalette(m_isDarkMode);

    Gdiplus::RectF rcCard(0.5f, static_cast<float>(y) + 0.5f, static_cast<float>(width) - 1.0f, static_cast<float>(height) - 1.0f);
    Gdiplus::GraphicsPath cardPath;
    AddRoundedRect(cardPath, rcCard, dpf(7.0f));

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

    if (isSelected) {
        Gdiplus::GraphicsPath selBarPath;
        AddRoundedRect(selBarPath, Gdiplus::RectF(dpf(2.0f), static_cast<float>(y) + dpf(6.0f), dpf(3.5f), static_cast<float>(height) - dpf(12.0f)), dpf(2.0f));
        Gdiplus::SolidBrush selBarBr(Gdiplus::Color(255, GetRValue(borderCol), GetGValue(borderCol), GetBValue(borderCol)));
        g.FillPath(&selBarBr, &selBarPath);
    }

    float dotX = dpf(18.0f);
    float dotY = static_cast<float>(y) + static_cast<float>(height) / 2.0f;
    Gdiplus::Color dotColor;
    if (it.profile.is_dhcp) {
        dotColor = Gdiplus::Color(255, 59, 130, 246);
    } else if (it.current_ping > 0 && it.current_ping < 60) {
        dotColor = Gdiplus::Color(255, 34, 197, 94);
    } else if (it.current_ping >= 60 && it.current_ping <= 150) {
        dotColor = Gdiplus::Color(255, 245, 158, 11);
    } else if (it.current_ping > 150) {
        dotColor = Gdiplus::Color(255, 239, 68, 68);
    } else {
        dotColor = Gdiplus::Color(255, 156, 163, 175);
    }

    float auraR = dpf(7.0f);
    float dotR  = dpf(4.0f);
    Gdiplus::SolidBrush auraBr(Gdiplus::Color(55, dotColor.GetR(), dotColor.GetG(), dotColor.GetB()));
    g.FillEllipse(&auraBr, dotX - auraR, dotY - auraR, auraR * 2.0f, auraR * 2.0f);
    Gdiplus::SolidBrush dotBr(dotColor);
    g.FillEllipse(&dotBr, dotX - dotR, dotY - dotR, dotR * 2.0f, dotR * 2.0f);

    Gdiplus::SolidBrush textPriBr(Gdiplus::Color(255, GetRValue(pal.textPrimary), GetGValue(pal.textPrimary), GetBValue(pal.textPrimary)));
    Gdiplus::SolidBrush textSecBr(Gdiplus::Color(255, GetRValue(pal.textSecondary), GetGValue(pal.textSecondary), GetBValue(pal.textSecondary)));

    std::wstring pName = utf8_to_wide(it.profile.name);
    float textLeft = dpf(34.0f);
    float pingColX = static_cast<float>(width) - dpf(275.0f);
    float maxBadgeX = pingColX - dpf(12.0f);

    Gdiplus::StringFormat fmtSingle;
    fmtSingle.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
    fmtSingle.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);

    Gdiplus::RectF nameBounds;
    g.MeasureString(pName.c_str(), -1, m_gpFontTitle.get(), Gdiplus::PointF(0.0f, 0.0f), &nameBounds);
    float drawNameW = std::min(nameBounds.Width + dpf(4.0f), std::max(dpf(60.0f), maxBadgeX - textLeft));
    g.DrawString(pName.c_str(), -1, m_gpFontTitle.get(),
        Gdiplus::RectF(textLeft, static_cast<float>(y) + dpf(6.0f), drawNameW, dpf(19.0f)), &fmtSingle, &textPriBr);

    float badgeX = textLeft + drawNameW + dpf(6.0f);

    auto drawInlineBadge = [&](const wchar_t* text, Gdiplus::Color col) {
        Gdiplus::RectF bBounds;
        g.MeasureString(text, -1, m_gpFontBadge.get(), Gdiplus::PointF(0.0f, 0.0f), &bBounds);
        if (badgeX + bBounds.Width <= maxBadgeX) {
            Gdiplus::SolidBrush bBr(col);
            g.DrawString(text, -1, m_gpFontBadge.get(), Gdiplus::PointF(badgeX, static_cast<float>(y) + dpf(8.0f)), &bBr);
            badgeX += bBounds.Width + dpf(6.0f);
        }
    };

    if (it.profile.is_smart_dns) {
        drawInlineBadge(L"[SmartDNS]", Gdiplus::Color(255, 34, 197, 94));
    }
    if (it.profile.id == m_currentActiveId) {
        drawInlineBadge(L"[Текущий]", Gdiplus::Color(255, GetRValue(pal.accentBlue), GetGValue(pal.accentBlue), GetBValue(pal.accentBlue)));
    }
    if (it.is_fastest_smart) {
        drawInlineBadge(L"[Быстрый Smart]", Gdiplus::Color(255, 34, 197, 94));
    }
    if (it.is_most_stable_smart) {
        drawInlineBadge(L"[Стабильный Smart]", Gdiplus::Color(255, 59, 130, 246));
    }
    if (it.is_fastest && !it.is_fastest_smart) {
        drawInlineBadge(L"[Самый быстрый]", Gdiplus::Color(255, 34, 197, 94));
    }
    if (it.is_most_stable && !it.is_most_stable_smart) {
        drawInlineBadge(L"[Самый стабильный]", Gdiplus::Color(255, 59, 130, 246));
    }

    std::wstring subInfo;
    if (it.profile.is_dhcp) {
        subInfo = L"DHCP роутера/провайдера";
    } else {
        std::string dispIp = select_profile_target_ip(it.profile);
        if (!dispIp.empty()) {
            subInfo = L"IP: " + utf8_to_wide(dispIp);
        } else if (!it.doh_template.empty()) {
            subInfo = L"DoH: " + utf8_to_wide(it.doh_template);
        } else {
            subInfo = L"IP: —";
        }
        if (it.avg_ping > 0) {
            std::wstringstream ss;
            ss << std::fixed << std::setprecision(1);
            ss << L"  \u2022  Ср: " << it.avg_ping << L" ms  \u2022  Мин: " << it.min_ping << L"  \u2022  Джиттер: \u00B1" << it.jitter << L" ms";
            subInfo += ss.str();
        } else if (!it.profile.unblocked_summary.empty() && it.profile.is_smart_dns) {
            subInfo += L"  \u2022  " + utf8_to_wide(it.profile.unblocked_summary);
        }
    }
    g.DrawString(subInfo.c_str(), -1, m_gpFontSub.get(),
        Gdiplus::RectF(textLeft, static_cast<float>(y) + dpf(26.0f), std::max(dpf(60.0f), pingColX - dpf(40.0f)), dpf(18.0f)), &fmtSingle, &textSecBr);

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

    Gdiplus::SolidBrush pingTextBr(pingTextColor);
    g.DrawString(pingValStr.c_str(), -1, m_gpFontPing.get(), Gdiplus::PointF(pingColX, static_cast<float>(y) + dpf(6.0f)), &pingTextBr);

    std::wstring qualStr = it.quality_label;
    if (it.total_queries > 1 && !it.profile.is_dhcp) {
        qualStr += L" (" + std::to_wstring(static_cast<int>(it.reliability)) + L"%)";
    }
    g.DrawString(qualStr.c_str(), -1, m_gpFontSub.get(), Gdiplus::PointF(pingColX, static_cast<float>(y) + dpf(26.0f)), &textSecBr);

    // Mini Sparkline Graph
    float graphW = dpf(140.0f);
    float graphH = dpf(34.0f);
    float graphX = static_cast<float>(width) - graphW - dpf(10.0f);
    float graphY = static_cast<float>(y) + (static_cast<float>(height) - graphH) / 2.0f;

    Gdiplus::GraphicsPath graphCont;
    AddRoundedRect(graphCont, Gdiplus::RectF(graphX, graphY, graphW, graphH), dpf(6.0f));
    Gdiplus::SolidBrush graphBgBr(Gdiplus::Color(255, GetRValue(pal.graphBg), GetGValue(pal.graphBg), GetBValue(pal.graphBg)));
    Gdiplus::Pen graphBorderPen(Gdiplus::Color(255, GetRValue(pal.graphBorder), GetGValue(pal.graphBorder), GetBValue(pal.graphBorder)), 1.0f);
    g.FillPath(&graphBgBr, &graphCont);
    g.DrawPath(&graphBorderPen, &graphCont);

    Gdiplus::Pen gridPen(Gdiplus::Color(60, GetRValue(pal.graphGrid), GetGValue(pal.graphGrid), GetBValue(pal.graphGrid)), 1.0f);
    gridPen.SetDashStyle(Gdiplus::DashStyleDash);
    g.DrawLine(&gridPen, graphX + dpf(4.0f), graphY + graphH / 2.0f, graphX + graphW - dpf(4.0f), graphY + graphH / 2.0f);

    const auto& hist = it.latency_history;
    if (!hist.empty()) {
        float pLeft   = graphX + dpf(6.0f);
        float pRight  = graphX + graphW - dpf(6.0f);
        float pTop    = graphY + dpf(5.0f);
        float pBottom = graphY + graphH - dpf(5.0f);
        float pWidth  = std::max(1.0f, pRight - pLeft);
        float pHeight = std::max(1.0f, pBottom - pTop);

        int maxMs = 70;
        for (int v : hist) {
            if (v > maxMs) maxMs = std::min(300, v + 15);
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
            std::vector<Gdiplus::PointF> poly = pts;
            poly.push_back(Gdiplus::PointF(pts.back().X, pBottom));
            poly.push_back(Gdiplus::PointF(pts.front().X, pBottom));
            Gdiplus::SolidBrush fillBr(Gdiplus::Color(45, dotColor.GetR(), dotColor.GetG(), dotColor.GetB()));
            g.FillPolygon(&fillBr, poly.data(), static_cast<int>(poly.size()));

            Gdiplus::Pen curvePen(dotColor, dpf(1.8f));
            curvePen.SetLineJoin(Gdiplus::LineJoinRound);
            curvePen.SetStartCap(Gdiplus::LineCapRound);
            curvePen.SetEndCap(Gdiplus::LineCapRound);
            g.DrawLines(&curvePen, pts.data(), static_cast<int>(pts.size()));
        }

        Gdiplus::PointF lastPt = pts.back();
        float haloR = dpf(4.5f);
        float ptR   = dpf(2.5f);
        Gdiplus::SolidBrush dotHalo(Gdiplus::Color(65, dotColor.GetR(), dotColor.GetG(), dotColor.GetB()));
        g.FillEllipse(&dotHalo, lastPt.X - haloR, lastPt.Y - haloR, haloR * 2.0f, haloR * 2.0f);
        Gdiplus::SolidBrush dotSolid(dotColor);
        g.FillEllipse(&dotSolid, lastPt.X - ptR, lastPt.Y - ptR, ptR * 2.0f, ptR * 2.0f);
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
    case WM_GETMINMAXINFO: {
        auto* pMMI = reinterpret_cast<MINMAXINFO*>(lParam);
        RECT rcMin = { 0, 0, dp(740), dp(540) };
        adjust_window_rect_for_dpi(&rcMin, GetWindowLongW(hWnd, GWL_STYLE), FALSE, GetWindowLongW(hWnd, GWL_EXSTYLE), m_dpi);
        int minW = rcMin.right - rcMin.left;
        int minH = rcMin.bottom - rcMin.top;
        RECT rcWork = get_monitor_work_area(hWnd);
        int workW = rcWork.right - rcWork.left;
        int workH = rcWork.bottom - rcWork.top;
        if (workW > 0) minW = std::min(minW, workW);
        if (workH > 0) minH = std::min(minH, workH);
        pMMI->ptMinTrackSize.x = minW;
        pMMI->ptMinTrackSize.y = minH;
        return 0;
    }

    case WM_DPICHANGED: {
        m_dpi = HIWORD(wParam);
        setup_fonts();
        apply_fonts_to_controls();
        const auto* pSuggested = reinterpret_cast<const RECT*>(lParam);
        if (pSuggested) {
            RECT rcWork = get_monitor_work_area(hWnd);
            int sugW = std::min(static_cast<int>(pSuggested->right - pSuggested->left), static_cast<int>(rcWork.right - rcWork.left));
            int sugH = std::min(static_cast<int>(pSuggested->bottom - pSuggested->top), static_cast<int>(rcWork.bottom - rcWork.top));
            int sugX = std::clamp(static_cast<int>(pSuggested->left), static_cast<int>(rcWork.left), std::max(static_cast<int>(rcWork.left), static_cast<int>(rcWork.right) - sugW));
            int sugY = std::clamp(static_cast<int>(pSuggested->top), static_cast<int>(rcWork.top), std::max(static_cast<int>(rcWork.top), static_cast<int>(rcWork.bottom) - sugH));
            SetWindowPos(hWnd, nullptr, sugX, sugY, sugW, sugH, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        reposition_controls();
        return 0;
    }

    case WM_SIZE: {
        if (wParam != SIZE_MINIMIZED) {
            reposition_controls();
        }
        return 0;
    }

    case WM_TIMER: {
        if (wParam == ID_TIMER_BENCHMARK_CLOCK) {
            update_timed_elapsed();
            return 0;
        }
        break;
    }

    case WM_MOUSEWHEEL: {
        if (m_hListWnd && IsWindow(m_hListWnd)) {
            return SendMessageW(m_hListWnd, uMsg, wParam, lParam);
        }
        break;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc{};
        GetClientRect(hWnd, &rc);
        int w = rc.right - rc.left;
        int h = rc.bottom - rc.top;
        if (w <= 0 || h <= 0) {
            EndPaint(hWnd, &ps);
            return 0;
        }

        HDC hdcMem = CreateCompatibleDC(hdc);
        HBITMAP hbmMem = CreateCompatibleBitmap(hdc, w, h);
        if (!hbmMem) {
            DeleteDC(hdcMem);
            EndPaint(hWnd, &ps);
            return 0;
        }
        HGDIOBJ oldBmp = SelectObject(hdcMem, hbmMem);

        auto pal = GetPalette(m_isDarkMode);

        HBRUSH hbrBg = CreateSolidBrush(pal.bg);
        FillRect(hdcMem, &rc, hbrBg);
        DeleteObject(hbrBg);

        if (m_gpHeaderTitle && m_gpHeaderSub && m_gpHeaderBold) {
            Gdiplus::Graphics g(hdcMem);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
            g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

            int marginX = dp(20);
            int contentW = std::max(dp(200), w - (marginX * 2));

            Gdiplus::SolidBrush textPriBr(Gdiplus::Color(255, GetRValue(pal.textPrimary), GetGValue(pal.textPrimary), GetBValue(pal.textPrimary)));
            Gdiplus::SolidBrush textSecBr(Gdiplus::Color(255, GetRValue(pal.textSecondary), GetGValue(pal.textSecondary), GetBValue(pal.textSecondary)));

            g.DrawString(L"Тест скорости и стабильности DNS", -1, m_gpHeaderTitle.get(),
                Gdiplus::PointF(static_cast<float>(marginX), dpf(12.0f)), &textPriBr);
            g.DrawString(L"Замер отклика DNS-серверов в реальном времени и тестирование стабильности для любого выбранного сайта.",
                -1, m_gpHeaderSub.get(), Gdiplus::PointF(static_cast<float>(marginX), dpf(36.0f)), &textSecBr);

            // Row 1 Bar Background
            Gdiplus::RectF rcBar1(static_cast<float>(marginX), dpf(58.0f), static_cast<float>(contentW), dpf(38.0f));
            Gdiplus::GraphicsPath bar1Path;
            AddRoundedRect(bar1Path, rcBar1, dpf(7.0f));
            Gdiplus::SolidBrush barCardBr(Gdiplus::Color(255, GetRValue(pal.cardBg), GetGValue(pal.cardBg), GetBValue(pal.cardBg)));
            Gdiplus::Pen barPen(Gdiplus::Color(255, GetRValue(pal.cardBorder), GetGValue(pal.cardBorder), GetBValue(pal.cardBorder)), 1.0f);
            g.FillPath(&barCardBr, &bar1Path);
            g.DrawPath(&barPen, &bar1Path);

            g.DrawString(L"Сайт:", -1, m_gpHeaderBold.get(),
                Gdiplus::PointF(static_cast<float>(marginX + dp(10)), dpf(68.0f)), &textPriBr);

            // Modern rounded Domain input container (replaces legacy 3D WS_EX_CLIENTEDGE)
            {
                Gdiplus::RectF domRc(
                    static_cast<float>(m_domainBoxRect.left) + 0.5f,
                    static_cast<float>(m_domainBoxRect.top) + 0.5f,
                    static_cast<float>(std::max<LONG>(20, m_domainBoxRect.right - m_domainBoxRect.left)) - 1.0f,
                    static_cast<float>(std::max<LONG>(16, m_domainBoxRect.bottom - m_domainBoxRect.top)) - 1.0f
                );
                Gdiplus::GraphicsPath domPath;
                AddRoundedRect(domPath, domRc, dpf(5.0f));
                Gdiplus::SolidBrush domBgBr(Gdiplus::Color(255, GetRValue(pal.bg), GetGValue(pal.bg), GetBValue(pal.bg)));
                g.FillPath(&domBgBr, &domPath);
                bool domActive = (m_domainEditFocused || m_domainEditHovered);
                COLORREF domBorderCol = domActive ? pal.accentBlue : pal.cardBorder;
                Gdiplus::Pen domBorderPen(Gdiplus::Color(255, GetRValue(domBorderCol), GetGValue(domBorderCol), GetBValue(domBorderCol)), domActive ? 1.5f : 1.0f);
                g.DrawPath(&domBorderPen, &domPath);
            }

            // Row 2 Bar Background
            Gdiplus::RectF rcBar2(static_cast<float>(marginX), dpf(102.0f), static_cast<float>(contentW), dpf(40.0f));
            Gdiplus::GraphicsPath bar2Path;
            AddRoundedRect(bar2Path, rcBar2, dpf(7.0f));
            g.FillPath(&barCardBr, &bar2Path);
            g.DrawPath(&barPen, &bar2Path);

            g.DrawString(L"Тест:", -1, m_gpHeaderBold.get(),
                Gdiplus::PointF(static_cast<float>(marginX + dp(10)), dpf(113.0f)), &textPriBr);

            // Row 3 Status & Dual Verdict Card
            Gdiplus::RectF rcVerdict(static_cast<float>(marginX), dpf(148.0f), static_cast<float>(contentW), dpf(66.0f));
            Gdiplus::GraphicsPath vPath;
            AddRoundedRect(vPath, rcVerdict, dpf(7.0f));
            g.FillPath(&barCardBr, &vPath);
            g.DrawPath(&barPen, &vPath);

            std::wstring currentDomainWide;
            {
                std::lock_guard<std::mutex> lock(m_domainMutex);
                currentDomainWide = utf8_to_wide(m_targetDomain);
            }

            bool isTimed = m_isTimedRunning.load(std::memory_order_acquire);
            bool isPaused = m_isPaused.load(std::memory_order_acquire);
            int totalSec = m_targetDurationSec.load(std::memory_order_acquire);
            if (totalSec <= 0) totalSec = 30;
            int elapsedSec = m_elapsedSec.load(std::memory_order_acquire);

            bool timedDone = false;
            std::wstring vSmartFastest;
            std::wstring vFastest;
            {
                std::lock_guard<std::mutex> lock(m_itemsMutex);
                timedDone = m_timedFinished;
                vSmartFastest = m_verdictSmartFastest;
                vFastest = m_verdictFastest;
            }

            if (isTimed) {
                int remainSec = std::max(0, totalSec - elapsedSec);
                std::wstring testMsg = L"Идет замер стабильности для \"" + currentDomainWide +
                    L"\" \u2022 Осталось: " + std::to_wstring(remainSec) +
                    L" сек (секунда " + std::to_wstring(elapsedSec) + L" из " + std::to_wstring(totalSec) + L")";
                if (isPaused) {
                    testMsg += L" [Пауза]";
                }
                g.DrawString(testMsg.c_str(), -1, m_gpHeaderBold.get(),
                    Gdiplus::PointF(static_cast<float>(marginX + dp(16)), dpf(158.0f)), &textPriBr);

                float progX = static_cast<float>(marginX + dp(16));
                float progY = dpf(186.0f);
                float progW = static_cast<float>(std::max(dp(40), contentW - dp(32)));
                float progH = dpf(10.0f);

                Gdiplus::RectF rcTrack(progX, progY, progW, progH);
                Gdiplus::GraphicsPath trackPath;
                AddRoundedRect(trackPath, rcTrack, dpf(5.0f));
                Gdiplus::SolidBrush trackBg(m_isDarkMode ? Gdiplus::Color(255, 38, 44, 56) : Gdiplus::Color(255, 226, 232, 240));
                g.FillPath(&trackBg, &trackPath);

                float ratio = std::clamp(static_cast<float>(elapsedSec) / static_cast<float>(totalSec), 0.0f, 1.0f);
                if (ratio > 0.01f) {
                    float fillW = std::max(dpf(10.0f), progW * ratio);
                    Gdiplus::RectF rcFill(progX, progY, fillW, progH);
                    Gdiplus::GraphicsPath fillPath;
                    AddRoundedRect(fillPath, rcFill, dpf(5.0f));
                    Gdiplus::SolidBrush fillBr(Gdiplus::Color(255, 34, 197, 94));
                    g.FillPath(&fillBr, &fillPath);
                }
            } else if (timedDone && (!vSmartFastest.empty() || !vFastest.empty())) {
                float halfW = static_cast<float>(contentW - dp(24)) / 2.0f;

                Gdiplus::SolidBrush greenTextBr(Gdiplus::Color(255, 34, 197, 94));
                g.DrawString(L"Самый быстрый SmartDNS:", -1, m_gpHeaderBold.get(),
                    Gdiplus::PointF(static_cast<float>(marginX + dp(14)), dpf(156.0f)), &greenTextBr);
                g.DrawString(vSmartFastest.c_str(), -1, m_gpHeaderSub.get(),
                    Gdiplus::PointF(static_cast<float>(marginX + dp(14)), dpf(178.0f)), &textPriBr);

                Gdiplus::SolidBrush blueTextBr(Gdiplus::Color(255, 59, 130, 246));
                g.DrawString(L"Самый быстрый среди всех:", -1, m_gpHeaderBold.get(),
                    Gdiplus::PointF(static_cast<float>(marginX + dp(14)) + halfW, dpf(156.0f)), &blueTextBr);
                g.DrawString(vFastest.c_str(), -1, m_gpHeaderSub.get(),
                    Gdiplus::PointF(static_cast<float>(marginX + dp(14)) + halfW, dpf(178.0f)), &textPriBr);
            } else {
                std::wstring readyMsg = L"Целевой сайт: " + currentDomainWide + L"  \u2022  " +
                    (isPaused ? L"Мониторинг приостановлен" : L"Автомониторинг активен (опрос каждые 1.0 сек)");
                g.DrawString(readyMsg.c_str(), -1, m_gpHeaderBold.get(),
                    Gdiplus::PointF(static_cast<float>(marginX + dp(16)), dpf(158.0f)), &textPriBr);

                std::wstring hintMsg = L"Нажмите «Старт» для замера стабильности с определением лучшего SmartDNS, или выберите сервер из списка.";
                g.DrawString(hintMsg.c_str(), -1, m_gpHeaderSub.get(),
                    Gdiplus::PointF(static_cast<float>(marginX + dp(16)), dpf(180.0f)), &textSecBr);
            }
        }

        BitBlt(hdc, 0, 0, w, h, hdcMem, 0, 0, SRCCOPY);
        SelectObject(hdcMem, oldBmp);
        DeleteObject(hbmMem);
        DeleteDC(hdcMem);
        EndPaint(hWnd, &ps);
        return 0;
    }

    case WM_APP_BENCHMARK_PING_UPDATE: {
        RECT rcClient = {};
        GetClientRect(hWnd, &rcClient);
        RECT rcVerdict = { dp(20), dp(148), rcClient.right - dp(20), dp(216) };
        InvalidateRect(hWnd, &rcVerdict, FALSE);
        if (m_hListWnd && IsWindow(m_hListWnd)) {
            InvalidateRect(m_hListWnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_APP_BENCHMARK_TIMED_FINISH: {
        stop_timed_test(true);
        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam);
        int code = HIWORD(wParam);

        if (id == IDC_BTN_BENCHMARK_START_STOP) {
            if (m_isTimedRunning.load(std::memory_order_acquire)) {
                stop_timed_test(false);
            } else {
                start_timed_test();
            }
            return 0;
        }

        if (id == IDC_BTN_BENCHMARK_RESET) {
            reset_all_stats();
            return 0;
        }

        if (id == IDC_BTN_BENCHMARK_AI_PRESETS) {
            show_ai_presets_menu();
            return 0;
        }

        if (id == IDC_BTN_FILTER_ALL) {
            set_filter_smart_only(false);
            return 0;
        }

        if (id == IDC_BTN_FILTER_SMART) {
            set_filter_smart_only(true);
            return 0;
        }

        if (id == IDC_BTN_DUR_10) {
            set_duration_mode(BenchmarkDurationMode::Timed10s);
            return 0;
        }

        if (id == IDC_BTN_DUR_30) {
            set_duration_mode(BenchmarkDurationMode::Timed30s);
            return 0;
        }

        if (id == IDC_BTN_DUR_60) {
            set_duration_mode(BenchmarkDurationMode::Timed60s);
            return 0;
        }

        if (id == IDC_BTN_DUR_CONT) {
            set_duration_mode(BenchmarkDurationMode::Continuous);
            return 0;
        }

        if (id == IDC_EDIT_BENCHMARK_DOMAIN && code == EN_CHANGE) {
            wchar_t buf[256] = {};
            GetWindowTextW(m_hEditDomain, buf, _countof(buf));
            {
                std::lock_guard<std::mutex> lock(m_domainMutex);
                m_targetDomain = DnsBenchmark::sanitize_domain(wide_to_utf8(buf));
            }
            return 0;
        }

        if (id == IDC_BTN_BENCHMARK_APPLY) {
            on_apply();
            return 0;
        }

        if (id == IDC_BTN_BENCHMARK_APPLY_SMART) {
            bool applied = false;
            {
                std::lock_guard<std::mutex> lock(m_itemsMutex);
                int targetIdx = (m_fastestSmartIndex >= 0) ? m_fastestSmartIndex : m_mostStableSmartIndex;
                if (targetIdx >= 0 && targetIdx < static_cast<int>(m_items.size())) {
                    m_selectedProfileId = m_items[static_cast<size_t>(targetIdx)].profile.id;
                    m_finished = true;
                    applied = true;
                } else {
                    for (const auto& item : m_items) {
                        if (item.profile.is_smart_dns) {
                            m_selectedProfileId = item.profile.id;
                            m_finished = true;
                            applied = true;
                            break;
                        }
                    }
                }
            }
            if (applied) {
                m_workerRunning.store(false, std::memory_order_release);
                if (m_hWnd && IsWindow(m_hWnd)) {
                    ShowWindow(m_hWnd, SW_HIDE);
                }
            }
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
        HDC hdc = reinterpret_cast<HDC>(wParam);
        auto pal = GetPalette(m_isDarkMode);
        SetTextColor(hdc, pal.textPrimary);
        SetBkMode(hdc, TRANSPARENT);
        return reinterpret_cast<INT_PTR>(m_hbrBg);
    }

    case WM_CTLCOLOREDIT: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        auto pal = GetPalette(m_isDarkMode);
        SetBkMode(hdc, OPAQUE);
        SetTextColor(hdc, pal.textPrimary);
        SetBkColor(hdc, pal.bg);
        return reinterpret_cast<INT_PTR>(m_hbrBg);
    }

    case WM_CLOSE: {
        on_close();
        return 0;
    }

    case WM_NCDESTROY: {
        KillTimer(hWnd, ID_TIMER_BENCHMARK_CLOCK);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, 0);
        m_hWnd = nullptr;
        m_finished = true;
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
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc{};
        GetClientRect(hWnd, &rc);
        int w = rc.right - rc.left;
        int h = rc.bottom - rc.top;
        if (w <= 0 || h <= 0) {
            EndPaint(hWnd, &ps);
            return 0;
        }

        HDC hdcMem = CreateCompatibleDC(hdc);
        HBITMAP hbmMem = CreateCompatibleBitmap(hdc, w, h);
        if (!hbmMem) {
            DeleteDC(hdcMem);
            EndPaint(hWnd, &ps);
            return 0;
        }
        HGDIOBJ oldBmp = SelectObject(hdcMem, hbmMem);

        auto pal = GetPalette(m_isDarkMode);

        HBRUSH hbrBg = CreateSolidBrush(pal.bg);
        FillRect(hdcMem, &rc, hbrBg);
        DeleteObject(hbrBg);

        {
            Gdiplus::Graphics g(hdcMem);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
            g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

            std::lock_guard<std::mutex> lock(m_itemsMutex);
            int rowHeight = get_row_height();
            int rowStep = get_row_step();
            int totalItems = static_cast<int>(m_filteredIndices.size());
            int totalContentHeight = totalItems * rowStep;
            bool needScroll = totalContentHeight > h;
            int maxScroll = needScroll ? (totalContentHeight - h) : 0;
            m_scrollOffset = std::clamp(m_scrollOffset, 0, maxScroll);

            int scrollBarW = dp(7);
            int scrollBarMarginRight = dp(4);
            int scrollReserve = dp(17);
            int itemW = needScroll ? std::max(dp(180), w - scrollReserve) : w;

            for (int i = 0; i < totalItems; ++i) {
                int y = i * rowStep - m_scrollOffset;
                if (y + rowHeight < 0 || y > h) continue;

                bool isSelected = (i == m_selectedIndex);
                bool isHovered = (i == m_hoveredIndex);
                draw_list_item(g, static_cast<size_t>(i), y, itemW, rowHeight, isSelected, isHovered);
            }

            if (needScroll) {
                float sbX = static_cast<float>(w - scrollBarMarginRight - scrollBarW);
                float sbY = dpf(4.0f);
                float sbH = std::max(dpf(20.0f), static_cast<float>(h) - dpf(8.0f));

                Gdiplus::SolidBrush trackBr(m_isDarkMode ? Gdiplus::Color(25, 255, 255, 255) : Gdiplus::Color(20, 0, 0, 0));
                Gdiplus::GraphicsPath trackPath;
                AddRoundedRect(trackPath, Gdiplus::RectF(sbX, sbY, static_cast<float>(scrollBarW), sbH), dpf(3.5f));
                g.FillPath(&trackBr, &trackPath);

                float thumbH = std::min(sbH, std::max(dpf(32.0f), (static_cast<float>(h) / static_cast<float>(totalContentHeight)) * sbH));
                float scrollRatio = (maxScroll > 0) ? (static_cast<float>(m_scrollOffset) / static_cast<float>(maxScroll)) : 0.0f;
                float thumbY = sbY + scrollRatio * (sbH - thumbH);

                Gdiplus::Color thumbColor;
                if (m_isDarkMode) {
                    if (m_isDraggingScroll) thumbColor = Gdiplus::Color(255, 120, 140, 175);
                    else if (m_scrollThumbHovered) thumbColor = Gdiplus::Color(255, 95, 110, 135);
                    else thumbColor = Gdiplus::Color(255, 65, 75, 95);
                } else {
                    if (m_isDraggingScroll) thumbColor = Gdiplus::Color(255, 130, 150, 180);
                    else if (m_scrollThumbHovered) thumbColor = Gdiplus::Color(255, 160, 175, 195);
                    else thumbColor = Gdiplus::Color(255, 200, 210, 222);
                }

                Gdiplus::SolidBrush thumbBr(thumbColor);
                Gdiplus::GraphicsPath thumbPath;
                AddRoundedRect(thumbPath, Gdiplus::RectF(sbX, thumbY, static_cast<float>(scrollBarW), thumbH), dpf(3.5f));
                g.FillPath(&thumbBr, &thumbPath);
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
        int mx = GET_X_LPARAM(lParam);
        int my = GET_Y_LPARAM(lParam);
        RECT rc{};
        GetClientRect(hWnd, &rc);
        int w = rc.right - rc.left;
        int h = rc.bottom - rc.top;

        int totalItems = 0;
        {
            std::lock_guard<std::mutex> lock(m_itemsMutex);
            totalItems = static_cast<int>(m_filteredIndices.size());
        }
        int rowStep = std::max(1, get_row_step());
        int totalContentHeight = totalItems * rowStep;
        bool needScroll = totalContentHeight > h;
        int maxScroll = needScroll ? (totalContentHeight - h) : 0;
        int scrollReserve = dp(17);

        if (m_isDraggingScroll) {
            if (needScroll) {
                float sbH = std::max(dpf(20.0f), static_cast<float>(h) - dpf(8.0f));
                float thumbH = std::min(sbH, std::max(dpf(32.0f), (static_cast<float>(h) / static_cast<float>(totalContentHeight)) * sbH));
                float availableTrack = sbH - thumbH;
                if (availableTrack > 0.0f) {
                    float deltaY = static_cast<float>(my - m_dragStartY);
                    float offsetDelta = (deltaY / availableTrack) * static_cast<float>(maxScroll);
                    m_scrollOffset = std::clamp(static_cast<int>(m_dragStartOffset + offsetDelta), 0, maxScroll);
                    InvalidateRect(hWnd, nullptr, FALSE);
                }
            }
            return 0;
        }

        bool overScrollArea = needScroll && (mx >= (w - scrollReserve));
        if (overScrollArea) {
            if (m_hoveredIndex != -1) {
                m_hoveredIndex = -1;
                InvalidateRect(hWnd, nullptr, FALSE);
            }
            float sbY = dpf(4.0f);
            float sbH = std::max(dpf(20.0f), static_cast<float>(h) - dpf(8.0f));
            float thumbH = std::min(sbH, std::max(dpf(32.0f), (static_cast<float>(h) / static_cast<float>(totalContentHeight)) * sbH));
            float scrollRatio = (maxScroll > 0) ? (static_cast<float>(m_scrollOffset) / static_cast<float>(maxScroll)) : 0.0f;
            float thumbY = sbY + scrollRatio * (sbH - thumbH);

            bool overThumb = (my >= thumbY && my <= thumbY + thumbH);
            if (m_scrollThumbHovered != overThumb) {
                m_scrollThumbHovered = overThumb;
                InvalidateRect(hWnd, nullptr, FALSE);
            }
        } else {
            if (m_scrollThumbHovered) {
                m_scrollThumbHovered = false;
                InvalidateRect(hWnd, nullptr, FALSE);
            }
            int contentY = my + m_scrollOffset;
            int idx = (contentY >= 0) ? (contentY / rowStep) : -1;
            if (idx >= 0 && idx < totalItems) {
                if (idx != m_hoveredIndex) {
                    m_hoveredIndex = idx;
                    InvalidateRect(hWnd, nullptr, FALSE);
                }
            } else if (m_hoveredIndex != -1) {
                m_hoveredIndex = -1;
                InvalidateRect(hWnd, nullptr, FALSE);
            }
        }

        TRACKMOUSEEVENT tme{};
        tme.cbSize = sizeof(TRACKMOUSEEVENT);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = hWnd;
        TrackMouseEvent(&tme);
        break;
    }

    case WM_MOUSELEAVE: {
        if (!m_isDraggingScroll) {
            m_hoveredIndex = -1;
            m_scrollThumbHovered = false;
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        break;
    }

    case WM_LBUTTONDOWN: {
        SetFocus(hWnd);
        int mx = GET_X_LPARAM(lParam);
        int my = GET_Y_LPARAM(lParam);
        RECT rc{};
        GetClientRect(hWnd, &rc);
        int w = rc.right - rc.left;
        int h = rc.bottom - rc.top;

        int rowStep = std::max(1, get_row_step());
        int scrollReserve = dp(17);
        bool startDrag = false;
        bool changed = false;

        {
            std::lock_guard<std::mutex> lock(m_itemsMutex);
            int totalItems = static_cast<int>(m_filteredIndices.size());
            int totalContentHeight = totalItems * rowStep;
            bool needScroll = totalContentHeight > h;
            int maxScroll = needScroll ? (totalContentHeight - h) : 0;

            if (needScroll && mx >= (w - scrollReserve)) {
                float sbY = dpf(4.0f);
                float sbH = std::max(dpf(20.0f), static_cast<float>(h) - dpf(8.0f));
                float thumbH = std::min(sbH, std::max(dpf(32.0f), (static_cast<float>(h) / static_cast<float>(totalContentHeight)) * sbH));
                float scrollRatio = (maxScroll > 0) ? (static_cast<float>(m_scrollOffset) / static_cast<float>(maxScroll)) : 0.0f;
                float thumbY = sbY + scrollRatio * (sbH - thumbH);

                if (my >= thumbY && my <= thumbY + thumbH) {
                    m_isDraggingScroll = true;
                    m_dragStartY = my;
                    m_dragStartOffset = m_scrollOffset;
                    startDrag = true;
                } else if (my < thumbY) {
                    m_scrollOffset = std::max(0, m_scrollOffset - h);
                } else {
                    m_scrollOffset = std::min(maxScroll, m_scrollOffset + h);
                }
                changed = true;
            } else {
                int contentY = my + m_scrollOffset;
                int idx = (contentY >= 0) ? (contentY / rowStep) : -1;
                if (idx >= 0 && idx < totalItems) {
                    m_selectedIndex = idx;
                    changed = true;
                }
            }
        }

        if (startDrag) {
            SetCapture(hWnd);
        }
        if (changed) {
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        if (m_isDraggingScroll) {
            m_isDraggingScroll = false;
            ReleaseCapture();
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        break;
    }

    case WM_CAPTURECHANGED: {
        m_isDraggingScroll = false;
        break;
    }

    case WM_LBUTTONDBLCLK: {
        int mx = GET_X_LPARAM(lParam);
        int my = GET_Y_LPARAM(lParam);
        RECT rc{};
        GetClientRect(hWnd, &rc);
        int w = rc.right - rc.left;
        int h = rc.bottom - rc.top;
        int rowStep = std::max(1, get_row_step());
        int scrollReserve = dp(17);

        bool applied = false;
        {
            std::lock_guard<std::mutex> lock(m_itemsMutex);
            int totalItems = static_cast<int>(m_filteredIndices.size());
            bool needScroll = (totalItems * rowStep) > h;
            if (!needScroll || mx < w - scrollReserve) {
                int contentY = my + m_scrollOffset;
                int idx = (contentY >= 0) ? (contentY / rowStep) : -1;
                if (idx >= 0 && idx < totalItems) {
                    m_selectedIndex = idx;
                    size_t realIdx = m_filteredIndices[static_cast<size_t>(idx)];
                    if (realIdx < m_items.size()) {
                        m_selectedProfileId = m_items[realIdx].profile.id;
                        m_finished = true;
                        applied = true;
                    }
                }
            }
        }
        if (applied) {
            m_workerRunning.store(false, std::memory_order_release);
            if (m_hWnd && IsWindow(m_hWnd)) {
                ShowWindow(m_hWnd, SW_HIDE);
            }
        }
        return 0;
    }

    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        if ((delta > 0 && m_wheelAccum < 0) || (delta < 0 && m_wheelAccum > 0)) {
            m_wheelAccum = 0;
        }
        int stepPx = std::max(1, get_row_step());
        m_wheelAccum += delta * stepPx;
        int movePx = m_wheelAccum / WHEEL_DELTA;
        m_wheelAccum %= WHEEL_DELTA;
        if (movePx != 0) {
            RECT rc{};
            GetClientRect(hWnd, &rc);
            int totalH = 0;
            {
                std::lock_guard<std::mutex> lock(m_itemsMutex);
                totalH = static_cast<int>(m_filteredIndices.size()) * stepPx;
            }
            int maxScroll = std::max<int>(0, totalH - static_cast<int>(rc.bottom - rc.top));
            m_scrollOffset = std::clamp(m_scrollOffset - movePx, 0, maxScroll);
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_KEYDOWN: {
        int rowStep = std::max(1, get_row_step());
        RECT rc{};
        GetClientRect(hWnd, &rc);
        int clientH = rc.bottom - rc.top;

        int totalItems = 0;
        {
            std::lock_guard<std::mutex> lock(m_itemsMutex);
            totalItems = static_cast<int>(m_filteredIndices.size());
        }

        if (wParam == VK_UP) {
            if (totalItems <= 0) return 0;
            m_selectedIndex = std::clamp(m_selectedIndex - 1, 0, totalItems - 1);
            int targetY = m_selectedIndex * rowStep;
            if (targetY < m_scrollOffset) m_scrollOffset = targetY;
            InvalidateRect(hWnd, nullptr, FALSE);
            return 0;
        }
        if (wParam == VK_DOWN) {
            if (totalItems <= 0) return 0;
            m_selectedIndex = std::clamp(m_selectedIndex + 1, 0, totalItems - 1);
            int targetY = (m_selectedIndex + 1) * rowStep;
            if (targetY > m_scrollOffset + clientH) {
                m_scrollOffset = targetY - clientH;
            }
            InvalidateRect(hWnd, nullptr, FALSE);
            return 0;
        }
        if (wParam == VK_PRIOR) {
            if (totalItems <= 0) return 0;
            m_scrollOffset = std::max(0, m_scrollOffset - clientH);
            InvalidateRect(hWnd, nullptr, FALSE);
            return 0;
        }
        if (wParam == VK_NEXT) {
            if (totalItems <= 0) return 0;
            int maxScroll = std::max<int>(0, totalItems * rowStep - clientH);
            m_scrollOffset = std::min(maxScroll, m_scrollOffset + clientH);
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
        POINT pt{};
        GetCursorPos(&pt);
        ScreenToClient(hWnd, &pt);
        RECT rc{};
        GetClientRect(hWnd, &rc);
        int totalItems = 0;
        {
            std::lock_guard<std::mutex> lock(m_itemsMutex);
            totalItems = static_cast<int>(m_filteredIndices.size());
        }
        bool needScroll = (totalItems * get_row_step()) > (rc.bottom - rc.top);
        if (needScroll && pt.x >= (rc.right - rc.left - dp(17))) {
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        } else {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
        }
        return TRUE;
    }

    case WM_NCDESTROY: {
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, 0);
        if (m_hListWnd == hWnd) {
            m_hListWnd = nullptr;
        }
        return 0;
    }
    }

    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

} // namespace quickdns
