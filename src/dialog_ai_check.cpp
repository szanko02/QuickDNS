#include "dialog_ai_check.h"
#include "encoding_utils.h"
#include "resource.h"
#include "ui_palette.h"
#include <dwmapi.h>
#include <windowsx.h>
#include <commctrl.h>
#include <algorithm>
#include <sstream>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdiplus.lib")

using QuickDns::Encoding::wide_to_utf8;
using QuickDns::Encoding::utf8_to_wide;

namespace {
    constexpr const wchar_t* AI_DIALOG_CLASS_NAME = L"QuickDNS_AiCheckDialogClass";
    constexpr const wchar_t* AI_LIST_CLASS_NAME   = L"QuickDNS_AiCheckListClass";

    constexpr int DIALOG_CLIENT_WIDTH  = 960;
    constexpr int DIALOG_CLIENT_HEIGHT = 620;

    constexpr UINT WM_APP_AI_CHECK_UPDATE = WM_APP + 20;

    constexpr int ROW_HEIGHT = 50;
    constexpr int ROW_GAP    = 6;
    constexpr int ROW_STEP   = ROW_HEIGHT + ROW_GAP;

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

std::optional<std::string> AiCheckDialog::show(
    HWND hParent,
    HINSTANCE hInstance,
    const std::vector<DnsProfile>& profiles,
    const std::string& currentActiveId,
    bool isDarkMode)
{
    AiCheckDialog dlg(hParent, hInstance, profiles, currentActiveId, isDarkMode);
    if (!dlg.create_dialog_window()) {
        return std::nullopt;
    }

    EnableWindow(hParent, FALSE);

    dlg.start_check();

    MSG msg;
    while (!dlg.m_finished && GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessageW(dlg.m_hWnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    dlg.stop_check();

    EnableWindow(hParent, TRUE);
    SetForegroundWindow(hParent);

    if (dlg.m_hWnd) {
        DestroyWindow(dlg.m_hWnd);
        dlg.m_hWnd = nullptr;
    }

    return dlg.m_selectedProfileId;
}

AiCheckDialog::AiCheckDialog(
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

    // Filter non-DHCP profiles: SmartDNS profiles first, then standard/custom DNS profiles
    std::vector<DnsProfile> smartProfiles;
    std::vector<DnsProfile> standardProfiles;
    for (const auto& p : profiles) {
        if (p.is_dhcp || p.primary_ipv4.empty()) continue;
        if (p.is_smart_dns) {
            smartProfiles.push_back(p);
        } else {
            standardProfiles.push_back(p);
        }
    }

    auto appendProfile = [&](const DnsProfile& p) {
        AiCheckRowItem item;
        item.profile = p;
        item.result.profile_id = p.id;
        item.result.profile_name = p.name;
        item.result.dns_ip_used = p.primary_ipv4;
        item.result.chatgpt = { AiService::ChatGPT, AiAccessStatus::Checking, "", -1, L"Проверка..." };
        item.result.claude  = { AiService::Claude,  AiAccessStatus::Checking, "", -1, L"Проверка..." };
        item.result.gemini  = { AiService::Gemini,  AiAccessStatus::Checking, "", -1, L"Проверка..." };
        item.result.copilot = { AiService::Copilot, AiAccessStatus::Checking, "", -1, L"Проверка..." };
        item.result.available_count = 0;
        item.result.avg_latency_ms = -1;
        item.is_completed = false;
        item.is_best = false;
        m_rows.push_back(std::move(item));
    };

    for (const auto& p : smartProfiles) appendProfile(p);
    for (const auto& p : standardProfiles) appendProfile(p);

    for (size_t i = 0; i < m_rows.size(); ++i) {
        if (m_rows[i].profile.id == m_currentActiveId) {
            m_selectedIndex = static_cast<int>(i);
            break;
        }
    }

    m_verdictTitle = L"Идет проверка доступности ChatGPT, Claude, Gemini и Copilot...";
    m_verdictSub = L"Опрашиваем все DNS-профили параллельно напрямую с вашего IP-адреса";

    setup_fonts();
}

AiCheckDialog::~AiCheckDialog() {
    stop_check();

    m_gpFontTotal.reset();
    m_gpFontPill.reset();
    m_gpFontBadge.reset();
    m_gpFontSub.reset();
    m_gpFontTitle.reset();
    m_gpMonoFamily.reset();
    m_gpFontFamily.reset();

    if (m_hFontRegular) DeleteObject(m_hFontRegular);
    if (m_hFontBold) DeleteObject(m_hFontBold);
    if (m_hFontHeader) DeleteObject(m_hFontHeader);
    if (m_hFontMonospace) DeleteObject(m_hFontMonospace);
    if (m_hFontSmall) DeleteObject(m_hFontSmall);
    if (m_hFontSmallBold) DeleteObject(m_hFontSmallBold);

    if (m_hbrBg) DeleteObject(m_hbrBg);
    if (m_hbrCard) DeleteObject(m_hbrCard);

    UnregisterClassW(AI_DIALOG_CLASS_NAME, m_hInstance);
    UnregisterClassW(AI_LIST_CLASS_NAME, m_hInstance);
}

void AiCheckDialog::setup_fonts() {
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

    m_hFontSmallBold = CreateFontW(-11, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    m_gpFontFamily = std::make_unique<Gdiplus::FontFamily>(L"Segoe UI");
    m_gpMonoFamily = std::make_unique<Gdiplus::FontFamily>(L"Consolas");
    m_gpFontTitle  = std::make_unique<Gdiplus::Font>(m_gpFontFamily.get(), 9.5f, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);
    m_gpFontSub    = std::make_unique<Gdiplus::Font>(m_gpFontFamily.get(), 8.2f, Gdiplus::FontStyleRegular, Gdiplus::UnitPoint);
    m_gpFontBadge  = std::make_unique<Gdiplus::Font>(m_gpFontFamily.get(), 7.8f, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);
    m_gpFontPill   = std::make_unique<Gdiplus::Font>(m_gpFontFamily.get(), 8.2f, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);
    m_gpFontTotal  = std::make_unique<Gdiplus::Font>(m_gpMonoFamily.get(), 10.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);
}

bool AiCheckDialog::create_dialog_window() {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &AiCheckDialog::dialog_proc_thunk;
    wc.hInstance = m_hInstance;
    wc.hIcon = LoadIconW(m_hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
    if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = AI_DIALOG_CLASS_NAME;
    RegisterClassExW(&wc);

    WNDCLASSEXW lwc = {};
    lwc.cbSize = sizeof(WNDCLASSEXW);
    lwc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    lwc.lpfnWndProc = &AiCheckDialog::list_proc_thunk;
    lwc.hInstance = m_hInstance;
    lwc.hCursor = LoadCursorW(nullptr, IDC_HAND);
    lwc.hbrBackground = nullptr;
    lwc.lpszClassName = AI_LIST_CLASS_NAME;
    RegisterClassExW(&lwc);

    DWORD dwStyle = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MAXIMIZEBOX | WS_MINIMIZEBOX | WS_VISIBLE;
    RECT wr = { 0, 0, DIALOG_CLIENT_WIDTH, DIALOG_CLIENT_HEIGHT };
    AdjustWindowRectEx(&wr, dwStyle, FALSE, 0);
    int winWidth = wr.right - wr.left;
    int winHeight = wr.bottom - wr.top;

    RECT parentRect = {};
    if (m_hParent && GetWindowRect(m_hParent, &parentRect)) {
        int posX = parentRect.left + ((parentRect.right - parentRect.left) - winWidth) / 2;
        int posY = parentRect.top + ((parentRect.bottom - parentRect.top) - winHeight) / 2;
        int screenW = GetSystemMetrics(SM_CXSCREEN);
        int screenH = GetSystemMetrics(SM_CYSCREEN);
        posX = std::clamp(posX, 10, std::max(10, screenW - winWidth - 10));
        posY = std::clamp(posY, 10, std::max(10, screenH - winHeight - 10));

        m_hWnd = CreateWindowExW(
            WS_EX_DLGMODALFRAME,
            AI_DIALOG_CLASS_NAME,
            L"Проверка доступности ИИ (ChatGPT, Claude, Gemini, Copilot)",
            dwStyle,
            posX, posY,
            winWidth, winHeight,
            m_hParent,
            nullptr,
            m_hInstance,
            this
        );
    } else {
        m_hWnd = CreateWindowExW(
            WS_EX_DLGMODALFRAME,
            AI_DIALOG_CLASS_NAME,
            L"Проверка доступности ИИ (ChatGPT, Claude, Gemini, Copilot)",
            dwStyle,
            CW_USEDEFAULT, CW_USEDEFAULT,
            winWidth, winHeight,
            m_hParent,
            nullptr,
            m_hInstance,
            this
        );
    }

    if (!m_hWnd) {
        return false;
    }

    apply_theme();
    create_controls();
    return true;
}

void AiCheckDialog::apply_theme() {
    BOOL useDarkMode = m_isDarkMode ? TRUE : FALSE;
    DwmSetWindowAttribute(m_hWnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &useDarkMode, sizeof(useDarkMode));
    DwmSetWindowAttribute(m_hWnd, 19, &useDarkMode, sizeof(useDarkMode));
    DWORD cornerPref = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(m_hWnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &cornerPref, sizeof(cornerPref));
}

void AiCheckDialog::create_controls() {
    RECT rcClient = {};
    GetClientRect(m_hWnd, &rcClient);
    int clientWidth = rcClient.right - rcClient.left;
    int clientHeight = rcClient.bottom - rcClient.top;

    int marginX = 20;
    int contentWidth = clientWidth - (marginX * 2);

    int listTop = 166;
    int bottomButtonsH = 38;
    int listBottomMargin = 16;
    int listHeight = std::max(120, clientHeight - listTop - bottomButtonsH - listBottomMargin - 14);

    m_hListWnd = CreateWindowExW(
        0,
        AI_LIST_CLASS_NAME,
        L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        marginX, listTop, contentWidth, listHeight,
        m_hWnd, (HMENU)IDC_LIST_AI_CHECK, m_hInstance, this
    );

    int curY = listTop + listHeight + 14;

    int btnApplyW = 240;
    int btnRecheckW = 190;
    int btnCloseW = 120;

    m_hBtnApply = CreateWindowExW(0, WC_BUTTONW, L"Применить выбранный DNS",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        marginX, curY, btnApplyW, bottomButtonsH,
        m_hWnd, (HMENU)IDC_BTN_AI_APPLY, m_hInstance, nullptr);

    m_hBtnRecheck = CreateWindowExW(0, WC_BUTTONW, L"Проверить заново",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        marginX + btnApplyW + 10, curY, btnRecheckW, bottomButtonsH,
        m_hWnd, (HMENU)IDC_BTN_AI_RECHECK, m_hInstance, nullptr);

    m_hBtnClose = CreateWindowExW(0, WC_BUTTONW, L"Закрыть",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        marginX + contentWidth - btnCloseW, curY, btnCloseW, bottomButtonsH,
        m_hWnd, (HMENU)IDC_BTN_AI_CLOSE, m_hInstance, nullptr);

    attach_button_effects(m_hBtnApply, 301);
    attach_button_effects(m_hBtnRecheck, 302);
    attach_button_effects(m_hBtnClose, 303);

    reposition_controls();
}

void AiCheckDialog::reposition_controls() {
    RECT rcClient = {};
    GetClientRect(m_hWnd, &rcClient);
    int clientWidth = rcClient.right - rcClient.left;
    int clientHeight = rcClient.bottom - rcClient.top;

    int marginX = 20;
    int contentWidth = clientWidth - (marginX * 2);
    if (contentWidth < 300) return;

    int listTop = 166;
    int bottomButtonsH = 38;
    int listBottomMargin = 16;
    int listHeight = std::max(100, clientHeight - listTop - bottomButtonsH - listBottomMargin - 14);

    if (m_hListWnd) {
        SetWindowPos(m_hListWnd, nullptr, marginX, listTop, contentWidth, listHeight, SWP_NOZORDER);
    }

    int curY = listTop + listHeight + 14;
    int btnApplyW = 240;
    int btnRecheckW = 190;
    int btnCloseW = 120;

    if (m_hBtnApply) {
        SetWindowPos(m_hBtnApply, nullptr, marginX, curY, btnApplyW, bottomButtonsH, SWP_NOZORDER);
    }
    if (m_hBtnRecheck) {
        SetWindowPos(m_hBtnRecheck, nullptr, marginX + btnApplyW + 10, curY, btnRecheckW, bottomButtonsH, SWP_NOZORDER);
    }
    if (m_hBtnClose) {
        SetWindowPos(m_hBtnClose, nullptr, marginX + contentWidth - btnCloseW, curY, btnCloseW, bottomButtonsH, SWP_NOZORDER);
    }

    InvalidateRect(m_hWnd, nullptr, FALSE);
    if (m_hListWnd) InvalidateRect(m_hListWnd, nullptr, TRUE);
}

void AiCheckDialog::start_check() {
    stop_check();

    std::vector<DnsProfile> profilesToCheck;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_cancelFlag = false;
        m_isChecking = true;
        m_completedCount = 0;
        m_bestRowIndex = -1;

        for (auto& row : m_rows) {
            row.is_completed = false;
            row.is_best = false;
            row.result.profile_id = row.profile.id;
            row.result.profile_name = row.profile.name;
            row.result.dns_ip_used = row.profile.primary_ipv4;
            row.result.chatgpt = { AiService::ChatGPT, AiAccessStatus::Checking, "", -1, L"Проверка..." };
            row.result.claude  = { AiService::Claude,  AiAccessStatus::Checking, "", -1, L"Проверка..." };
            row.result.gemini  = { AiService::Gemini,  AiAccessStatus::Checking, "", -1, L"Проверка..." };
            row.result.copilot = { AiService::Copilot, AiAccessStatus::Checking, "", -1, L"Проверка..." };
            row.result.available_count = 0;
            row.result.avg_latency_ms = -1;
            profilesToCheck.push_back(row.profile);
        }

        m_verdictTitle = L"Идет проверка доступности ChatGPT, Claude, Gemini и Copilot...";
        m_verdictSub = L"Проверено 0 из " + std::to_wstring(m_rows.size()) + L" DNS-профилей";
    }

    if (m_hBtnRecheck) {
        EnableWindow(m_hBtnRecheck, FALSE);
        SetWindowTextW(m_hBtnRecheck, L"Проверка...");
    }

    if (m_hWnd && IsWindow(m_hWnd)) {
        InvalidateRect(m_hWnd, nullptr, FALSE);
    }
    if (m_hListWnd && IsWindow(m_hListWnd)) {
        InvalidateRect(m_hListWnd, nullptr, FALSE);
    }

    HWND hWnd = m_hWnd;
    m_checkThread = std::thread([this, hWnd, profilesToCheck]() {
        ClientIpInfo ipInfo = AiChecker::get_client_ip_info(2500);
        if (m_cancelFlag.load()) return;

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_clientIpInfo = ipInfo;
            m_clientIpFetched = true;
        }
        if (!m_cancelFlag.load() && hWnd && IsWindow(hWnd)) {
            PostMessageW(hWnd, WM_APP_AI_CHECK_UPDATE, 0, -1);
        }

        AiChecker::check_profiles(
            profilesToCheck,
            ipInfo.country_code,
            &m_cancelFlag,
            [this, hWnd](size_t idx, const ProfileAiCheckResult& res) {
                if (m_cancelFlag.load()) return;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    for (size_t i = 0; i < m_rows.size(); ++i) {
                        if (m_rows[i].profile.id == res.profile_id) {
                            m_rows[i].result = res;
                            m_rows[i].is_completed = true;
                            break;
                        }
                    }
                    m_completedCount++;
                    update_best_verdict_locked();
                }
                if (!m_cancelFlag.load() && hWnd && IsWindow(hWnd)) {
                    PostMessageW(hWnd, WM_APP_AI_CHECK_UPDATE, 0, static_cast<LPARAM>(idx));
                }
            }
        );

        if (m_cancelFlag.load()) return;

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_isChecking = false;
            sort_rows_locked();
            update_best_verdict_locked();
        }

        if (!m_cancelFlag.load() && hWnd && IsWindow(hWnd)) {
            PostMessageW(hWnd, WM_APP_AI_CHECK_UPDATE, 1, 0);
        }
    });
}

void AiCheckDialog::stop_check() {
    m_cancelFlag = true;
    if (m_checkThread.joinable()) {
        m_checkThread.join();
    }
    m_isChecking = false;
}

void AiCheckDialog::sort_rows_locked() {
    std::string preservedId;
    if (m_selectedIndex >= 0 && m_selectedIndex < static_cast<int>(m_rows.size())) {
        preservedId = m_rows[m_selectedIndex].profile.id;
    }

    std::stable_sort(m_rows.begin(), m_rows.end(), [](const AiCheckRowItem& a, const AiCheckRowItem& b) {
        if (a.result.available_count != b.result.available_count) {
            return a.result.available_count > b.result.available_count;
        }
        if (a.profile.is_smart_dns != b.profile.is_smart_dns) {
            return a.profile.is_smart_dns > b.profile.is_smart_dns;
        }
        int latA = (a.result.avg_latency_ms > 0) ? a.result.avg_latency_ms : 999999;
        int latB = (b.result.avg_latency_ms > 0) ? b.result.avg_latency_ms : 999999;
        return latA < latB;
    });

    if (!m_userChangedSelection && !m_rows.empty() && m_rows[0].result.available_count > 0) {
        m_selectedIndex = 0;
    } else if (!preservedId.empty()) {
        for (size_t i = 0; i < m_rows.size(); ++i) {
            if (m_rows[i].profile.id == preservedId) {
                m_selectedIndex = static_cast<int>(i);
                break;
            }
        }
    }
}

void AiCheckDialog::update_best_verdict_locked() {
    int bestIdx = -1;
    int bestCount = -1;
    int bestLatency = 999999;

    for (size_t i = 0; i < m_rows.size(); ++i) {
        m_rows[i].is_best = false;
        if (!m_rows[i].is_completed) continue;

        int cnt = m_rows[i].result.available_count;
        int lat = (m_rows[i].result.avg_latency_ms > 0) ? m_rows[i].result.avg_latency_ms : 999999;

        if (cnt > bestCount || (cnt == bestCount && cnt > 0 && lat < bestLatency)) {
            bestCount = cnt;
            bestLatency = lat;
            bestIdx = static_cast<int>(i);
        }
    }

    m_bestRowIndex = (bestCount > 0) ? bestIdx : -1;
    if (m_bestRowIndex >= 0 && m_bestRowIndex < static_cast<int>(m_rows.size())) {
        m_rows[m_bestRowIndex].is_best = true;
        const auto& bestRow = m_rows[m_bestRowIndex];
        std::wstringstream ss;
        ss << L"Лучший DNS для ИИ: " << utf8_to_wide(bestRow.profile.name)
           << L" — " << bestRow.result.available_count << L"/4 ИИ доступно";
        if (bestRow.result.avg_latency_ms > 0) {
            ss << L" (средний отклик " << bestRow.result.avg_latency_ms << L" мс)";
        }
        m_verdictTitle = ss.str();
    } else if (!m_isChecking.load()) {
        m_verdictTitle = L"Лучший DNS для ИИ: Ни один профиль не открыл доступ к сервисам ИИ (0/4)";
    } else {
        m_verdictTitle = L"Идет проверка доступности ChatGPT, Claude, Gemini и Copilot...";
    }

    if (m_isChecking.load()) {
        m_verdictSub = L"Проверено профилей: " + std::to_wstring(m_completedCount) + L" из " +
                       std::to_wstring(m_rows.size()) + L"  \u2022  Результаты обновляются в реальном времени";
    } else {
        m_verdictSub = L"Проверка завершена (" + std::to_wstring(m_rows.size()) +
                       L" профилей)  \u2022  Выберите DNS в таблице или дважды кликните по строке для применения";
    }
}

void AiCheckDialog::on_apply() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_selectedIndex >= 0 && m_selectedIndex < static_cast<int>(m_rows.size())) {
        m_selectedProfileId = m_rows[m_selectedIndex].profile.id;
        m_finished = true;
    }
}

