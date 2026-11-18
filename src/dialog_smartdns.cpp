#include "dialog_smartdns.h"
#include "smartdns_verifier.h"
#include "encoding_utils.h"
#include "resource.h"
#include "ui_palette.h"
#include <dwmapi.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <thread>

#ifdef _MSC_VER
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shell32.lib")
#endif

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

using QuickDns::Encoding::wide_to_utf8;
using QuickDns::Encoding::utf8_to_wide;

namespace {
    constexpr const wchar_t* DIALOG_CLASS_NAME = L"QuickDNS_SmartDnsDialogClass";
    constexpr const wchar_t* LIST_CLASS_NAME   = L"QuickDNS_SmartDnsListClass";

    constexpr int DIALOG_CLIENT_WIDTH  = 920;
    constexpr int DIALOG_CLIENT_HEIGHT = 720;

    constexpr int CARD_ROW_GAP_BASE = 10;

    constexpr UINT WM_APP_SMARTDNS_VERIFY_DONE     = WM_APP + 55;
    constexpr UINT WM_APP_SMARTDNS_VERIFY_PROGRESS = WM_APP + 56;

    constexpr const wchar_t* WEBSITE_PILL_LABEL = L"Сайт провайдера \u2197";

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

    bool has_valid_http_url(const std::wstring& url) {
        return !url.empty() &&
               (_wcsnicmp(url.c_str(), L"https://", 8) == 0 ||
                _wcsnicmp(url.c_str(), L"http://", 7) == 0);
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
            KillTimer(hWnd, 4001);
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
                SetTimer(hWnd, 4001, 16, nullptr);
            }
            break;
        }
        case WM_MOUSELEAVE: {
            anim->isTracking = false;
            anim->targetHover = 0.0f;
            SetTimer(hWnd, 4001, 16, nullptr);
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
            if (wParam == 4001) {
                float diff = anim->targetHover - anim->hover;
                if (std::abs(diff) < 0.02f) {
                    anim->hover = anim->targetHover;
                    KillTimer(hWnd, 4001);
                } else {
                    anim->hover += diff * 0.28f;
                }
                InvalidateRect(hWnd, nullptr, FALSE);
                return 0;
            }
            break;
        }
        case WM_NCDESTROY: {
            KillTimer(hWnd, 4001);
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

int SmartDnsDialog::dp(int px) const {
    return MulDiv(px, static_cast<int>(m_dpi), 96);
}

float SmartDnsDialog::dpf(float px) const {
    return px * (static_cast<float>(m_dpi) / 96.0f);
}

std::optional<std::string> SmartDnsDialog::show(
    HWND hParent,
    HINSTANCE hInstance,
    const std::vector<DnsProfile>& profiles,
    const std::string& currentActiveId,
    bool isDarkMode,
    ConfigManager* pConfig)
{
    SmartDnsDialog dlg(hParent, hInstance, profiles, currentActiveId, isDarkMode, pConfig);
    if (!dlg.create_dialog_window()) {
        return std::nullopt;
    }

    if (hParent && IsWindow(hParent)) {
        EnableWindow(hParent, FALSE);
    }

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

    dlg.stop_verify_worker();

    if (dlg.m_hWnd && IsWindow(dlg.m_hWnd)) {
        DestroyWindow(dlg.m_hWnd);
        dlg.m_hWnd = nullptr;
    }

    return dlg.m_selectedProfileId;
}

SmartDnsDialog::SmartDnsDialog(
    HWND hParent,
    HINSTANCE hInstance,
    const std::vector<DnsProfile>& profiles,
    const std::string& currentActiveId,
    bool isDarkMode,
    ConfigManager* pConfig)
    : m_hParent(hParent)
    , m_hInstance(hInstance)
    , m_pConfig(pConfig)
    , m_isDarkMode(isDarkMode)
    , m_currentActiveId(currentActiveId)
{
    m_dpi = query_window_dpi(m_hParent);
    auto pal = GetPalette(m_isDarkMode);
    m_hbrBg = CreateSolidBrush(pal.bg);
    m_hbrCard = CreateSolidBrush(pal.cardBg);

    setup_fonts();
    populate_smartdns_data(profiles);
}

SmartDnsDialog::~SmartDnsDialog() {
    stop_verify_worker();

    m_gpCardLink.reset();
    m_gpCardTag.reset();
    m_gpCardCatHeader.reset();
    m_gpCardSub.reset();
    m_gpCardBadge.reset();
    m_gpCardTitle.reset();
    m_gpHeaderStatus.reset();
    m_gpHeaderLbl.reset();
    m_gpHeaderSub.reset();
    m_gpHeaderTitle.reset();
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

void SmartDnsDialog::stop_verify_worker() {
    m_cancelVerify.store(true, std::memory_order_release);
    if (m_verifyThread.joinable()) {
        m_verifyThread.join();
    }
    m_isVerifying.store(false, std::memory_order_release);
}

void SmartDnsDialog::populate_smartdns_data(const std::vector<DnsProfile>& profiles) {
    // 1. GeoHide DNS
    {
        SmartDnsEntry e;
        e.profile_id = "geohide";
        e.name = L"GeoHide DNS";
        e.website_url = L"https://geohide.ru/";
        e.primary_ipv4 = L"193.233.112.67";
        e.secondary_ipv4 = L"193.233.112.88";
        e.doh_template = L"https://dns.geohide.ru/dns-query";
        e.location = L"Россия / Anycast";
        e.description = L"Российский SmartDNS для обхода географических ограничений нейросетей и веб-сервисов.";
        e.categories = {
            { L"Нейросети и AI", { L"ChatGPT", L"Claude", L"Gemini", L"Grok", L"Groq", L"Cursor AI", L"DeepL", L"ElevenLabs", L"Trae.ai", L"Windsurf", L"Coze", L"Perplexity" } },
            { L"Медиа и Стриминг", { L"Spotify", L"Netflix", L"Deezer", L"SoundCloud", L"Tidal", L"BBC iPlayer" } },
            { L"Работа и ПО", { L"Notion", L"Canva", L"Autodesk", L"JetBrains", L"Docker Hub", L"Figma", L"Atlassian", L"Miro" } },
            { L"IT и Вендоры", { L"Microsoft (Copilot, Xbox)", L"Intel", L"AMD", L"Nvidia", L"Dell", L"ASUS", L"Epic Games" } },
            { L"Соцсети и Сайты", { L"Discord", L"LinkedIn", L"Medium", L"Badoo", L"Tinder", L"RuTracker" } }
        };
        e.is_active = (e.profile_id == m_currentActiveId);
        m_allEntries.push_back(std::move(e));
    }

    // 2. Comss.one DNS
    {
        SmartDnsEntry e;
        e.profile_id = "comss";
        e.name = L"Comss.one DNS";
        e.website_url = L"https://dns.comss.one/";
        e.primary_ipv4 = L"83.220.169.155";
        e.secondary_ipv4 = L"212.109.195.93";
        e.doh_template = L"https://dns.comss.one/dns-query";
        e.location = L"Россия / Нидерланды";
        e.description = L"Популярный SmartDNS от портала Comss с защитой от рекламы, трекеров и разблокировкой зарубежных сайтов.";
        e.categories = {
            { L"Нейросети и AI", { L"ChatGPT", L"Microsoft Copilot", L"Claude AI", L"Perplexity", L"Midjourney" } },
            { L"Медиа и Музыка", { L"Spotify", L"Deezer", L"YouTube Music", L"Tidal" } },
            { L"Соцсети и Связь", { L"Instagram", L"X (Twitter)", L"Facebook", L"LinkedIn", L"Discord" } },
            { L"Работа и Инструменты", { L"Canva", L"Notion", L"Adobe Creative Cloud", L"Intel", L"AMD", L"Dell", L"ASUS", L"AdGuard" } },
            { L"Безопасность", { L"Антиреклама (AdBlock)", L"Антитрекинг", L"Блокировка фишинга" } }
        };
        e.is_active = (e.profile_id == m_currentActiveId);
        m_allEntries.push_back(std::move(e));
    }

    // 3. Zapret DNS
    {
        SmartDnsEntry e;
        e.profile_id = "zapret";
        e.name = L"Zapret DNS";
        e.website_url = L"https://zapret.moe/";
        e.primary_ipv4 = L"83.217.211.149";
        e.secondary_ipv4 = L"";
        e.doh_template = L"https://dns.zapret.moe/dns-query";
        e.location = L"Нидерланды (Zapret)";
        e.description = L"Независимый DNS от создателей проекта Zapret для прямого доступа к заблокированным сервисам и нейросетям.";
        e.categories = {
            { L"Связь и Медиа", { L"Discord", L"YouTube", L"Twitch", L"X (Twitter)", L"Instagram", L"Facebook", L"Reddit" } },
            { L"Нейросети и AI", { L"ChatGPT", L"OpenAI API", L"Anthropic Claude" } },
            { L"Ресурсы и Трекеры", { L"RuTracker", L"NoName Club", L"Зарубежные библиотеки", L"Независимые СМИ" } }
        };
        e.is_active = (e.profile_id == m_currentActiveId);
        m_allEntries.push_back(std::move(e));
    }

    // 4. Без межаў DNS
    {
        SmartDnsEntry e;
        e.profile_id = "bezmezhau";
        e.name = L"Без межаў DNS";
        e.website_url = L"https://bezmezhau.com/";
        e.primary_ipv4 = L"181.215.40.49";
        e.secondary_ipv4 = L"143.20.64.55";
        e.doh_template = L"https://dns.bezmezhau.com/dns-query";
        e.location = L"Финляндия / Германия";
        e.description = L"Свободный независимый SmartDNS без цензуры со встроенной фильтрацией баннеров и рекламы.";
        e.categories = {
            { L"Нейросети и AI", { L"ChatGPT", L"Anthropic Claude", L"Microsoft Copilot" } },
            { L"Медиа и Соцсети", { L"YouTube", L"Discord", L"Spotify", L"X (Twitter)", L"Instagram", L"Reddit", L"TikTok", L"Twitch", L"Netflix" } },
            { L"Защита", { L"Блокировка рекламы", L"Защита от слежки и трекеров" } }
        };
        e.is_active = (e.profile_id == m_currentActiveId);
        m_allEntries.push_back(std::move(e));
    }

    // 4b. Без межаў DNS (РФ)
    {
        SmartDnsEntry e;
        e.profile_id = "bezmezhau_ru";
        e.name = L"Без межаў DNS (РФ)";
        e.website_url = L"https://bezmezhau.com/";
        e.primary_ipv4 = L"185.236.10.189";
        e.secondary_ipv4 = L"31.58.216.201";
        e.doh_template = L"https://sync.bezmezhau.com/dns-query";
        e.location = L"РФ / Европа";
        e.description = L"Выделенный сервер Без межаў для пользователей из России (sync.bezmezhau.com).";
        e.categories = {
            { L"Нейросети и AI", { L"ChatGPT", L"Anthropic Claude", L"Microsoft Copilot" } },
            { L"Медиа и Соцсети", { L"YouTube", L"Discord", L"Spotify", L"X (Twitter)", L"Instagram", L"Reddit", L"TikTok", L"Twitch", L"Netflix" } },
            { L"Защита", { L"Блокировка рекламы", L"Защита от слежки и трекеров" } }
        };
        e.is_active = (e.profile_id == m_currentActiveId);
        m_allEntries.push_back(std::move(e));
    }

    // 5. Malw.link DNS
    {
        SmartDnsEntry e;
        e.profile_id = "malw";
        e.name = L"Malw.link DNS";
        e.website_url = L"https://info.dns.malw.link/";
        e.primary_ipv4 = L"179.254.166.171";
        e.secondary_ipv4 = L"164.5.224.81";
        e.doh_template = L"https://dns.malw.link/dns-query";
        e.location = L"Германия / Нидерланды";
        e.description = L"DNS-сервер с открытой политикой фильтрации, разблокировкой ресурсов и антифишингом.";
        e.categories = {
            { L"Нейросети и AI", { L"ChatGPT", L"Google Gemini", L"Anthropic Claude" } },
            { L"Медиа и Соцсети", { L"Spotify", L"Discord", L"Instagram", L"Facebook", L"X (Twitter)", L"TikTok", L"Reddit", L"Twitch" } },
            { L"Инструменты", { L"Notion", L"Canva", L"RuTracker", L"Торрент-трекеры" } },
            { L"Защита", { L"AdBlock", L"Антифишинг", L"Приватность" } }
        };
        e.is_active = (e.profile_id == m_currentActiveId);
        m_allEntries.push_back(std::move(e));
    }

    // 6. GeoHide EU
    {
        SmartDnsEntry e;
        e.profile_id = "geohide_eu";
        e.name = L"GeoHide EU (Европа)";
        e.website_url = L"https://geohide.ru/";
        e.primary_ipv4 = L"45.13.237.213";
        e.secondary_ipv4 = L"5.175.161.3";
        e.doh_template = L"https://eu.geohide.ru/dns-query";
        e.location = L"Нидерланды / Германия";
        e.description = L"Европейские ноды GeoHide для разблокировки ChatGPT, Claude, Gemini, Spotify, Canva и Notion.";
        e.categories = {
            { L"Нейросети и AI", { L"ChatGPT", L"Claude", L"Gemini", L"Grok", L"Cursor AI", L"DeepL" } },
            { L"Медиа и Стриминг", { L"Spotify", L"Netflix", L"Deezer", L"SoundCloud", L"Tidal" } },
            { L"Работа и ПО", { L"Notion", L"Canva", L"Autodesk", L"JetBrains", L"Docker Hub" } }
        };
        e.is_active = (e.profile_id == m_currentActiveId);
        m_allEntries.push_back(std::move(e));
    }

    // 7. GeoHide US
    {
        SmartDnsEntry e;
        e.profile_id = "geohide_us";
        e.name = L"GeoHide US (США)";
        e.website_url = L"https://geohide.ru/";
        e.primary_ipv4 = L"192.255.159.241";
        e.secondary_ipv4 = L"192.3.233.253";
        e.doh_template = L"https://us.geohide.ru/dns-query";
        e.location = L"США / Северная Америка";
        e.description = L"Североамериканские ноды GeoHide для доступа к сервисам США, ChatGPT, Claude и американским медиа.";
        e.categories = {
            { L"Нейросети и AI", { L"ChatGPT", L"Claude", L"Gemini", L"Copilot", L"Grok", L"Perplexity" } },
            { L"Медиа и Стриминг", { L"Netflix US", L"Hulu", L"HBO Max", L"Spotify", L"YouTube" } },
            { L"Работа и ПО", { L"Notion", L"Canva", L"Figma", L"Docker Hub" } }
        };
        e.is_active = (e.profile_id == m_currentActiveId);
        m_allEntries.push_back(std::move(e));
    }

    // 8. Astracat DNS
    {
        SmartDnsEntry e;
        e.profile_id = "astracat";
        e.name = L"Astracat DNS";
        e.website_url = L"https://astracat.ru/";
        e.primary_ipv4 = L"135.106.217.200";
        e.secondary_ipv4 = L"185.139.69.24";
        e.doh_template = L"https://dns.astracat.ru/dns-query";
        e.location = L"Россия / Европа";
        e.description = L"Быстрый SmartDNS для обхода блокировок нейросетей, Discord, голосовых каналов и YouTube.";
        e.categories = {
            { L"Нейросети и AI", { L"ChatGPT", L"Claude", L"Gemini", L"Perplexity" } },
            { L"Медиа и Соцсети", { L"Discord", L"YouTube", L"Spotify", L"Twitter/X", L"Instagram" } },
            { L"Сервисы", { L"Notion", L"Twitch", L"Reddit", L"RuTracker" } }
        };
        e.is_active = (e.profile_id == m_currentActiveId);
        m_allEntries.push_back(std::move(e));
    }

    // 9. Mafioznik DNS
    {
        SmartDnsEntry e;
        e.profile_id = "mafioznik";
        e.name = L"Mafioznik DNS";
        e.website_url = L"https://dns.mafioznik.xyz/";
        e.primary_ipv4 = L"103.27.157.38";
        e.secondary_ipv4 = L"103.27.157.100";
        e.doh_template = L"https://dns.mafioznik.xyz/dns-query";
        e.location = L"Сингапур / Anycast";
        e.description = L"Популярный комьюнити SmartDNS для разблокировки Discord, YouTube, Steam и нейросетей.";
        e.categories = {
            { L"Игры и Связь", { L"Discord (войс и медиа)", L"Steam Community", L"Twitch" } },
            { L"Нейросети и AI", { L"ChatGPT", L"Claude", L"Microsoft Copilot" } },
            { L"Медиа", { L"YouTube", L"Spotify", L"Instagram", L"Twitter/X" } }
        };
        e.is_active = (e.profile_id == m_currentActiveId);
        m_allEntries.push_back(std::move(e));
    }

    // 10. Shecan DNS
    {
        SmartDnsEntry e;
        e.profile_id = "shecan";
        e.name = L"Shecan DNS";
        e.website_url = L"https://shecan.ir/";
        e.primary_ipv4 = L"178.22.122.100";
        e.secondary_ipv4 = L"185.51.200.2";
        e.doh_template = L"https://free.shecan.ir/dns-query";
        e.location = L"Anycast / Ближний Восток";
        e.description = L"Специализированный антисанкционный SmartDNS для разблокировки IT-сервисов, вендоров и репозиториев.";
        e.categories = {
            { L"IT и Разработка", { L"Docker Hub", L"Android Developers", L"Gradle", L"Oracle", L"GitLab" } },
            { L"Вендоры и Драйверы", { L"NVIDIA", L"AMD", L"Intel", L"Dell", L"VMware" } },
            { L"Сервисы", { L"Google Services", L"Spotify", L"Adobe" } }
        };
        e.is_active = (e.profile_id == m_currentActiveId);
        m_allEntries.push_back(std::move(e));
    }

    // 11. Control D (ControlID)
    {
        SmartDnsEntry e;
        e.profile_id = "controld";
        e.name = L"Control D (ControlID)";
        e.website_url = L"https://controld.com/";
        e.primary_ipv4 = L"76.76.2.0";
        e.secondary_ipv4 = L"76.76.10.0";
        e.doh_template = L"https://freedns.controld.com/p0";
        e.location = L"Глобальный Anycast (100+ локаций)";
        e.description = L"Продвинутый Anycast SmartDNS от Windscribe со сверхнизким пингом, фильтрацией трекеров и разблокировкой.";
        e.categories = {
            { L"Приватность и Защита", { L"Блокировка рекламы", L"Антитрекинг", L"Защита от вредоносных доменов" } },
            { L"Нейросети и AI", { L"ChatGPT", L"Claude", L"Gemini", L"Copilot" } },
            { L"Медиа", { L"YouTube", L"Twitch", L"Reddit", L"Discord" } }
        };
        e.is_active = (e.profile_id == m_currentActiveId);
        m_allEntries.push_back(std::move(e));
    }

    // 12. Akile DNS (AKDNS)
    {
        SmartDnsEntry e;
        e.profile_id = "akile";
        e.name = L"Akile DNS (AKDNS)";
        e.website_url = L"https://dns.akile.ai/";
        e.primary_ipv4 = L"66.66.66.66";
        e.secondary_ipv4 = L"108.160.138.51";
        e.doh_template = L"https://dns.akile.ai/dns-query";
        e.location = L"Гонконг / Anycast";
        e.description = L"SmartDNS сервис для разблокировки зарубежного стриминга и нейросетей (требуется привязка IP в консоли).";
        e.categories = {
            { L"Стриминг", { L"Netflix", L"Disney+", L"YouTube Premium", L"Spotify" } },
            { L"Нейросети", { L"ChatGPT", L"Claude", L"Gemini" } },
            { L"Соцсети", { L"TikTok", L"Twitter/X", L"Instagram" } }
        };
        e.is_active = (e.profile_id == m_currentActiveId);
        m_allEntries.push_back(std::move(e));
    }

    // 13. DNS-AI (DoH)
    {
        SmartDnsEntry e;
        e.profile_id = "dns_ai";
        e.name = L"DNS-AI (DoH)";
        e.website_url = L"https://dns-ai.ru/";
        e.primary_ipv4 = L"194.87.196.13";
        e.secondary_ipv4 = L"45.151.102.219";
        e.doh_template = L"https://dns.dns-ai.ru/dns-query";
        e.location = L"Россия / Европа (только DoH)";
        e.description = L"Шифрованный SmartDNS (только DoH/DoT, порт 53 закрыт) для нейросетей без рекламы, трекеров и телеметрии (Windows 11+).";
        e.categories = {
            { L"Нейросети и AI", { L"ChatGPT", L"OpenAI API", L"Claude", L"Gemini", L"Google AI Studio", L"Microsoft Copilot", L"Grok", L"Cursor AI" } },
            { L"Защита и Приватность", { L"Только шифрованный DoH (HTTP/2)", L"AdBlock (600k+ фильтров)", L"Антитрекинг и защита от малвари" } }
        };
        e.is_active = (e.profile_id == m_currentActiveId);
        m_allEntries.push_back(std::move(e));
    }

    // User-defined / Custom SmartDNS profiles
    for (const auto& p : profiles) {
        if (!p.is_smart_dns) continue;
        bool alreadyExists = false;
        for (const auto& e : m_allEntries) {
            if (e.profile_id == p.id) {
                alreadyExists = true;
                break;
            }
        }
        if (!alreadyExists) {
            SmartDnsEntry e;
            e.profile_id = p.id;
            e.name = utf8_to_wide(p.name);
            e.website_url = utf8_to_wide(p.website_url);
            e.primary_ipv4 = utf8_to_wide(p.primary_ipv4);
            e.secondary_ipv4 = utf8_to_wide(p.secondary_ipv4);
            e.doh_template = utf8_to_wide(p.doh_template);
            e.location = L"Пользовательский";
            e.description = utf8_to_wide(p.description);
            if (!p.unblocked_summary.empty()) {
                e.categories = {
                    { L"Разблокировка", { utf8_to_wide(p.unblocked_summary) } }
                };
            } else {
                e.categories = {
                    { L"Сервис", { L"SmartDNS", L"Обход ограничений" } }
                };
            }
            e.is_active = (e.profile_id == m_currentActiveId);
            m_allEntries.push_back(std::move(e));
        }
    }

    // Sync primary/secondary IPs and populate ip_pool from loaded profiles
    for (auto& entry : m_allEntries) {
        const DnsProfile* matchedProfile = nullptr;
        for (const auto& p : profiles) {
            if (p.id == entry.profile_id) {
                matchedProfile = &p;
                break;
            }
        }

        if (matchedProfile && !matchedProfile->primary_ipv4.empty()) {
            entry.primary_ipv4 = utf8_to_wide(matchedProfile->primary_ipv4);
            entry.secondary_ipv4 = utf8_to_wide(matchedProfile->secondary_ipv4);
        }

        auto addPoolIp = [&](const std::wstring& ip) {
            if (ip.empty()) return;
            if (std::find(entry.ip_pool.begin(), entry.ip_pool.end(), ip) == entry.ip_pool.end()) {
                entry.ip_pool.push_back(ip);
            }
        };

        addPoolIp(entry.primary_ipv4);
        addPoolIp(entry.secondary_ipv4);
        if (matchedProfile) {
            for (const auto& ipStr : matchedProfile->ip_pool) {
                addPoolIp(utf8_to_wide(ipStr));
            }
        }
    }

    update_filtered_list();
    for (size_t i = 0; i < m_filteredIndices.size(); ++i) {
        if (m_allEntries[m_filteredIndices[i]].profile_id == m_currentActiveId) {
            m_selectedIndex = static_cast<int>(i);
            break;
        }
    }
}

void SmartDnsDialog::update_action_buttons_state() {
    bool hasSelection = (m_selectedIndex >= 0 && m_selectedIndex < static_cast<int>(m_filteredIndices.size()));
    bool hasWebsite = false;
    bool hasPool = false;

    if (hasSelection) {
        const auto& e = m_allEntries[m_filteredIndices[static_cast<size_t>(m_selectedIndex)]];
        hasWebsite = has_valid_http_url(e.website_url);
        hasPool = !e.ip_pool.empty() || !e.primary_ipv4.empty();
    }

    if (m_hBtnApply && IsWindow(m_hBtnApply)) {
        EnableWindow(m_hBtnApply, hasSelection ? TRUE : FALSE);
    }
    if (m_hBtnPickIp && IsWindow(m_hBtnPickIp)) {
        EnableWindow(m_hBtnPickIp, (hasSelection && hasPool) ? TRUE : FALSE);
    }
    if (m_hBtnWebsite && IsWindow(m_hBtnWebsite)) {
        EnableWindow(m_hBtnWebsite, hasWebsite ? TRUE : FALSE);
    }
}

void SmartDnsDialog::update_filtered_list() {
    m_filteredIndices.clear();

    std::wstring query = m_searchQuery;
    std::transform(query.begin(), query.end(), query.begin(), [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });

    for (size_t i = 0; i < m_allEntries.size(); ++i) {
        const auto& e = m_allEntries[i];
        if (query.empty()) {
            m_filteredIndices.push_back(i);
            continue;
        }

        std::wstring nameLower = e.name;
        std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
        if (nameLower.find(query) != std::wstring::npos) {
            m_filteredIndices.push_back(i);
            continue;
        }

        std::wstring descLower = e.description;
        std::transform(descLower.begin(), descLower.end(), descLower.begin(), [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
        if (descLower.find(query) != std::wstring::npos) {
            m_filteredIndices.push_back(i);
            continue;
        }

        bool foundInServices = false;
        for (const auto& cat : e.categories) {
            for (const auto& s : cat.services) {
                std::wstring sLower = s;
                std::transform(sLower.begin(), sLower.end(), sLower.begin(), [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
                if (sLower.find(query) != std::wstring::npos) {
                    foundInServices = true;
                    break;
                }
            }
            if (foundInServices) break;
        }

        if (foundInServices) {
            m_filteredIndices.push_back(i);
        }
    }

    if (m_filteredIndices.empty()) {
        m_selectedIndex = -1;
    } else if (m_selectedIndex < 0 || m_selectedIndex >= static_cast<int>(m_filteredIndices.size())) {
        m_selectedIndex = 0;
    }

    recalculate_item_layouts();
    update_action_buttons_state();

    m_statusText = L"Найдено SmartDNS серверов: " + std::to_wstring(m_filteredIndices.size()) + L" из " + std::to_wstring(m_allEntries.size());
    if (!query.empty()) {
        m_statusText += L" (по запросу: \"" + m_searchQuery + L"\")";
    } else {
        m_statusText += L" \u2022 Нажмите на карточку для выбора";
    }

    if (m_hWnd && IsWindow(m_hWnd)) {
        InvalidateRect(m_hWnd, nullptr, FALSE);
    }
}

void SmartDnsDialog::setup_fonts() {
    m_gpCardLink.reset();
    m_gpCardTag.reset();
    m_gpCardCatHeader.reset();
    m_gpCardSub.reset();
    m_gpCardBadge.reset();
    m_gpCardTitle.reset();
    m_gpHeaderStatus.reset();
    m_gpHeaderLbl.reset();
    m_gpHeaderSub.reset();
    m_gpHeaderTitle.reset();
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
    const Gdiplus::FontFamily* pRegular = m_gpFontFamily->IsAvailable()
        ? m_gpFontFamily.get()
        : Gdiplus::FontFamily::GenericSansSerif();
    const Gdiplus::FontFamily* pDisplay = m_gpDisplayFamily->IsAvailable()
        ? m_gpDisplayFamily.get()
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

    m_gpHeaderTitle  = std::make_unique<Gdiplus::Font>(pDisplay, dpf(18.0f), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    m_gpHeaderSub    = std::make_unique<Gdiplus::Font>(pRegular, dpf(12.0f), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    m_gpHeaderLbl    = std::make_unique<Gdiplus::Font>(pRegular, dpf(12.7f), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    m_gpHeaderStatus = std::make_unique<Gdiplus::Font>(pRegular, dpf(11.3f), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);

    m_gpCardTitle     = std::make_unique<Gdiplus::Font>(pRegular, dpf(15.3f), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    m_gpCardBadge     = std::make_unique<Gdiplus::Font>(pRegular, dpf(11.3f), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    m_gpCardSub       = std::make_unique<Gdiplus::Font>(pRegular, dpf(12.0f), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    m_gpCardCatHeader = std::make_unique<Gdiplus::Font>(pRegular, dpf(11.3f), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    m_gpCardTag       = std::make_unique<Gdiplus::Font>(pRegular, dpf(11.3f), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    m_gpCardLink      = std::make_unique<Gdiplus::Font>(pRegular, dpf(11.3f), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
}

void SmartDnsDialog::apply_fonts_to_controls() {
    if (m_hSearchEdit && IsWindow(m_hSearchEdit)) {
        SendMessageW(m_hSearchEdit, WM_SETFONT, reinterpret_cast<WPARAM>(m_hFontRegular), TRUE);
    }
    HWND btns[] = { m_hBtnApply, m_hBtnPickIp, m_hBtnWebsite, m_hBtnVerify, m_hBtnClose };
    for (HWND hBtn : btns) {
        if (hBtn && IsWindow(hBtn)) {
            InvalidateRect(hBtn, nullptr, FALSE);
        }
    }
}

bool SmartDnsDialog::create_dialog_window() {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &SmartDnsDialog::dialog_proc_thunk;
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
    lwc.lpfnWndProc = &SmartDnsDialog::list_proc_thunk;
    lwc.hInstance = m_hInstance;
    lwc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
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
        L"Каталог SmartDNS — Разблокировка нейросетей и сервисов",
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

void SmartDnsDialog::apply_theme() {
    BOOL useDarkMode = m_isDarkMode ? TRUE : FALSE;
    DwmSetWindowAttribute(m_hWnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &useDarkMode, sizeof(useDarkMode));
    DwmSetWindowAttribute(m_hWnd, 19, &useDarkMode, sizeof(useDarkMode));
    DWORD cornerPref = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(m_hWnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &cornerPref, sizeof(cornerPref));
}

LRESULT CALLBACK SmartDnsDialog::search_edit_subclass(
    HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
    UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
    auto* pDlg = reinterpret_cast<SmartDnsDialog*>(dwRefData);
    if (!pDlg) return DefSubclassProc(hWnd, uMsg, wParam, lParam);

    auto invalidateSearchCard = [pDlg]() {
        if (pDlg->m_hWnd && IsWindow(pDlg->m_hWnd)) {
            RECT rcClient{};
            GetClientRect(pDlg->m_hWnd, &rcClient);
            RECT rcInv = {
                pDlg->dp(172),
                pDlg->dp(80),
                rcClient.right - pDlg->dp(18),
                pDlg->dp(116)
            };
            InvalidateRect(pDlg->m_hWnd, &rcInv, FALSE);
        }
    };

    switch (uMsg) {
    case WM_SETFOCUS: {
        pDlg->m_searchEditFocused = true;
        invalidateSearchCard();
        break;
    }
    case WM_KILLFOCUS: {
        pDlg->m_searchEditFocused = false;
        invalidateSearchCard();
        break;
    }
    case WM_MOUSEMOVE: {
        if (!pDlg->m_searchEditHovered) {
            pDlg->m_searchEditHovered = true;
            TRACKMOUSEEVENT tme{};
            tme.cbSize = sizeof(TRACKMOUSEEVENT);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hWnd;
            TrackMouseEvent(&tme);
            invalidateSearchCard();
        }
        break;
    }
    case WM_MOUSELEAVE: {
        pDlg->m_searchEditHovered = false;
        invalidateSearchCard();
        break;
    }
    case WM_CHAR: {
        if (wParam == VK_RETURN) {
            return 0;
        }
        break;
    }
    case WM_NCDESTROY: {
        RemoveWindowSubclass(hWnd, search_edit_subclass, uIdSubclass);
        return 0;
    }
    }
    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

void SmartDnsDialog::create_controls() {
    RECT rcClient = {};
    GetClientRect(m_hWnd, &rcClient);
    int clientWidth = rcClient.right - rcClient.left;
    int clientHeight = rcClient.bottom - rcClient.top;

    int marginX = dp(20);
    int contentWidth = std::max(dp(240), clientWidth - (marginX * 2));

    int searchBoxX = marginX + dp(154);
    int searchBoxY = dp(82);
    int searchBoxW = std::max(dp(140), contentWidth - dp(154));
    int editInsetX = dp(10);
    int editInsetY = dp(6);
    int editH = dp(20);

    m_hSearchEdit = CreateWindowExW(0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        searchBoxX + editInsetX, searchBoxY + editInsetY, std::max(dp(80), searchBoxW - 2 * editInsetX), editH,
        m_hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EDIT_SMARTDNS_SEARCH)), m_hInstance, nullptr);
    SendMessageW(m_hSearchEdit, WM_SETFONT, reinterpret_cast<WPARAM>(m_hFontRegular), TRUE);
    SendMessageW(m_hSearchEdit, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Введите название (например: ChatGPT, Spotify, Discord, Canva, Notion, Claude)..."));
    SetWindowSubclass(m_hSearchEdit, search_edit_subclass, 501, reinterpret_cast<DWORD_PTR>(this));

    int listTop = dp(148);
    int bottomButtonsH = dp(38);
    int listBottomMargin = dp(16);
    int gapBelowList = dp(16);
    int listHeight = std::max(dp(80), clientHeight - listTop - bottomButtonsH - listBottomMargin - gapBelowList);

    m_hListWnd = CreateWindowExW(
        0,
        LIST_CLASS_NAME,
        L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        marginX, listTop, contentWidth, listHeight,
        m_hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LIST_SMARTDNS)), m_hInstance, this
    );

    int curY = listTop + listHeight + gapBelowList;

    int btnApplyW  = dp(165);
    int btnPickW   = dp(110);
    int btnWebW    = dp(140);
    int btnVerifyW = dp(165);
    int btnCloseW  = dp(95);
    int gap        = dp(8);

    m_hBtnApply = CreateWindowExW(0, WC_BUTTONW, L"Применить этот DNS",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        marginX, curY, btnApplyW, bottomButtonsH,
        m_hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_SMARTDNS_APPLY)), m_hInstance, nullptr);

    m_hBtnPickIp = CreateWindowExW(0, WC_BUTTONW, L"Пул IP \u25BE",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        marginX + btnApplyW + gap, curY, btnPickW, bottomButtonsH,
        m_hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_SMARTDNS_PICK_IP)), m_hInstance, nullptr);

    m_hBtnWebsite = CreateWindowExW(0, WC_BUTTONW, L"Официальный сайт",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        marginX + btnApplyW + gap + btnPickW + gap, curY, btnWebW, bottomButtonsH,
        m_hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_SMARTDNS_WEBSITE)), m_hInstance, nullptr);

    m_hBtnVerify = CreateWindowExW(0, WC_BUTTONW, L"Обновить IP и DoH",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        marginX + btnApplyW + gap + btnPickW + gap + btnWebW + gap, curY, btnVerifyW, bottomButtonsH,
        m_hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_SMARTDNS_SCRAPE_VERIFY)), m_hInstance, nullptr);

    m_hBtnClose = CreateWindowExW(0, WC_BUTTONW, L"Закрыть",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        marginX + contentWidth - btnCloseW, curY, btnCloseW, bottomButtonsH,
        m_hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_SMARTDNS_CLOSE)), m_hInstance, nullptr);

    attach_button_effects(m_hBtnApply, 301);
    attach_button_effects(m_hBtnPickIp, 305);
    attach_button_effects(m_hBtnWebsite, 302);
    attach_button_effects(m_hBtnVerify, 304);
    attach_button_effects(m_hBtnClose, 303);

    reposition_controls();
    update_action_buttons_state();
}

void SmartDnsDialog::reposition_controls() {
    if (!m_hWnd || !IsWindow(m_hWnd)) return;
    RECT rcClient = {};
    GetClientRect(m_hWnd, &rcClient);
    int clientWidth = rcClient.right - rcClient.left;
    int clientHeight = rcClient.bottom - rcClient.top;
    if (clientWidth < 200 || clientHeight < 200) return;

    int marginX = dp(20);
    int contentWidth = std::max(dp(200), clientWidth - (marginX * 2));

    int searchBoxX = marginX + dp(154);
    int searchBoxY = dp(82);
    int searchBoxW = std::max(dp(120), contentWidth - dp(154));
    int editInsetX = dp(10);
    int editInsetY = dp(6);
    int editH = dp(20);

    if (m_hSearchEdit) {
        SetWindowPos(m_hSearchEdit, nullptr,
            searchBoxX + editInsetX, searchBoxY + editInsetY,
            std::max(dp(60), searchBoxW - 2 * editInsetX), editH, SWP_NOZORDER);
    }

    int listTop = dp(148);
    int bottomButtonsH = dp(38);
    int listBottomMargin = dp(16);
    int gapBelowList = dp(16);
    int listHeight = std::max(dp(80), clientHeight - listTop - bottomButtonsH - listBottomMargin - gapBelowList);

    if (m_hListWnd) {
        SetWindowPos(m_hListWnd, nullptr, marginX, listTop, contentWidth, listHeight, SWP_NOZORDER);
    }

    int curY = listTop + listHeight + gapBelowList;
    int btnApplyW  = dp(165);
    int btnPickW   = dp(110);
    int btnWebW    = dp(140);
    int btnVerifyW = dp(165);
    int btnCloseW  = dp(95);
    int gap        = dp(8);

    int totalMinNeeded = btnApplyW + btnPickW + btnWebW + btnVerifyW + btnCloseW + 4 * gap;
    if (totalMinNeeded > contentWidth && totalMinNeeded > 0) {
        float ratio = static_cast<float>(std::max(100, contentWidth - 4 * gap)) /
                      static_cast<float>(btnApplyW + btnPickW + btnWebW + btnVerifyW + btnCloseW);
        btnApplyW  = std::max(dp(70), static_cast<int>(btnApplyW * ratio));
        btnPickW   = std::max(dp(55), static_cast<int>(btnPickW * ratio));
        btnWebW    = std::max(dp(65), static_cast<int>(btnWebW * ratio));
        btnVerifyW = std::max(dp(70), static_cast<int>(btnVerifyW * ratio));
        btnCloseW  = std::max(dp(55), static_cast<int>(btnCloseW * ratio));
    }

    if (m_hBtnApply)   SetWindowPos(m_hBtnApply, nullptr, marginX, curY, btnApplyW, bottomButtonsH, SWP_NOZORDER);
    if (m_hBtnPickIp)  SetWindowPos(m_hBtnPickIp, nullptr, marginX + btnApplyW + gap, curY, btnPickW, bottomButtonsH, SWP_NOZORDER);
    if (m_hBtnWebsite) SetWindowPos(m_hBtnWebsite, nullptr, marginX + btnApplyW + gap + btnPickW + gap, curY, btnWebW, bottomButtonsH, SWP_NOZORDER);
    if (m_hBtnVerify)  SetWindowPos(m_hBtnVerify, nullptr, marginX + btnApplyW + gap + btnPickW + gap + btnWebW + gap, curY, btnVerifyW, bottomButtonsH, SWP_NOZORDER);
    if (m_hBtnClose)   SetWindowPos(m_hBtnClose, nullptr, marginX + contentWidth - btnCloseW, curY, btnCloseW, bottomButtonsH, SWP_NOZORDER);

    recalculate_item_layouts();
    InvalidateRect(m_hWnd, nullptr, FALSE);
    if (m_hListWnd) InvalidateRect(m_hListWnd, nullptr, TRUE);
}

Gdiplus::RectF SmartDnsDialog::get_website_pill_rect(Gdiplus::Graphics& g, int cardWidth, int cardY) const {
    Gdiplus::StringFormat sfCenter(Gdiplus::StringFormat::GenericTypographic());
    sfCenter.SetAlignment(Gdiplus::StringAlignmentCenter);
    sfCenter.SetLineAlignment(Gdiplus::StringAlignmentCenter);

    Gdiplus::RectF linkBounds;
    g.MeasureString(WEBSITE_PILL_LABEL, -1, m_gpCardLink.get(), Gdiplus::PointF(0.0f, 0.0f), &sfCenter, &linkBounds);
    float linkBtnW = linkBounds.Width + dpf(22.0f);
    float linkBtnH = dpf(24.0f);
    float linkBtnX = static_cast<float>(cardWidth) - linkBtnW - dpf(16.0f);
    float linkBtnY = static_cast<float>(cardY) + dpf(9.0f);
    return Gdiplus::RectF(linkBtnX, linkBtnY, linkBtnW, linkBtnH);
}

void SmartDnsDialog::recalculate_item_layouts() {
    if (!m_hListWnd || !IsWindow(m_hListWnd)) return;
    RECT rc = {};
    GetClientRect(m_hListWnd, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0) return;

    size_t count = m_filteredIndices.size();
    m_itemHeights.resize(count);
    m_itemYPositions.resize(count + 1);
    if (count == 0) {
        m_itemYPositions[0] = 0;
        m_totalContentHeight = 0;
        return;
    }

    HDC hdc = GetDC(m_hListWnd);
    if (!hdc) return;

    {
        Gdiplus::Graphics g(hdc);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

        int rowGap = dp(CARD_ROW_GAP_BASE);
        int scrollReserve = dp(17);

        auto measureAll = [&](int cardW) -> int {
            int curY = 0;
            for (size_t i = 0; i < count; ++i) {
                m_itemYPositions[i] = curY;
                int itemH = calculate_item_height(g, m_allEntries[m_filteredIndices[i]], cardW);
                m_itemHeights[i] = itemH;
                curY += itemH + rowGap;
            }
            m_itemYPositions[count] = curY;
            return curY;
        };

        int fullW = std::max(dp(180), w);
        int totalNoScroll = measureAll(fullW);
        if (h > 0 && totalNoScroll > h) {
            int scrolledW = std::max(dp(180), w - scrollReserve);
            m_totalContentHeight = measureAll(scrolledW);
        } else {
            m_totalContentHeight = totalNoScroll;
        }
    }

    ReleaseDC(m_hListWnd, hdc);
}

int SmartDnsDialog::calculate_item_height(Gdiplus::Graphics& g, const SmartDnsEntry& e, int cardWidth) const {
    if (!m_gpCardTitle || !m_gpCardBadge || !m_gpCardSub || !m_gpCardCatHeader || !m_gpCardTag || !m_gpCardLink) {
        return dp(220);
    }

    Gdiplus::StringFormat sfCenter(Gdiplus::StringFormat::GenericTypographic());
    sfCenter.SetAlignment(Gdiplus::StringAlignmentCenter);
    sfCenter.SetLineAlignment(Gdiplus::StringAlignmentCenter);

    float curCardY = dpf(10.0f);
    float curCardX = dpf(16.0f);
    float headerStepY = dpf(26.0f);

    float maxHeaderLeftW = static_cast<float>(cardWidth) - dpf(24.0f);
    if (has_valid_http_url(e.website_url)) {
        Gdiplus::RectF linkRc = get_website_pill_rect(g, cardWidth, 0);
        maxHeaderLeftW = linkRc.X - dpf(12.0f);
    }

    Gdiplus::RectF nameBounds;
    g.MeasureString(e.name.c_str(), -1, m_gpCardTitle.get(), Gdiplus::PointF(0.0f, 0.0f), &nameBounds);
    float badgeX = curCardX + nameBounds.Width + dpf(12.0f);
    if (badgeX > maxHeaderLeftW) {
        curCardY += headerStepY;
        badgeX = curCardX;
        maxHeaderLeftW = static_cast<float>(cardWidth) - dpf(24.0f);
    }

    auto checkWrapBadge = [&](float neededW) {
        if (badgeX + neededW > maxHeaderLeftW) {
            curCardY += headerStepY;
            badgeX = curCardX;
            maxHeaderLeftW = static_cast<float>(cardWidth) - dpf(24.0f);
        }
    };

    {
        Gdiplus::RectF sBounds;
        g.MeasureString(L"SmartDNS", -1, m_gpCardBadge.get(), Gdiplus::PointF(0.0f, 0.0f), &sfCenter, &sBounds);
        float pillW = sBounds.Width + dpf(20.0f);
        checkWrapBadge(pillW);
        badgeX += pillW + dpf(8.0f);
    }

    if (!e.location.empty()) {
        Gdiplus::RectF locBounds;
        g.MeasureString(e.location.c_str(), -1, m_gpCardBadge.get(), Gdiplus::PointF(0.0f, 0.0f), &sfCenter, &locBounds);
        float locW = locBounds.Width + dpf(18.0f);
        checkWrapBadge(locW);
        badgeX += locW + dpf(8.0f);
    }

    if (e.is_active) {
        Gdiplus::RectF actBounds;
        g.MeasureString(L"Активен в системе", -1, m_gpCardBadge.get(), Gdiplus::PointF(0.0f, 0.0f), &sfCenter, &actBounds);
        float actW = actBounds.Width + dpf(18.0f);
        checkWrapBadge(actW);
    }

    curCardY += headerStepY;

    // Addresses row
    std::wstring addrStr = L"IPv4: " + e.primary_ipv4;
    if (!e.secondary_ipv4.empty()) addrStr += L", " + e.secondary_ipv4;
    if (e.ip_pool.size() > 2) {
        addrStr += L"  [Пул: " + std::to_wstring(e.ip_pool.size()) + L" IP]";
    }
    if (!e.doh_template.empty()) addrStr += L"   |   DoH: " + e.doh_template;

    Gdiplus::StringFormat sfAddr;
    sfAddr.SetTrimming(Gdiplus::StringTrimmingWord);
    float maxAddrW = std::max(dpf(120.0f), static_cast<float>(cardWidth) - dpf(36.0f));
    Gdiplus::RectF addrMeasured;
    g.MeasureString(addrStr.c_str(), -1, m_gpCardSub.get(), Gdiplus::SizeF(maxAddrW, 0.0f), &sfAddr, &addrMeasured);
    float addrH = std::max(dpf(20.0f), std::ceil(addrMeasured.Height));
    curCardY += addrH + dpf(2.0f);

    if (e.is_verified) {
        curCardY += dpf(18.0f);
    }

    // Description
    Gdiplus::StringFormat sfDesc;
    sfDesc.SetTrimming(Gdiplus::StringTrimmingWord);
    float maxDescW = std::max(dpf(120.0f), static_cast<float>(cardWidth) - dpf(36.0f));
    Gdiplus::RectF descMeasured;
    g.MeasureString(e.description.c_str(), -1, m_gpCardSub.get(), Gdiplus::SizeF(maxDescW, 0.0f), &sfDesc, &descMeasured);
    float descH = std::max(dpf(18.0f), std::ceil(descMeasured.Height));
    curCardY += descH + dpf(8.0f);

    // Separator
    curCardY += dpf(10.0f);

    // Categories & service chips
    float maxRowW = std::max(dpf(160.0f), static_cast<float>(cardWidth) - dpf(24.0f));
    float chipRowStep = dpf(27.0f);
    float catBottomStep = dpf(29.0f);

    for (const auto& cat : e.categories) {
        std::wstring catTitle = cat.name + L":";
        Gdiplus::RectF catTitleBounds;
        g.MeasureString(catTitle.c_str(), -1, m_gpCardCatHeader.get(), Gdiplus::PointF(0.0f, 0.0f), &catTitleBounds);
        float indentX = curCardX + dpf(152.0f);
        if (curCardX + catTitleBounds.Width + dpf(10.0f) > indentX) {
            indentX = curCardX + catTitleBounds.Width + dpf(10.0f);
        }
        if (indentX > maxRowW - dpf(80.0f)) {
            curCardY += dpf(22.0f);
            indentX = curCardX;
        }

        float chipX = indentX;
        float chipY = curCardY;
        for (const auto& svc : cat.services) {
            Gdiplus::RectF svcBounds;
            g.MeasureString(svc.c_str(), -1, m_gpCardTag.get(), Gdiplus::PointF(0.0f, 0.0f), &sfCenter, &svcBounds);
            float chipW = svcBounds.Width + dpf(20.0f);
            if (chipX > indentX && chipX + chipW > maxRowW) {
                chipX = indentX;
                chipY += chipRowStep;
            }
            chipX += chipW + dpf(6.0f);
        }
        curCardY = chipY + catBottomStep;
    }

    curCardY += dpf(6.0f);
    return std::max(dp(140), static_cast<int>(std::ceil(curCardY)));
}

int SmartDnsDialog::get_item_at_point(int mouseY, int& outItemY) const {
    int targetY = mouseY + m_scrollOffset;
    if (targetY < 0 || targetY > m_totalContentHeight || m_filteredIndices.empty()) {
        outItemY = 0;
        return -1;
    }

    for (size_t i = 0; i < m_filteredIndices.size(); ++i) {
        if (i >= m_itemYPositions.size() || i >= m_itemHeights.size()) break;
        int itemTop = m_itemYPositions[i];
        int itemBottom = itemTop + m_itemHeights[i];
        if (targetY >= itemTop && targetY <= itemBottom) {
            outItemY = itemTop - m_scrollOffset;
            return static_cast<int>(i);
        }
    }
    outItemY = 0;
    return -1;
}

void SmartDnsDialog::on_apply_selected() {
    if (m_selectedIndex >= 0 && m_selectedIndex < static_cast<int>(m_filteredIndices.size())) {
        size_t realIdx = m_filteredIndices[static_cast<size_t>(m_selectedIndex)];
        m_selectedProfileId = m_allEntries[realIdx].profile_id;
        m_cancelVerify.store(true, std::memory_order_release);
        if (m_hWnd && IsWindow(m_hWnd)) {
            ShowWindow(m_hWnd, SW_HIDE);
        }
        m_finished = true;
    }
}

void SmartDnsDialog::on_pick_ip_from_pool() {
    if (m_selectedIndex < 0 || m_selectedIndex >= static_cast<int>(m_filteredIndices.size())) {
        return;
    }

    size_t realIdx = m_filteredIndices[static_cast<size_t>(m_selectedIndex)];
    auto& entry = m_allEntries[realIdx];

    auto ensureInPool = [&](const std::wstring& ip) {
        if (ip.empty()) return;
        if (std::find(entry.ip_pool.begin(), entry.ip_pool.end(), ip) == entry.ip_pool.end()) {
            entry.ip_pool.push_back(ip);
        }
    };
    ensureInPool(entry.primary_ipv4);
    ensureInPool(entry.secondary_ipv4);

    if (entry.ip_pool.empty()) {
        return;
    }

    auto formatIpLabel = [&](const std::wstring& ipWide) -> std::wstring {
        std::string ipUtf8 = wide_to_utf8(ipWide);
        std::wstring label = ipWide;
        for (const auto& c : entry.candidates) {
            if (c.ip == ipUtf8) {
                if (c.is_alive && c.latency_ms >= 0) {
                    label += L" (" + std::to_wstring(c.latency_ms) + L" ms)";
                } else if (entry.doh_working && !entry.udp_alive) {
                    label += L" (только DoH)";
                } else {
                    label += L" (тайм-аут)";
                }
                break;
            }
        }
        return label;
    };

    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) return;

    HMENU hSubPri = CreatePopupMenu();
    if (!hSubPri) {
        DestroyMenu(hMenu);
        return;
    }

    HMENU hSubSec = CreatePopupMenu();
    if (!hSubSec) {
        DestroyMenu(hSubPri);
        DestroyMenu(hMenu);
        return;
    }

    size_t maxItems = std::min<size_t>(entry.ip_pool.size(), 50);
    for (size_t i = 0; i < maxItems; ++i) {
        std::wstring itemLabel = formatIpLabel(entry.ip_pool[i]);
        UINT priFlags = MF_STRING | ((entry.ip_pool[i] == entry.primary_ipv4) ? MF_CHECKED : 0);
        AppendMenuW(hSubPri, priFlags, IDM_POOL_PRIMARY_BASE + i, itemLabel.c_str());

        UINT secFlags = MF_STRING | ((entry.ip_pool[i] == entry.secondary_ipv4) ? MF_CHECKED : 0);
        AppendMenuW(hSubSec, secFlags, IDM_POOL_SECONDARY_BASE + i, itemLabel.c_str());
    }

    AppendMenuW(hSubSec, MF_SEPARATOR, 0, nullptr);
    UINT noneFlags = MF_STRING | (entry.secondary_ipv4.empty() ? MF_CHECKED : 0);
    AppendMenuW(hSubSec, noneFlags, IDM_POOL_SECONDARY_NONE, L"Не использовать (только 1 IP)");

    if (!AppendMenuW(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hSubPri), L"Основной DNS (Primary)")) {
        DestroyMenu(hSubPri);
    }
    if (!AppendMenuW(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hSubSec), L"Резервный DNS (Secondary)")) {
        DestroyMenu(hSubSec);
    }
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

    for (size_t i = 0; i < maxItems; ++i) {
        std::wstring label = formatIpLabel(entry.ip_pool[i]);
        if (entry.ip_pool[i] == entry.primary_ipv4) {
            label += L"  [Основной]";
        } else if (entry.ip_pool[i] == entry.secondary_ipv4) {
            label += L"  [Резервный]";
        }
        UINT flags = MF_STRING | ((entry.ip_pool[i] == entry.primary_ipv4) ? MF_CHECKED : 0);
        AppendMenuW(hMenu, flags, IDM_POOL_PRIMARY_BASE + i, label.c_str());
    }

    POINT pt = {};
    RECT rcBtn = {};
    if (m_hBtnPickIp && GetWindowRect(m_hBtnPickIp, &rcBtn)) {
        pt.x = rcBtn.left;
        pt.y = rcBtn.top;
    } else {
        GetCursorPos(&pt);
    }

    int cmd = TrackPopupMenu(hMenu, TPM_LEFTALIGN | TPM_BOTTOMALIGN | TPM_RETURNCMD, pt.x, pt.y, 0, m_hWnd, nullptr);
    DestroyMenu(hMenu);

    bool updated = false;
    if (cmd >= IDM_POOL_PRIMARY_BASE && cmd < IDM_POOL_PRIMARY_BASE + static_cast<int>(maxItems)) {
        size_t idx = static_cast<size_t>(cmd - IDM_POOL_PRIMARY_BASE);
        std::wstring prevPri = entry.primary_ipv4;
        entry.primary_ipv4 = entry.ip_pool[idx];
        if (entry.secondary_ipv4 == entry.ip_pool[idx]) {
            entry.secondary_ipv4 = (prevPri != entry.ip_pool[idx]) ? prevPri : L"";
        }
        updated = true;
    } else if (cmd >= IDM_POOL_SECONDARY_BASE && cmd < IDM_POOL_SECONDARY_BASE + static_cast<int>(maxItems)) {
        size_t idx = static_cast<size_t>(cmd - IDM_POOL_SECONDARY_BASE);
        entry.secondary_ipv4 = (entry.ip_pool[idx] == entry.primary_ipv4) ? L"" : entry.ip_pool[idx];
        updated = true;
    } else if (cmd == IDM_POOL_SECONDARY_NONE) {
        entry.secondary_ipv4 = L"";
        updated = true;
    }

    if (updated) {
        ensureInPool(entry.primary_ipv4);
        ensureInPool(entry.secondary_ipv4);

        std::string newPriUtf8 = wide_to_utf8(entry.primary_ipv4);
        std::string newSecUtf8 = wide_to_utf8(entry.secondary_ipv4);
        for (const auto& c : entry.candidates) {
            if (c.ip == newPriUtf8) {
                entry.udp_alive = c.is_alive && c.latency_ms > 0;
                entry.ping_ms = c.is_alive ? c.latency_ms : (entry.doh_working ? entry.doh_latency_ms : -1);
                break;
            }
        }

        if (m_pConfig) {
            auto optProf = m_pConfig->get_profile_by_id(entry.profile_id);
            if (optProf) {
                DnsProfile p = *optProf;
                p.primary_ipv4 = newPriUtf8;
                p.secondary_ipv4 = newSecUtf8;
                p.ip_pool.clear();
                for (const auto& wIp : entry.ip_pool) {
                    std::string ipUtf8 = wide_to_utf8(wIp);
                    if (!ipUtf8.empty() && std::find(p.ip_pool.begin(), p.ip_pool.end(), ipUtf8) == p.ip_pool.end()) {
                        p.ip_pool.push_back(std::move(ipUtf8));
                    }
                }
                m_pConfig->update_profile(p);
                m_pConfig->save_to_file();
            }
        }

        m_statusText = L"Выбран IP для " + entry.name + L": " + entry.primary_ipv4 +
                       (entry.secondary_ipv4.empty() ? L"" : (L", " + entry.secondary_ipv4));
        recalculate_item_layouts();
        if (m_hListWnd) InvalidateRect(m_hListWnd, nullptr, TRUE);
        InvalidateRect(m_hWnd, nullptr, FALSE);
    }
}

void SmartDnsDialog::on_open_website(size_t index) {
    if (index < m_filteredIndices.size()) {
        size_t realIdx = m_filteredIndices[index];
        const auto& entry = m_allEntries[realIdx];
        if (has_valid_http_url(entry.website_url)) {
            ShellExecuteW(m_hWnd, L"open", entry.website_url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    }
}

void SmartDnsDialog::on_verify_and_scrape_ips() {
    if (m_isVerifying.load(std::memory_order_acquire)) return;
    if (m_verifyThread.joinable()) {
        m_verifyThread.join();
    }

    m_cancelVerify.store(false, std::memory_order_release);
    m_isVerifying.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(m_verifyMutex);
        m_pendingVerifyResults.clear();
        m_hasVerifyResults = false;
    }

    if (m_hBtnVerify && IsWindow(m_hBtnVerify)) {
        EnableWindow(m_hBtnVerify, FALSE);
        SetWindowTextW(m_hBtnVerify, L"Проверка...");
        InvalidateRect(m_hBtnVerify, nullptr, FALSE);
    }
    m_statusText = L"Скрейпинг актуальных IP и тестирование DoH...";
    InvalidateRect(m_hWnd, nullptr, FALSE);

    HWND hWndDlg = m_hWnd;
    auto entriesCopy = m_allEntries;

    m_verifyThread = std::thread([this, hWndDlg, entriesCopy]() {
        std::vector<DnsProfile> profilesToVerify;
        profilesToVerify.reserve(entriesCopy.size());
        for (const auto& e : entriesCopy) {
            DnsProfile p;
            p.id = e.profile_id;
            p.name = QuickDns::Encoding::wide_to_utf8(e.name);
            p.primary_ipv4 = QuickDns::Encoding::wide_to_utf8(e.primary_ipv4);
            p.secondary_ipv4 = QuickDns::Encoding::wide_to_utf8(e.secondary_ipv4);
            p.doh_template = QuickDns::Encoding::wide_to_utf8(e.doh_template);
            p.website_url = QuickDns::Encoding::wide_to_utf8(e.website_url);
            p.is_smart_dns = true;
            for (const auto& poolIp : e.ip_pool) {
                p.ip_pool.push_back(QuickDns::Encoding::wide_to_utf8(poolIp));
            }
            profilesToVerify.push_back(std::move(p));
        }

        auto results = SmartDnsVerifier::verify_profiles(
            profilesToVerify,
            [this, hWndDlg](size_t completed, size_t total, const SmartDnsVerificationResult&) {
                if (m_cancelVerify.load(std::memory_order_acquire)) return;
                if (hWndDlg && IsWindow(hWndDlg)) {
                    PostMessageW(hWndDlg, WM_APP_SMARTDNS_VERIFY_PROGRESS,
                                 static_cast<WPARAM>(completed), static_cast<LPARAM>(total));
                }
            },
            &m_cancelVerify
        );

        if (m_cancelVerify.load(std::memory_order_acquire)) {
            m_isVerifying.store(false, std::memory_order_release);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(m_verifyMutex);
            m_pendingVerifyResults = std::move(results);
            m_hasVerifyResults = true;
        }

        if (!m_cancelVerify.load(std::memory_order_acquire) && hWndDlg && IsWindow(hWndDlg)) {
            PostMessageW(hWndDlg, WM_APP_SMARTDNS_VERIFY_DONE, 0, 0);
        } else {
            m_isVerifying.store(false, std::memory_order_release);
        }
    });
}

void SmartDnsDialog::on_close() {
    m_cancelVerify.store(true, std::memory_order_release);
    if (m_hWnd && IsWindow(m_hWnd)) {
        ShowWindow(m_hWnd, SW_HIDE);
    }
    m_finished = true;
}

void SmartDnsDialog::attach_button_effects(HWND hBtn, UINT_PTR subclassId) {
    if (!hBtn) return;
    auto* anim = new ButtonAnimationState();
    SetPropW(hBtn, L"QuickDnsAnim", reinterpret_cast<HANDLE>(anim));
    SetWindowSubclass(hBtn, AnimatedButtonSubclassProc, subclassId, reinterpret_cast<DWORD_PTR>(anim));
}

void SmartDnsDialog::draw_custom_button(DRAWITEMSTRUCT* pDis) {
    if (!pDis) return;

    HDC hdc = pDis->hDC;
    RECT rc = pDis->rcItem;
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;

    auto pal = GetPalette(m_isDarkMode);
    bool isPrimary = (pDis->CtlID == IDC_BTN_SMARTDNS_APPLY);
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
    if (!hbmMem) {
        DeleteDC(hdcMem);
        return;
    }
    HGDIOBJ oldBmp = SelectObject(hdcMem, hbmMem);

    RECT rcLocal = { 0, 0, w, h };
    HBRUSH hbrWin = CreateSolidBrush(pal.bg);
    FillRect(hdcMem, &rcLocal, hbrWin);
    DeleteObject(hbrWin);

    {
        Gdiplus::Graphics g(hdcMem);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

        Gdiplus::GraphicsPath path;
        AddRoundedRect(path, Gdiplus::RectF(0.5f, 0.5f, static_cast<float>(w) - 1.0f, static_cast<float>(h) - 1.0f), dpf(7.0f));

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

void SmartDnsDialog::draw_card_item(Gdiplus::Graphics& g, size_t index, int y, int width, int height, bool isSelected, bool isHovered) {
    if (index >= m_filteredIndices.size()) return;
    if (!m_gpCardTitle || !m_gpCardBadge || !m_gpCardSub || !m_gpCardCatHeader || !m_gpCardTag || !m_gpCardLink) return;

    size_t realIdx = m_filteredIndices[index];
    const auto& e = m_allEntries[realIdx];
    auto pal = GetPalette(m_isDarkMode);

    // 1. Card container background
    Gdiplus::RectF rcCard(0.5f, static_cast<float>(y) + 0.5f, static_cast<float>(width) - 1.0f, static_cast<float>(height) - 1.0f);
    Gdiplus::GraphicsPath cardPath;
    AddRoundedRect(cardPath, rcCard, dpf(8.0f));

    COLORREF bgCol = isSelected
        ? (m_isDarkMode ? RGB(36, 44, 60) : RGB(232, 240, 255))
        : pal.cardBg;
    COLORREF borderCol = isSelected
        ? (m_isDarkMode ? RGB(96, 165, 250) : RGB(37, 99, 235))
        : (isHovered ? (m_isDarkMode ? RGB(75, 90, 115) : RGB(180, 195, 220)) : pal.cardBorder);
    float borderWidth = isSelected ? 1.5f : 1.0f;

    Gdiplus::SolidBrush cardBr(Gdiplus::Color(255, GetRValue(bgCol), GetGValue(bgCol), GetBValue(bgCol)));
    Gdiplus::Pen cardPen(Gdiplus::Color(255, GetRValue(borderCol), GetGValue(borderCol), GetBValue(borderCol)), borderWidth);
    g.FillPath(&cardBr, &cardPath);
    g.DrawPath(&cardPen, &cardPath);

    // Left indicator bar
    if (isSelected || e.is_active) {
        Gdiplus::GraphicsPath selBarPath;
        AddRoundedRect(selBarPath, Gdiplus::RectF(dpf(2.0f), static_cast<float>(y) + dpf(8.0f), dpf(3.5f), static_cast<float>(height) - dpf(16.0f)), dpf(2.0f));
        COLORREF barColor = e.is_active ? RGB(34, 197, 94) : (m_isDarkMode ? RGB(96, 165, 250) : RGB(37, 99, 235));
        Gdiplus::SolidBrush selBarBr(Gdiplus::Color(255, GetRValue(barColor), GetGValue(barColor), GetBValue(barColor)));
        g.FillPath(&selBarBr, &selBarPath);
    }

    Gdiplus::SolidBrush textPriBr(Gdiplus::Color(255, GetRValue(pal.textPrimary), GetGValue(pal.textPrimary), GetBValue(pal.textPrimary)));
    Gdiplus::SolidBrush textSecBr(Gdiplus::Color(255, GetRValue(pal.textSecondary), GetGValue(pal.textSecondary), GetBValue(pal.textSecondary)));
    Gdiplus::SolidBrush textMutedBr(Gdiplus::Color(255, GetRValue(pal.textMuted), GetGValue(pal.textMuted), GetBValue(pal.textMuted)));

    Gdiplus::StringFormat sfCenter(Gdiplus::StringFormat::GenericTypographic());
    sfCenter.SetAlignment(Gdiplus::StringAlignmentCenter);
    sfCenter.SetLineAlignment(Gdiplus::StringAlignmentCenter);

    float maxHeaderLeftW = static_cast<float>(width) - dpf(24.0f);
    if (has_valid_http_url(e.website_url)) {
        Gdiplus::RectF linkRc = get_website_pill_rect(g, width, y);
        Gdiplus::GraphicsPath linkPath;
        AddRoundedRect(linkPath, linkRc, dpf(4.5f));
        Gdiplus::SolidBrush linkBgBr(m_isDarkMode ? Gdiplus::Color(40, 59, 130, 246) : Gdiplus::Color(25, 37, 99, 235));
        Gdiplus::Pen linkPen(m_isDarkMode ? Gdiplus::Color(100, 96, 165, 250) : Gdiplus::Color(80, 37, 99, 235), 1.0f);
        g.FillPath(&linkBgBr, &linkPath);
        g.DrawPath(&linkPen, &linkPath);

        Gdiplus::SolidBrush linkTextBr(m_isDarkMode ? Gdiplus::Color(255, 147, 197, 253) : Gdiplus::Color(255, 37, 99, 235));
        g.DrawString(WEBSITE_PILL_LABEL, -1, m_gpCardLink.get(), linkRc, &sfCenter, &linkTextBr);
        maxHeaderLeftW = linkRc.X - dpf(12.0f);
    }

    float curCardY = static_cast<float>(y) + dpf(10.0f);
    float curCardX = dpf(16.0f);
    float headerStepY = dpf(26.0f);
    float badgeH = dpf(22.0f);

    g.DrawString(e.name.c_str(), -1, m_gpCardTitle.get(), Gdiplus::PointF(curCardX, curCardY), &textPriBr);

    Gdiplus::RectF nameBounds;
    g.MeasureString(e.name.c_str(), -1, m_gpCardTitle.get(), Gdiplus::PointF(0.0f, 0.0f), &nameBounds);
    float badgeX = curCardX + nameBounds.Width + dpf(12.0f);
    if (badgeX > maxHeaderLeftW) {
        curCardY += headerStepY;
        badgeX = curCardX;
        maxHeaderLeftW = static_cast<float>(width) - dpf(24.0f);
    }

    auto checkWrapBadge = [&](float neededW) {
        if (badgeX + neededW > maxHeaderLeftW) {
            curCardY += headerStepY;
            badgeX = curCardX;
            maxHeaderLeftW = static_cast<float>(width) - dpf(24.0f);
        }
    };

    // [SmartDNS] green pill badge
    {
        Gdiplus::RectF sBounds;
        g.MeasureString(L"SmartDNS", -1, m_gpCardBadge.get(), Gdiplus::PointF(0.0f, 0.0f), &sfCenter, &sBounds);
        float pillW = sBounds.Width + dpf(20.0f);
        checkWrapBadge(pillW);

        Gdiplus::RectF pillRc(badgeX, curCardY, pillW, badgeH);
        Gdiplus::GraphicsPath pillPath;
        AddRoundedRect(pillPath, pillRc, dpf(4.5f));
        Gdiplus::SolidBrush pillBg(Gdiplus::Color(45, 34, 197, 94));
        Gdiplus::Pen pillPen(Gdiplus::Color(140, 34, 197, 94), 1.0f);
        g.FillPath(&pillBg, &pillPath);
        g.DrawPath(&pillPen, &pillPath);

        Gdiplus::SolidBrush pillText(Gdiplus::Color(255, 34, 197, 94));
        g.DrawString(L"SmartDNS", -1, m_gpCardBadge.get(), pillRc, &sfCenter, &pillText);
        badgeX += pillW + dpf(8.0f);
    }

    // Location badge
    if (!e.location.empty()) {
        Gdiplus::RectF locBounds;
        g.MeasureString(e.location.c_str(), -1, m_gpCardBadge.get(), Gdiplus::PointF(0.0f, 0.0f), &sfCenter, &locBounds);
        float locW = locBounds.Width + dpf(18.0f);
        checkWrapBadge(locW);

        Gdiplus::RectF locRc(badgeX, curCardY, locW, badgeH);
        Gdiplus::GraphicsPath locPath;
        AddRoundedRect(locPath, locRc, dpf(4.5f));

        Gdiplus::SolidBrush locBg(m_isDarkMode ? Gdiplus::Color(255, 38, 44, 56) : Gdiplus::Color(255, 235, 240, 248));
        Gdiplus::Pen locPen(m_isDarkMode ? Gdiplus::Color(255, 60, 70, 88) : Gdiplus::Color(255, 205, 215, 228), 1.0f);
        g.FillPath(&locBg, &locPath);
        g.DrawPath(&locPen, &locPath);

        g.DrawString(e.location.c_str(), -1, m_gpCardBadge.get(), locRc, &sfCenter, &textSecBr);
        badgeX += locW + dpf(8.0f);
    }

    // Active badge
    if (e.is_active) {
        Gdiplus::RectF actBounds;
        g.MeasureString(L"Активен в системе", -1, m_gpCardBadge.get(), Gdiplus::PointF(0.0f, 0.0f), &sfCenter, &actBounds);
        float actW = actBounds.Width + dpf(18.0f);
        checkWrapBadge(actW);

        Gdiplus::RectF actRc(badgeX, curCardY, actW, badgeH);
        Gdiplus::GraphicsPath actPath;
        AddRoundedRect(actPath, actRc, dpf(4.5f));
        Gdiplus::SolidBrush actBg(Gdiplus::Color(45, 34, 197, 94));
        Gdiplus::Pen actPen(Gdiplus::Color(140, 34, 197, 94), 1.0f);
        g.FillPath(&actBg, &actPath);
        g.DrawPath(&actPen, &actPath);

        Gdiplus::SolidBrush actText(Gdiplus::Color(255, 34, 197, 94));
        g.DrawString(L"Активен в системе", -1, m_gpCardBadge.get(), actRc, &sfCenter, &actText);
    }

    curCardY += headerStepY;

    // 3. Addresses: IPv4, DoH
    std::wstring addrStr = L"IPv4: " + e.primary_ipv4;
    if (!e.secondary_ipv4.empty()) addrStr += L", " + e.secondary_ipv4;
    if (e.ip_pool.size() > 2) {
        addrStr += L"  [Пул: " + std::to_wstring(e.ip_pool.size()) + L" IP]";
    }
    if (!e.doh_template.empty()) addrStr += L"   |   DoH: " + e.doh_template;

    Gdiplus::StringFormat sfAddr;
    sfAddr.SetTrimming(Gdiplus::StringTrimmingWord);
    float maxAddrW = std::max(dpf(120.0f), static_cast<float>(width) - dpf(36.0f));
    Gdiplus::RectF addrMeasured;
    g.MeasureString(addrStr.c_str(), -1, m_gpCardSub.get(), Gdiplus::SizeF(maxAddrW, 0.0f), &sfAddr, &addrMeasured);
    float addrH = std::max(dpf(20.0f), std::ceil(addrMeasured.Height));
    Gdiplus::RectF addrRect(curCardX, curCardY, maxAddrW, addrH);
    g.DrawString(addrStr.c_str(), -1, m_gpCardSub.get(), addrRect, &sfAddr, &textSecBr);
    curCardY += addrH + dpf(2.0f);

    if (e.is_verified) {
        std::wstring statStr;
        Gdiplus::Color statColor;
        if (e.udp_alive && e.ping_ms > 0) {
            statStr = L"Статус: UDP " + std::to_wstring(e.ping_ms) + L" ms";
            if (!e.doh_status.empty()) {
                statStr += L"   |   " + e.doh_status;
            }
            statColor = Gdiplus::Color(255, GetRValue(pal.accentGreen), GetGValue(pal.accentGreen), GetBValue(pal.accentGreen));
        } else if (e.doh_working) {
            int dohMs = (e.doh_latency_ms > 0) ? e.doh_latency_ms : e.ping_ms;
            if (dohMs > 0) {
                statStr = L"Статус: DoH OK (только шифрованный DoH, " + std::to_wstring(dohMs) + L" мс)";
            } else {
                statStr = L"Статус: DoH OK (только шифрованный DoH)";
            }
            statColor = Gdiplus::Color(255, GetRValue(pal.accentGreen), GetGValue(pal.accentGreen), GetBValue(pal.accentGreen));
        } else {
            statStr = L"Статус: UDP тайм-аут";
            if (!e.doh_status.empty()) {
                statStr += L"   |   " + e.doh_status;
            }
            statColor = Gdiplus::Color(255, GetRValue(pal.accentRed), GetGValue(pal.accentRed), GetBValue(pal.accentRed));
        }
        Gdiplus::SolidBrush statBr(statColor);
        g.DrawString(statStr.c_str(), -1, m_gpCardBadge.get(), Gdiplus::PointF(curCardX, curCardY), &statBr);
        curCardY += dpf(18.0f);
    }

    // Description line
    Gdiplus::StringFormat sfDesc;
    sfDesc.SetTrimming(Gdiplus::StringTrimmingWord);
    float maxDescW = std::max(dpf(120.0f), static_cast<float>(width) - dpf(36.0f));
    Gdiplus::RectF descMeasured;
    g.MeasureString(e.description.c_str(), -1, m_gpCardSub.get(), Gdiplus::SizeF(maxDescW, 0.0f), &sfDesc, &descMeasured);
    float descH = std::max(dpf(18.0f), std::ceil(descMeasured.Height));
    Gdiplus::RectF descRect(curCardX, curCardY, maxDescW, descH);
    g.DrawString(e.description.c_str(), -1, m_gpCardSub.get(), descRect, &sfDesc, &textMutedBr);
    curCardY += descH + dpf(8.0f);

    // Separator line inside card
    Gdiplus::Pen sepPen(Gdiplus::Color(70, GetRValue(pal.separator), GetGValue(pal.separator), GetBValue(pal.separator)), 1.0f);
    g.DrawLine(&sepPen, curCardX, curCardY, static_cast<float>(width) - dpf(16.0f), curCardY);
    curCardY += dpf(10.0f);

    // 4. Categorized Service Tags
    std::wstring searchLower = m_searchQuery;
    std::transform(searchLower.begin(), searchLower.end(), searchLower.begin(), [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });

    float maxRowW = std::max(dpf(160.0f), static_cast<float>(width) - dpf(24.0f));
    float chipRowStep = dpf(27.0f);
    float catBottomStep = dpf(29.0f);
    float chipH = dpf(22.0f);

    for (const auto& cat : e.categories) {
        std::wstring catTitle = cat.name + L":";
        g.DrawString(catTitle.c_str(), -1, m_gpCardCatHeader.get(), Gdiplus::PointF(curCardX, curCardY + dpf(3.0f)), &textPriBr);

        Gdiplus::RectF catTitleBounds;
        g.MeasureString(catTitle.c_str(), -1, m_gpCardCatHeader.get(), Gdiplus::PointF(0.0f, 0.0f), &catTitleBounds);
        float indentX = curCardX + dpf(152.0f);
        if (curCardX + catTitleBounds.Width + dpf(10.0f) > indentX) {
            indentX = curCardX + catTitleBounds.Width + dpf(10.0f);
        }
        if (indentX > maxRowW - dpf(80.0f)) {
            curCardY += dpf(22.0f);
            indentX = curCardX;
        }

        float chipX = indentX;
        float chipY = curCardY;

        for (const auto& svc : cat.services) {
            Gdiplus::RectF svcBounds;
            g.MeasureString(svc.c_str(), -1, m_gpCardTag.get(), Gdiplus::PointF(0.0f, 0.0f), &sfCenter, &svcBounds);
            float chipW = svcBounds.Width + dpf(20.0f);

            if (chipX > indentX && chipX + chipW > maxRowW) {
                chipX = indentX;
                chipY += chipRowStep;
            }

            bool isMatched = false;
            if (!searchLower.empty()) {
                std::wstring sLower = svc;
                std::transform(sLower.begin(), sLower.end(), sLower.begin(), [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
                if (sLower.find(searchLower) != std::wstring::npos) {
                    isMatched = true;
                }
            }

            Gdiplus::RectF chipRc(chipX, chipY, chipW, chipH);
            Gdiplus::GraphicsPath chipPath;
            AddRoundedRect(chipPath, chipRc, dpf(4.0f));

            Gdiplus::Color chipBg = isMatched
                ? Gdiplus::Color(60, 34, 197, 94)
                : (m_isDarkMode ? Gdiplus::Color(255, 34, 40, 52) : Gdiplus::Color(255, 240, 244, 250));
            Gdiplus::Color chipBorder = isMatched
                ? Gdiplus::Color(160, 34, 197, 94)
                : (m_isDarkMode ? Gdiplus::Color(255, 55, 65, 82) : Gdiplus::Color(255, 210, 220, 232));
            Gdiplus::Color chipTextColor = isMatched
                ? Gdiplus::Color(255, 34, 197, 94)
                : Gdiplus::Color(255, GetRValue(pal.textPrimary), GetGValue(pal.textPrimary), GetBValue(pal.textPrimary));

            Gdiplus::SolidBrush chipBgBr(chipBg);
            Gdiplus::Pen chipPen(chipBorder, 1.0f);
            g.FillPath(&chipBgBr, &chipPath);
            g.DrawPath(&chipPen, &chipPath);

            Gdiplus::SolidBrush chipTextBr(chipTextColor);
            g.DrawString(svc.c_str(), -1, m_gpCardTag.get(), chipRc, &sfCenter, &chipTextBr);

            chipX += chipW + dpf(6.0f);
        }

        curCardY = chipY + catBottomStep;
    }
}

LRESULT CALLBACK SmartDnsDialog::dialog_proc_thunk(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    SmartDnsDialog* pThis = nullptr;
    if (uMsg == WM_NCCREATE) {
        auto* pCreate = reinterpret_cast<CREATESTRUCTW*>(lParam);
        pThis = reinterpret_cast<SmartDnsDialog*>(pCreate->lpCreateParams);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pThis));
        pThis->m_hWnd = hWnd;
    } else {
        pThis = reinterpret_cast<SmartDnsDialog*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    }

    if (pThis) {
        return pThis->handle_message(hWnd, uMsg, wParam, lParam);
    }
    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

LRESULT SmartDnsDialog::handle_message(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_COMMAND: {
        int id = LOWORD(wParam);
        int code = HIWORD(wParam);

        if (id == IDC_EDIT_SMARTDNS_SEARCH && code == EN_CHANGE) {
            wchar_t buf[256] = {};
            if (m_hSearchEdit) {
                GetWindowTextW(m_hSearchEdit, buf, 256);
                m_searchQuery = buf;
                m_scrollOffset = 0;
                m_wheelAccum = 0;
                update_filtered_list();
                if (m_hListWnd) InvalidateRect(m_hListWnd, nullptr, FALSE);
            }
            return 0;
        }

        if (id == IDC_BTN_SMARTDNS_APPLY) {
            on_apply_selected();
            return 0;
        }
        if (id == IDC_BTN_SMARTDNS_PICK_IP) {
            on_pick_ip_from_pool();
            return 0;
        }
        if (id == IDC_BTN_SMARTDNS_WEBSITE) {
            if (m_selectedIndex >= 0) {
                on_open_website(static_cast<size_t>(m_selectedIndex));
            }
            return 0;
        }
        if (id == IDC_BTN_SMARTDNS_CLOSE || id == IDCANCEL) {
            on_close();
            return 0;
        }
        if (id == IDC_BTN_SMARTDNS_SCRAPE_VERIFY) {
            on_verify_and_scrape_ips();
            return 0;
        }
        break;
    }

    case WM_APP_SMARTDNS_VERIFY_PROGRESS: {
        if (m_isVerifying.load(std::memory_order_acquire)) {
            size_t completed = static_cast<size_t>(wParam);
            size_t total = static_cast<size_t>(lParam);
            m_statusText = L"Проверка SmartDNS и тестирование DoH (" +
                           std::to_wstring(completed) + L" из " + std::to_wstring(total) + L")...";
            RECT rcClient{};
            GetClientRect(hWnd, &rcClient);
            RECT rcStatus = { 0, dp(116), rcClient.right, dp(148) };
            InvalidateRect(hWnd, &rcStatus, FALSE);
        }
        return 0;
    }

    case WM_APP_SMARTDNS_VERIFY_DONE: {
        std::vector<SmartDnsVerificationResult> results;
        bool hasData = false;
        {
            std::lock_guard<std::mutex> lock(m_verifyMutex);
            if (m_hasVerifyResults) {
                results = std::move(m_pendingVerifyResults);
                m_pendingVerifyResults.clear();
                m_hasVerifyResults = false;
                hasData = true;
            }
        }

        if (m_verifyThread.joinable()) {
            m_verifyThread.join();
        }
        m_isVerifying.store(false, std::memory_order_release);

        if (hasData && !m_cancelVerify.load(std::memory_order_acquire)) {
            std::wstring changesMsg;
            std::vector<std::pair<DnsProfile, SmartDnsVerificationResult>> pendingChanges;

            for (const auto& r : results) {
                for (auto& e : m_allEntries) {
                    if (e.profile_id == r.profile_id) {
                        e.doh_status = r.doh_result.status_text;
                        e.doh_working = r.doh_result.is_working;
                        e.doh_latency_ms = r.doh_result.latency_ms;
                        bool anyUdpAlive = false;
                        for (const auto& c : r.candidate_ips) {
                            if (c.is_alive && c.latency_ms > 0) {
                                anyUdpAlive = true;
                                break;
                            }
                        }
                        e.udp_alive = anyUdpAlive;
                        e.ping_ms = (anyUdpAlive && r.best_primary_latency > 0)
                            ? r.best_primary_latency
                            : (r.doh_result.is_working ? r.doh_result.latency_ms : r.best_primary_latency);
                        e.is_verified = true;
                        e.candidates = r.candidate_ips;
                        for (const auto& c : r.candidate_ips) {
                            if (c.is_alive && !c.ip.empty()) {
                                std::wstring wIp = QuickDns::Encoding::utf8_to_wide(c.ip);
                                if (std::find(e.ip_pool.begin(), e.ip_pool.end(), wIp) == e.ip_pool.end()) {
                                    e.ip_pool.push_back(wIp);
                                }
                            }
                        }

                        if (m_pConfig && r.ip_updated && !r.best_primary_ip.empty()) {
                            auto optProfile = m_pConfig->get_profile_by_id(r.profile_id);
                            if (optProfile) {
                                std::string oldPri = optProfile->primary_ipv4;
                                std::string newPri = r.best_primary_ip;
                                if (oldPri != newPri) {
                                    pendingChanges.push_back({ *optProfile, r });
                                    changesMsg += e.name + L": " + QuickDns::Encoding::utf8_to_wide(oldPri) +
                                                  L" -> " + QuickDns::Encoding::utf8_to_wide(newPri) + L"\n";
                                }
                            }
                        }
                        break;
                    }
                }
            }

            int appliedCount = 0;
            if (!pendingChanges.empty() && m_hWnd && IsWindow(m_hWnd)) {
                std::wstring confirmMsg = L"Найдены обновленные IP-адреса для следующих профилей:\n\n" +
                                          changesMsg +
                                          L"\nПрименить эти изменения к профилям?";
                int res = MessageBoxW(m_hWnd, confirmMsg.c_str(), L"Подтверждение обновления SmartDNS", MB_YESNO | MB_ICONQUESTION);
                if (res == IDYES) {
                    auto addUtf8PoolIp = [](std::vector<std::string>& pool, const std::string& ip) {
                        if (!ip.empty() && std::find(pool.begin(), pool.end(), ip) == pool.end()) {
                            pool.push_back(ip);
                        }
                    };
                    auto addWidePoolIp = [](std::vector<std::wstring>& pool, const std::wstring& ip) {
                        if (!ip.empty() && std::find(pool.begin(), pool.end(), ip) == pool.end()) {
                            pool.push_back(ip);
                        }
                    };

                    for (const auto& item : pendingChanges) {
                        DnsProfile p = item.first;
                        p.primary_ipv4 = item.second.best_primary_ip;
                        if (!item.second.best_secondary_ip.empty()) {
                            p.secondary_ipv4 = item.second.best_secondary_ip;
                        }
                        addUtf8PoolIp(p.ip_pool, p.primary_ipv4);
                        addUtf8PoolIp(p.ip_pool, p.secondary_ipv4);
                        for (const auto& c : item.second.candidate_ips) {
                            if (c.is_alive && !c.ip.empty()) {
                                addUtf8PoolIp(p.ip_pool, c.ip);
                            }
                        }

                        m_pConfig->update_profile(p);
                        appliedCount++;

                        for (auto& e : m_allEntries) {
                            if (e.profile_id == p.id) {
                                e.primary_ipv4 = QuickDns::Encoding::utf8_to_wide(p.primary_ipv4);
                                if (!p.secondary_ipv4.empty()) {
                                    e.secondary_ipv4 = QuickDns::Encoding::utf8_to_wide(p.secondary_ipv4);
                                }
                                addWidePoolIp(e.ip_pool, e.primary_ipv4);
                                addWidePoolIp(e.ip_pool, e.secondary_ipv4);
                                break;
                            }
                        }
                    }
                    if (appliedCount > 0) {
                        m_pConfig->save_to_file();
                    }
                }
            }

            m_statusText = L"Проверка завершена. Обновлено IP провайдеров: " + std::to_wstring(appliedCount);
            recalculate_item_layouts();
            update_action_buttons_state();
        }

        if (m_hBtnVerify && IsWindow(m_hBtnVerify)) {
            EnableWindow(m_hBtnVerify, TRUE);
            SetWindowTextW(m_hBtnVerify, L"Обновить IP и DoH");
            InvalidateRect(m_hBtnVerify, nullptr, FALSE);
        }
        if (m_hListWnd && IsWindow(m_hListWnd)) {
            InvalidateRect(m_hListWnd, nullptr, TRUE);
        }
        if (m_hWnd && IsWindow(m_hWnd)) {
            InvalidateRect(m_hWnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_GETMINMAXINFO: {
        auto* pMMI = reinterpret_cast<MINMAXINFO*>(lParam);
        RECT rcMin = { 0, 0, dp(760), dp(560) };
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

    case WM_MOUSEWHEEL: {
        if (m_hListWnd && IsWindow(m_hListWnd)) {
            return SendMessageW(m_hListWnd, uMsg, wParam, lParam);
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

        if (m_gpHeaderTitle && m_gpHeaderSub && m_gpHeaderLbl && m_gpHeaderStatus) {
            Gdiplus::Graphics g(hdcMem);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
            g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

            Gdiplus::SolidBrush brPri(Gdiplus::Color(255, GetRValue(pal.textPrimary), GetGValue(pal.textPrimary), GetBValue(pal.textPrimary)));
            Gdiplus::SolidBrush brSec(Gdiplus::Color(255, GetRValue(pal.textSecondary), GetGValue(pal.textSecondary), GetBValue(pal.textSecondary)));
            Gdiplus::SolidBrush brMuted(Gdiplus::Color(255, GetRValue(pal.textMuted), GetGValue(pal.textMuted), GetBValue(pal.textMuted)));

            int marginX = dp(20);
            g.DrawString(L"Каталог SmartDNS и разблокируемые сервисы", -1, m_gpHeaderTitle.get(),
                Gdiplus::PointF(static_cast<float>(marginX), dpf(14.0f)), &brPri);

            Gdiplus::StringFormat sfSub;
            sfSub.SetTrimming(Gdiplus::StringTrimmingWord);
            Gdiplus::RectF rcSub(static_cast<float>(marginX), dpf(40.0f),
                static_cast<float>(std::max(dp(100), w - 2 * marginX)), dpf(38.0f));
            g.DrawString(L"Серверы с автоматическим проксированием для доступа к зарубежным нейросетям, музыке и сайтам без VPN.",
                -1, m_gpHeaderSub.get(), rcSub, &sfSub, &brSec);

            g.DrawString(L"Поиск сервиса:", -1, m_gpHeaderLbl.get(),
                Gdiplus::PointF(static_cast<float>(marginX), dpf(88.0f)), &brPri);

            int searchBoxX = marginX + dp(154);
            int searchBoxY = dp(82);
            int searchBoxW = std::max(dp(120), (w - 2 * marginX) - dp(154));
            int searchBoxH = dp(32);

            Gdiplus::GraphicsPath sbPath;
            AddRoundedRect(sbPath,
                Gdiplus::RectF(static_cast<float>(searchBoxX) + 0.5f, static_cast<float>(searchBoxY) + 0.5f,
                               static_cast<float>(searchBoxW) - 1.0f, static_cast<float>(searchBoxH) - 1.0f),
                dpf(6.0f));
            Gdiplus::SolidBrush sbBg(Gdiplus::Color(255, GetRValue(pal.cardBg), GetGValue(pal.cardBg), GetBValue(pal.cardBg)));
            g.FillPath(&sbBg, &sbPath);

            bool isSearchActive = (m_searchEditFocused || m_searchEditHovered);
            COLORREF sbBorderCol = isSearchActive
                ? (m_isDarkMode ? RGB(96, 165, 250) : RGB(37, 99, 235))
                : pal.cardBorder;
            float sbBorderWidth = isSearchActive ? 1.5f : 1.0f;
            Gdiplus::Pen sbBorderPen(Gdiplus::Color(255, GetRValue(sbBorderCol), GetGValue(sbBorderCol), GetBValue(sbBorderCol)), sbBorderWidth);
            g.DrawPath(&sbBorderPen, &sbPath);

            g.DrawString(m_statusText.c_str(), -1, m_gpHeaderStatus.get(),
                Gdiplus::PointF(static_cast<float>(marginX), dpf(122.0f)), &brMuted);
        }

        BitBlt(hdc, 0, 0, w, h, hdcMem, 0, 0, SRCCOPY);
        SelectObject(hdcMem, oldBmp);
        DeleteObject(hbmMem);
        DeleteDC(hdcMem);
        EndPaint(hWnd, &ps);
        return 0;
    }

    case WM_CTLCOLORSTATIC: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        auto pal = GetPalette(m_isDarkMode);
        SetTextColor(hdc, pal.textPrimary);
        SetBkMode(hdc, TRANSPARENT);
        return reinterpret_cast<INT_PTR>(m_hbrBg);
    }

    case WM_CTLCOLOREDIT: {
        HDC hdcEdit = reinterpret_cast<HDC>(wParam);
        auto pal = GetPalette(m_isDarkMode);
        SetBkMode(hdcEdit, OPAQUE);
        SetTextColor(hdcEdit, pal.textPrimary);
        SetBkColor(hdcEdit, pal.cardBg);
        return reinterpret_cast<INT_PTR>(m_hbrCard);
    }

    case WM_CLOSE: {
        on_close();
        return 0;
    }

    case WM_NCDESTROY: {
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, 0);
        m_hWnd = nullptr;
        m_finished = true;
        return 0;
    }
    }

    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

LRESULT CALLBACK SmartDnsDialog::list_proc_thunk(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    SmartDnsDialog* pThis = nullptr;
    if (uMsg == WM_NCCREATE) {
        auto* pCreate = reinterpret_cast<CREATESTRUCTW*>(lParam);
        pThis = reinterpret_cast<SmartDnsDialog*>(pCreate->lpCreateParams);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pThis));
    } else {
        pThis = reinterpret_cast<SmartDnsDialog*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    }

    if (pThis) {
        return pThis->handle_list_message(hWnd, uMsg, wParam, lParam);
    }
    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

LRESULT SmartDnsDialog::handle_list_message(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
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

            int totalItems = static_cast<int>(m_filteredIndices.size());
            int totalContentHeight = m_totalContentHeight;
            bool needScroll = totalContentHeight > h;
            int maxScroll = needScroll ? (totalContentHeight - h) : 0;
            m_scrollOffset = std::clamp(m_scrollOffset, 0, maxScroll);

            int scrollBarW = dp(7);
            int scrollBarMarginRight = dp(4);
            int scrollReserve = dp(17);
            int itemW = needScroll ? std::max(dp(160), w - scrollReserve) : w;

            for (int i = 0; i < totalItems; ++i) {
                int itemH = (i < static_cast<int>(m_itemHeights.size())) ? m_itemHeights[static_cast<size_t>(i)] : dp(205);
                int y = (i < static_cast<int>(m_itemYPositions.size())) ? (m_itemYPositions[static_cast<size_t>(i)] - m_scrollOffset) : (i * dp(215) - m_scrollOffset);
                if (y + itemH < 0 || y > h) continue;

                bool isSelected = (i == m_selectedIndex);
                bool isHovered = (i == m_hoveredIndex);
                draw_card_item(g, static_cast<size_t>(i), y, itemW, itemH, isSelected, isHovered);
            }

            if (needScroll) {
                float sbX = static_cast<float>(w - scrollBarMarginRight - scrollBarW);
                float sbY = dpf(4.0f);
                float sbH = std::max(dpf(20.0f), static_cast<float>(h) - dpf(8.0f));

                Gdiplus::SolidBrush trackBr(m_isDarkMode ? Gdiplus::Color(25, 255, 255, 255) : Gdiplus::Color(20, 0, 0, 0));
                Gdiplus::GraphicsPath trackPath;
                AddRoundedRect(trackPath, Gdiplus::RectF(sbX, sbY, static_cast<float>(scrollBarW), sbH), dpf(3.5f));
                g.FillPath(&trackBr, &trackPath);

                float thumbH = std::max(dpf(36.0f), (static_cast<float>(h) / static_cast<float>(totalContentHeight)) * sbH);
                thumbH = std::min(thumbH, sbH);
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

        int totalItems = static_cast<int>(m_filteredIndices.size());
        int totalContentHeight = m_totalContentHeight;
        bool needScroll = totalContentHeight > h;
        int maxScroll = needScroll ? (totalContentHeight - h) : 0;
        int scrollReserve = dp(17);

        if (m_isDraggingScroll) {
            if (needScroll) {
                float sbH = std::max(dpf(20.0f), static_cast<float>(h) - dpf(8.0f));
                float thumbH = std::min(sbH, std::max(dpf(36.0f), (static_cast<float>(h) / static_cast<float>(totalContentHeight)) * sbH));
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
            float thumbH = std::min(sbH, std::max(dpf(36.0f), (static_cast<float>(h) / static_cast<float>(totalContentHeight)) * sbH));
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
            int dummyY = 0;
            int idx = get_item_at_point(my, dummyY);
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

        int totalItems = static_cast<int>(m_filteredIndices.size());
        int totalContentHeight = m_totalContentHeight;
        bool needScroll = totalContentHeight > h;
        int maxScroll = needScroll ? (totalContentHeight - h) : 0;
        int scrollReserve = dp(17);

        if (needScroll && mx >= (w - scrollReserve)) {
            float sbY = dpf(4.0f);
            float sbH = std::max(dpf(20.0f), static_cast<float>(h) - dpf(8.0f));
            float thumbH = std::min(sbH, std::max(dpf(36.0f), (static_cast<float>(h) / static_cast<float>(totalContentHeight)) * sbH));
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

        int cardY = 0;
        int idx = get_item_at_point(my, cardY);
        if (idx >= 0 && idx < totalItems) {
            m_selectedIndex = idx;
            update_action_buttons_state();
            InvalidateRect(hWnd, nullptr, FALSE);

            const auto& entry = m_allEntries[m_filteredIndices[static_cast<size_t>(idx)]];
            if (has_valid_http_url(entry.website_url) && m_gpCardLink) {
                int itemW = needScroll ? std::max(dp(160), w - scrollReserve) : w;
                HDC hdc = GetDC(hWnd);
                if (hdc) {
                    Gdiplus::RectF linkRc;
                    {
                        Gdiplus::Graphics g(hdc);
                        g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
                        linkRc = get_website_pill_rect(g, itemW, cardY);
                    }
                    ReleaseDC(hWnd, hdc);
                    if (mx >= linkRc.X && mx <= linkRc.GetRight() &&
                        my >= linkRc.Y && my <= linkRc.GetBottom()) {
                        on_open_website(static_cast<size_t>(idx));
                    }
                }
            }
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
        RECT rc{};
        GetClientRect(hWnd, &rc);
        int w = rc.right - rc.left;
        int h = rc.bottom - rc.top;
        bool needScroll = m_totalContentHeight > h;
        int scrollReserve = dp(17);

        if (!needScroll || mx < w - scrollReserve) {
            int cardY = 0;
            int idx = get_item_at_point(my, cardY);
            if (idx >= 0 && idx < static_cast<int>(m_filteredIndices.size())) {
                const auto& entry = m_allEntries[m_filteredIndices[static_cast<size_t>(idx)]];
                if (has_valid_http_url(entry.website_url) && m_gpCardLink) {
                    int itemW = needScroll ? std::max(dp(160), w - scrollReserve) : w;
                    HDC hdc = GetDC(hWnd);
                    if (hdc) {
                        Gdiplus::RectF linkRc;
                        {
                            Gdiplus::Graphics g(hdc);
                            g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
                            linkRc = get_website_pill_rect(g, itemW, cardY);
                        }
                        ReleaseDC(hWnd, hdc);
                        if (mx >= linkRc.X && mx <= linkRc.GetRight() &&
                            my >= linkRc.Y && my <= linkRc.GetBottom()) {
                            return 0;
                        }
                    }
                }
                m_selectedIndex = idx;
                update_action_buttons_state();
                on_apply_selected();
            }
        }
        break;
    }

    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        if ((delta > 0 && m_wheelAccum < 0) || (delta < 0 && m_wheelAccum > 0)) {
            m_wheelAccum = 0;
        }
        int stepPx = std::max(1, dp(60));
        m_wheelAccum += delta * stepPx;
        int movePx = m_wheelAccum / WHEEL_DELTA;
        m_wheelAccum %= WHEEL_DELTA;
        if (movePx != 0) {
            RECT rc{};
            GetClientRect(hWnd, &rc);
            int maxScroll = std::max<int>(0, m_totalContentHeight - static_cast<int>(rc.bottom - rc.top));
            m_scrollOffset = std::clamp(m_scrollOffset - movePx, 0, maxScroll);
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_KEYDOWN: {
        int totalItems = static_cast<int>(m_filteredIndices.size());
        if (wParam == VK_UP) {
            if (totalItems <= 0) return 0;
            m_selectedIndex = std::clamp(m_selectedIndex - 1, 0, totalItems - 1);
            update_action_buttons_state();
            if (m_selectedIndex < static_cast<int>(m_itemYPositions.size())) {
                int targetY = m_itemYPositions[static_cast<size_t>(m_selectedIndex)];
                if (targetY < m_scrollOffset) m_scrollOffset = targetY;
            }
            InvalidateRect(hWnd, nullptr, FALSE);
            return 0;
        }
        if (wParam == VK_DOWN) {
            if (totalItems <= 0) return 0;
            m_selectedIndex = std::clamp(m_selectedIndex + 1, 0, totalItems - 1);
            update_action_buttons_state();
            RECT rc{};
            GetClientRect(hWnd, &rc);
            int clientH = rc.bottom - rc.top;
            if (m_selectedIndex < static_cast<int>(m_itemYPositions.size()) &&
                m_selectedIndex < static_cast<int>(m_itemHeights.size())) {
                int targetY = m_itemYPositions[static_cast<size_t>(m_selectedIndex)] +
                              m_itemHeights[static_cast<size_t>(m_selectedIndex)];
                if (targetY > m_scrollOffset + clientH) {
                    m_scrollOffset = targetY - clientH;
                }
            }
            InvalidateRect(hWnd, nullptr, FALSE);
            return 0;
        }
        if (wParam == VK_PRIOR) {
            RECT rc{};
            GetClientRect(hWnd, &rc);
            m_scrollOffset = std::max(0, m_scrollOffset - static_cast<int>(rc.bottom - rc.top));
            InvalidateRect(hWnd, nullptr, FALSE);
            return 0;
        }
        if (wParam == VK_NEXT) {
            RECT rc{};
            GetClientRect(hWnd, &rc);
            int maxScroll = std::max<int>(0, m_totalContentHeight - static_cast<int>(rc.bottom - rc.top));
            m_scrollOffset = std::min(maxScroll, m_scrollOffset + static_cast<int>(rc.bottom - rc.top));
            InvalidateRect(hWnd, nullptr, FALSE);
            return 0;
        }
        if (wParam == VK_RETURN) {
            on_apply_selected();
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
        bool needScroll = m_totalContentHeight > (rc.bottom - rc.top);
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
