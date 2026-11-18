#include "dns_manager.h"
#include "dialog_add_profile.h"
#include "encoding_utils.h"
#include "ui_palette.h"
#include "resource.h"
#include <dwmapi.h>
#include <windowsx.h>
#include <commctrl.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cwctype>

#ifdef _MSC_VER
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdiplus.lib")
#endif

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

using QuickDns::Encoding::wide_to_utf8;

namespace {
    constexpr const wchar_t* DIALOG_CLASS_NAME = L"QuickDNS_AddProfileDialogClass";
    constexpr int DIALOG_WIDTH = 580;
    constexpr int DIALOG_HEIGHT = 660;

    constexpr int ID_EDIT_DOH       = 1107;
    constexpr int ID_CHECK_SMARTDNS = 1108;

    constexpr const wchar_t* SMARTDNS_CHECK_LABEL = L"Профиль SmartDNS (разблокировка сервисов и медиа)";
    constexpr const wchar_t* SMARTDNS_SUB_TEXT = L"Отметьте, если сервер предоставляет функцию обхода блокировок и стриминга";

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

    struct CheckboxAnimState {
        AddProfileDialog* pDlg = nullptr;
        bool isHovered = false;
        bool isPressed = false;
    };

    static LRESULT CALLBACK CustomCheckboxSubclassProc(
        HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
        UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
    {
        auto* state = reinterpret_cast<CheckboxAnimState*>(dwRefData);
        if (!state || !state->pDlg) return DefSubclassProc(hWnd, uMsg, wParam, lParam);

        switch (uMsg) {
        case WM_GETDLGCODE:
            return DLGC_BUTTON;

        case BM_GETCHECK:
            return state->pDlg->is_smart_dns() ? BST_CHECKED : BST_UNCHECKED;

        case BM_SETCHECK:
            state->pDlg->set_smart_dns(wParam == BST_CHECKED);
            return 0;

        case BM_CLICK:
            if (IsWindowEnabled(hWnd)) {
                state->pDlg->toggle_smart_dns();
            }
            return 0;

        case WM_SETCURSOR: {
            if (LOWORD(lParam) == HTCLIENT && IsWindowEnabled(hWnd)) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            break;
        }
        case WM_SETFOCUS:
        case WM_KILLFOCUS: {
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        }
        case WM_MOUSEMOVE: {
            if (!state->isHovered) {
                state->isHovered = true;
                TRACKMOUSEEVENT tme{};
                tme.cbSize = sizeof(TRACKMOUSEEVENT);
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = hWnd;
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
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK: {
            if (!IsWindowEnabled(hWnd)) return 0;
            SetFocus(hWnd);
            state->isPressed = true;
            SetCapture(hWnd);
            InvalidateRect(hWnd, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONUP: {
            if (state->isPressed) {
                state->isPressed = false;
                if (GetCapture() == hWnd) {
                    ReleaseCapture();
                }
                RECT rc{};
                GetClientRect(hWnd, &rc);
                POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                if (PtInRect(&rc, pt) && IsWindowEnabled(hWnd)) {
                    state->pDlg->toggle_smart_dns();
                } else {
                    InvalidateRect(hWnd, nullptr, FALSE);
                }
            }
            return 0;
        }
        case WM_CAPTURECHANGED: {
            if (state->isPressed && reinterpret_cast<HWND>(lParam) != hWnd) {
                state->isPressed = false;
                InvalidateRect(hWnd, nullptr, FALSE);
            }
            break;
        }
        case WM_KEYDOWN: {
            if (wParam == VK_SPACE && IsWindowEnabled(hWnd)) {
                state->isPressed = true;
                InvalidateRect(hWnd, nullptr, FALSE);
                return 0;
            }
            break;
        }
        case WM_KEYUP: {
            if (wParam == VK_SPACE && IsWindowEnabled(hWnd)) {
                state->isPressed = false;
                state->pDlg->toggle_smart_dns();
                return 0;
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

            bool isDark = state->pDlg->is_dark_mode();
            auto pal = quickdns::GetPalette(isDark);

            HBRUSH hbrCard = CreateSolidBrush(pal.cardBg);
            FillRect(hdcMem, &rc, hbrCard);
            DeleteObject(hbrCard);

            bool isChecked = state->pDlg->is_smart_dns();
            bool hasFocus = (GetFocus() == hWnd);

            {
                Gdiplus::Graphics g(hdcMem);
                g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

                int boxSize = std::clamp(state->pDlg->dp(18), 14, std::max(14, h - 2));
                int boxX = state->pDlg->dp(2);
                int boxY = (h - boxSize) / 2;
                float cornerR = state->pDlg->dpf(4.0f);
                Gdiplus::RectF boxRc(
                    static_cast<float>(boxX) + 0.5f,
                    static_cast<float>(boxY) + 0.5f,
                    static_cast<float>(boxSize) - 1.0f,
                    static_cast<float>(boxSize) - 1.0f
                );
                Gdiplus::GraphicsPath boxPath;
                quickdns::AddRoundedRect(boxPath, boxRc, cornerR);

                if (isChecked) {
                    Gdiplus::SolidBrush checkBgBr(Gdiplus::Color(255, 34, 197, 94));
                    g.FillPath(&checkBgBr, &boxPath);

                    float s = static_cast<float>(boxSize) / 18.0f;
                    Gdiplus::Pen checkPen(Gdiplus::Color(255, 255, 255, 255), std::max(1.8f, 2.2f * s));
                    checkPen.SetStartCap(Gdiplus::LineCapRound);
                    checkPen.SetEndCap(Gdiplus::LineCapRound);
                    checkPen.SetLineJoin(Gdiplus::LineJoinRound);
                    Gdiplus::PointF pts[3] = {
                        Gdiplus::PointF(boxX + 4.5f * s, boxY + 9.5f * s),
                        Gdiplus::PointF(boxX + 7.5f * s, boxY + 13.0f * s),
                        Gdiplus::PointF(boxX + 13.5f * s, boxY + 5.5f * s)
                    };
                    g.DrawLines(&checkPen, pts, 3);
                } else {
                    Gdiplus::SolidBrush uncheckBgBr(Gdiplus::Color(255, GetRValue(pal.bg), GetGValue(pal.bg), GetBValue(pal.bg)));
                    g.FillPath(&uncheckBgBr, &boxPath);
                    COLORREF borderCol = (state->isHovered || hasFocus) ? pal.accentBlue : pal.cardBorder;
                    Gdiplus::Pen uncheckPen(Gdiplus::Color(255, GetRValue(borderCol), GetGValue(borderCol), GetBValue(borderCol)), state->pDlg->dpf(1.5f));
                    g.DrawPath(&uncheckPen, &boxPath);
                }

                if (hasFocus) {
                    Gdiplus::RectF focusRc(0.5f, 0.5f, static_cast<float>(w) - 1.0f, static_cast<float>(h) - 1.0f);
                    Gdiplus::GraphicsPath focusPath;
                    quickdns::AddRoundedRect(focusPath, focusRc, state->pDlg->dpf(4.0f));
                    Gdiplus::Pen focusPen(Gdiplus::Color(110, GetRValue(pal.accentBlue), GetGValue(pal.accentBlue), GetBValue(pal.accentBlue)), 1.0f);
                    focusPen.SetDashStyle(Gdiplus::DashStyleDot);
                    g.DrawPath(&focusPen, &focusPath);
                }
            }

            int textLeft = state->pDlg->dp(28);
            RECT rcText = { textLeft, 0, w - state->pDlg->dp(4), h };
            SetBkMode(hdcMem, TRANSPARENT);
            SetTextColor(hdcMem, pal.textPrimary);
            HGDIOBJ oldFont = SelectObject(hdcMem, state->pDlg->get_font_bold());
            DrawTextW(hdcMem, SMARTDNS_CHECK_LABEL, -1, &rcText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            SelectObject(hdcMem, oldFont);

            BitBlt(hdc, 0, 0, w, h, hdcMem, 0, 0, SRCCOPY);
            SelectObject(hdcMem, oldBmp);
            DeleteObject(hbmMem);
            DeleteDC(hdcMem);
            EndPaint(hWnd, &ps);
            return 0;
        }
        case WM_NCDESTROY: {
            RemovePropW(hWnd, L"QuickDnsCheckAnim");
            delete state;
            RemoveWindowSubclass(hWnd, CustomCheckboxSubclassProc, uIdSubclass);
            return 0;
        }
        }
        return DefSubclassProc(hWnd, uMsg, wParam, lParam);
    }
}

int AddProfileDialog::dp(int px) const {
    float s = (m_activeFontScale > 0.0f) ? m_activeFontScale : (static_cast<float>(m_dpi) / 96.0f);
    return static_cast<int>(std::lround(static_cast<float>(px) * s));
}

float AddProfileDialog::dpf(float px) const {
    float s = (m_activeFontScale > 0.0f) ? m_activeFontScale : (static_cast<float>(m_dpi) / 96.0f);
    return px * s;
}

void AddProfileDialog::set_smart_dns(bool enabled) {
    if (m_isSmartDns != enabled) {
        m_isSmartDns = enabled;
        if (m_hCheckSmartDns && IsWindow(m_hCheckSmartDns)) {
            InvalidateRect(m_hCheckSmartDns, nullptr, FALSE);
        }
        if (m_hWnd && IsWindow(m_hWnd)) {
            InvalidateRect(m_hWnd, &m_smartCardRect, FALSE);
        }
    }
}

void AddProfileDialog::toggle_smart_dns() {
    set_smart_dns(!m_isSmartDns);
}

std::optional<DnsProfile> AddProfileDialog::show(HWND hParent, HINSTANCE hInstance, bool isDarkMode,
                                                  const std::wstring& defaultPriIp,
                                                  const std::wstring& defaultSecIp) {
    AddProfileDialog dlg(hParent, hInstance, isDarkMode, defaultPriIp, defaultSecIp);
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

    if (hParent && IsWindow(hParent)) {
        EnableWindow(hParent, TRUE);
        if (IsWindowVisible(hParent)) {
            SetForegroundWindow(hParent);
        }
    }

    if (dlg.m_hWnd && IsWindow(dlg.m_hWnd)) {
        DestroyWindow(dlg.m_hWnd);
        dlg.m_hWnd = nullptr;
    }

    return dlg.m_result;
}

AddProfileDialog::AddProfileDialog(HWND hParent, HINSTANCE hInstance, bool isDarkMode,
                                   const std::wstring& defaultPriIp,
                                   const std::wstring& defaultSecIp)
    : m_hParent(hParent), m_hInstance(hInstance), m_isDarkMode(isDarkMode),
      m_defaultPriIp(defaultPriIp), m_defaultSecIp(defaultSecIp)
{
    m_dpi = query_window_dpi(m_hParent);
    auto pal = quickdns::GetPalette(m_isDarkMode);
    m_hbrBg = CreateSolidBrush(pal.bg);
    m_hbrCard = CreateSolidBrush(pal.cardBg);

    setup_fonts(static_cast<float>(m_dpi) / 96.0f);
}

AddProfileDialog::~AddProfileDialog() {
    if (m_hFontRegular) DeleteObject(m_hFontRegular);
    if (m_hFontBold) DeleteObject(m_hFontBold);
    if (m_hFontHeader) DeleteObject(m_hFontHeader);
    if (m_hFontSmall) DeleteObject(m_hFontSmall);
    if (m_hFontSmallBold) DeleteObject(m_hFontSmallBold);

    if (m_hbrBg) DeleteObject(m_hbrBg);
    if (m_hbrCard) DeleteObject(m_hbrCard);

    UnregisterClassW(DIALOG_CLASS_NAME, m_hInstance);
}

void AddProfileDialog::setup_fonts(float scale) {
    scale = std::clamp(scale, 0.75f, 3.0f);
    if (std::abs(m_activeFontScale - scale) < 0.005f && m_hFontRegular) {
        return;
    }
    m_activeFontScale = scale;

    if (m_hFontRegular) { DeleteObject(m_hFontRegular); m_hFontRegular = nullptr; }
    if (m_hFontBold) { DeleteObject(m_hFontBold); m_hFontBold = nullptr; }
    if (m_hFontHeader) { DeleteObject(m_hFontHeader); m_hFontHeader = nullptr; }
    if (m_hFontSmall) { DeleteObject(m_hFontSmall); m_hFontSmall = nullptr; }
    if (m_hFontSmallBold) { DeleteObject(m_hFontSmallBold); m_hFontSmallBold = nullptr; }

    auto sc = [scale](int px) -> int {
        return -std::max(8, static_cast<int>(std::lround(static_cast<float>(px) * scale)));
    };

    Gdiplus::FontFamily ffDisplay(L"Segoe UI Variable Display");
    const wchar_t* headerFace = ffDisplay.IsAvailable() ? L"Segoe UI Variable Display" : L"Segoe UI";

    m_hFontRegular = CreateFontW(sc(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    m_hFontBold = CreateFontW(sc(14), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    m_hFontHeader = CreateFontW(sc(18), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, headerFace);

    m_hFontSmall = CreateFontW(sc(12), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    m_hFontSmallBold = CreateFontW(sc(12), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

void AddProfileDialog::apply_fonts_to_controls() {
    HWND edits[] = { m_hEditName, m_hEditPri, m_hEditSec, m_hEditDesc, m_hEditDoh };
    for (HWND hEd : edits) {
        if (hEd && IsWindow(hEd)) {
            SendMessageW(hEd, WM_SETFONT, reinterpret_cast<WPARAM>(m_hFontRegular), TRUE);
        }
    }
    if (m_hCheckSmartDns && IsWindow(m_hCheckSmartDns)) {
        InvalidateRect(m_hCheckSmartDns, nullptr, FALSE);
    }
    if (m_hBtnSave && IsWindow(m_hBtnSave)) {
        InvalidateRect(m_hBtnSave, nullptr, FALSE);
    }
    if (m_hBtnCancel && IsWindow(m_hBtnCancel)) {
        InvalidateRect(m_hBtnCancel, nullptr, FALSE);
    }
}

bool AddProfileDialog::create_dialog_window() {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = &AddProfileDialog::dialog_proc_thunk;
    wc.hInstance = m_hInstance;
    wc.hIcon = LoadIconW(m_hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
    if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm = LoadIconW(m_hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = DIALOG_CLASS_NAME;
    RegisterClassExW(&wc);

    DWORD dwStyle = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MAXIMIZEBOX | WS_VISIBLE;
    DWORD dwExStyle = WS_EX_DLGMODALFRAME;

    int targetClientW = MulDiv(DIALOG_WIDTH, static_cast<int>(m_dpi), 96);
    int targetClientH = MulDiv(DIALOG_HEIGHT, static_cast<int>(m_dpi), 96);
    RECT wr = { 0, 0, targetClientW, targetClientH };
    adjust_window_rect_for_dpi(&wr, dwStyle, FALSE, dwExStyle, m_dpi);
    int winWidth = wr.right - wr.left;
    int winHeight = wr.bottom - wr.top;

    RECT rcWork = get_monitor_work_area(m_hParent);
    int workW = std::max(240, static_cast<int>(rcWork.right - rcWork.left));
    int workH = std::max(240, static_cast<int>(rcWork.bottom - rcWork.top));
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
        L"Добавить свой DNS сервер",
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

    m_dpi = query_window_dpi(m_hWnd);

    HICON hIcon = LoadIconW(m_hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
    if (hIcon) {
        SendMessageW(m_hWnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(hIcon));
        SendMessageW(m_hWnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(hIcon));
    }

    apply_theme();
    create_controls();
    SetFocus(m_hEditName);
    return true;
}

void AddProfileDialog::apply_theme() {
    BOOL useDarkMode = m_isDarkMode ? TRUE : FALSE;
    DwmSetWindowAttribute(m_hWnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &useDarkMode, sizeof(useDarkMode));
    DwmSetWindowAttribute(m_hWnd, 19, &useDarkMode, sizeof(useDarkMode));
    DWORD cornerPref = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(m_hWnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &cornerPref, sizeof(cornerPref));
}

AddProfileDialog::LayoutMetrics AddProfileDialog::compute_layout(int clientW, int clientH) const {
    float dpiScale = static_cast<float>(m_dpi) / 96.0f;
    float fitScaleH = (clientH > 0) ? (static_cast<float>(clientH) / static_cast<float>(DIALOG_HEIGHT)) : dpiScale;
    float s = std::clamp(std::min(dpiScale, fitScaleH), 0.75f, 3.0f);

    auto sc = [s](int px) -> int {
        return static_cast<int>(std::lround(static_cast<float>(px) * s));
    };

    LayoutMetrics lm{};
    lm.scale = s;
    lm.marginX = sc(24);
    lm.contentW = std::max(sc(180), clientW - 2 * lm.marginX);
    lm.headerY = sc(16);
    lm.subY = sc(44);
    lm.subH = sc(38);
    lm.fieldH = std::max(sc(30), sc(36));
    lm.innerEditH = std::max(sc(18), sc(22));
    lm.innerOffset = std::max(2, (lm.fieldH - lm.innerEditH) / 2);
    lm.labelOffsetY = sc(22);

    int stepField = sc(76);
    lm.yName = sc(102);
    lm.yPri  = lm.yName + stepField;
    lm.ySec  = lm.yPri + stepField;
    lm.yDesc = lm.ySec + stepField;
    lm.yDoh  = lm.yDesc + stepField;
    lm.ySmart = lm.yDoh + lm.fieldH + sc(18);
    lm.checkH = std::max(sc(22), sc(26));
    lm.smartSubTopPad = sc(8) + lm.checkH + sc(4);

    float subFontPx = std::max(9.0f, 12.0f * s);
    int subTextH = sc(20);
    if (m_hWnd && IsWindow(m_hWnd)) {
        HDC hdc = GetDC(m_hWnd);
        if (hdc) {
            Gdiplus::Graphics g(hdc);
            g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
            Gdiplus::FontFamily ffRegular(L"Segoe UI");
            Gdiplus::Font fontSub(&ffRegular, subFontPx, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
            Gdiplus::StringFormat sfSmartSub;
            sfSmartSub.SetTrimming(Gdiplus::StringTrimmingWord);
            sfSmartSub.SetFormatFlags(Gdiplus::StringFormatFlagsNoClip);
            Gdiplus::RectF measuredBounds;
            float maxTextW = static_cast<float>(std::max(80, lm.contentW - sc(28)));
            g.MeasureString(SMARTDNS_SUB_TEXT, -1, &fontSub, Gdiplus::SizeF(maxTextW, 0.0f), &sfSmartSub, &measuredBounds);
            subTextH = std::max(sc(18), static_cast<int>(std::ceil(measuredBounds.Height)));
            ReleaseDC(m_hWnd, hdc);
        }
    }

    lm.smartH = lm.smartSubTopPad + subTextH + sc(12);
    lm.btnH = std::max(sc(32), sc(40));
    lm.btnGap = sc(14);
    lm.yButtons = lm.ySmart + lm.smartH + sc(14);
    if (clientH > 0 && lm.yButtons + lm.btnH + sc(10) > clientH) {
        lm.yButtons = std::max(lm.ySmart + lm.smartH + sc(6), clientH - lm.btnH - sc(10));
    }
    return lm;
}

void AddProfileDialog::create_controls() {
    RECT rcClient = {};
    GetClientRect(m_hWnd, &rcClient);
    LayoutMetrics lm = compute_layout(rcClient.right - rcClient.left, rcClient.bottom - rcClient.top);
    setup_fonts(lm.scale);

    int editPadX = static_cast<int>(std::lround(10.0f * lm.scale));
    int editW = std::max(80, lm.contentW - 2 * editPadX);

    m_hEditName = CreateWindowExW(0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        lm.marginX + editPadX, lm.yName + lm.innerOffset, editW, lm.innerEditH,
        m_hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EDIT_NAME)), m_hInstance, nullptr);
    SendMessageW(m_hEditName, WM_SETFONT, reinterpret_cast<WPARAM>(m_hFontRegular), TRUE);
    SendMessageW(m_hEditName, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Например: AdGuard DNS, Мой DNS, Офисный сервер"));

    m_hEditPri = CreateWindowExW(0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        lm.marginX + editPadX, lm.yPri + lm.innerOffset, editW, lm.innerEditH,
        m_hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EDIT_PRI_IPV4)), m_hInstance, nullptr);
    SendMessageW(m_hEditPri, WM_SETFONT, reinterpret_cast<WPARAM>(m_hFontRegular), TRUE);
    SendMessageW(m_hEditPri, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Например: 94.140.14.14 или 77.88.8.8"));

    m_hEditSec = CreateWindowExW(0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        lm.marginX + editPadX, lm.ySec + lm.innerOffset, editW, lm.innerEditH,
        m_hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EDIT_SEC_IPV4)), m_hInstance, nullptr);
    SendMessageW(m_hEditSec, WM_SETFONT, reinterpret_cast<WPARAM>(m_hFontRegular), TRUE);
    SendMessageW(m_hEditSec, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Например: 94.140.15.15 (необязательно)"));

    m_hEditDesc = CreateWindowExW(0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        lm.marginX + editPadX, lm.yDesc + lm.innerOffset, editW, lm.innerEditH,
        m_hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EDIT_DESC)), m_hInstance, nullptr);
    SendMessageW(m_hEditDesc, WM_SETFONT, reinterpret_cast<WPARAM>(m_hFontRegular), TRUE);
    SendMessageW(m_hEditDesc, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Краткое описание или назначение (необязательно)"));

    m_hEditDoh = CreateWindowExW(0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        lm.marginX + editPadX, lm.yDoh + lm.innerOffset, editW, lm.innerEditH,
        m_hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_EDIT_DOH)), m_hInstance, nullptr);
    SendMessageW(m_hEditDoh, WM_SETFONT, reinterpret_cast<WPARAM>(m_hFontRegular), TRUE);
    SendMessageW(m_hEditDoh, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"https://dns.adguard-dns.com/dns-query (необязательно)"));

    int checkInsetX = static_cast<int>(std::lround(14.0f * lm.scale));
    int checkInsetY = static_cast<int>(std::lround(8.0f * lm.scale));
    m_hCheckSmartDns = CreateWindowExW(0, WC_BUTTONW, SMARTDNS_CHECK_LABEL,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        lm.marginX + checkInsetX, lm.ySmart + checkInsetY, std::max(80, lm.contentW - 2 * checkInsetX), lm.checkH,
        m_hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_CHECK_SMARTDNS)), m_hInstance, nullptr);

    auto* chkState = new CheckboxAnimState();
    chkState->pDlg = this;
    SetPropW(m_hCheckSmartDns, L"QuickDnsCheckAnim", reinterpret_cast<HANDLE>(chkState));
    SetWindowSubclass(m_hCheckSmartDns, CustomCheckboxSubclassProc, 401, reinterpret_cast<DWORD_PTR>(chkState));

    int btnWidth = (lm.contentW - lm.btnGap) / 2;

    m_hBtnSave = CreateWindowExW(0, WC_BUTTONW, L"Сохранить профиль",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        lm.marginX, lm.yButtons, btnWidth, lm.btnH,
        m_hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_SAVE_PROFILE)), m_hInstance, nullptr);

    m_hBtnCancel = CreateWindowExW(0, WC_BUTTONW, L"Отмена",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        lm.marginX + btnWidth + lm.btnGap, lm.yButtons, btnWidth, lm.btnH,
        m_hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_CANCEL_PROFILE)), m_hInstance, nullptr);

    attach_button_effects(m_hBtnSave, 301);
    attach_button_effects(m_hBtnCancel, 302);

    if (!m_defaultPriIp.empty()) {
        SetWindowTextW(m_hEditPri, m_defaultPriIp.c_str());
        SetWindowTextW(m_hEditName, L"DNS Адаптера");
    }
    if (!m_defaultSecIp.empty()) {
        SetWindowTextW(m_hEditSec, m_defaultSecIp.c_str());
    }

    reposition_controls();
}

void AddProfileDialog::reposition_controls() {
    if (!m_hWnd || !IsWindow(m_hWnd)) return;
    RECT rcClient = {};
    GetClientRect(m_hWnd, &rcClient);
    int clientW = rcClient.right - rcClient.left;
    int clientH = rcClient.bottom - rcClient.top;
    if (clientW < 200 || clientH < 200) return;

    LayoutMetrics lm = compute_layout(clientW, clientH);
    float prevScale = m_activeFontScale;
    setup_fonts(lm.scale);
    if (std::abs(prevScale - m_activeFontScale) >= 0.005f) {
        apply_fonts_to_controls();
    }

    int editPadX = static_cast<int>(std::lround(10.0f * lm.scale));
    int editW = std::max(80, lm.contentW - 2 * editPadX);

    if (m_hEditName) SetWindowPos(m_hEditName, nullptr, lm.marginX + editPadX, lm.yName + lm.innerOffset, editW, lm.innerEditH, SWP_NOZORDER);
    if (m_hEditPri)  SetWindowPos(m_hEditPri, nullptr, lm.marginX + editPadX, lm.yPri + lm.innerOffset, editW, lm.innerEditH, SWP_NOZORDER);
    if (m_hEditSec)  SetWindowPos(m_hEditSec, nullptr, lm.marginX + editPadX, lm.ySec + lm.innerOffset, editW, lm.innerEditH, SWP_NOZORDER);
    if (m_hEditDesc) SetWindowPos(m_hEditDesc, nullptr, lm.marginX + editPadX, lm.yDesc + lm.innerOffset, editW, lm.innerEditH, SWP_NOZORDER);
    if (m_hEditDoh)  SetWindowPos(m_hEditDoh, nullptr, lm.marginX + editPadX, lm.yDoh + lm.innerOffset, editW, lm.innerEditH, SWP_NOZORDER);

    int checkInsetX = static_cast<int>(std::lround(14.0f * lm.scale));
    int checkInsetY = static_cast<int>(std::lround(8.0f * lm.scale));
    if (m_hCheckSmartDns) {
        SetWindowPos(m_hCheckSmartDns, nullptr,
            lm.marginX + checkInsetX, lm.ySmart + checkInsetY,
            std::max(80, lm.contentW - 2 * checkInsetX), lm.checkH, SWP_NOZORDER);
    }

    m_smartCardRect = { lm.marginX, lm.ySmart, lm.marginX + lm.contentW, lm.ySmart + lm.smartH };

    int btnWidth = (lm.contentW - lm.btnGap) / 2;
    if (m_hBtnSave)   SetWindowPos(m_hBtnSave, nullptr, lm.marginX, lm.yButtons, btnWidth, lm.btnH, SWP_NOZORDER);
    if (m_hBtnCancel) SetWindowPos(m_hBtnCancel, nullptr, lm.marginX + btnWidth + lm.btnGap, lm.yButtons, btnWidth, lm.btnH, SWP_NOZORDER);

    InvalidateRect(m_hWnd, nullptr, TRUE);
}

void AddProfileDialog::attach_button_effects(HWND hBtn, UINT_PTR subclassId) {
    if (!hBtn) return;
    auto* anim = new ButtonAnimationState();
    SetPropW(hBtn, L"QuickDnsAnim", reinterpret_cast<HANDLE>(anim));
    SetWindowSubclass(hBtn, AnimatedButtonSubclassProc, subclassId, reinterpret_cast<DWORD_PTR>(anim));
}

void AddProfileDialog::draw_custom_button(DRAWITEMSTRUCT* pDis) {
    if (!pDis) return;

    HDC hdc = pDis->hDC;
    RECT rc = pDis->rcItem;
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;

    auto pal = quickdns::GetPalette(m_isDarkMode);
    bool isPrimary = (pDis->CtlID == IDC_BTN_SAVE_PROFILE);
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

    COLORREF curBg = isDisabled ? baseBg : (isPressed ? pressBg : quickdns::LerpColor(baseBg, hoverBg, hoverVal));
    COLORREF curBorder = isDisabled ? baseBorder : quickdns::LerpColor(baseBorder, hoverBorder, hoverVal);

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
        quickdns::AddRoundedRect(path, Gdiplus::RectF(0.5f, 0.5f, static_cast<float>(w) - 1.0f, static_cast<float>(h) - 1.0f), dpf(7.0f));

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

void AddProfileDialog::on_save() {
    wchar_t nameBuf[256] = {};
    wchar_t priBuf[128] = {};
    wchar_t secBuf[128] = {};
    wchar_t descBuf[256] = {};
    wchar_t dohBuf[512] = {};

    GetWindowTextW(m_hEditName, nameBuf, _countof(nameBuf));
    GetWindowTextW(m_hEditPri, priBuf, _countof(priBuf));
    GetWindowTextW(m_hEditSec, secBuf, _countof(secBuf));
    GetWindowTextW(m_hEditDesc, descBuf, _countof(descBuf));
    GetWindowTextW(m_hEditDoh, dohBuf, _countof(dohBuf));

    std::wstring nameStr = nameBuf;
    std::wstring priStr = priBuf;
    std::wstring secStr = secBuf;
    std::wstring descStr = descBuf;
    std::wstring dohStr = dohBuf;

    auto trim = [](std::wstring& s) {
        while (!s.empty() && std::iswspace(s.front())) s.erase(s.begin());
        while (!s.empty() && std::iswspace(s.back())) s.pop_back();
    };
    trim(nameStr);
    trim(priStr);
    trim(secStr);
    trim(descStr);
    trim(dohStr);

    if (nameStr.empty()) {
        MessageBoxW(m_hWnd, L"Пожалуйста, введите название профиля.", L"Ошибка ввода", MB_OK | MB_ICONWARNING);
        SetFocus(m_hEditName);
        return;
    }

    if (priStr.empty() || !quickdns::DnsManager::is_valid_ipv4(priStr)) {
        MessageBoxW(m_hWnd, L"Пожалуйста, укажите корректный основной IPv4 адрес (например, 77.88.8.8 или 94.140.14.14).", L"Некорректный IP", MB_OK | MB_ICONWARNING);
        SetFocus(m_hEditPri);
        return;
    }

    if (!secStr.empty() && !quickdns::DnsManager::is_valid_ipv4(secStr)) {
        MessageBoxW(m_hWnd, L"Дополнительный адрес должен быть корректным IPv4 адресом (или оставьте это поле пустым).", L"Некорректный IP", MB_OK | MB_ICONWARNING);
        SetFocus(m_hEditSec);
        return;
    }

    if (!dohStr.empty() && !quickdns::DnsManager::is_valid_doh_template(dohStr)) {
        MessageBoxW(m_hWnd, L"Некорректный адрес DoH-шаблона. Шаблон должен начинаться с https:// и содержать корректный URL.", L"Ошибка ввода", MB_OK | MB_ICONWARNING);
        SetFocus(m_hEditDoh);
        return;
    }

    static std::atomic<uint64_t> s_idSeq{1};
    uint64_t seq = s_idSeq.fetch_add(1, std::memory_order_relaxed);

    DnsProfile profile;
    profile.id = "custom_" + std::to_string(GetTickCount64()) + "_" + std::to_string(seq);
    profile.name = wide_to_utf8(nameStr);
    profile.primary_ipv4 = wide_to_utf8(priStr);
    profile.secondary_ipv4 = wide_to_utf8(secStr);
    profile.doh_template = wide_to_utf8(dohStr);
    profile.is_smart_dns = m_isSmartDns;
    if (descStr.empty()) {
        profile.description = m_isSmartDns
            ? ("Пользовательский SmartDNS: " + profile.primary_ipv4)
            : ("Пользовательский DNS: " + profile.primary_ipv4);
    } else {
        profile.description = wide_to_utf8(descStr);
    }
    if (!profile.primary_ipv4.empty()) {
        profile.ip_pool.push_back(profile.primary_ipv4);
    }
    if (!profile.secondary_ipv4.empty() && profile.secondary_ipv4 != profile.primary_ipv4) {
        profile.ip_pool.push_back(profile.secondary_ipv4);
    }
    profile.is_builtin = false;
    profile.is_dhcp = false;

    m_result = profile;
    m_finished = true;
}

void AddProfileDialog::on_cancel() {
    m_result = std::nullopt;
    m_finished = true;
}

LRESULT CALLBACK AddProfileDialog::dialog_proc_thunk(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    AddProfileDialog* pThis = nullptr;
    if (uMsg == WM_NCCREATE) {
        auto* pCreate = reinterpret_cast<CREATESTRUCTW*>(lParam);
        pThis = reinterpret_cast<AddProfileDialog*>(pCreate->lpCreateParams);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pThis));
        pThis->m_hWnd = hWnd;
    } else {
        pThis = reinterpret_cast<AddProfileDialog*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    }

    if (pThis) {
        return pThis->handle_message(hWnd, uMsg, wParam, lParam);
    }
    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

LRESULT AddProfileDialog::handle_message(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_GETMINMAXINFO: {
        auto* pMMI = reinterpret_cast<MINMAXINFO*>(lParam);
        RECT rcMin = { 0, 0, MulDiv(500, static_cast<int>(m_dpi), 96), MulDiv(620, static_cast<int>(m_dpi), 96) };
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

    case WM_SETCURSOR: {
        if (reinterpret_cast<HWND>(wParam) == hWnd && LOWORD(lParam) == HTCLIENT) {
            POINT pt{};
            GetCursorPos(&pt);
            ScreenToClient(hWnd, &pt);
            if (PtInRect(&m_smartCardRect, pt)) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
        }
        break;
    }

    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (PtInRect(&m_smartCardRect, pt)) {
            if (m_hCheckSmartDns && IsWindow(m_hCheckSmartDns)) {
                SetFocus(m_hCheckSmartDns);
            }
            toggle_smart_dns();
            return 0;
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

        auto pal = quickdns::GetPalette(m_isDarkMode);

        HBRUSH hbrBg = CreateSolidBrush(pal.bg);
        FillRect(hdcMem, &rc, hbrBg);
        DeleteObject(hbrBg);

        {
            Gdiplus::Graphics g(hdcMem);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
            g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

            LayoutMetrics lm = compute_layout(w, h);
            float s = lm.scale;

            Gdiplus::FontFamily ffRegular(L"Segoe UI");
            Gdiplus::FontFamily ffDisplay(L"Segoe UI Variable Display");
            const Gdiplus::FontFamily* pDisplay = ffDisplay.IsAvailable() ? &ffDisplay : &ffRegular;

            Gdiplus::Font fontHeader(pDisplay, 18.5f * s, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
            Gdiplus::Font fontLabel(&ffRegular, 12.7f * s, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
            Gdiplus::Font fontSub(&ffRegular, 12.0f * s, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);

            Gdiplus::SolidBrush textPriBr(Gdiplus::Color(255, GetRValue(pal.textPrimary), GetGValue(pal.textPrimary), GetBValue(pal.textPrimary)));
            Gdiplus::SolidBrush textSecBr(Gdiplus::Color(255, GetRValue(pal.textSecondary), GetGValue(pal.textSecondary), GetBValue(pal.textSecondary)));

            g.DrawString(L"Добавить свой DNS сервер", -1, &fontHeader,
                Gdiplus::PointF(static_cast<float>(lm.marginX), static_cast<float>(lm.headerY)), &textPriBr);

            Gdiplus::StringFormat sfSub;
            sfSub.SetTrimming(Gdiplus::StringTrimmingWord);
            sfSub.SetFormatFlags(Gdiplus::StringFormatFlagsLineLimit);
            Gdiplus::RectF rcSub(static_cast<float>(lm.marginX), static_cast<float>(lm.subY),
                static_cast<float>(lm.contentW), static_cast<float>(lm.subH));
            g.DrawString(L"Создайте собственный профиль DNS с поддержкой IPv4, DoH и SmartDNS", -1, &fontSub, rcSub, &sfSub, &textSecBr);

            Gdiplus::SolidBrush cardBgBr(Gdiplus::Color(255, GetRValue(pal.cardBg), GetGValue(pal.cardBg), GetBValue(pal.cardBg)));
            Gdiplus::Pen cardBorderPen(Gdiplus::Color(255, GetRValue(pal.cardBorder), GetGValue(pal.cardBorder), GetBValue(pal.cardBorder)), 1.0f);

            auto drawFieldCard = [&](const wchar_t* label, int y, int fieldH) {
                g.DrawString(label, -1, &fontLabel,
                    Gdiplus::PointF(static_cast<float>(lm.marginX) + 8.0f * s, static_cast<float>(y - lm.labelOffsetY)), &textPriBr);
                Gdiplus::GraphicsPath p;
                quickdns::AddRoundedRect(p,
                    Gdiplus::RectF(static_cast<float>(lm.marginX), static_cast<float>(y),
                                   static_cast<float>(lm.contentW), static_cast<float>(fieldH)),
                    6.0f * s);
                g.FillPath(&cardBgBr, &p);
                g.DrawPath(&cardBorderPen, &p);
            };

            drawFieldCard(L"Название профиля (обязательно):", lm.yName, lm.fieldH);
            drawFieldCard(L"Основной DNS IPv4 (обязательно):", lm.yPri, lm.fieldH);
            drawFieldCard(L"Дополнительный DNS IPv4 (необязательно):", lm.ySec, lm.fieldH);
            drawFieldCard(L"Описание / заметка (необязательно):", lm.yDesc, lm.fieldH);
            drawFieldCard(L"DNS over HTTPS (DoH) URL (необязательно):", lm.yDoh, lm.fieldH);

            Gdiplus::GraphicsPath smartCardPath;
            quickdns::AddRoundedRect(smartCardPath,
                Gdiplus::RectF(static_cast<float>(lm.marginX), static_cast<float>(lm.ySmart),
                               static_cast<float>(lm.contentW), static_cast<float>(lm.smartH)),
                8.0f * s);
            g.FillPath(&cardBgBr, &smartCardPath);
            if (m_isSmartDns) {
                Gdiplus::Pen activeSmartPen(Gdiplus::Color(180, 34, 197, 94), 1.5f);
                g.DrawPath(&activeSmartPen, &smartCardPath);
            } else {
                g.DrawPath(&cardBorderPen, &smartCardPath);
            }

            Gdiplus::StringFormat sfSmartSub;
            sfSmartSub.SetTrimming(Gdiplus::StringTrimmingWord);
            sfSmartSub.SetFormatFlags(Gdiplus::StringFormatFlagsNoClip);

            float insetX = 14.0f * s;
            float subTextH = std::max(18.0f * s, static_cast<float>(lm.smartH - lm.smartSubTopPad) - 4.0f * s);
            Gdiplus::RectF rcSmartSub(
                static_cast<float>(lm.marginX) + insetX,
                static_cast<float>(lm.ySmart + lm.smartSubTopPad),
                static_cast<float>(lm.contentW) - 2.0f * insetX,
                subTextH
            );
            g.DrawString(SMARTDNS_SUB_TEXT, -1, &fontSub, rcSmartSub, &sfSmartSub, &textSecBr);
        }

        BitBlt(hdc, 0, 0, w, h, hdcMem, 0, 0, SRCCOPY);
        SelectObject(hdcMem, oldBmp);
        DeleteObject(hbmMem);
        DeleteDC(hdcMem);
        EndPaint(hWnd, &ps);
        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam);
        int code = HIWORD(wParam);
        if (id == ID_CHECK_SMARTDNS && code == BN_CLICKED) {
            toggle_smart_dns();
            return 0;
        }
        if (id == IDC_BTN_SAVE_PROFILE || id == IDOK) {
            on_save();
            return 0;
        }
        if (id == IDC_BTN_CANCEL_PROFILE || id == IDCANCEL) {
            on_cancel();
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
        HDC hdcStatic = reinterpret_cast<HDC>(wParam);
        HWND hwndStatic = reinterpret_cast<HWND>(lParam);
        auto pal = quickdns::GetPalette(m_isDarkMode);
        SetTextColor(hdcStatic, pal.textPrimary);
        SetBkMode(hdcStatic, TRANSPARENT);
        if (hwndStatic == m_hCheckSmartDns) {
            return reinterpret_cast<INT_PTR>(m_hbrCard);
        }
        return reinterpret_cast<INT_PTR>(m_hbrBg);
    }

    case WM_CTLCOLOREDIT: {
        HDC hdcEdit = reinterpret_cast<HDC>(wParam);
        auto pal = quickdns::GetPalette(m_isDarkMode);
        SetBkMode(hdcEdit, OPAQUE);
        SetTextColor(hdcEdit, pal.textPrimary);
        SetBkColor(hdcEdit, pal.cardBg);
        return reinterpret_cast<INT_PTR>(m_hbrCard);
    }

    case WM_CLOSE: {
        on_cancel();
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