void AiCheckDialog::on_recheck() {
    if (m_isChecking.load()) return;
    start_check();
}

void AiCheckDialog::on_close() {
    m_cancelFlag = true;
    m_finished = true;
}

void AiCheckDialog::attach_button_effects(HWND hBtn, UINT_PTR subclassId) {
    if (!hBtn) return;
    auto* anim = new ButtonAnimationState();
    SetPropW(hBtn, L"QuickDnsAnim", reinterpret_cast<HANDLE>(anim));
    SetWindowSubclass(hBtn, AnimatedButtonSubclassProc, subclassId, reinterpret_cast<DWORD_PTR>(anim));
}

void AiCheckDialog::draw_custom_button(DRAWITEMSTRUCT* pDis) {
    if (!pDis) return;

    HDC hdc = pDis->hDC;
    RECT rc = pDis->rcItem;
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;

    auto pal = GetPalette(m_isDarkMode);
    int ctlId = pDis->CtlID;

    bool isPrimary = (ctlId == IDC_BTN_AI_APPLY);
    bool isDisabled = (pDis->itemState & ODS_DISABLED) != 0;

    auto* anim = reinterpret_cast<ButtonAnimationState*>(GetPropW(pDis->hwndItem, L"QuickDnsAnim"));
    float hoverVal = anim ? anim->hover : 0.0f;
    bool isPressed = (pDis->itemState & ODS_SELECTED) || (anim && anim->isPressed);

    COLORREF baseBg, hoverBg, pressBg, baseBorder, hoverBorder, textColor;

    if (isPrimary) {
        if (isDisabled) {
            baseBg = m_isDarkMode ? RGB(30, 42, 60) : RGB(210, 222, 238);
            baseBorder = m_isDarkMode ? RGB(45, 58, 80) : RGB(180, 198, 222);
            textColor = m_isDarkMode ? RGB(110, 128, 155) : RGB(130, 150, 180);
            hoverBg = baseBg; pressBg = baseBg; hoverBorder = baseBorder;
        } else {
            baseBg = pal.btnPrimaryBg;
            hoverBg = pal.btnPrimaryHover;
            pressBg = pal.btnPrimaryPress;
            baseBorder = pal.btnPrimaryBorder;
            hoverBorder = pal.btnPrimaryHover;
            textColor = pal.btnPrimaryText;
        }
    } else {
        if (isDisabled) {
            baseBg = m_isDarkMode ? RGB(24, 27, 36) : RGB(241, 245, 249);
            baseBorder = m_isDarkMode ? RGB(38, 43, 56) : RGB(226, 232, 240);
            textColor = m_isDarkMode ? RGB(85, 95, 115) : RGB(148, 163, 184);
            hoverBg = baseBg; pressBg = baseBg; hoverBorder = baseBorder;
        } else {
            baseBg = pal.btnUtilBg;
            hoverBg = pal.btnUtilHover;
            pressBg = pal.btnUtilPress;
            baseBorder = pal.btnUtilBorder;
            hoverBorder = m_isDarkMode ? RGB(70, 85, 110) : RGB(180, 195, 220);
            textColor = pal.btnUtilText;
        }
    }

    COLORREF curBg = isDisabled ? baseBg : (isPressed ? pressBg : LerpColor(baseBg, hoverBg, hoverVal));
    COLORREF curBorder = isDisabled ? baseBorder : LerpColor(baseBorder, hoverBorder, hoverVal);

    HDC hdcMem = CreateCompatibleDC(hdc);
    HBITMAP hbmMem = CreateCompatibleBitmap(hdc, w, h);
    HGDIOBJ oldBmp = SelectObject(hdcMem, hbmMem);

    HBRUSH hbrWin = CreateSolidBrush(pal.bg);
    FillRect(hdcMem, &rc, hbrWin);
    DeleteObject(hbrWin);

    {
        Gdiplus::Graphics g(hdcMem);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

        Gdiplus::GraphicsPath path;
        AddRoundedRect(path, Gdiplus::RectF(0.5f, 0.5f, static_cast<float>(w) - 1.0f, static_cast<float>(h) - 1.0f), 6.0f);

        Gdiplus::SolidBrush br(Gdiplus::Color(255, GetRValue(curBg), GetGValue(curBg), GetBValue(curBg)));
        g.FillPath(&br, &path);

        float penW = isPrimary ? 1.5f : 1.0f;
        Gdiplus::Pen pen(Gdiplus::Color(255, GetRValue(curBorder), GetGValue(curBorder), GetBValue(curBorder)), penW);
        g.DrawPath(&pen, &path);
    }

    wchar_t btnText[128] = {};
    GetWindowTextW(pDis->hwndItem, btnText, 128);

    SetBkMode(hdcMem, TRANSPARENT);
    SetTextColor(hdcMem, textColor);
    HFONT hFont = isPrimary ? m_hFontBold : m_hFontRegular;
    HGDIOBJ oldFont = SelectObject(hdcMem, hFont);

    RECT rcText = rc;
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

void AiCheckDialog::draw_service_pill(
    Gdiplus::Graphics& g,
    const AiServiceCheckResult& svc,
    float x, float y, float w, float h)
{
    Gdiplus::Color fillCol;
    Gdiplus::Color borderCol;
    Gdiplus::Color textCol;
    std::wstring label;

    switch (svc.status) {
    case AiAccessStatus::Available: {
        fillCol   = m_isDarkMode ? Gdiplus::Color(42, 34, 197, 94)  : Gdiplus::Color(30, 22, 163, 74);
        borderCol = m_isDarkMode ? Gdiplus::Color(165, 34, 197, 94) : Gdiplus::Color(150, 22, 163, 74);
        textCol   = m_isDarkMode ? Gdiplus::Color(255, 74, 222, 128) : Gdiplus::Color(255, 21, 128, 61);
        if (svc.latency_ms >= 0) {
            label = L"Доступен \u2022 " + std::to_wstring(svc.latency_ms) + L" мс";
        } else {
            label = L"Доступен";
        }
        break;
    }
    case AiAccessStatus::GeoBlocked: {
        fillCol   = m_isDarkMode ? Gdiplus::Color(42, 245, 158, 11)  : Gdiplus::Color(30, 217, 119, 6);
        borderCol = m_isDarkMode ? Gdiplus::Color(165, 245, 158, 11) : Gdiplus::Color(150, 217, 119, 6);
        textCol   = m_isDarkMode ? Gdiplus::Color(255, 251, 191, 36) : Gdiplus::Color(255, 180, 83, 9);
        label = L"Гео-блок";
        break;
    }
    case AiAccessStatus::DnsFailed: {
        fillCol   = m_isDarkMode ? Gdiplus::Color(40, 239, 68, 68)  : Gdiplus::Color(28, 220, 38, 38);
        borderCol = m_isDarkMode ? Gdiplus::Color(155, 239, 68, 68) : Gdiplus::Color(140, 220, 38, 38);
        textCol   = m_isDarkMode ? Gdiplus::Color(255, 248, 113, 113) : Gdiplus::Color(255, 185, 28, 28);
        label = L"DNS сбой";
        break;
    }
    case AiAccessStatus::Unreachable: {
        fillCol   = m_isDarkMode ? Gdiplus::Color(40, 239, 68, 68)  : Gdiplus::Color(28, 220, 38, 38);
        borderCol = m_isDarkMode ? Gdiplus::Color(155, 239, 68, 68) : Gdiplus::Color(140, 220, 38, 38);
        textCol   = m_isDarkMode ? Gdiplus::Color(255, 248, 113, 113) : Gdiplus::Color(255, 185, 28, 28);
        label = L"Недоступен";
        break;
    }
    case AiAccessStatus::Checking:
    case AiAccessStatus::Pending:
    default: {
        fillCol   = m_isDarkMode ? Gdiplus::Color(38, 59, 130, 246)  : Gdiplus::Color(26, 37, 99, 235);
        borderCol = m_isDarkMode ? Gdiplus::Color(140, 59, 130, 246) : Gdiplus::Color(130, 37, 99, 235);
        textCol   = m_isDarkMode ? Gdiplus::Color(255, 96, 165, 250) : Gdiplus::Color(255, 29, 78, 216);
        label = L"Проверка...";
        break;
    }
    }

    Gdiplus::RectF rcPill(x, y, w, h);
    Gdiplus::GraphicsPath pillPath;
    AddRoundedRect(pillPath, rcPill, 6.0f);

    Gdiplus::SolidBrush fillBr(fillCol);
    Gdiplus::Pen borderPen(borderCol, 1.0f);
    g.FillPath(&fillBr, &pillPath);
    g.DrawPath(&borderPen, &pillPath);

    Gdiplus::SolidBrush textBr(textCol);
    Gdiplus::StringFormat fmt;
    fmt.SetAlignment(Gdiplus::StringAlignmentCenter);
    fmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    fmt.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
    fmt.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);

    g.DrawString(label.c_str(), -1, m_gpFontPill.get(), rcPill, &fmt, &textBr);
}

