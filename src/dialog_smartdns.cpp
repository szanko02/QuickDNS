#include "dialog_smartdns.h"
#include "encoding_utils.h"
#include "resource.h"
#include "ui_palette.h"
#include <dwmapi.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <algorithm>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shell32.lib")

using QuickDns::Encoding::wide_to_utf8;
using QuickDns::Encoding::utf8_to_wide;

namespace {
    constexpr const wchar_t* DIALOG_CLASS_NAME = L"QuickDNS_SmartDnsDialogClass";
    constexpr const wchar_t* LIST_CLASS_NAME   = L"QuickDNS_SmartDnsListClass";

    constexpr int DIALOG_CLIENT_WIDTH  = 740;
    constexpr int DIALOG_CLIENT_HEIGHT = 680;

    constexpr int CARD_ROW_HEIGHT = 205;
    constexpr int CARD_ROW_GAP    = 10;
    constexpr int CARD_ROW_STEP   = CARD_ROW_HEIGHT + CARD_ROW_GAP;

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

std::optional<std::string> SmartDnsDialog::show(
    HWND hParent,
    HINSTANCE hInstance,
    const std::vector<DnsProfile>& profiles,
    const std::string& currentActiveId,
    bool isDarkMode)
{
    SmartDnsDialog dlg(hParent, hInstance, profiles, currentActiveId, isDarkMode);
    if (!dlg.create_dialog_window()) {
        return std::nullopt;
    }

    EnableWindow(hParent, FALSE);

    MSG msg;
    while (!dlg.m_finished && GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessageW(dlg.m_hWnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    EnableWindow(hParent, TRUE);
    SetForegroundWindow(hParent);

    if (dlg.m_hWnd) {
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
    bool isDarkMode)
    : m_hParent(hParent)
    , m_hInstance(hInstance)
    , m_currentActiveId(currentActiveId)
    , m_isDarkMode(isDarkMode)
{
    auto pal = GetPalette(m_isDarkMode);
    m_hbrBg = CreateSolidBrush(pal.bg);
    m_hbrCard = CreateSolidBrush(pal.cardBg);

    populate_smartdns_data(profiles);
    setup_fonts();
}

SmartDnsDialog::~SmartDnsDialog() {
    if (m_hFontRegular) DeleteObject(m_hFontRegular);
    if (m_hFontBold) DeleteObject(m_hFontBold);
    if (m_hFontHeader) DeleteObject(m_hFontHeader);
    if (m_hFontMonospace) DeleteObject(m_hFontMonospace);
    if (m_hFontSmall) DeleteObject(m_hFontSmall);
    if (m_hFontSmallBold) DeleteObject(m_hFontSmallBold);

    if (m_hbrBg) DeleteObject(m_hbrBg);
    if (m_hbrCard) DeleteObject(m_hbrCard);
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
        e.primary_ipv4 = L"143.20.64.55";
        e.secondary_ipv4 = L"31.58.216.201";
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

    // 6. User-defined / Custom SmartDNS profiles
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

    // Select active profile if in catalog
    update_filtered_list();
    for (size_t i = 0; i < m_filteredIndices.size(); ++i) {
        if (m_allEntries[m_filteredIndices[i]].profile_id == m_currentActiveId) {
            m_selectedIndex = static_cast<int>(i);
            break;
        }
    }
}

void SmartDnsDialog::update_filtered_list() {
    m_filteredIndices.clear();

    std::wstring query = m_searchQuery;
    std::transform(query.begin(), query.end(), query.begin(), ::towlower);

    for (size_t i = 0; i < m_allEntries.size(); ++i) {
        const auto& e = m_allEntries[i];
        if (query.empty()) {
            m_filteredIndices.push_back(i);
            continue;
        }

        // Search in Name
        std::wstring nameLower = e.name;
        std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::towlower);
        if (nameLower.find(query) != std::wstring::npos) {
            m_filteredIndices.push_back(i);
            continue;
        }

        // Search in Description
        std::wstring descLower = e.description;
        std::transform(descLower.begin(), descLower.end(), descLower.begin(), ::towlower);
        if (descLower.find(query) != std::wstring::npos) {
            m_filteredIndices.push_back(i);
            continue;
        }

        // Search in Services
        bool foundInServices = false;
        for (const auto& cat : e.categories) {
            for (const auto& s : cat.services) {
                std::wstring sLower = s;
                std::transform(sLower.begin(), sLower.end(), sLower.begin(), ::towlower);
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

    if (m_selectedIndex >= static_cast<int>(m_filteredIndices.size())) {
        m_selectedIndex = m_filteredIndices.empty() ? -1 : 0;
    }

    m_statusText = L"Найдено SmartDNS серверов: " + std::to_wstring(m_filteredIndices.size()) + L" из " + std::to_wstring(m_allEntries.size());
    if (!query.empty()) {
        m_statusText += L" (по запросу: \"" + m_searchQuery + L"\")";
    } else {
        m_statusText += L" • Нажмите на карточку для выбора";
    }

    if (m_hWnd) {
        RECT rcStatus = { 20, 105, DIALOG_CLIENT_WIDTH - 20, 130 };
        InvalidateRect(m_hWnd, &rcStatus, FALSE);
    }
}

void SmartDnsDialog::setup_fonts() {
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

bool SmartDnsDialog::create_dialog_window() {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &SmartDnsDialog::dialog_proc_thunk;
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
    lwc.lpfnWndProc = &SmartDnsDialog::list_proc_thunk;
    lwc.hInstance = m_hInstance;
    lwc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
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

    apply_theme();
    create_controls();
    return true;
}

void SmartDnsDialog::apply_theme() {
    BOOL useDarkMode = m_isDarkMode ? TRUE : FALSE;
    DwmSetWindowAttribute(m_hWnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &useDarkMode, sizeof(useDarkMode));
    DWORD cornerPref = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(m_hWnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &cornerPref, sizeof(cornerPref));
}

LRESULT CALLBACK SmartDnsDialog::search_edit_subclass(
    HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
    UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
    auto* pDlg = reinterpret_cast<SmartDnsDialog*>(dwRefData);
    if (!pDlg) return DefSubclassProc(hWnd, uMsg, wParam, lParam);

    switch (uMsg) {
    case WM_SETFOCUS: {
        pDlg->m_searchEditFocused = true;
        RECT rc = { 20 + 115 - 2, 70 - 2, DIALOG_CLIENT_WIDTH - 20 + 2, 70 + 32 + 2 };
        InvalidateRect(pDlg->m_hWnd, &rc, FALSE);
        break;
    }
    case WM_KILLFOCUS: {
        pDlg->m_searchEditFocused = false;
        RECT rc = { 20 + 115 - 2, 70 - 2, DIALOG_CLIENT_WIDTH - 20 + 2, 70 + 32 + 2 };
        InvalidateRect(pDlg->m_hWnd, &rc, FALSE);
        break;
    }
    case WM_MOUSEMOVE: {
        if (!pDlg->m_searchEditHovered) {
            pDlg->m_searchEditHovered = true;
            TRACKMOUSEEVENT tme = { sizeof(TRACKMOUSEEVENT), TME_LEAVE, hWnd, 0 };
            TrackMouseEvent(&tme);
            RECT rc = { 20 + 115 - 2, 70 - 2, DIALOG_CLIENT_WIDTH - 20 + 2, 70 + 32 + 2 };
            InvalidateRect(pDlg->m_hWnd, &rc, FALSE);
        }
        break;
    }
    case WM_MOUSELEAVE: {
        pDlg->m_searchEditHovered = false;
        RECT rc = { 20 + 115 - 2, 70 - 2, DIALOG_CLIENT_WIDTH - 20 + 2, 70 + 32 + 2 };
        InvalidateRect(pDlg->m_hWnd, &rc, FALSE);
        break;
    }
    case WM_CHAR: {
        if (wParam == VK_RETURN) {
            return 0; // Prevent beep
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

    int marginX = 20;
    int contentWidth = clientWidth - (marginX * 2);

    // Modern Search Edit Control inset inside the GDI+ rounded container card
    int searchBoxX = marginX + 115;
    int searchBoxY = 70;
    int searchBoxW = contentWidth - 115;
    int searchBoxH = 32;

    // Create EDIT control WITHOUT WS_EX_CLIENTEDGE (use 0) so no harsh 3D borders appear!
    m_hSearchEdit = CreateWindowExW(0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        searchBoxX + 10, searchBoxY + 6, searchBoxW - 20, 20,
        m_hWnd, (HMENU)IDC_EDIT_SMARTDNS_SEARCH, m_hInstance, nullptr);
    SendMessageW(m_hSearchEdit, WM_SETFONT, (WPARAM)m_hFontRegular, TRUE);
    SendMessageW(m_hSearchEdit, EM_SETCUEBANNER, TRUE, (LPARAM)L"Введите название (например: ChatGPT, Spotify, Discord, Canva, Notion, Claude)...");
    SetWindowSubclass(m_hSearchEdit, search_edit_subclass, 501, reinterpret_cast<DWORD_PTR>(this));

    // Custom Scrollable List of SmartDNS Cards
    int listTop = 132;
    int bottomButtonsH = 38;
    int listBottomMargin = 16;
    int listHeight = clientHeight - listTop - bottomButtonsH - listBottomMargin - 16;

    m_hListWnd = CreateWindowExW(
        0,
        LIST_CLASS_NAME,
        L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        marginX, listTop, contentWidth, listHeight,
        m_hWnd, (HMENU)IDC_LIST_SMARTDNS, m_hInstance, this
    );

    int curY = listTop + listHeight + 16;

    // Bottom Action Buttons
    int btnApplyW = 210;
    int btnWebW = 180;
    int btnCloseW = 110;

    m_hBtnApply = CreateWindowExW(0, WC_BUTTONW, L"Применить этот DNS",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        marginX, curY, btnApplyW, bottomButtonsH,
        m_hWnd, (HMENU)IDC_BTN_SMARTDNS_APPLY, m_hInstance, nullptr);

    m_hBtnWebsite = CreateWindowExW(0, WC_BUTTONW, L"Официальный сайт",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        marginX + btnApplyW + 12, curY, btnWebW, bottomButtonsH,
        m_hWnd, (HMENU)IDC_BTN_SMARTDNS_WEBSITE, m_hInstance, nullptr);

    m_hBtnClose = CreateWindowExW(0, WC_BUTTONW, L"Закрыть",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        marginX + contentWidth - btnCloseW, curY, btnCloseW, bottomButtonsH,
        m_hWnd, (HMENU)IDC_BTN_SMARTDNS_CLOSE, m_hInstance, nullptr);

    attach_button_effects(m_hBtnApply, 301);
    attach_button_effects(m_hBtnWebsite, 302);
    attach_button_effects(m_hBtnClose, 303);
}

void SmartDnsDialog::on_apply_selected() {
    if (m_selectedIndex >= 0 && m_selectedIndex < static_cast<int>(m_filteredIndices.size())) {
        size_t realIdx = m_filteredIndices[m_selectedIndex];
        m_selectedProfileId = m_allEntries[realIdx].profile_id;
        m_finished = true;
    }
}

void SmartDnsDialog::on_open_website(size_t index) {
    if (index < m_filteredIndices.size()) {
        size_t realIdx = m_filteredIndices[index];
        const auto& entry = m_allEntries[realIdx];
        if (!entry.website_url.empty()) {
            ShellExecuteW(m_hWnd, L"open", entry.website_url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    }
}

void SmartDnsDialog::on_close() {
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

    auto pal = GetPalette(m_isDarkMode);
    bool isPrimary = (pDis->CtlID == IDC_BTN_SMARTDNS_APPLY);

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

void SmartDnsDialog::draw_card_item(Gdiplus::Graphics& g, size_t index, int y, int width, int height, bool isSelected, bool isHovered) {
    if (index >= m_filteredIndices.size()) return;
    size_t realIdx = m_filteredIndices[index];
    const auto& e = m_allEntries[realIdx];
    auto pal = GetPalette(m_isDarkMode);

    // 1. Card container background
    Gdiplus::RectF rcCard(0.5f, static_cast<float>(y) + 0.5f, static_cast<float>(width) - 1.0f, static_cast<float>(height) - 1.0f);
    Gdiplus::GraphicsPath cardPath;
    AddRoundedRect(cardPath, rcCard, 8.0f);

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
        AddRoundedRect(selBarPath, Gdiplus::RectF(2.0f, static_cast<float>(y) + 8.0f, 3.5f, static_cast<float>(height) - 16.0f), 2.0f);
        COLORREF barColor = e.is_active ? RGB(34, 197, 94) : (m_isDarkMode ? RGB(96, 165, 250) : RGB(37, 99, 235));
        Gdiplus::SolidBrush selBarBr(Gdiplus::Color(255, GetRValue(barColor), GetGValue(barColor), GetBValue(barColor)));
        g.FillPath(&selBarBr, &selBarPath);
    }

    // 2. Header Row: Name + Badges
    Gdiplus::FontFamily fontFamily(L"Segoe UI");
    Gdiplus::Font fontTitle(&fontFamily, 11.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);
    Gdiplus::Font fontBadge(&fontFamily, 8.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);
    Gdiplus::Font fontSub(&fontFamily, 8.5f, Gdiplus::FontStyleRegular, Gdiplus::UnitPoint);
    Gdiplus::Font fontCatHeader(&fontFamily, 8.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);
    Gdiplus::Font fontTag(&fontFamily, 7.5f, Gdiplus::FontStyleRegular, Gdiplus::UnitPoint);

    Gdiplus::SolidBrush textPriBr(Gdiplus::Color(255, GetRValue(pal.textPrimary), GetGValue(pal.textPrimary), GetBValue(pal.textPrimary)));
    Gdiplus::SolidBrush textSecBr(Gdiplus::Color(255, GetRValue(pal.textSecondary), GetGValue(pal.textSecondary), GetBValue(pal.textSecondary)));
    Gdiplus::SolidBrush textMutedBr(Gdiplus::Color(255, GetRValue(pal.textMuted), GetGValue(pal.textMuted), GetBValue(pal.textMuted)));

    float curCardY = static_cast<float>(y) + 8.0f;
    float curCardX = 16.0f;

    // Provider Title
    g.DrawString(e.name.c_str(), -1, &fontTitle, Gdiplus::PointF(curCardX, curCardY), &textPriBr);

    Gdiplus::RectF nameBounds;
    g.MeasureString(e.name.c_str(), -1, &fontTitle, Gdiplus::PointF(0, 0), &nameBounds);
    float badgeX = curCardX + nameBounds.Width + 10.0f;

    // [SmartDNS] pill tag
    {
        Gdiplus::RectF pillRc(badgeX, curCardY + 2.0f, 68.0f, 18.0f);
        Gdiplus::GraphicsPath pillPath;
        AddRoundedRect(pillPath, pillRc, 4.0f);
        Gdiplus::SolidBrush pillBg(Gdiplus::Color(35, 34, 197, 94));
        Gdiplus::Pen pillPen(Gdiplus::Color(120, 34, 197, 94), 1.0f);
        g.FillPath(&pillBg, &pillPath);
        g.DrawPath(&pillPen, &pillPath);

        Gdiplus::SolidBrush pillText(Gdiplus::Color(255, 34, 197, 94));
        g.DrawString(L"SmartDNS", -1, &fontBadge, Gdiplus::PointF(badgeX + 6.0f, curCardY + 4.0f), &pillText);
        badgeX += 76.0f;
    }

    // Location badge
    if (!e.location.empty()) {
        Gdiplus::RectF locBounds;
        g.MeasureString(e.location.c_str(), -1, &fontBadge, Gdiplus::PointF(0, 0), &locBounds);
        float locW = locBounds.Width + 12.0f;
        Gdiplus::RectF locRc(badgeX, curCardY + 2.0f, locW, 18.0f);
        Gdiplus::GraphicsPath locPath;
        AddRoundedRect(locPath, locRc, 4.0f);

        Gdiplus::SolidBrush locBg(m_isDarkMode ? Gdiplus::Color(30, 255, 255, 255) : Gdiplus::Color(20, 0, 0, 0));
        Gdiplus::Pen locPen(m_isDarkMode ? Gdiplus::Color(60, 255, 255, 255) : Gdiplus::Color(40, 0, 0, 0), 1.0f);
        g.FillPath(&locBg, &locPath);
        g.DrawPath(&locPen, &locPath);

        g.DrawString(e.location.c_str(), -1, &fontBadge, Gdiplus::PointF(badgeX + 6.0f, curCardY + 4.0f), &textSecBr);
        badgeX += locW + 8.0f;
    }

    // Active badge [Активен]
    if (e.is_active) {
        Gdiplus::SolidBrush activeBadgeBr(Gdiplus::Color(255, 34, 197, 94));
        g.DrawString(L"[Текущий DNS в системе]", -1, &fontBadge, Gdiplus::PointF(badgeX, curCardY + 4.0f), &activeBadgeBr);
    }

    // Right side: Website Link indicator
    float rightLinkX = static_cast<float>(width) - 180.0f;
    Gdiplus::Font fontLink(&fontFamily, 8.5f, Gdiplus::FontStyleUnderline, Gdiplus::UnitPoint);
    Gdiplus::SolidBrush linkBr(Gdiplus::Color(255, GetRValue(pal.accentBlue), GetGValue(pal.accentBlue), GetBValue(pal.accentBlue)));
    g.DrawString(L"Открыть сайт провайдера ->", -1, &fontLink, Gdiplus::PointF(rightLinkX, curCardY + 3.0f), &linkBr);

    curCardY += 24.0f;

    // 3. Addresses: IPv4, DoH, Website
    std::wstring addrStr = L"IPv4: " + e.primary_ipv4;
    if (!e.secondary_ipv4.empty()) addrStr += L", " + e.secondary_ipv4;
    if (!e.doh_template.empty()) addrStr += L"   |   DoH: " + e.doh_template;
    g.DrawString(addrStr.c_str(), -1, &fontSub, Gdiplus::PointF(curCardX, curCardY), &textSecBr);
    curCardY += 19.0f;

    // Description line
    g.DrawString(e.description.c_str(), -1, &fontSub, Gdiplus::PointF(curCardX, curCardY), &textMutedBr);
    curCardY += 22.0f;

    // Separator line inside card
    Gdiplus::Pen sepPen(Gdiplus::Color(70, GetRValue(pal.separator), GetGValue(pal.separator), GetBValue(pal.separator)), 1.0f);
    g.DrawLine(&sepPen, curCardX, curCardY, static_cast<float>(width) - 16.0f, curCardY);
    curCardY += 8.0f;

    // 4. Categorized Service Tags
    std::wstring searchLower = m_searchQuery;
    std::transform(searchLower.begin(), searchLower.end(), searchLower.begin(), ::towlower);

    float maxRowW = static_cast<float>(width) - 24.0f;

    for (size_t c = 0; c < e.categories.size(); ++c) {
        if (curCardY + 20.0f > static_cast<float>(y + height)) break;

        const auto& cat = e.categories[c];
        float catX = curCardX;

        // Category title (e.g. "Нейросети и AI:")
        std::wstring catTitle = cat.name + L":";
        g.DrawString(catTitle.c_str(), -1, &fontCatHeader, Gdiplus::PointF(catX, curCardY + 2.0f), &textPriBr);

        Gdiplus::RectF catTitleBounds;
        g.MeasureString(catTitle.c_str(), -1, &fontCatHeader, Gdiplus::PointF(0, 0), &catTitleBounds);
        catX += catTitleBounds.Width + 8.0f;

        // Render chips
        for (const auto& svc : cat.services) {
            Gdiplus::RectF svcBounds;
            g.MeasureString(svc.c_str(), -1, &fontTag, Gdiplus::PointF(0, 0), &svcBounds);
            float chipW = svcBounds.Width + 10.0f;

            if (catX + chipW > maxRowW) {
                // If overflowed line, show ellipsis tag and break category row
                g.DrawString(L"...", -1, &fontTag, Gdiplus::PointF(catX, curCardY + 2.0f), &textMutedBr);
                break;
            }

            // Check if matches search
            bool isMatched = false;
            if (!searchLower.empty()) {
                std::wstring sLower = svc;
                std::transform(sLower.begin(), sLower.end(), sLower.begin(), ::towlower);
                if (sLower.find(searchLower) != std::wstring::npos) {
                    isMatched = true;
                }
            }

            Gdiplus::RectF chipRc(catX, curCardY + 1.0f, chipW, 16.0f);
            Gdiplus::GraphicsPath chipPath;
            AddRoundedRect(chipPath, chipRc, 3.5f);

            Gdiplus::Color chipBg = isMatched 
                ? Gdiplus::Color(60, 34, 197, 94)
                : (m_isDarkMode ? Gdiplus::Color(25, 255, 255, 255) : Gdiplus::Color(18, 0, 0, 0));
            Gdiplus::Color chipBorder = isMatched
                ? Gdiplus::Color(160, 34, 197, 94)
                : (m_isDarkMode ? Gdiplus::Color(45, 255, 255, 255) : Gdiplus::Color(35, 0, 0, 0));
            Gdiplus::Color chipTextColor = isMatched
                ? Gdiplus::Color(255, 34, 197, 94)
                : Gdiplus::Color(255, GetRValue(pal.textSecondary), GetGValue(pal.textSecondary), GetBValue(pal.textSecondary));

            Gdiplus::SolidBrush chipBgBr(chipBg);
            Gdiplus::Pen chipPen(chipBorder, 1.0f);
            g.FillPath(&chipBgBr, &chipPath);
            g.DrawPath(&chipPen, &chipPath);

            Gdiplus::SolidBrush chipTextBr(chipTextColor);
            g.DrawString(svc.c_str(), -1, &fontTag, Gdiplus::PointF(catX + 5.0f, curCardY + 2.5f), &chipTextBr);

            catX += chipW + 5.0f;
        }

        curCardY += 21.0f;
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
                update_filtered_list();
                if (m_hListWnd) InvalidateRect(m_hListWnd, nullptr, FALSE);
            }
            return 0;
        }

        if (id == IDC_BTN_SMARTDNS_APPLY) {
            on_apply_selected();
            return 0;
        }
        if (id == IDC_BTN_SMARTDNS_WEBSITE) {
            on_open_website(static_cast<size_t>(std::max(0, m_selectedIndex)));
            return 0;
        }
        if (id == IDC_BTN_SMARTDNS_CLOSE || id == IDCANCEL) {
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

            Gdiplus::FontFamily ffDisplay(L"Segoe UI Variable Display");
            Gdiplus::FontFamily ffRegular(L"Segoe UI");
            Gdiplus::Font fontTitle(&ffDisplay, 13.5f, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);
            Gdiplus::Font fontSub(&ffRegular, 9.0f, Gdiplus::FontStyleRegular, Gdiplus::UnitPoint);
            Gdiplus::Font fontLbl(&ffRegular, 9.5f, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);
            Gdiplus::Font fontStatus(&ffRegular, 8.5f, Gdiplus::FontStyleRegular, Gdiplus::UnitPoint);

            Gdiplus::SolidBrush brPri(Gdiplus::Color(255, GetRValue(pal.textPrimary), GetGValue(pal.textPrimary), GetBValue(pal.textPrimary)));
            Gdiplus::SolidBrush brSec(Gdiplus::Color(255, GetRValue(pal.textSecondary), GetGValue(pal.textSecondary), GetBValue(pal.textSecondary)));
            Gdiplus::SolidBrush brMuted(Gdiplus::Color(255, GetRValue(pal.textMuted), GetGValue(pal.textMuted), GetBValue(pal.textMuted)));

            // 1. Header Title & Subtitle
            g.DrawString(L"Каталог SmartDNS и разблокируемые сервисы", -1, &fontTitle, Gdiplus::PointF(20.0f, 14.0f), &brPri);
            g.DrawString(L"Серверы с автоматическим проксированием для доступа к зарубежным нейросетям, музыке и сайтам без VPN.", -1, &fontSub, Gdiplus::PointF(20.0f, 42.0f), &brSec);

            // 2. Search label
            g.DrawString(L"Поиск сервиса:", -1, &fontLbl, Gdiplus::PointF(20.0f, 76.0f), &brPri);

            // 3. Search edit container card
            int searchBoxX = 20 + 115;
            int searchBoxY = 70;
            int searchBoxW = (w - 40) - 115;
            int searchBoxH = 32;

            Gdiplus::GraphicsPath sbPath;
            AddRoundedRect(sbPath, Gdiplus::RectF(static_cast<float>(searchBoxX) + 0.5f, static_cast<float>(searchBoxY) + 0.5f, static_cast<float>(searchBoxW) - 1.0f, static_cast<float>(searchBoxH) - 1.0f), 6.0f);
            Gdiplus::SolidBrush sbBg(Gdiplus::Color(255, GetRValue(pal.cardBg), GetGValue(pal.cardBg), GetBValue(pal.cardBg)));
            g.FillPath(&sbBg, &sbPath);

            bool isSearchActive = (m_searchEditFocused || m_searchEditHovered);
            COLORREF sbBorderCol = isSearchActive 
                ? (m_isDarkMode ? RGB(96, 165, 250) : RGB(37, 99, 235))
                : pal.cardBorder;
            float sbBorderWidth = isSearchActive ? 1.5f : 1.0f;
            Gdiplus::Pen sbBorderPen(Gdiplus::Color(255, GetRValue(sbBorderCol), GetGValue(sbBorderCol), GetBValue(sbBorderCol)), sbBorderWidth);
            g.DrawPath(&sbBorderPen, &sbPath);

            // 4. Status text
            g.DrawString(m_statusText.c_str(), -1, &fontStatus, Gdiplus::PointF(20.0f, 108.0f), &brMuted);
        }

        BitBlt(hdc, 0, 0, w, h, hdcMem, 0, 0, SRCCOPY);
        SelectObject(hdcMem, oldBmp);
        DeleteObject(hbmMem);
        DeleteDC(hdcMem);
        EndPaint(hWnd, &ps);
        return 0;
    }

    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wParam;
        auto pal = GetPalette(m_isDarkMode);
        SetTextColor(hdc, pal.textPrimary);
        SetBkMode(hdc, TRANSPARENT);
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

    case WM_CLOSE: {
        on_close();
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

            int totalItems = static_cast<int>(m_filteredIndices.size());
            int totalContentHeight = totalItems * CARD_ROW_STEP;
            bool needScroll = totalContentHeight > h;
            int maxScroll = needScroll ? (totalContentHeight - h) : 0;
            m_scrollOffset = std::clamp(m_scrollOffset, 0, maxScroll);

            int scrollBarW = 7;
            int scrollBarMarginRight = 4;
            int itemW = needScroll ? (w - scrollBarW - scrollBarMarginRight - 6) : w;

            for (int i = 0; i < totalItems; ++i) {
                int y = i * CARD_ROW_STEP - m_scrollOffset;
                if (y + CARD_ROW_HEIGHT < 0 || y > h) continue;

                bool isSelected = (i == m_selectedIndex);
                bool isHovered = (i == m_hoveredIndex);
                draw_card_item(g, static_cast<size_t>(i), y, itemW, CARD_ROW_HEIGHT, isSelected, isHovered);
            }

            // Anti-aliased themed scrollbar
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
                float thumbH = std::max(36.0f, (static_cast<float>(h) / static_cast<float>(totalContentHeight)) * sbH);
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

        int totalItems = static_cast<int>(m_filteredIndices.size());
        int totalContentHeight = totalItems * CARD_ROW_STEP;
        bool needScroll = totalContentHeight > h;
        int maxScroll = needScroll ? (totalContentHeight - h) : 0;

        if (m_isDraggingScroll) {
            if (needScroll) {
                float sbY = 4.0f;
                float sbH = static_cast<float>(h - 8);
                float thumbH = std::max(36.0f, (static_cast<float>(h) / static_cast<float>(totalContentHeight)) * sbH);
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
            float thumbH = std::max(36.0f, (static_cast<float>(h) / static_cast<float>(totalContentHeight)) * sbH);
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
            int idx = contentY / CARD_ROW_STEP;
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

        int totalItems = static_cast<int>(m_filteredIndices.size());
        int totalContentHeight = totalItems * CARD_ROW_STEP;
        bool needScroll = totalContentHeight > h;
        int maxScroll = needScroll ? (totalContentHeight - h) : 0;

        if (needScroll && mx >= (w - 17)) {
            float sbY = 4.0f;
            float sbH = static_cast<float>(h - 8);
            float thumbH = std::max(36.0f, (static_cast<float>(h) / static_cast<float>(totalContentHeight)) * sbH);
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
        int idx = contentY / CARD_ROW_STEP;
        if (idx >= 0 && idx < totalItems) {
            m_selectedIndex = idx;
            InvalidateRect(hWnd, nullptr, FALSE);

            // Check if clicked the website link area in top right of card
            int cardY = idx * CARD_ROW_STEP - m_scrollOffset;
            int rightLinkX = w - 180;
            if (mx >= rightLinkX && my >= cardY + 5 && my <= cardY + 30) {
                on_open_website(static_cast<size_t>(idx));
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
        RECT rc;
        GetClientRect(hWnd, &rc);
        int w = rc.right - rc.left;
        if (mx < w - 17) {
            int contentY = my + m_scrollOffset;
            int idx = contentY / CARD_ROW_STEP;
            if (idx >= 0 && idx < static_cast<int>(m_filteredIndices.size())) {
                m_selectedIndex = idx;
                on_apply_selected();
            }
        }
        break;
    }

    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        int step = CARD_ROW_STEP;
        RECT rc;
        GetClientRect(hWnd, &rc);
        int totalH = static_cast<int>(m_filteredIndices.size()) * CARD_ROW_STEP;
        int maxScroll = std::max<int>(0, totalH - static_cast<int>(rc.bottom - rc.top));
        m_scrollOffset -= (delta / WHEEL_DELTA) * step;
        m_scrollOffset = std::clamp(m_scrollOffset, 0, maxScroll);
        InvalidateRect(hWnd, nullptr, FALSE);
        return 0;
    }

    case WM_KEYDOWN: {
        int totalItems = static_cast<int>(m_filteredIndices.size());
        if (wParam == VK_UP) {
            m_selectedIndex = std::max(0, m_selectedIndex - 1);
            int targetY = m_selectedIndex * CARD_ROW_STEP;
            if (targetY < m_scrollOffset) m_scrollOffset = targetY;
            InvalidateRect(hWnd, nullptr, FALSE);
            return 0;
        }
        if (wParam == VK_DOWN) {
            m_selectedIndex = std::min(totalItems - 1, m_selectedIndex + 1);
            RECT rc; GetClientRect(hWnd, &rc);
            int targetY = (m_selectedIndex + 1) * CARD_ROW_STEP;
            if (targetY > m_scrollOffset + (rc.bottom - rc.top)) {
                m_scrollOffset = targetY - (rc.bottom - rc.top);
            }
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
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(hWnd, &pt);
        RECT rc;
        GetClientRect(hWnd, &rc);
        int totalItems = static_cast<int>(m_filteredIndices.size());
        bool needScroll = (totalItems * CARD_ROW_STEP) > (rc.bottom - rc.top);
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
