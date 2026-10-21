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
#include <iomanip>
#include <sstream>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdiplus.lib")

using QuickDns::Encoding::wide_to_utf8;
using QuickDns::Encoding::utf8_to_wide;

namespace {
    constexpr const wchar_t* DIALOG_CLASS_NAME = L"QuickDNS_BenchmarkDialogClass";
    constexpr const wchar_t* LIST_CLASS_NAME   = L"QuickDNS_BenchmarkListClass";

    constexpr int DIALOG_CLIENT_WIDTH  = 760;
    constexpr int DIALOG_CLIENT_HEIGHT = 700;

    constexpr UINT WM_APP_BENCHMARK_PING_UPDATE = WM_APP + 20;

    constexpr int IDC_COMBO_BENCHMARK_DOMAIN   = 1210;
    constexpr int IDC_COMBO_BENCHMARK_DURATION = 1211;
    constexpr int IDC_BTN_BENCHMARK_START_STOP = 1212;
    constexpr int IDC_BTN_BENCHMARK_RESET      = 1213;
    constexpr int IDC_BTN_BENCHMARK_PAUSE      = 1214;

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
            item.status_text = L"Ожидание...";
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
    if (m_hFontSmallBold) DeleteObject(m_hFontSmallBold);

    if (m_hbrBg) DeleteObject(m_hbrBg);
    if (m_hbrCard) DeleteObject(m_hbrCard);

    UnregisterClassW(DIALOG_CLASS_NAME, m_hInstance);
    UnregisterClassW(LIST_CLASS_NAME, m_hInstance);
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