void AiCheckDialog::draw_list_item(
    Gdiplus::Graphics& g,
    size_t rowIdx,
    int y, int width, int height,
    bool isSelected, bool isHovered)
{
    if (rowIdx >= m_rows.size()) return;
    const auto& row = m_rows[rowIdx];
    auto pal = GetPalette(m_isDarkMode);

    Gdiplus::RectF rcCard(0.5f, static_cast<float>(y) + 0.5f, static_cast<float>(width) - 1.0f, static_cast<float>(height) - 1.0f);
    Gdiplus::GraphicsPath cardPath;
    AddRoundedRect(cardPath, rcCard, 7.0f);

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
        AddRoundedRect(selBarPath, Gdiplus::RectF(2.0f, static_cast<float>(y) + 6.0f, 3.5f, static_cast<float>(height) - 12.0f), 2.0f);
        Gdiplus::SolidBrush selBarBr(Gdiplus::Color(255, GetRValue(borderCol), GetGValue(borderCol), GetBValue(borderCol)));
        g.FillPath(&selBarBr, &selBarPath);
    }

    // Column geometry matching header
    float profileColW = std::max(230.0f, static_cast<float>(width) * 0.30f);
    float totalColW   = 74.0f;
    float svcAreaW    = static_cast<float>(width) - profileColW - totalColW - 10.0f;
    float svcColW     = svcAreaW / 4.0f;

    // Status dot in profile column
    float dotX = 16.0f;
    float dotY = static_cast<float>(y) + static_cast<float>(height) / 2.0f;
    Gdiplus::Color dotColor;
    if (!row.is_completed) {
        dotColor = Gdiplus::Color(255, 59, 130, 246);
    } else if (row.result.available_count == 4) {
        dotColor = Gdiplus::Color(255, 34, 197, 94);
    } else if (row.result.available_count >= 2) {
        dotColor = Gdiplus::Color(255, 245, 158, 11);
    } else if (row.result.available_count == 1) {
        dotColor = Gdiplus::Color(255, 245, 158, 11);
    } else {
        dotColor = Gdiplus::Color(255, 239, 68, 68);
    }

    Gdiplus::SolidBrush auraBr(Gdiplus::Color(50, dotColor.GetR(), dotColor.GetG(), dotColor.GetB()));
    g.FillEllipse(&auraBr, dotX - 6.5f, dotY - 6.5f, 13.0f, 13.0f);
    Gdiplus::SolidBrush dotBr(dotColor);
    g.FillEllipse(&dotBr, dotX - 3.5f, dotY - 3.5f, 7.0f, 7.0f);

    Gdiplus::SolidBrush textPriBr(Gdiplus::Color(255, GetRValue(pal.textPrimary), GetGValue(pal.textPrimary), GetBValue(pal.textPrimary)));
    Gdiplus::SolidBrush textSecBr(Gdiplus::Color(255, GetRValue(pal.textSecondary), GetGValue(pal.textSecondary), GetBValue(pal.textSecondary)));

    // 1. DNS Profile Name + Badges
    std::wstring pName = utf8_to_wide(row.profile.name);
    float textLeft = 30.0f;
    float maxProfileRight = profileColW - 6.0f;

    Gdiplus::RectF nameBounds;
    g.MeasureString(pName.c_str(), -1, m_gpFontTitle.get(), Gdiplus::PointF(0, 0), &nameBounds);

    float availNameW = maxProfileRight - textLeft;
    if (row.profile.id == m_currentActiveId) availNameW -= 58.0f;
    if (row.is_best && row.result.available_count > 0) availNameW -= 60.0f;
    availNameW = std::max(70.0f, availNameW);

    Gdiplus::StringFormat fmtName;
    fmtName.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
    fmtName.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
    float drawNameW = std::min(nameBounds.Width + 4.0f, availNameW);
    g.DrawString(pName.c_str(), -1, m_gpFontTitle.get(),
        Gdiplus::RectF(textLeft, static_cast<float>(y) + 6.0f, drawNameW, 18.0f), &fmtName, &textPriBr);

    float badgeX = textLeft + drawNameW + 4.0f;
    if (row.profile.id == m_currentActiveId && badgeX + 54.0f <= maxProfileRight) {
        Gdiplus::SolidBrush activeBr(Gdiplus::Color(255, GetRValue(pal.accentBlue), GetGValue(pal.accentBlue), GetBValue(pal.accentBlue)));
        g.DrawString(L"[Активен]", -1, m_gpFontBadge.get(), Gdiplus::PointF(badgeX, static_cast<float>(y) + 8.0f), &activeBr);
        badgeX += 56.0f;
    }
    if (row.is_best && row.result.available_count > 0 && badgeX + 54.0f <= maxProfileRight) {
        Gdiplus::SolidBrush bestBr(Gdiplus::Color(255, 34, 197, 94));
        g.DrawString(L"[Лучший]", -1, m_gpFontBadge.get(), Gdiplus::PointF(badgeX, static_cast<float>(y) + 8.0f), &bestBr);
        badgeX += 56.0f;
    }

    // Sub-line: primary IP + SmartDNS tag / avg latency
    std::wstring subLine = utf8_to_wide(row.profile.primary_ipv4);
    if (row.profile.is_smart_dns) {
        subLine += L" \u2022 SmartDNS";
    }
    if (row.is_completed && row.result.avg_latency_ms > 0) {
        subLine += L" \u2022 ср. " + std::to_wstring(row.result.avg_latency_ms) + L" мс";
    }
    g.DrawString(subLine.c_str(), -1, m_gpFontSub.get(),
        Gdiplus::RectF(textLeft, static_cast<float>(y) + 27.0f, maxProfileRight - textLeft, 17.0f), &fmtName, &textSecBr);

    // 2..5. Service Status Pills (ChatGPT, Claude, Gemini, Copilot)
    float pillH = 26.0f;
    float pillY = static_cast<float>(y) + (static_cast<float>(height) - pillH) / 2.0f;
    float pillPadX = 4.0f;
    float pillW = std::max(60.0f, svcColW - (pillPadX * 2.0f));

    draw_service_pill(g, row.result.chatgpt, profileColW + 0.0f * svcColW + pillPadX, pillY, pillW, pillH);
    draw_service_pill(g, row.result.claude,  profileColW + 1.0f * svcColW + pillPadX, pillY, pillW, pillH);
    draw_service_pill(g, row.result.gemini,  profileColW + 2.0f * svcColW + pillPadX, pillY, pillW, pillH);
    draw_service_pill(g, row.result.copilot, profileColW + 3.0f * svcColW + pillPadX, pillY, pillW, pillH);

    // 6. Total Badge (X/4)
    float totalX = static_cast<float>(width) - totalColW;
    float badgeW = totalColW - 10.0f;
    float badgeH = 26.0f;
    float badgeY = pillY;

    Gdiplus::Color totFill;
    Gdiplus::Color totBorder;
    Gdiplus::Color totText;
    std::wstring totStr;

    if (!row.is_completed) {
        totFill   = m_isDarkMode ? Gdiplus::Color(35, 96, 165, 250) : Gdiplus::Color(25, 37, 99, 235);
        totBorder = m_isDarkMode ? Gdiplus::Color(120, 96, 165, 250) : Gdiplus::Color(110, 37, 99, 235);
        totText   = m_isDarkMode ? Gdiplus::Color(255, 147, 197, 253) : Gdiplus::Color(255, 29, 78, 216);
        totStr    = L"...";
    } else if (row.result.available_count == 4) {
        totFill   = m_isDarkMode ? Gdiplus::Color(55, 34, 197, 94)  : Gdiplus::Color(38, 22, 163, 74);
        totBorder = m_isDarkMode ? Gdiplus::Color(190, 34, 197, 94) : Gdiplus::Color(170, 22, 163, 74);
        totText   = m_isDarkMode ? Gdiplus::Color(255, 74, 222, 128) : Gdiplus::Color(255, 21, 128, 61);
        totStr    = L"4/4";
    } else if (row.result.available_count >= 2) {
        totFill   = m_isDarkMode ? Gdiplus::Color(50, 245, 158, 11)  : Gdiplus::Color(35, 217, 119, 6);
        totBorder = m_isDarkMode ? Gdiplus::Color(180, 245, 158, 11) : Gdiplus::Color(160, 217, 119, 6);
        totText   = m_isDarkMode ? Gdiplus::Color(255, 251, 191, 36) : Gdiplus::Color(255, 180, 83, 9);
        totStr    = std::to_wstring(row.result.available_count) + L"/4";
    } else {
        totFill   = m_isDarkMode ? Gdiplus::Color(40, 239, 68, 68)  : Gdiplus::Color(28, 220, 38, 38);
        totBorder = m_isDarkMode ? Gdiplus::Color(150, 239, 68, 68) : Gdiplus::Color(135, 220, 38, 38);
        totText   = m_isDarkMode ? Gdiplus::Color(255, 248, 113, 113) : Gdiplus::Color(255, 185, 28, 28);
        totStr    = std::to_wstring(row.result.available_count) + L"/4";
    }

    Gdiplus::RectF rcTot(totalX + 2.0f, badgeY, badgeW, badgeH);
    Gdiplus::GraphicsPath totPath;
    AddRoundedRect(totPath, rcTot, 6.0f);
    Gdiplus::SolidBrush totFillBr(totFill);
    Gdiplus::Pen totBorderPen(totBorder, 1.2f);
    g.FillPath(&totFillBr, &totPath);
    g.DrawPath(&totBorderPen, &totPath);

    Gdiplus::SolidBrush totTextBr(totText);
    Gdiplus::StringFormat fmtTot;
    fmtTot.SetAlignment(Gdiplus::StringAlignmentCenter);
    fmtTot.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    g.DrawString(totStr.c_str(), -1, m_gpFontTotal.get(), rcTot, &fmtTot, &totTextBr);
}