    m_hFontSmallBold = CreateFontW(-11, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
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
    wc.hbrBackground = nullptr; // Explicitly handled in WM_ERASEBKGND/WM_PAINT
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

void BenchmarkDialog::create_controls() {
    RECT rcClient = {};
    GetClientRect(m_hWnd, &rcClient);
    int clientWidth = rcClient.right - rcClient.left;
    int clientHeight = rcClient.bottom - rcClient.top;

    int marginX = 20;
    int contentWidth = clientWidth - (marginX * 2);

    // Target Domain & Mode Bar (Y = 62)
    int barY = 62;
    int comboDomainW = 210;
    int comboDurW = 180;
    int btnStartW = 180;
    int btnResetW = 110;

    // Domain ComboBox (editable dropdown with popular presets)
    int domainX = marginX + 54;
    m_hComboDomain = CreateWindowExW(0, WC_COMBOBOXW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWN | CBS_AUTOHSCROLL,
        domainX, barY + 7, comboDomainW, 200,
        m_hWnd, (HMENU)IDC_COMBO_BENCHMARK_DOMAIN, m_hInstance, nullptr);
    SendMessageW(m_hComboDomain, WM_SETFONT, (WPARAM)m_hFontRegular, TRUE);

    const wchar_t* presetDomains[] = {
        L"google.com",
        L"youtube.com",
        L"discord.com",
        L"ya.ru",
        L"chatgpt.com",
        L"steamcommunity.com",
        L"roblox.com",
        L"twitch.tv",
        L"cloudflare.com",
        L"github.com"
    };
    for (const auto* d : presetDomains) {
        SendMessageW(m_hComboDomain, CB_ADDSTRING, 0, (LPARAM)d);
    }
    SendMessageW(m_hComboDomain, CB_SETCURSEL, 0, 0);

    // Duration ComboBox
    int durX = domainX + comboDomainW + 68;
    m_hComboDuration = CreateWindowExW(0, WC_COMBOBOXW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
        durX, barY + 7, comboDurW, 160,
        m_hWnd, (HMENU)IDC_COMBO_BENCHMARK_DURATION, m_hInstance, nullptr);
    SendMessageW(m_hComboDuration, WM_SETFONT, (WPARAM)m_hFontRegular, TRUE);

    SendMessageW(m_hComboDuration, CB_ADDSTRING, 0, (LPARAM)L"30 сек (Стандартный)");
    SendMessageW(m_hComboDuration, CB_ADDSTRING, 0, (LPARAM)L"10 сек (Быстрый)");
    SendMessageW(m_hComboDuration, CB_ADDSTRING, 0, (LPARAM)L"60 сек (Глубокий)");
    SendMessageW(m_hComboDuration, CB_ADDSTRING, 0, (LPARAM)L"Непрерывный мониторинг");
    SendMessageW(m_hComboDuration, CB_SETCURSEL, 0, 0);

    // Start / Stop Button
    int startBtnX = durX + comboDurW + 12;
    m_hBtnStartStop = CreateWindowExW(0, WC_BUTTONW, L"Старт (30 сек)",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        startBtnX, barY + 4, btnStartW, 34,
        m_hWnd, (HMENU)IDC_BTN_BENCHMARK_START_STOP, m_hInstance, nullptr);
    attach_button_effects(m_hBtnStartStop, 201);

    // Custom Sparkline Benchmark List
    int listTop = 186;
    int bottomButtonsH = 38;
    int listBottomMargin = 16;
    int listHeight = clientHeight - listTop - bottomButtonsH - listBottomMargin - 16;

    m_hListWnd = CreateWindowExW(
        0,
        LIST_CLASS_NAME,
        L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        marginX, listTop, contentWidth, listHeight,
        m_hWnd, (HMENU)IDC_LIST_BENCHMARK, m_hInstance, this
    );

    int curY = listTop + listHeight + 16;

    // Bottom Action Buttons with modern styling
    int btnApplyW = 210;
    int btnPauseW = 170;
    int btnCloseW = 110;

    m_hBtnApply = CreateWindowExW(0, WC_BUTTONW, L"Применить этот DNS",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        marginX, curY, btnApplyW, bottomButtonsH,
        m_hWnd, (HMENU)IDC_BTN_BENCHMARK_APPLY, m_hInstance, nullptr);

    m_hBtnPauseResume = CreateWindowExW(0, WC_BUTTONW, L"Пауза",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        marginX + btnApplyW + 10, curY, btnPauseW, bottomButtonsH,
        m_hWnd, (HMENU)IDC_BTN_BENCHMARK_PAUSE, m_hInstance, nullptr);

    m_hBtnReset = CreateWindowExW(0, WC_BUTTONW, L"Сбросить историю",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        marginX + btnApplyW + btnPauseW + 20, curY, 160, bottomButtonsH,
        m_hWnd, (HMENU)IDC_BTN_BENCHMARK_RESET, m_hInstance, nullptr);

    m_hBtnClose = CreateWindowExW(0, WC_BUTTONW, L"Закрыть",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        marginX + contentWidth - btnCloseW, curY, btnCloseW, bottomButtonsH,
        m_hWnd, (HMENU)IDC_BTN_BENCHMARK_CLOSE, m_hInstance, nullptr);

    attach_button_effects(m_hBtnApply, 101);
    attach_button_effects(m_hBtnPauseResume, 102);
    attach_button_effects(m_hBtnReset, 103);
    attach_button_effects(m_hBtnClose, 104);

    // Start 30s timed test by default for instant stability and speed insights
    start_timed_test();
}

void BenchmarkDialog::reset_all_stats() {
    std::lock_guard<std::mutex> lock(m_itemsMutex);
    for (auto& it : m_items) {
        it.reset_stats();
    }
    m_fastestIndex = -1;
    m_mostStableIndex = -1;
    m_verdictFastest.clear();
    m_verdictStable.clear();
    m_timedFinished = false;
    m_elapsedSec = 0;

    if (m_hWnd && IsWindow(m_hWnd)) {
        InvalidateRect(m_hWnd, nullptr, FALSE);
    }
}

void BenchmarkDialog::start_timed_test() {
    // Read domain from ComboBox
    wchar_t domBuf[256] = {};
    GetWindowTextW(m_hComboDomain, domBuf, _countof(domBuf));
    std::string rawDomain = wide_to_utf8(domBuf);
    m_targetDomain = DnsBenchmark::sanitize_domain(rawDomain);

    // Read duration
    int curDurIdx = static_cast<int>(SendMessageW(m_hComboDuration, CB_GETCURSEL, 0, 0));
    if (curDurIdx == 1) {
        m_durationMode = BenchmarkDurationMode::Timed10s;
        m_targetDurationSec = 10;
    } else if (curDurIdx == 2) {
        m_durationMode = BenchmarkDurationMode::Timed60s;
        m_targetDurationSec = 60;
    } else if (curDurIdx == 3) {
        m_durationMode = BenchmarkDurationMode::Continuous;
        m_targetDurationSec = 0;
    } else {
        m_durationMode = BenchmarkDurationMode::Timed30s;
        m_targetDurationSec = 30;
    }

    reset_all_stats();

    if (m_durationMode != BenchmarkDurationMode::Continuous) {
        m_isTimedRunning = true;
        if (m_hBtnStartStop) {
            SetWindowTextW(m_hBtnStartStop, L"Остановить");
        }
    } else {
        m_isTimedRunning = false;
        if (m_hBtnStartStop) {
            SetWindowTextW(m_hBtnStartStop, L"Тест стабильности");
        }
    }

    if (m_hWnd && IsWindow(m_hWnd)) {
        InvalidateRect(m_hWnd, nullptr, FALSE);
    }
}

void BenchmarkDialog::stop_timed_test(bool finishedNaturally) {
    m_isTimedRunning = false;
    m_timedFinished = true;

    if (m_hBtnStartStop) {
        std::wstring btnLabel = L"Старт (" + std::to_wstring(m_targetDurationSec > 0 ? m_targetDurationSec : 30) + L" сек)";
        SetWindowTextW(m_hBtnStartStop, btnLabel.c_str());
    }

    update_verdict();

    if (m_hWnd && IsWindow(m_hWnd)) {
        InvalidateRect(m_hWnd, nullptr, FALSE);
    }
}

void BenchmarkDialog::update_verdict() {
    std::lock_guard<std::mutex> lock(m_itemsMutex);

    int bestFastIdx = -1;
    int bestFastPing = 999999;

    int bestStableIdx = -1;
    float bestJitter = 999999.0f;

    for (size_t i = 0; i < m_items.size(); ++i) {
        auto& it = m_items[i];
        it.is_fastest = false;
        it.is_most_stable = false;

        if (it.profile.is_dhcp) continue;
        if (it.avg_ping <= 0 || it.reliability < 50.0f) continue;

        // Fastest: lowest avg_ping with >= 70% reliability
        if (it.reliability >= 70.0f && it.avg_ping < bestFastPing) {
            bestFastPing = it.avg_ping;
            bestFastIdx = static_cast<int>(i);
        }

        // Most Stable: lowest jitter with >= 85% reliability
        if (it.reliability >= 85.0f && it.jitter < bestJitter) {
            bestJitter = it.jitter;
            bestStableIdx = static_cast<int>(i);
        }
    }

    m_fastestIndex = bestFastIdx;
    m_mostStableIndex = bestStableIdx;

    if (bestFastIdx >= 0) {
        m_items[bestFastIdx].is_fastest = true;
        std::wstringstream ss;
        ss << utf8_to_wide(m_items[bestFastIdx].profile.name) << L" (ср. "
           << m_items[bestFastIdx].avg_ping << L" ms, мин. "
           << m_items[bestFastIdx].min_ping << L" ms)";
        m_verdictFastest = ss.str();
    } else {
        m_verdictFastest = L"Нет стабильных ответов";
    }

    if (bestStableIdx >= 0) {
        m_items[bestStableIdx].is_most_stable = true;
        std::wstringstream ss;
        ss << std::fixed << std::setprecision(1);
        ss << utf8_to_wide(m_items[bestStableIdx].profile.name) << L" (джиттер ±"
           << m_items[bestStableIdx].jitter << L" ms, "
           << static_cast<int>(m_items[bestStableIdx].reliability) << L"% стабильность)";
        m_verdictStable = ss.str();
    } else {
        m_verdictStable = L"Недостаточно данных";
    }
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
            std::string currentDomain = m_targetDomain;
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

                futures.push_back(std::async(std::launch::async, [i, ip, isDhcp, currentDomain]() -> std::pair<size_t, int> {
                    if (isDhcp || ip.empty()) return { i, -2 }; // -2 for DHCP
                    int ping = DnsBenchmark::ping_dns_server_domain(ip, currentDomain, 750);
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

            // Timed test progress management
            if (m_isTimedRunning && m_targetDurationSec > 0) {
                m_elapsedSec++;
                if (m_elapsedSec >= m_targetDurationSec) {
                    stop_timed_test(true);
                }
            }

            if (m_hWnd && IsWindow(m_hWnd)) {
                PostMessageW(m_hWnd, WM_APP_BENCHMARK_PING_UPDATE, 0, 0);
            }

            // Sleep 1.0 second between query rounds for balanced load and fast convergence
            for (int s = 0; s < 10 && m_workerRunning && !m_isPaused; ++s) {
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
        SetWindowTextW(m_hBtnPauseResume, m_isPaused ? L"Возобновить" : L"Пауза");
    }
    if (m_hWnd && IsWindow(m_hWnd)) {
        InvalidateRect(m_hWnd, nullptr, FALSE);
    }
}

void BenchmarkDialog::on_apply() {
    std::lock_guard<std::mutex> lock(m_itemsMutex);
    if (m_selectedIndex >= 0 && m_selectedIndex < static_cast<int>(m_items.size())) {
        m_selectedProfileId = m_items[m_selectedIndex].profile.id;
        m_finished = true;
    }
}

void BenchmarkDialog::apply_best_profile(bool applyFastest) {
    std::lock_guard<std::mutex> lock(m_itemsMutex);
    int targetIdx = applyFastest ? m_fastestIndex : m_mostStableIndex;
    if (targetIdx >= 0 && targetIdx < static_cast<int>(m_items.size())) {
        m_selectedProfileId = m_items[targetIdx].profile.id;
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
    bool isPrimary = (pDis->CtlID == IDC_BTN_BENCHMARK_APPLY || pDis->CtlID == IDC_BTN_BENCHMARK_START_STOP);

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

    // 3. Profile Name and Badges
    Gdiplus::FontFamily fontFamily(L"Segoe UI");
    Gdiplus::Font fontTitle(&fontFamily, 10.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);
    Gdiplus::Font fontSub(&fontFamily, 8.5f, Gdiplus::FontStyleRegular, Gdiplus::UnitPoint);
    Gdiplus::Font fontBadge(&fontFamily, 8.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);

    Gdiplus::SolidBrush textPriBr(Gdiplus::Color(255, GetRValue(pal.textPrimary), GetGValue(pal.textPrimary), GetBValue(pal.textPrimary)));
    Gdiplus::SolidBrush textSecBr(Gdiplus::Color(255, GetRValue(pal.textSecondary), GetGValue(pal.textSecondary), GetBValue(pal.textSecondary)));

    std::wstring pName = utf8_to_wide(it.profile.name);
    g.DrawString(pName.c_str(), -1, &fontTitle, Gdiplus::PointF(34.0f, static_cast<float>(y) + 6.0f), &textPriBr);

    Gdiplus::RectF nameBounds;
    g.MeasureString(pName.c_str(), -1, &fontTitle, Gdiplus::PointF(0, 0), &nameBounds);
    float badgeX = 34.0f + nameBounds.Width + 8.0f;

    // SmartDNS badge
    if (it.profile.is_smart_dns) {
        Gdiplus::SolidBrush smartBadgeBr(Gdiplus::Color(255, 34, 197, 94));
        g.DrawString(L"[SmartDNS]", -1, &fontBadge, Gdiplus::PointF(badgeX, static_cast<float>(y) + 8.0f), &smartBadgeBr);
        badgeX += 68.0f;
    }

    // Active profile badge
    if (it.profile.id == m_currentActiveId) {
        Gdiplus::SolidBrush activeBadgeBr(Gdiplus::Color(255, GetRValue(pal.accentBlue), GetGValue(pal.accentBlue), GetBValue(pal.accentBlue)));
        g.DrawString(L"[Текущий]", -1, &fontBadge, Gdiplus::PointF(badgeX, static_cast<float>(y) + 8.0f), &activeBadgeBr);
        badgeX += 62.0f;
    }

    // Winner Badges
    if (it.is_fastest) {
        Gdiplus::SolidBrush fastBadgeBr(Gdiplus::Color(255, 34, 197, 94));
        g.DrawString(L"[Самый быстрый]", -1, &fontBadge, Gdiplus::PointF(badgeX, static_cast<float>(y) + 8.0f), &fastBadgeBr);
        badgeX += 105.0f;
    }
    if (it.is_most_stable) {
        Gdiplus::SolidBrush stableBadgeBr(Gdiplus::Color(255, 59, 130, 246));
        g.DrawString(L"[Самый стабильный]", -1, &fontBadge, Gdiplus::PointF(badgeX, static_cast<float>(y) + 8.0f), &stableBadgeBr);
    }

    // Subtitle: Primary IP & Stability Stats
    std::wstring subInfo;
    if (it.profile.is_dhcp) {
        subInfo = L"DHCP роутера/провайдера";
    } else {
        subInfo = L"IP: " + utf8_to_wide(it.profile.primary_ipv4);
        if (it.avg_ping > 0) {
            std::wstringstream ss;
            ss << std::fixed << std::setprecision(1);
            ss << L"  •  Ср: " << it.avg_ping << L" ms  •  Мин: " << it.min_ping << L"  •  Джиттер: ±" << it.jitter << L" ms";
            subInfo += ss.str();
        } else if (!it.profile.unblocked_summary.empty() && it.profile.is_smart_dns) {
            subInfo += L"  •  " + utf8_to_wide(it.profile.unblocked_summary);
        }
    }
    float pingColX = static_cast<float>(width) - 275.0f;
    Gdiplus::StringFormat fmtSub;
    fmtSub.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
    fmtSub.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
    g.DrawString(subInfo.c_str(), -1, &fontSub, Gdiplus::RectF(34.0f, static_cast<float>(y) + 26.0f, pingColX - 40.0f, 18.0f), &fmtSub, &textSecBr);

    // 4. Ping Column (X ~ width - 275)
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
    if (it.total_queries > 1 && !it.profile.is_dhcp) {
        qualStr += L" (" + std::to_wstring(static_cast<int>(it.reliability)) + L"%)";
    }
    g.DrawString(qualStr.c_str(), -1, &fontSub, Gdiplus::PointF(pingColX, static_cast<float>(y) + 26.0f), &textSecBr);

    // 5. Mini Sparkline Graph Column (X: width - 150, Width: 140, Height: 34)
    float graphX = static_cast<float>(width) - 150.0f;
    float graphY = static_cast<float>(y) + 7.0f;
    float graphW = 140.0f;
    float graphH = 34.0f;

    Gdiplus::GraphicsPath graphCont;
    AddRoundedRect(graphCont, Gdiplus::RectF(graphX, graphY, graphW, graphH), 6.0f);
    Gdiplus::SolidBrush graphBgBr(Gdiplus::Color(255, GetRValue(pal.graphBg), GetGValue(pal.graphBg), GetBValue(pal.graphBg)));
    Gdiplus::Pen graphBorderPen(Gdiplus::Color(255, GetRValue(pal.graphBorder), GetGValue(pal.graphBorder), GetBValue(pal.graphBorder)), 1.0f);
    g.FillPath(&graphBgBr, &graphCont);
    g.DrawPath(&graphBorderPen, &graphCont);

    // Grid line
    Gdiplus::Pen gridPen(Gdiplus::Color(70, GetRValue(pal.graphGrid), GetGValue(pal.graphGrid), GetBValue(pal.graphGrid)), 1.0f);
    gridPen.SetDashStyle(Gdiplus::DashStyleDash);
    g.DrawLine(&gridPen, graphX + 4.0f, graphY + graphH / 2.0f, graphX + graphW - 4.0f, graphY + graphH / 2.0f);

    const auto& hist = it.latency_history;
    if (!hist.empty()) {
        float pLeft = graphX + 6.0f;
        float pRight = graphX + graphW - 6.0f;
        float pTop = graphY + 5.0f;
        float pBottom = graphY + graphH - 5.0f;
        float pWidth = pRight - pLeft;
        float pHeight = pBottom - pTop;

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

            Gdiplus::Pen curvePen(dotColor, 1.8f);
            curvePen.SetLineJoin(Gdiplus::LineJoinRound);
            curvePen.SetStartCap(Gdiplus::LineCapRound);
            curvePen.SetEndCap(Gdiplus::LineCapRound);
            g.DrawLines(&curvePen, pts.data(), static_cast<int>(pts.size()));
        }

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
    case WM_ERASEBKGND:
        return 1; // Seamless double buffer

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

        // Pre-fill window background
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

            // 1. Header Typography
            Gdiplus::FontFamily ffDisplay(L"Segoe UI Variable Display");
            Gdiplus::FontFamily ffRegular(L"Segoe UI");
            Gdiplus::Font fontHeader(&ffDisplay, 13.5f, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);
            Gdiplus::Font fontSub(&ffRegular, 9.0f, Gdiplus::FontStyleRegular, Gdiplus::UnitPoint);
            Gdiplus::Font fontBold(&ffRegular, 9.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);

            Gdiplus::SolidBrush textPriBr(Gdiplus::Color(255, GetRValue(pal.textPrimary), GetGValue(pal.textPrimary), GetBValue(pal.textPrimary)));
            Gdiplus::SolidBrush textSecBr(Gdiplus::Color(255, GetRValue(pal.textSecondary), GetGValue(pal.textSecondary), GetBValue(pal.textSecondary)));

            g.DrawString(L"Тест скорости и стабильности DNS", -1, &fontHeader, Gdiplus::PointF(static_cast<float>(marginX), 14.0f), &textPriBr);
            g.DrawString(L"Замер отклика DNS-серверов в реальном времени и тестирование стабильности для любого выбранного сайта.",
                -1, &fontSub, Gdiplus::PointF(static_cast<float>(marginX), 38.0f), &textSecBr);

            // 2. Control Bar Card Background
            Gdiplus::RectF rcBar(static_cast<float>(marginX), 60.0f, static_cast<float>(contentW), 44.0f);
            Gdiplus::GraphicsPath barPath;
            AddRoundedRect(barPath, rcBar, 8.0f);
            Gdiplus::SolidBrush barCardBr(Gdiplus::Color(255, GetRValue(pal.cardBg), GetGValue(pal.cardBg), GetBValue(pal.cardBg)));
            Gdiplus::Pen barPen(Gdiplus::Color(255, GetRValue(pal.cardBorder), GetGValue(pal.cardBorder), GetBValue(pal.cardBorder)), 1.0f);
            g.FillPath(&barCardBr, &barPath);
            g.DrawPath(&barPen, &barPath);

            // Labels inside bar
            g.DrawString(L"Сайт:", -1, &fontBold, Gdiplus::PointF(static_cast<float>(marginX + 12), 73.0f), &textPriBr);
            int durLabelX = marginX + 54 + 210 + 16;
            g.DrawString(L"Режим:", -1, &fontBold, Gdiplus::PointF(static_cast<float>(durLabelX), 73.0f), &textPriBr);

            // 3. Status / Verdict / Progress Container (Y = 112 to 174)
            Gdiplus::RectF rcVerdict(static_cast<float>(marginX), 112.0f, static_cast<float>(contentW), 62.0f);
            Gdiplus::GraphicsPath vPath;
            AddRoundedRect(vPath, rcVerdict, 8.0f);
            g.FillPath(&barCardBr, &vPath);
            g.DrawPath(&barPen, &vPath);

            if (m_isTimedRunning) {
                // Progress Bar & Countdown Mode
                int totalSec = m_targetDurationSec > 0 ? m_targetDurationSec : 30;
                int remainSec = std::max(0, totalSec - m_elapsedSec);

                std::wstring testMsg = L"Идет замер стабильности для \"" + utf8_to_wide(m_targetDomain) + 
                    L"\" • Осталось: " + std::to_wstring(remainSec) + L" сек (секунда " + std::to_wstring(m_elapsedSec) + L" из " + std::to_wstring(totalSec) + L")";
                g.DrawString(testMsg.c_str(), -1, &fontBold, Gdiplus::PointF(static_cast<float>(marginX + 16), 122.0f), &textPriBr);

                // Animated Progress Track
                float progX = static_cast<float>(marginX + 16);
                float progY = 148.0f;
                float progW = static_cast<float>(contentW - 32);
                float progH = 10.0f;

                Gdiplus::RectF rcTrack(progX, progY, progW, progH);
                Gdiplus::GraphicsPath trackPath;
                AddRoundedRect(trackPath, rcTrack, 5.0f);
                Gdiplus::SolidBrush trackBg(m_isDarkMode ? Gdiplus::Color(255, 38, 44, 56) : Gdiplus::Color(255, 226, 232, 240));
                g.FillPath(&trackBg, &trackPath);

                float ratio = std::clamp(static_cast<float>(m_elapsedSec) / static_cast<float>(totalSec), 0.0f, 1.0f);
                if (ratio > 0.01f) {
                    float fillW = std::max(10.0f, progW * ratio);
                    Gdiplus::RectF rcFill(progX, progY, fillW, progH);
                    Gdiplus::GraphicsPath fillPath;
                    AddRoundedRect(fillPath, rcFill, 5.0f);
                    Gdiplus::SolidBrush fillBr(Gdiplus::Color(255, 34, 197, 94));
                    g.FillPath(&fillBr, &fillPath);
                }
            } else if (m_timedFinished && (!m_verdictFastest.empty() || !m_verdictStable.empty())) {
                // Verdict Highlights: Fastest & Most Stable
                float colW = static_cast<float>(contentW - 24) / 2.0f;

                // Fastest Box
                Gdiplus::SolidBrush greenTextBr(Gdiplus::Color(255, 34, 197, 94));
                g.DrawString(L"Самый быстрый DNS:", -1, &fontBold, Gdiplus::PointF(static_cast<float>(marginX + 14), 121.0f), &greenTextBr);
                g.DrawString(m_verdictFastest.c_str(), -1, &fontSub, Gdiplus::PointF(static_cast<float>(marginX + 14), 142.0f), &textPriBr);

                // Most Stable Box
                Gdiplus::SolidBrush blueTextBr(Gdiplus::Color(255, 59, 130, 246));
                g.DrawString(L"Самый стабильный DNS:", -1, &fontBold, Gdiplus::PointF(static_cast<float>(marginX + 14 + colW), 121.0f), &blueTextBr);
                g.DrawString(m_verdictStable.c_str(), -1, &fontSub, Gdiplus::PointF(static_cast<float>(marginX + 14 + colW), 142.0f), &textPriBr);
            } else {
                // Continuous Monitoring / Ready Mode
                std::wstring readyMsg = L"Целевой сайт: " + utf8_to_wide(m_targetDomain) + L"  •  " +
                    (m_isPaused ? L"Мониторинг приостановлен" : L"Автомониторинг активен (опрос каждые 1.0 сек)");
                g.DrawString(readyMsg.c_str(), -1, &fontBold, Gdiplus::PointF(static_cast<float>(marginX + 16), 122.0f), &textPriBr);

                std::wstring hintMsg = L"Нажмите «Старт» для замера стабильности с определением лучшего сервера, или выберите DNS из списка.";
                g.DrawString(hintMsg.c_str(), -1, &fontSub, Gdiplus::PointF(static_cast<float>(marginX + 16), 144.0f), &textSecBr);
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
        RECT rcVerdict = { 20, 112, DIALOG_CLIENT_WIDTH - 20, 176 };
        InvalidateRect(hWnd, &rcVerdict, FALSE);
        if (m_hListWnd && IsWindow(m_hListWnd)) {
            InvalidateRect(m_hListWnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam);
        int code = HIWORD(wParam);

        if (id == IDC_BTN_BENCHMARK_START_STOP) {
            if (m_isTimedRunning) {
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

        if (id == IDC_COMBO_BENCHMARK_DOMAIN && (code == CBN_SELCHANGE || code == CBN_EDITCHANGE)) {
            wchar_t buf[256] = {};
            GetWindowTextW(m_hComboDomain, buf, _countof(buf));
            m_targetDomain = DnsBenchmark::sanitize_domain(wide_to_utf8(buf));
            return 0;
        }

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

    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLOREDIT: {
        HDC hdc = (HDC)wParam;
        auto pal = GetPalette(m_isDarkMode);
        SetTextColor(hdc, pal.textPrimary);
        SetBkColor(hdc, pal.cardBg);
        return (INT_PTR)m_hbrCard;
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
            bool needScroll = totalContentHeight > h;
            int maxScroll = needScroll ? (totalContentHeight - h) : 0;
            m_scrollOffset = std::clamp(m_scrollOffset, 0, maxScroll);

            int scrollBarW = 7;
            int scrollBarMarginRight = 4;
            int itemW = needScroll ? (w - scrollBarW - scrollBarMarginRight - 6) : w;

            for (int i = 0; i < totalItems; ++i) {
                int y = i * rowStep - m_scrollOffset;
                if (y + rowHeight < 0 || y > h) continue;

                bool isSelected = (i == m_selectedIndex);
                bool isHovered = (i == m_hoveredIndex);
                draw_list_item(g, i, y, itemW, rowHeight, isSelected, isHovered);
            }

            // Custom modern anti-aliased scrollbar themed to match the app
            if (needScroll) {
                float sbX = static_cast<float>(w - scrollBarMarginRight - scrollBarW);
                float sbY = 4.0f;
                float sbH = static_cast<float>(h - 8);

                // Track
                Gdiplus::SolidBrush trackBr(m_isDarkMode ? Gdiplus::Color(25, 255, 255, 255) : Gdiplus::Color(20, 0, 0, 0));
                Gdiplus::GraphicsPath trackPath;
                AddRoundedRect(trackPath, Gdiplus::RectF(sbX, sbY, static_cast<float>(scrollBarW), sbH), 3.5f);
                g.FillPath(&trackBr, &trackPath);

                // Thumb
                float thumbH = std::max(32.0f, (static_cast<float>(h) / static_cast<float>(totalContentHeight)) * sbH);
                float scrollRatio = (maxScroll > 0) ? (static_cast<float>(m_scrollOffset) / static_cast<float>(maxScroll)) : 0.0f;
                float thumbY = sbY + scrollRatio * (sbH - thumbH);

                Gdiplus::Color thumbColor;
                if (m_isDarkMode) {
                    if (m_isDraggingScroll) {
                        thumbColor = Gdiplus::Color(255, 120, 140, 175);
                    } else if (m_scrollThumbHovered) {
                        thumbColor = Gdiplus::Color(255, 95, 110, 135);
                    } else {
                        thumbColor = Gdiplus::Color(255, 65, 75, 95);
                    }
                } else {
                    if (m_isDraggingScroll) {
                        thumbColor = Gdiplus::Color(255, 130, 150, 180);
                    } else if (m_scrollThumbHovered) {
                        thumbColor = Gdiplus::Color(255, 160, 175, 195);
                    } else {
                        thumbColor = Gdiplus::Color(255, 200, 210, 222);
                    }
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
            std::lock_guard<std::mutex> lock(m_itemsMutex);
            totalItems = static_cast<int>(m_items.size());
        }
        int totalContentHeight = totalItems * 54;
        bool needScroll = totalContentHeight > h;
        int maxScroll = needScroll ? (totalContentHeight - h) : 0;

        if (m_isDraggingScroll) {
            if (needScroll) {
                float sbY = 4.0f;
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
            int idx = contentY / 54;
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
            std::lock_guard<std::mutex> lock(m_itemsMutex);
            totalItems = static_cast<int>(m_items.size());
        }
        int totalContentHeight = totalItems * 54;
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
        int idx = contentY / 54;
        if (idx >= 0 && idx < totalItems) {
            m_selectedIndex = idx;
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
            int idx = contentY / 54;
            int totalItems = 0;
            {
                std::lock_guard<std::mutex> lock(m_itemsMutex);
                totalItems = static_cast<int>(m_items.size());
            }
            if (idx >= 0 && idx < totalItems) {
                m_selectedIndex = idx;
                on_apply();
            }
        }
        break;
    }

    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        int step = 54;
        RECT rc;
        GetClientRect(hWnd, &rc);
        int totalH = 0;
        {
            std::lock_guard<std::mutex> lock(m_itemsMutex);
            totalH = static_cast<int>(m_items.size()) * 54;
        }
        int maxScroll = std::max<int>(0, totalH - static_cast<int>(rc.bottom - rc.top));
        m_scrollOffset -= (delta / WHEEL_DELTA) * step;
        m_scrollOffset = std::clamp(m_scrollOffset, 0, maxScroll);
        InvalidateRect(hWnd, nullptr, FALSE);
        return 0;
    }

    case WM_KEYDOWN: {
        int totalItems = 0;
        {
            std::lock_guard<std::mutex> lock(m_itemsMutex);
            totalItems = static_cast<int>(m_items.size());
        }
        if (wParam == VK_UP) {
            m_selectedIndex = std::max(0, m_selectedIndex - 1);
            int targetY = m_selectedIndex * 54;
            if (targetY < m_scrollOffset) m_scrollOffset = targetY;
            InvalidateRect(hWnd, nullptr, FALSE);
            return 0;
        }
        if (wParam == VK_DOWN) {
            m_selectedIndex = std::min(totalItems - 1, m_selectedIndex + 1);
            RECT rc; GetClientRect(hWnd, &rc);
            int targetY = (m_selectedIndex + 1) * 54;
            if (targetY > m_scrollOffset + (rc.bottom - rc.top)) {
                m_scrollOffset = targetY - (rc.bottom - rc.top);
            }
            InvalidateRect(hWnd, nullptr, FALSE);
            return 0;
        }
        if (wParam == VK_PRIOR) {
            RECT rc; GetClientRect(hWnd, &rc);
            m_scrollOffset = std::max(0, m_scrollOffset - static_cast<int>(rc.bottom - rc.top));
            InvalidateRect(hWnd, nullptr, FALSE);
            return 0;
        }
        if (wParam == VK_NEXT) {
            RECT rc; GetClientRect(hWnd, &rc);
            int maxScroll = std::max<int>(0, totalItems * 54 - static_cast<int>(rc.bottom - rc.top));
            m_scrollOffset = std::min(maxScroll, m_scrollOffset + static_cast<int>(rc.bottom - rc.top));
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
            std::lock_guard<std::mutex> lock(m_itemsMutex);
            totalItems = static_cast<int>(m_items.size());
        }
        bool needScroll = (totalItems * 54) > (rc.bottom - rc.top);
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