LRESULT CALLBACK AiCheckDialog::dialog_proc_thunk(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    AiCheckDialog* pThis = nullptr;
    if (uMsg == WM_NCCREATE) {
        auto* pCreate = reinterpret_cast<CREATESTRUCTW*>(lParam);
        pThis = reinterpret_cast<AiCheckDialog*>(pCreate->lpCreateParams);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pThis));
        pThis->m_hWnd = hWnd;
    } else {
        pThis = reinterpret_cast<AiCheckDialog*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    }

    if (pThis) {
        return pThis->handle_message(hWnd, uMsg, wParam, lParam);
    }
    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

LRESULT AiCheckDialog::handle_message(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_GETMINMAXINFO: {
        auto* pMMI = reinterpret_cast<MINMAXINFO*>(lParam);
        RECT rcMin = { 0, 0, 820, 480 };
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

    case WM_ERASEBKGND:
        return 1;

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

        HBRUSH hbrBg = CreateSolidBrush(pal.bg);
        FillRect(hdcMem, &rc, hbrBg);
        DeleteObject(hbrBg);

        {
            Gdiplus::Graphics g(hdcMem);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
            g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

            int marginX = 20;
            int contentW = w - (marginX * 2);

            Gdiplus::FontFamily ffDisplay(L"Segoe UI Variable Display");
            Gdiplus::FontFamily ffRegular(L"Segoe UI");
            Gdiplus::Font fontHeader(&ffDisplay, 13.5f, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);
            Gdiplus::Font fontSub(&ffRegular, 9.0f, Gdiplus::FontStyleRegular, Gdiplus::UnitPoint);
            Gdiplus::Font fontBold(&ffRegular, 9.5f, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);
            Gdiplus::Font fontColHdr(&ffRegular, 8.5f, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);

            Gdiplus::SolidBrush textPriBr(Gdiplus::Color(255, GetRValue(pal.textPrimary), GetGValue(pal.textPrimary), GetBValue(pal.textPrimary)));
            Gdiplus::SolidBrush textSecBr(Gdiplus::Color(255, GetRValue(pal.textSecondary), GetGValue(pal.textSecondary), GetBValue(pal.textSecondary)));
            Gdiplus::SolidBrush textMutedBr(Gdiplus::Color(255, GetRValue(pal.textMuted), GetGValue(pal.textMuted), GetBValue(pal.textMuted)));

            // 1. Title & Client IP Subtitle
            g.DrawString(L"Проверка доступности ИИ (ChatGPT, Claude, Gemini, Copilot)",
                -1, &fontHeader, Gdiplus::PointF(static_cast<float>(marginX), 12.0f), &textPriBr);

            std::wstring ipSubtitle;
            std::wstring verdictTitle;
            std::wstring verdictSub;
            int completedCnt = 0;
            int totalCnt = 0;
            bool hasBest = false;
            bool checking = m_isChecking.load();

            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (!m_clientIpFetched) {
                    ipSubtitle = L"Текущий IP клиента: Определение...  |  Проверка напрямую с вашего IP через каждый DNS-сервер";
                } else if (m_clientIpInfo.valid && !m_clientIpInfo.ip.empty()) {
                    std::wstring reg = m_clientIpInfo.country_code.empty() ? L"—" : utf8_to_wide(m_clientIpInfo.country_code);
                    ipSubtitle = L"Текущий IP клиента: " + utf8_to_wide(m_clientIpInfo.ip) +
                                 L" (Регион: " + reg + L")  |  Проверка напрямую с вашего IP через каждый DNS-сервер";
                } else {
                    ipSubtitle = L"Текущий IP клиента: Не определен  |  Проверка напрямую с вашего IP через каждый DNS-сервер";
                }
                verdictTitle = m_verdictTitle;
                verdictSub   = m_verdictSub;
                completedCnt = m_completedCount;
                totalCnt     = static_cast<int>(m_rows.size());
                hasBest      = (m_bestRowIndex >= 0);
            }

            g.DrawString(ipSubtitle.c_str(), -1, &fontSub, Gdiplus::PointF(static_cast<float>(marginX), 36.0f), &textSecBr);

            // 2. Verdict Card (Y = 60..126)
            Gdiplus::RectF rcVerdict(static_cast<float>(marginX), 60.0f, static_cast<float>(contentW), 66.0f);
            Gdiplus::GraphicsPath vPath;
            AddRoundedRect(vPath, rcVerdict, 8.0f);
            Gdiplus::SolidBrush cardBr(Gdiplus::Color(255, GetRValue(pal.cardBg), GetGValue(pal.cardBg), GetBValue(pal.cardBg)));
            Gdiplus::Pen cardPen(Gdiplus::Color(255, GetRValue(pal.cardBorder), GetGValue(pal.cardBorder), GetBValue(pal.cardBorder)), 1.0f);
            g.FillPath(&cardBr, &vPath);
            g.DrawPath(&cardPen, &vPath);

            Gdiplus::Color vTitleCol = hasBest
                ? (m_isDarkMode ? Gdiplus::Color(255, 74, 222, 128) : Gdiplus::Color(255, 21, 128, 61))
                : Gdiplus::Color(255, GetRValue(pal.textPrimary), GetGValue(pal.textPrimary), GetBValue(pal.textPrimary));
            Gdiplus::SolidBrush vTitleBr(vTitleCol);

            g.DrawString(verdictTitle.c_str(), -1, &fontBold, Gdiplus::PointF(static_cast<float>(marginX + 14), 69.0f), &vTitleBr);
            g.DrawString(verdictSub.c_str(), -1, &fontSub, Gdiplus::PointF(static_cast<float>(marginX + 14), 92.0f), &textSecBr);

            if (checking && totalCnt > 0) {
                float progX = static_cast<float>(marginX + 14);
                float progY = 114.0f;
                float progW = static_cast<float>(contentW - 28);
                float progH = 6.0f;

                Gdiplus::RectF rcTrack(progX, progY, progW, progH);
                Gdiplus::GraphicsPath trackPath;
                AddRoundedRect(trackPath, rcTrack, 3.0f);
                Gdiplus::SolidBrush trackBg(m_isDarkMode ? Gdiplus::Color(255, 38, 44, 56) : Gdiplus::Color(255, 226, 232, 240));
                g.FillPath(&trackBg, &trackPath);

                float ratio = std::clamp(static_cast<float>(completedCnt) / static_cast<float>(totalCnt), 0.0f, 1.0f);
                if (ratio > 0.01f) {
                    float fillW = std::max(8.0f, progW * ratio);
                    Gdiplus::RectF rcFill(progX, progY, fillW, progH);
                    Gdiplus::GraphicsPath fillPath;
                    AddRoundedRect(fillPath, rcFill, 3.0f);
                    Gdiplus::SolidBrush fillBr(Gdiplus::Color(255, 34, 197, 94));
                    g.FillPath(&fillBr, &fillPath);
                }
            }

            // 3. Table Column Header Bar (Y = 134..160)
            int listH = std::max(100, h - 166 - 38 - 16 - 14);
            bool needScroll = (totalCnt * ROW_STEP) > listH;
            int itemW = needScroll ? (contentW - 7 - 4 - 6) : contentW;

            float profileColW = std::max(230.0f, static_cast<float>(itemW) * 0.30f);
            float totalColW   = 74.0f;
            float svcAreaW    = static_cast<float>(itemW) - profileColW - totalColW - 10.0f;
            float svcColW     = svcAreaW / 4.0f;

            float hdrY = 138.0f;
            float hdrH = 22.0f;

            g.DrawString(L"DNS Профиль", -1, &fontColHdr, Gdiplus::PointF(static_cast<float>(marginX + 14), hdrY + 2.0f), &textMutedBr);

            Gdiplus::StringFormat fmtCenter;
            fmtCenter.SetAlignment(Gdiplus::StringAlignmentCenter);
            fmtCenter.SetLineAlignment(Gdiplus::StringAlignmentCenter);

            float baseLeft = static_cast<float>(marginX);
            g.DrawString(L"ChatGPT", -1, &fontColHdr, Gdiplus::RectF(baseLeft + profileColW + 0.0f * svcColW, hdrY, svcColW, hdrH), &fmtCenter, &textMutedBr);
            g.DrawString(L"Claude",  -1, &fontColHdr, Gdiplus::RectF(baseLeft + profileColW + 1.0f * svcColW, hdrY, svcColW, hdrH), &fmtCenter, &textMutedBr);
            g.DrawString(L"Gemini",  -1, &fontColHdr, Gdiplus::RectF(baseLeft + profileColW + 2.0f * svcColW, hdrY, svcColW, hdrH), &fmtCenter, &textMutedBr);
            g.DrawString(L"Copilot", -1, &fontColHdr, Gdiplus::RectF(baseLeft + profileColW + 3.0f * svcColW, hdrY, svcColW, hdrH), &fmtCenter, &textMutedBr);
            g.DrawString(L"Итог",    -1, &fontColHdr, Gdiplus::RectF(baseLeft + static_cast<float>(itemW) - totalColW, hdrY, totalColW, hdrH), &fmtCenter, &textMutedBr);
        }

        BitBlt(hdc, 0, 0, w, h, hdcMem, 0, 0, SRCCOPY);
        SelectObject(hdcMem, oldBmp);
        DeleteObject(hbmMem);
        DeleteDC(hdcMem);
        EndPaint(hWnd, &ps);
        return 0;
    }

    case WM_APP_AI_CHECK_UPDATE: {
        if (wParam == 1 && m_hBtnRecheck) {
            EnableWindow(m_hBtnRecheck, TRUE);
            SetWindowTextW(m_hBtnRecheck, L"Проверить заново");
        }
        RECT rcClient = {};
        GetClientRect(hWnd, &rcClient);
        RECT rcHeader = { 0, 0, rcClient.right, 166 };
        InvalidateRect(hWnd, &rcHeader, FALSE);
        if (m_hListWnd && IsWindow(m_hListWnd)) {
            InvalidateRect(m_hListWnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (id == IDC_BTN_AI_APPLY) {
            on_apply();
            return 0;
        }
        if (id == IDC_BTN_AI_RECHECK) {
            on_recheck();
            return 0;
        }
        if (id == IDC_BTN_AI_CLOSE || id == IDCANCEL) {
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

    case WM_CLOSE: {
        on_close();
        return 0;
    }
    }

    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

LRESULT CALLBACK AiCheckDialog::list_proc_thunk(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    AiCheckDialog* pThis = nullptr;
    if (uMsg == WM_NCCREATE) {
        auto* pCreate = reinterpret_cast<CREATESTRUCTW*>(lParam);
        pThis = reinterpret_cast<AiCheckDialog*>(pCreate->lpCreateParams);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pThis));
    } else {
        pThis = reinterpret_cast<AiCheckDialog*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    }

    if (pThis) {
        return pThis->handle_list_message(hWnd, uMsg, wParam, lParam);
    }
    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

LRESULT AiCheckDialog::handle_list_message(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_ERASEBKGND:
        return 1;

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

        HBRUSH hbrBg = CreateSolidBrush(pal.bg);
        FillRect(hdcMem, &rc, hbrBg);
        DeleteObject(hbrBg);

        {
            Gdiplus::Graphics g(hdcMem);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
            g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

            std::lock_guard<std::mutex> lock(m_mutex);
            int totalItems = static_cast<int>(m_rows.size());
            int totalContentHeight = totalItems * ROW_STEP;
            bool needScroll = totalContentHeight > h;
            int maxScroll = needScroll ? (totalContentHeight - h) : 0;
            m_scrollOffset = std::clamp(m_scrollOffset, 0, maxScroll);

            int scrollBarW = 7;
            int scrollBarMarginRight = 4;
            int itemW = needScroll ? (w - scrollBarW - scrollBarMarginRight - 6) : w;

            for (int i = 0; i < totalItems; ++i) {
                int y = i * ROW_STEP - m_scrollOffset;
                if (y + ROW_HEIGHT < 0 || y > h) continue;

                bool isSelected = (i == m_selectedIndex);
                bool isHovered  = (i == m_hoveredIndex);
                draw_list_item(g, static_cast<size_t>(i), y, itemW, ROW_HEIGHT, isSelected, isHovered);
            }

            if (needScroll) {
                float sbX = static_cast<float>(w - scrollBarMarginRight - scrollBarW);
                float sbY = 4.0f;
                float sbH = static_cast<float>(h - 8);

                Gdiplus::SolidBrush trackBr(m_isDarkMode ? Gdiplus::Color(25, 255, 255, 255) : Gdiplus::Color(20, 0, 0, 0));
                Gdiplus::GraphicsPath trackPath;
                AddRoundedRect(trackPath, Gdiplus::RectF(sbX, sbY, static_cast<float>(scrollBarW), sbH), 3.5f);
                g.FillPath(&trackBr, &trackPath);

                float thumbH = std::max(32.0f, (static_cast<float>(h) / static_cast<float>(totalContentHeight)) * sbH);
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
                AddRoundedRect(thumbPath, Gdiplus::RectF(sbX, thumbY, static_cast<float>(scrollBarW), thumbH), 3.5f);
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
        RECT rc;
        GetClientRect(hWnd, &rc);
        int w = rc.right - rc.left;
        int h = rc.bottom - rc.top;

        int totalItems = 0;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            totalItems = static_cast<int>(m_rows.size());
        }
        int totalContentHeight = totalItems * ROW_STEP;
        bool needScroll = totalContentHeight > h;
        int maxScroll = needScroll ? (totalContentHeight - h) : 0;

        if (m_isDraggingScroll) {
            if (needScroll) {
                float sbH = static_cast<float>(h - 8);
                float thumbH = std::max(32.0f, (static_cast<float>(h) / static_cast<float>(totalContentHeight)) * sbH);
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

        bool overScrollArea = needScroll && (mx >= (w - 17));
        if (overScrollArea) {
            if (m_hoveredIndex != -1) {
                m_hoveredIndex = -1;
                InvalidateRect(hWnd, nullptr, FALSE);
            }
            float sbY = 4.0f;
            float sbH = static_cast<float>(h - 8);
            float thumbH = std::max(32.0f, (static_cast<float>(h) / static_cast<float>(totalContentHeight)) * sbH);
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
            int idx = contentY / ROW_STEP;
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

        TRACKMOUSEEVENT tme = { sizeof(TRACKMOUSEEVENT), TME_LEAVE, hWnd, 0 };
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
        RECT rc;
        GetClientRect(hWnd, &rc);
        int w = rc.right - rc.left;
        int h = rc.bottom - rc.top;

        int totalItems = 0;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            totalItems = static_cast<int>(m_rows.size());
        }
        int totalContentHeight = totalItems * ROW_STEP;
        bool needScroll = totalContentHeight > h;
        int maxScroll = needScroll ? (totalContentHeight - h) : 0;

        if (needScroll && mx >= (w - 17)) {
            float sbY = 4.0f;
            float sbH = static_cast<float>(h - 8);
            float thumbH = std::max(32.0f, (static_cast<float>(h) / static_cast<float>(totalContentHeight)) * sbH);
            float scrollRatio = (maxScroll > 0) ? (static_cast<float>(m_scrollOffset) / static_cast<float>(maxScroll)) : 0.0f;
            float thumbY = sbY + scrollRatio * (sbH - thumbH);

            if (my >= thumbY && my <= thumbY + thumbH) {
                m_isDraggingScroll = true;
                m_dragStartY = my;
                m_dragStartOffset = m_scrollOffset;
                SetCapture(hWnd);
            } else if (my < thumbY) {
                m_scrollOffset = std::max(0, m_scrollOffset - h);
            } else {
                m_scrollOffset = std::min(maxScroll, m_scrollOffset + h);
            }
            InvalidateRect(hWnd, nullptr, FALSE);
            return 0;
        }

        int contentY = my + m_scrollOffset;
        int idx = contentY / ROW_STEP;
        if (idx >= 0 && idx < totalItems) {
            m_selectedIndex = idx;
            m_userChangedSelection = true;
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        break;
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
        RECT rc;
        GetClientRect(hWnd, &rc);
        int w = rc.right - rc.left;
        if (mx < w - 17) {
            int contentY = my + m_scrollOffset;
            int idx = contentY / ROW_STEP;
            int totalItems = 0;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                totalItems = static_cast<int>(m_rows.size());
            }
            if (idx >= 0 && idx < totalItems) {
                m_selectedIndex = idx;
                m_userChangedSelection = true;
                on_apply();
            }
        }
        break;
    }

    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        RECT rc;
        GetClientRect(hWnd, &rc);
        int totalH = 0;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            totalH = static_cast<int>(m_rows.size()) * ROW_STEP;
        }
        int maxScroll = std::max<int>(0, totalH - static_cast<int>(rc.bottom - rc.top));
        m_scrollOffset -= (delta / WHEEL_DELTA) * ROW_STEP;
        m_scrollOffset = std::clamp(m_scrollOffset, 0, maxScroll);
        InvalidateRect(hWnd, nullptr, FALSE);
        return 0;
    }

    case WM_KEYDOWN: {
        int totalItems = 0;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            totalItems = static_cast<int>(m_rows.size());
        }
        if (wParam == VK_UP) {
            m_selectedIndex = std::max(0, m_selectedIndex - 1);
            m_userChangedSelection = true;
            int targetY = m_selectedIndex * ROW_STEP;
            if (targetY < m_scrollOffset) m_scrollOffset = targetY;
            InvalidateRect(hWnd, nullptr, FALSE);
            return 0;
        }
        if (wParam == VK_DOWN) {
            m_selectedIndex = std::min(std::max(0, totalItems - 1), m_selectedIndex + 1);
            m_userChangedSelection = true;
            RECT rc; GetClientRect(hWnd, &rc);
            int targetY = (m_selectedIndex + 1) * ROW_STEP;
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
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(hWnd, &pt);
        RECT rc;
        GetClientRect(hWnd, &rc);
        int totalItems = 0;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            totalItems = static_cast<int>(m_rows.size());
        }
        bool needScroll = (totalItems * ROW_STEP) > (rc.bottom - rc.top);
        if (needScroll && pt.x >= (rc.right - rc.left - 17)) {
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        } else {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
        }
        return TRUE;
    }
    }

    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

} // namespace quickdns
