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
#include <iterator>

#ifdef _MSC_VER
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdiplus.lib")
#endif

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

#ifndef DM_GETDEFID
#define DM_GETDEFID (WM_USER + 0)
#endif
#ifndef DM_SETDEFID
#define DM_SETDEFID (WM_USER + 1)
#endif
#ifndef DC_HASDEFID
#define DC_HASDEFID 0x534B
#endif

using QuickDns::Encoding::wide_to_utf8;

namespace {
    constexpr const wchar_t* DIALOG_CLASS_NAME = L"QuickDNS_AddProfileDialogClass";
    constexpr int DIALOG_CLIENT_WIDTH  = 600;
    constexpr int DIALOG_CLIENT_HEIGHT = 572;

    constexpr const wchar_t* HEADER_TITLE_TEXT = L"Добавить свой DNS сервер";
    constexpr const wchar_t* HEADER_SUB_TEXT   = L"Настройте пользовательский профиль с поддержкой IPv4, шифрованного DoH и режима SmartDNS.";

    constexpr const wchar_t* SMARTDNS_CARD_TITLE = L"Профиль SmartDNS";
    constexpr const wchar_t* SMARTDNS_BADGE_TEXT = L"SmartDNS";
    constexpr const wchar_t* SMARTDNS_SUB_TEXT   = L"Включает сервер в каталог SmartDNS и фильтр опроса для обхода гео-блокировок и ИИ";

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

    LRESULT CALLBACK AnimatedButtonSubclassProc(
        HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
        UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
    {
        auto* anim = reinterpret_cast<ButtonAnimationState*>(dwRefData);
        if (!anim) return DefSubclassProc(hWnd, uMsg, wParam, lParam);

        switch (uMsg) {
        case BM_SETSTYLE:
            // Preserve BS_OWNERDRAW when IsDialogMessageW moves default focus across buttons
            InvalidateRect(hWnd, nullptr, FALSE);
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
        default:
            break;
        }

        return DefSubclassProc(hWnd, uMsg, wParam, lParam);
    }

    struct EditFieldState {
        AddProfileDialog* pDlg = nullptr;
        size_t fieldIndex = 0;
        bool isTracking = false;
    };

    LRESULT CALLBACK EditFieldSubclassProc(
        HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
        UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
    {
        auto* state = reinterpret_cast<EditFieldState*>(dwRefData);
        if (!state || !state->pDlg) return DefSubclassProc(hWnd, uMsg, wParam, lParam);

        switch (uMsg) {
        case WM_SETFOCUS: {
            state->pDlg->set_field_focused(state->fieldIndex, true);
            break;
        }
        case WM_KILLFOCUS: {
            state->pDlg->set_field_focused(state->fieldIndex, false);
            break;
        }
        case WM_MOUSEMOVE: {
            if (!state->isTracking) {
                state->isTracking = true;
                TRACKMOUSEEVENT tme{};
                tme.cbSize = sizeof(TRACKMOUSEEVENT);
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = hWnd;
                TrackMouseEvent(&tme);
                state->pDlg->set_field_hovered(state->fieldIndex, true);
            }
            break;
        }
        case WM_MOUSELEAVE: {
            state->isTracking = false;
            state->pDlg->set_field_hovered(state->fieldIndex, false);
            break;
        }
        case WM_KEYDOWN: {
            if (wParam == VK_RETURN) {
                state->pDlg->on_save();
                return 0;
            }
            if (wParam == VK_ESCAPE) {
                state->pDlg->on_cancel();
                return 0;
            }
            break;
        }
        case WM_CHAR: {
            if (wParam == VK_RETURN || wParam == VK_ESCAPE) {
                return 0;
            }
            if (wParam == 0x01) { // Ctrl+A Select All
                SendMessageW(hWnd, EM_SETSEL, 0, -1);
                return 0;
            }
            break;
        }
        case WM_NCDESTROY: {
            delete state;
            RemoveWindowSubclass(hWnd, EditFieldSubclassProc, uIdSubclass);
            return 0;
        }
        default:
            break;
        }

        return DefSubclassProc(hWnd, uMsg, wParam, lParam);
    }

    struct SmartDnsCardState {
        AddProfileDialog* pDlg = nullptr;
        float hover = 0.0f;
        float targetHover = 0.0f;
        bool isTracking = false;
        bool isPressed = false;
    };

    LRESULT CALLBACK SmartDnsCardSubclassProc(
        HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
        UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
    {
        auto* state = reinterpret_cast<SmartDnsCardState*>(dwRefData);
        if (!state || !state->pDlg) return DefSubclassProc(hWnd, uMsg, wParam, lParam);

        switch (uMsg) {
        case WM_GETDLGCODE:
            return DLGC_BUTTON;

        case BM_SETSTYLE:
            InvalidateRect(hWnd, nullptr, FALSE);
            return 0;

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
            if (!IsWindowEnabled(hWnd)) break;
            if (!state->isTracking) {
                state->isTracking = true;
                state->targetHover = 1.0f;
                TRACKMOUSEEVENT tme{};
                tme.cbSize = sizeof(TRACKMOUSEEVENT);
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = hWnd;
                TrackMouseEvent(&tme);
                SetTimer(hWnd, 4002, 16, nullptr);
            }
            break;
        }
        case WM_MOUSELEAVE: {
            state->isTracking = false;
            state->targetHover = 0.0f;
            SetTimer(hWnd, 4002, 16, nullptr);
            break;
        }
        case WM_TIMER: {
            if (wParam == 4002) {
                float diff = state->targetHover - state->hover;
                if (std::abs(diff) < 0.02f) {
                    state->hover = state->targetHover;
                    KillTimer(hWnd, 4002);
                } else {
                    state->hover += diff * 0.28f;
                }
                InvalidateRect(hWnd, nullptr, FALSE);
                return 0;
            }
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
                if (!state->isPressed) {
                    state->isPressed = true;
                    InvalidateRect(hWnd, nullptr, FALSE);
                }
                return 0;
            }
            if (wParam == VK_RETURN) {
                state->pDlg->on_save();
                return 0;
            }
            if (wParam == VK_ESCAPE) {
                state->pDlg->on_cancel();
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
        case WM_CHAR: {
            if (wParam == VK_RETURN || wParam == VK_ESCAPE || wParam == VK_SPACE) {
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

            const bool isDark = state->pDlg->is_dark_mode();
            const auto pal = quickdns::GetPalette(isDark);
            const bool isChecked = state->pDlg->is_smart_dns();
            const bool hasFocus = (GetFocus() == hWnd);
            const float hoverVal = state->hover;

            // Clear with parent dialog bg so rounded corners blend cleanly
            HBRUSH hbrBg = CreateSolidBrush(pal.bg);
            FillRect(hdcMem, &rc, hbrBg);
            DeleteObject(hbrBg);

            {
                Gdiplus::Graphics g(hdcMem);
                g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
                g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

                const float s = state->pDlg->dpf(1.0f);
                const float cardRadius = 9.0f * s;
                Gdiplus::RectF cardRc(0.5f, 0.5f, static_cast<float>(w) - 1.0f, static_cast<float>(h) - 1.0f);
                Gdiplus::GraphicsPath cardPath;
                quickdns::AddRoundedRect(cardPath, cardRc, cardRadius);

                COLORREF baseFill = isChecked
                    ? (isDark ? RGB(24, 42, 37) : RGB(236, 250, 242))
                    : pal.cardBg;
                COLORREF hoverFill = isChecked
                    ? (isDark ? RGB(29, 50, 44) : RGB(226, 246, 235))
                    : (isDark ? RGB(35, 41, 53) : RGB(250, 252, 255));
                COLORREF pressFill = isChecked
                    ? (isDark ? RGB(20, 36, 32) : RGB(218, 242, 228))
                    : (isDark ? RGB(26, 30, 39) : RGB(241, 245, 251));

                COLORREF curFill = state->isPressed ? pressFill : quickdns::LerpColor(baseFill, hoverFill, hoverVal);
                Gdiplus::SolidBrush cardFillBr(Gdiplus::Color(255, GetRValue(curFill), GetGValue(curFill), GetBValue(curFill)));
                g.FillPath(&cardFillBr, &cardPath);

                COLORREF baseBorder = isChecked ? pal.accentGreen : (hasFocus ? pal.accentBlue : pal.cardBorder);
                COLORREF hoverBorder = isChecked
                    ? (isDark ? RGB(74, 222, 128) : RGB(34, 150, 88))
                    : (hasFocus ? pal.accentBlue : (isDark ? RGB(78, 94, 122) : RGB(168, 186, 214)));
                COLORREF curBorder = quickdns::LerpColor(baseBorder, hoverBorder, hoverVal);
                float borderW = (isChecked || hasFocus) ? (1.5f * s) : ((1.0f + 0.35f * hoverVal) * s);

                Gdiplus::Pen cardBorderPen(Gdiplus::Color(255, GetRValue(curBorder), GetGValue(curBorder), GetBValue(curBorder)), borderW);
                g.DrawPath(&cardBorderPen, &cardPath);

                // Left accent indicator bar when checked (matches SmartDnsDialog / AiCheckDialog cards)
                if (isChecked) {
                    Gdiplus::GraphicsPath barPath;
                    quickdns::AddRoundedRect(barPath,
                        Gdiplus::RectF(2.5f * s, 9.0f * s, 3.5f * s, std::max(12.0f * s, static_cast<float>(h) - 18.0f * s)),
                        2.0f * s);
                    Gdiplus::SolidBrush barBr(Gdiplus::Color(255, GetRValue(pal.accentGreen), GetGValue(pal.accentGreen), GetBValue(pal.accentGreen)));
                    g.FillPath(&barBr, &barPath);
                }

                // Subtle inner keyboard focus ring
                if (hasFocus) {
                    Gdiplus::RectF focusRc(2.5f * s, 2.5f * s, static_cast<float>(w) - 5.0f * s, static_cast<float>(h) - 5.0f * s);
                    Gdiplus::GraphicsPath focusPath;
                    quickdns::AddRoundedRect(focusPath, focusRc, std::max(2.0f, cardRadius - 2.0f * s));
                    Gdiplus::Pen focusPen(Gdiplus::Color(isDark ? 120 : 100, GetRValue(pal.accentBlue), GetGValue(pal.accentBlue), GetBValue(pal.accentBlue)), 1.0f);
                    focusPen.SetDashStyle(Gdiplus::DashStyleDot);
                    g.DrawPath(&focusPen, &focusPath);
                }

                // Left rounded checkbox
                const float boxSize = 20.0f * s;
                const float boxX = 16.0f * s;
                const float boxY = 14.0f * s;
                Gdiplus::RectF boxRc(boxX + 0.5f, boxY + 0.5f, boxSize - 1.0f, boxSize - 1.0f);
                Gdiplus::GraphicsPath boxPath;
                quickdns::AddRoundedRect(boxPath, boxRc, 5.0f * s);

                if (isChecked) {
                    Gdiplus::SolidBrush chkBg(Gdiplus::Color(255, GetRValue(pal.accentGreen), GetGValue(pal.accentGreen), GetBValue(pal.accentGreen)));
                    g.FillPath(&chkBg, &boxPath);

                    Gdiplus::Pen chkPen(Gdiplus::Color(255, 255, 255, 255), std::max(1.8f, 2.1f * s));
                    chkPen.SetStartCap(Gdiplus::LineCapRound);
                    chkPen.SetEndCap(Gdiplus::LineCapRound);
                    chkPen.SetLineJoin(Gdiplus::LineJoinRound);
                    Gdiplus::PointF pts[3] = {
                        Gdiplus::PointF(boxX + 5.0f * s, boxY + 10.4f * s),
                        Gdiplus::PointF(boxX + 8.6f * s, boxY + 14.0f * s),
                        Gdiplus::PointF(boxX + 15.2f * s, boxY + 6.4f * s)
                    };
                    g.DrawLines(&chkPen, pts, 3);
                } else {
                    Gdiplus::SolidBrush unchkBg(Gdiplus::Color(255, GetRValue(pal.bg), GetGValue(pal.bg), GetBValue(pal.bg)));
                    g.FillPath(&unchkBg, &boxPath);
                    COLORREF chkBorder = (hoverVal > 0.25f || hasFocus) ? pal.accentBlue : pal.cardBorder;
                    Gdiplus::Pen unchkPen(Gdiplus::Color(255, GetRValue(chkBorder), GetGValue(chkBorder), GetBValue(chkBorder)), 1.4f * s);
                    g.DrawPath(&unchkPen, &boxPath);
                }

                // Right-aligned modern toggle switch pill
                const float swW = 36.0f * s;
                const float swH = 20.0f * s;
                const float swX = static_cast<float>(w) - swW - 16.0f * s;
                const float swY = 14.0f * s;
                Gdiplus::RectF swRc(swX, swY, swW, swH);
                Gdiplus::GraphicsPath swPath;
                quickdns::AddRoundedRect(swPath, swRc, swH * 0.5f);

                if (isChecked) {
                    Gdiplus::SolidBrush swBr(Gdiplus::Color(255, GetRValue(pal.accentGreen), GetGValue(pal.accentGreen), GetBValue(pal.accentGreen)));
                    g.FillPath(&swBr, &swPath);
                } else {
                    COLORREF offTrack = isDark ? RGB(50, 58, 74) : RGB(203, 213, 225);
                    COLORREF offHover = isDark ? RGB(64, 75, 96) : RGB(184, 197, 214);
                    COLORREF trackCol = quickdns::LerpColor(offTrack, offHover, hoverVal);
                    Gdiplus::SolidBrush swBr(Gdiplus::Color(255, GetRValue(trackCol), GetGValue(trackCol), GetBValue(trackCol)));
                    g.FillPath(&swBr, &swPath);
                }

                const float thumbD = 14.0f * s;
                const float thumbPad = (swH - thumbD) * 0.5f;
                const float thumbX = isChecked ? (swX + swW - thumbD - thumbPad) : (swX + thumbPad);
                const float thumbY = swY + thumbPad;
                Gdiplus::SolidBrush thumbBr(Gdiplus::Color(255, 255, 255, 255));
                g.FillEllipse(&thumbBr, thumbX, thumbY, thumbD, thumbD);

                // Title + [SmartDNS] pill badge (matches SmartDnsDialog card header)
                Gdiplus::FontFamily ffRegular(L"Segoe UI");
                Gdiplus::Font fontTitle(&ffRegular, 13.3f * s, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
                Gdiplus::Font fontBadge(&ffRegular, 10.7f * s, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
                Gdiplus::Font fontSub(&ffRegular, 11.8f * s, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);

                Gdiplus::SolidBrush priBr(Gdiplus::Color(255, GetRValue(pal.textPrimary), GetGValue(pal.textPrimary), GetBValue(pal.textPrimary)));
                Gdiplus::SolidBrush secBr(Gdiplus::Color(255, GetRValue(pal.textSecondary), GetGValue(pal.textSecondary), GetBValue(pal.textSecondary)));

                const float textLeft = 46.0f * s;
                const float titleY = 14.0f * s;
                g.DrawString(SMARTDNS_CARD_TITLE, -1, &fontTitle, Gdiplus::PointF(textLeft, titleY), &priBr);

                Gdiplus::RectF titleBounds;
                g.MeasureString(SMARTDNS_CARD_TITLE, -1, &fontTitle, Gdiplus::PointF(0.0f, 0.0f), &titleBounds);

                Gdiplus::StringFormat sfCenter(Gdiplus::StringFormat::GenericTypographic());
                sfCenter.SetAlignment(Gdiplus::StringAlignmentCenter);
                sfCenter.SetLineAlignment(Gdiplus::StringAlignmentCenter);

                Gdiplus::RectF badgeBounds;
                g.MeasureString(SMARTDNS_BADGE_TEXT, -1, &fontBadge, Gdiplus::PointF(0.0f, 0.0f), &sfCenter, &badgeBounds);
                const float pillW = badgeBounds.Width + 18.0f * s;
                const float pillH = 20.0f * s;
                const float pillX = textLeft + titleBounds.Width + 8.0f * s;
                const float pillY = 14.0f * s;

                if (pillX + pillW < swX - 8.0f * s) {
                    Gdiplus::RectF pillRc(pillX, pillY, pillW, pillH);
                    Gdiplus::GraphicsPath pillPath;
                    quickdns::AddRoundedRect(pillPath, pillRc, 4.5f * s);

                    Gdiplus::Color pillBg = isChecked
                        ? (isDark ? Gdiplus::Color(50, 34, 197, 94) : Gdiplus::Color(32, 22, 163, 74))
                        : (isDark ? Gdiplus::Color(255, 38, 44, 56) : Gdiplus::Color(255, 236, 241, 248));
                    Gdiplus::Color pillBorder = isChecked
                        ? (isDark ? Gdiplus::Color(155, 34, 197, 94) : Gdiplus::Color(145, 22, 163, 74))
                        : (isDark ? Gdiplus::Color(255, 58, 67, 84) : Gdiplus::Color(255, 208, 217, 230));
                    Gdiplus::Color pillTxt = isChecked
                        ? (isDark ? Gdiplus::Color(255, 74, 222, 128) : Gdiplus::Color(255, 21, 128, 61))
                        : Gdiplus::Color(255, GetRValue(pal.textSecondary), GetGValue(pal.textSecondary), GetBValue(pal.textSecondary));

                    Gdiplus::SolidBrush pBgBr(pillBg);
                    Gdiplus::Pen pPen(pillBorder, 1.0f);
                    Gdiplus::SolidBrush pTxtBr(pillTxt);
                    g.FillPath(&pBgBr, &pillPath);
                    g.DrawPath(&pPen, &pillPath);
                    g.DrawString(SMARTDNS_BADGE_TEXT, -1, &fontBadge, pillRc, &sfCenter, &pTxtBr);
                }

                // Subtitle text below title row
                Gdiplus::StringFormat sfSub;
                sfSub.SetTrimming(Gdiplus::StringTrimmingWord);
                sfSub.SetFormatFlags(Gdiplus::StringFormatFlagsNoClip);
                const float subY = 39.0f * s;
                const float subW = std::max(60.0f * s, static_cast<float>(w) - textLeft - 16.0f * s);
                const float subH = std::max(18.0f * s, static_cast<float>(h) - subY - 8.0f * s);
                g.DrawString(SMARTDNS_SUB_TEXT, -1, &fontSub, Gdiplus::RectF(textLeft, subY, subW, subH), &sfSub, &secBr);
            }

            BitBlt(hdc, 0, 0, w, h, hdcMem, 0, 0, SRCCOPY);
            SelectObject(hdcMem, oldBmp);
            DeleteObject(hbmMem);
            DeleteDC(hdcMem);
            EndPaint(hWnd, &ps);
            return 0;
        }
        case WM_NCDESTROY: {
            KillTimer(hWnd, 4002);
            RemovePropW(hWnd, L"QuickDnsSmartCard");
            delete state;
            RemoveWindowSubclass(hWnd, SmartDnsCardSubclassProc, uIdSubclass);
            return 0;
        }
        default:
            break;
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
            RECT rcClient{};
            GetClientRect(m_hWnd, &rcClient);
            RECT rcHeader = { 0, 0, rcClient.right, dp(72) };
            InvalidateRect(m_hWnd, &rcHeader, FALSE);
        }
    }
}

void AddProfileDialog::toggle_smart_dns() {
    set_smart_dns(!m_isSmartDns);
}

void AddProfileDialog::invalidate_field_box(size_t index) {
    if (index >= m_fieldRects.size() || !m_hWnd || !IsWindow(m_hWnd)) return;
    RECT rcInv = m_fieldRects[index];
    int pad = std::max(4, dp(5));
    InflateRect(&rcInv, pad, pad);
    InvalidateRect(m_hWnd, &rcInv, FALSE);
}

void AddProfileDialog::set_field_focused(size_t index, bool focused) {
    if (index >= m_fieldFocused.size()) return;
    if (m_fieldFocused[index] != focused) {
        m_fieldFocused[index] = focused;
        invalidate_field_box(index);
    }
}

void AddProfileDialog::set_field_hovered(size_t index, bool hovered) {
    if (index >= m_fieldEditHovered.size()) return;
    if (m_fieldEditHovered[index] != hovered) {
        m_fieldEditHovered[index] = hovered;
        invalidate_field_box(index);
    }
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

        if (dlg.m_hWnd && (msg.hwnd == dlg.m_hWnd || IsChild(dlg.m_hWnd, msg.hwnd))) {
            if (msg.message == WM_KEYDOWN) {
                if (msg.wParam == VK_ESCAPE) {
                    dlg.on_cancel();
                    continue;
                }
                if (msg.wParam == VK_RETURN) {
                    if (GetFocus() == dlg.m_hBtnCancel) {
                        dlg.on_cancel();
                    } else {
                        dlg.on_save();
                    }
                    continue;
                }
            }
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

    if (dlg.m_hWnd && IsWindow(dlg.m_hWnd)) {
        DestroyWindow(dlg.m_hWnd);
        dlg.m_hWnd = nullptr;
    }

    return dlg.m_result;
}

AddProfileDialog::AddProfileDialog(HWND hParent, HINSTANCE hInstance, bool isDarkMode,
                                   const std::wstring& defaultPriIp,
                                   const std::wstring& defaultSecIp)
    : m_hParent(hParent)
    , m_hInstance(hInstance)
    , m_isDarkMode(isDarkMode)
    , m_defaultPriIp(defaultPriIp)
    , m_defaultSecIp(defaultSecIp)
{
    m_dpi = query_window_dpi(m_hParent);
    auto pal = quickdns::GetPalette(m_isDarkMode);
    m_hbrBg = CreateSolidBrush(pal.bg);
    m_hbrCard = CreateSolidBrush(pal.cardBg);
    m_hbrInput = CreateSolidBrush(pal.bg);

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
    if (m_hbrInput) DeleteObject(m_hbrInput);

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

    const wchar_t* headerFace = quickdns::get_display_font_name();

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
    HWND edits[] = { m_hEditName, m_hEditPri, m_hEditSec, m_hEditDoh, m_hEditDesc };
    for (HWND hEd : edits) {
        if (hEd && IsWindow(hEd)) {
            SendMessageW(hEd, WM_SETFONT, reinterpret_cast<WPARAM>(m_hFontRegular), TRUE);
        }
    }
    if (m_hCheckSmartDns && IsWindow(m_hCheckSmartDns)) {
        InvalidateRect(m_hCheckSmartDns, nullptr, FALSE);
    }
    if (m_hBtnCancel && IsWindow(m_hBtnCancel)) {
        InvalidateRect(m_hBtnCancel, nullptr, FALSE);
    }
    if (m_hBtnSave && IsWindow(m_hBtnSave)) {
        InvalidateRect(m_hBtnSave, nullptr, FALSE);
    }
}

bool AddProfileDialog::create_dialog_window() {
    WNDCLASSEXW wc{};
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

    int targetClientW = MulDiv(DIALOG_CLIENT_WIDTH, static_cast<int>(m_dpi), 96);
    int targetClientH = MulDiv(DIALOG_CLIENT_HEIGHT, static_cast<int>(m_dpi), 96);
    RECT wr = { 0, 0, targetClientW, targetClientH };
    adjust_window_rect_for_dpi(&wr, dwStyle, FALSE, dwExStyle, m_dpi);
    int winWidth = wr.right - wr.left;
    int winHeight = wr.bottom - wr.top;

    RECT rcWork = get_monitor_work_area(m_hParent);
    int workW = std::max(280, static_cast<int>(rcWork.right - rcWork.left));
    int workH = std::max(280, static_cast<int>(rcWork.bottom - rcWork.top));
    winWidth = std::min(winWidth, workW);
    winHeight = std::min(winHeight, workH);

    int posX = rcWork.left + (workW - winWidth) / 2;
    int posY = rcWork.top + (workH - winHeight) / 2;
    RECT parentRect{};
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
        HEADER_TITLE_TEXT,
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
        setup_fonts(static_cast<float>(m_dpi) / 96.0f);
    }

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
    float fitScaleH = (clientH > 0) ? (static_cast<float>(clientH) / static_cast<float>(DIALOG_CLIENT_HEIGHT)) : dpiScale;
    float s = std::clamp(std::min(dpiScale, fitScaleH), 0.75f, 3.0f);

    auto sc = [s](int px) -> int {
        return static_cast<int>(std::lround(static_cast<float>(px) * s));
    };

    LayoutMetrics lm{};
    lm.scale = s;
    lm.marginX = sc(20);
    lm.contentW = std::max(sc(220), clientW - 2 * lm.marginX);

    lm.headerTitleY = sc(14);
    lm.headerSubY   = sc(39);
    lm.headerSubH   = sc(20);
    lm.headerSepY   = sc(66);

    lm.cardTop  = sc(80);
    lm.cardPadX = sc(16);
    lm.innerX   = lm.marginX + lm.cardPadX;
    lm.innerW   = std::max(sc(180), lm.contentW - 2 * lm.cardPadX);
    lm.colGap   = sc(14);
    lm.colW     = std::max(sc(80), (lm.innerW - lm.colGap) / 2);

    lm.fieldH         = std::max(sc(30), sc(36));
    lm.innerEditPadX  = sc(12);
    lm.labelOffsetY   = sc(20);

    int measuredFontH = sc(20);
    if (m_hWnd && IsWindow(m_hWnd) && m_hFontRegular) {
        HDC hdc = GetDC(m_hWnd);
        if (hdc) {
            HGDIOBJ oldFont = SelectObject(hdc, m_hFontRegular);
            TEXTMETRICW tm{};
            if (GetTextMetricsW(hdc, &tm) && tm.tmHeight > 0) {
                measuredFontH = static_cast<int>(tm.tmHeight) + sc(2);
            }
            SelectObject(hdc, oldFont);
            ReleaseDC(m_hWnd, hdc);
        }
    }
    lm.innerEditH = std::clamp(measuredFontH, sc(16), std::max(sc(16), lm.fieldH - sc(6)));
    lm.innerEditOffsetY = std::max(2, (lm.fieldH - lm.innerEditH) / 2);

    lm.yName         = lm.cardTop + sc(35);
    lm.yIpRow        = lm.yName + lm.fieldH + sc(35);
    lm.cardInnerSepY = lm.yIpRow + lm.fieldH + sc(16);
    lm.yDoh          = lm.cardInnerSepY + sc(34);
    lm.yDesc         = lm.yDoh + lm.fieldH + sc(35);
    lm.cardH         = (lm.yDesc + lm.fieldH + sc(16)) - lm.cardTop;

    lm.ySmart = lm.cardTop + lm.cardH + sc(14);

    int subTextH = sc(28);
    if (m_hWnd && IsWindow(m_hWnd)) {
        HDC hdc = GetDC(m_hWnd);
        if (hdc) {
            Gdiplus::Graphics g(hdc);
            g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
            Gdiplus::FontFamily ffRegular(L"Segoe UI");
            Gdiplus::Font fontSub(&ffRegular, std::max(9.0f, 11.8f * s), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
            Gdiplus::StringFormat sfSub;
            sfSub.SetTrimming(Gdiplus::StringTrimmingWord);
            sfSub.SetFormatFlags(Gdiplus::StringFormatFlagsNoClip);
            Gdiplus::RectF measuredBounds;
            float maxSubW = static_cast<float>(std::max(sc(80), lm.contentW - sc(62)));
            g.MeasureString(SMARTDNS_SUB_TEXT, -1, &fontSub, Gdiplus::SizeF(maxSubW, 0.0f), &sfSub, &measuredBounds);
            subTextH = std::max(sc(20), static_cast<int>(std::ceil(measuredBounds.Height)));
            ReleaseDC(m_hWnd, hdc);
        }
    }
    lm.smartH = std::max(sc(72), sc(39) + subTextH + sc(11));

    lm.btnH       = std::max(sc(32), sc(38));
    lm.btnCancelW = sc(118);
    lm.btnSaveW   = sc(196);
    lm.btnGap     = sc(10);

    if (lm.btnCancelW + lm.btnSaveW + lm.btnGap > lm.contentW) {
        lm.btnCancelW = std::max(sc(70), (lm.contentW - lm.btnGap) * 2 / 5);
        lm.btnSaveW   = std::max(sc(90), lm.contentW - lm.btnGap - lm.btnCancelW);
    }

    int naturalFooterSepY = lm.ySmart + lm.smartH + sc(16);
    int naturalButtonsY   = naturalFooterSepY + sc(14);
    int bottomMargin      = sc(16);

    if (clientH > naturalButtonsY + lm.btnH + bottomMargin) {
        lm.yButtons   = clientH - lm.btnH - bottomMargin;
        lm.footerSepY = lm.yButtons - sc(14);
    } else {
        lm.footerSepY = naturalFooterSepY;
        lm.yButtons   = naturalButtonsY;
        if (clientH > 0 && lm.yButtons + lm.btnH + sc(8) > clientH) {
            lm.yButtons   = std::max(lm.ySmart + lm.smartH + sc(8), clientH - lm.btnH - sc(8));
            lm.footerSepY = std::max(lm.ySmart + lm.smartH + sc(4), lm.yButtons - sc(8));
        }
    }

    return lm;
}

void AddProfileDialog::attach_edit_effects(HWND hEdit, size_t fieldIndex, UINT_PTR subclassId) {
    if (!hEdit) return;
    auto* state = new EditFieldState();
    state->pDlg = this;
    state->fieldIndex = fieldIndex;
    SetWindowSubclass(hEdit, EditFieldSubclassProc, subclassId, reinterpret_cast<DWORD_PTR>(state));
}

void AddProfileDialog::create_controls() {
    RECT rcClient{};
    GetClientRect(m_hWnd, &rcClient);
    LayoutMetrics lm = compute_layout(rcClient.right - rcClient.left, rcClient.bottom - rcClient.top);
    setup_fonts(lm.scale);
    lm = compute_layout(rcClient.right - rcClient.left, rcClient.bottom - rcClient.top);

    const int fullEditW = std::max(60, lm.innerW - 2 * lm.innerEditPadX);
    const int colEditW  = std::max(50, lm.colW - 2 * lm.innerEditPadX);
    const int col2X     = lm.innerX + lm.colW + lm.colGap;
    const int col2W     = std::max(80, (lm.innerX + lm.innerW) - col2X);
    const int col2EditW = std::max(50, col2W - 2 * lm.innerEditPadX);

    m_hEditName = CreateWindowExW(0, WC_EDITW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        lm.innerX + lm.innerEditPadX, lm.yName + lm.innerEditOffsetY, fullEditW, lm.innerEditH,
        m_hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EDIT_NAME)), m_hInstance, nullptr);
    SendMessageW(m_hEditName, WM_SETFONT, reinterpret_cast<WPARAM>(m_hFontRegular), TRUE);
    SendMessageW(m_hEditName, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Например: DNS-AI"));
    attach_edit_effects(m_hEditName, static_cast<size_t>(FieldIndex::Name), 501);

    m_hEditPri = CreateWindowExW(0, WC_EDITW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        lm.innerX + lm.innerEditPadX, lm.yIpRow + lm.innerEditOffsetY, colEditW, lm.innerEditH,
        m_hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EDIT_PRI_IPV4)), m_hInstance, nullptr);
    SendMessageW(m_hEditPri, WM_SETFONT, reinterpret_cast<WPARAM>(m_hFontRegular), TRUE);
    SendMessageW(m_hEditPri, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Основной IPv4 (обязательно)"));
    attach_edit_effects(m_hEditPri, static_cast<size_t>(FieldIndex::PrimaryIpv4), 502);

    m_hEditSec = CreateWindowExW(0, WC_EDITW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        col2X + lm.innerEditPadX, lm.yIpRow + lm.innerEditOffsetY, col2EditW, lm.innerEditH,
        m_hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EDIT_SEC_IPV4)), m_hInstance, nullptr);
    SendMessageW(m_hEditSec, WM_SETFONT, reinterpret_cast<WPARAM>(m_hFontRegular), TRUE);
    SendMessageW(m_hEditSec, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Вторичный IPv4 (необязательно)"));
    attach_edit_effects(m_hEditSec, static_cast<size_t>(FieldIndex::SecondaryIpv4), 503);

    m_hEditDoh = CreateWindowExW(0, WC_EDITW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        lm.innerX + lm.innerEditPadX, lm.yDoh + lm.innerEditOffsetY, fullEditW, lm.innerEditH,
        m_hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EDIT_DOH)), m_hInstance, nullptr);
    SendMessageW(m_hEditDoh, WM_SETFONT, reinterpret_cast<WPARAM>(m_hFontRegular), TRUE);
    SendMessageW(m_hEditDoh, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"https://.../dns-query (необязательно)"));
    attach_edit_effects(m_hEditDoh, static_cast<size_t>(FieldIndex::DohTemplate), 504);

    m_hEditDesc = CreateWindowExW(0, WC_EDITW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        lm.innerX + lm.innerEditPadX, lm.yDesc + lm.innerEditOffsetY, fullEditW, lm.innerEditH,
        m_hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EDIT_DESC)), m_hInstance, nullptr);
    SendMessageW(m_hEditDesc, WM_SETFONT, reinterpret_cast<WPARAM>(m_hFontRegular), TRUE);
    SendMessageW(m_hEditDesc, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Краткое описание или сервисы (необязательно)"));
    attach_edit_effects(m_hEditDesc, static_cast<size_t>(FieldIndex::Description), 505);

    m_hCheckSmartDns = CreateWindowExW(0, WC_BUTTONW, SMARTDNS_CARD_TITLE,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        lm.marginX, lm.ySmart, lm.contentW, lm.smartH,
        m_hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_CHECK_SMARTDNS)), m_hInstance, nullptr);

    auto* chkState = new SmartDnsCardState();
    chkState->pDlg = this;
    SetPropW(m_hCheckSmartDns, L"QuickDnsSmartCard", reinterpret_cast<HANDLE>(chkState));
    SetWindowSubclass(m_hCheckSmartDns, SmartDnsCardSubclassProc, 401, reinterpret_cast<DWORD_PTR>(chkState));

    const int saveX   = lm.marginX + lm.contentW - lm.btnSaveW;
    const int cancelX = saveX - lm.btnGap - lm.btnCancelW;

    m_hBtnCancel = CreateWindowExW(0, WC_BUTTONW, L"Отмена",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        cancelX, lm.yButtons, lm.btnCancelW, lm.btnH,
        m_hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_CANCEL_PROFILE)), m_hInstance, nullptr);

    m_hBtnSave = CreateWindowExW(0, WC_BUTTONW, L"Сохранить профиль",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        saveX, lm.yButtons, lm.btnSaveW, lm.btnH,
        m_hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_SAVE_PROFILE)), m_hInstance, nullptr);

    attach_button_effects(m_hBtnCancel, 302);
    attach_button_effects(m_hBtnSave, 301);

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
    RECT rcClient{};
    GetClientRect(m_hWnd, &rcClient);
    int clientW = rcClient.right - rcClient.left;
    int clientH = rcClient.bottom - rcClient.top;
    if (clientW < 200 || clientH < 200) return;

    LayoutMetrics lm = compute_layout(clientW, clientH);
    float prevScale = m_activeFontScale;
    setup_fonts(lm.scale);
    if (std::abs(prevScale - m_activeFontScale) >= 0.005f) {
        apply_fonts_to_controls();
        lm = compute_layout(clientW, clientH);
    }

    const int fullEditW = std::max(60, lm.innerW - 2 * lm.innerEditPadX);
    const int colEditW  = std::max(50, lm.colW - 2 * lm.innerEditPadX);
    const int col2X     = lm.innerX + lm.colW + lm.colGap;
    const int col2W     = std::max(80, (lm.innerX + lm.innerW) - col2X);
    const int col2EditW = std::max(50, col2W - 2 * lm.innerEditPadX);

    m_fieldRects[static_cast<size_t>(FieldIndex::Name)]          = { lm.innerX, lm.yName,  lm.innerX + lm.innerW, lm.yName + lm.fieldH };
    m_fieldRects[static_cast<size_t>(FieldIndex::PrimaryIpv4)]   = { lm.innerX, lm.yIpRow, lm.innerX + lm.colW,   lm.yIpRow + lm.fieldH };
    m_fieldRects[static_cast<size_t>(FieldIndex::SecondaryIpv4)] = { col2X,     lm.yIpRow, col2X + col2W,         lm.yIpRow + lm.fieldH };
    m_fieldRects[static_cast<size_t>(FieldIndex::DohTemplate)]   = { lm.innerX, lm.yDoh,   lm.innerX + lm.innerW, lm.yDoh + lm.fieldH };
    m_fieldRects[static_cast<size_t>(FieldIndex::Description)]   = { lm.innerX, lm.yDesc,  lm.innerX + lm.innerW, lm.yDesc + lm.fieldH };

    if (m_hEditName) SetWindowPos(m_hEditName, nullptr, lm.innerX + lm.innerEditPadX, lm.yName + lm.innerEditOffsetY, fullEditW, lm.innerEditH, SWP_NOZORDER);
    if (m_hEditPri)  SetWindowPos(m_hEditPri,  nullptr, lm.innerX + lm.innerEditPadX, lm.yIpRow + lm.innerEditOffsetY, colEditW, lm.innerEditH, SWP_NOZORDER);
    if (m_hEditSec)  SetWindowPos(m_hEditSec,  nullptr, col2X + lm.innerEditPadX,     lm.yIpRow + lm.innerEditOffsetY, col2EditW, lm.innerEditH, SWP_NOZORDER);
    if (m_hEditDoh)  SetWindowPos(m_hEditDoh,  nullptr, lm.innerX + lm.innerEditPadX, lm.yDoh + lm.innerEditOffsetY, fullEditW, lm.innerEditH, SWP_NOZORDER);
    if (m_hEditDesc) SetWindowPos(m_hEditDesc, nullptr, lm.innerX + lm.innerEditPadX, lm.yDesc + lm.innerEditOffsetY, fullEditW, lm.innerEditH, SWP_NOZORDER);

    m_smartCardRect = { lm.marginX, lm.ySmart, lm.marginX + lm.contentW, lm.ySmart + lm.smartH };
    if (m_hCheckSmartDns) {
        SetWindowPos(m_hCheckSmartDns, nullptr, lm.marginX, lm.ySmart, lm.contentW, lm.smartH, SWP_NOZORDER);
    }

    const int saveX   = lm.marginX + lm.contentW - lm.btnSaveW;
    const int cancelX = saveX - lm.btnGap - lm.btnCancelW;
    if (m_hBtnCancel) SetWindowPos(m_hBtnCancel, nullptr, cancelX, lm.yButtons, lm.btnCancelW, lm.btnH, SWP_NOZORDER);
    if (m_hBtnSave)   SetWindowPos(m_hBtnSave,   nullptr, saveX,   lm.yButtons, lm.btnSaveW,   lm.btnH, SWP_NOZORDER);

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
    bool hasFocus = ((pDis->itemState & ODS_FOCUS) != 0) || (GetFocus() == pDis->hwndItem);

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
            baseBorder = hasFocus ? pal.accentBlue : pal.btnUtilBorder;
            hoverBorder = m_isDarkMode ? RGB(75, 90, 115) : RGB(175, 192, 218);
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

        const float radius = dpf(7.0f);
        Gdiplus::GraphicsPath path;
        quickdns::AddRoundedRect(path, Gdiplus::RectF(0.5f, 0.5f, static_cast<float>(w) - 1.0f, static_cast<float>(h) - 1.0f), radius);

        Gdiplus::SolidBrush br(Gdiplus::Color(255, GetRValue(curBg), GetGValue(curBg), GetBValue(curBg)));
        g.FillPath(&br, &path);

        float penW = (isPrimary || hasFocus) ? dpf(1.4f) : dpf(1.0f);
        Gdiplus::Pen pen(Gdiplus::Color(255, GetRValue(curBorder), GetGValue(curBorder), GetBValue(curBorder)), penW);
        g.DrawPath(&pen, &path);

        if (hasFocus && !isDisabled) {
            Gdiplus::GraphicsPath focusPath;
            quickdns::AddRoundedRect(focusPath,
                Gdiplus::RectF(dpf(2.5f), dpf(2.5f), static_cast<float>(w) - dpf(5.0f), static_cast<float>(h) - dpf(5.0f)),
                std::max(dpf(2.0f), radius - dpf(2.0f)));
            Gdiplus::Color focusCol = isPrimary
                ? Gdiplus::Color(150, 255, 255, 255)
                : Gdiplus::Color(130, GetRValue(pal.accentBlue), GetGValue(pal.accentBlue), GetBValue(pal.accentBlue));
            Gdiplus::Pen focusPen(focusCol, 1.0f);
            focusPen.SetDashStyle(Gdiplus::DashStyleDot);
            g.DrawPath(&focusPen, &focusPath);
        }
    }

    wchar_t btnText[128] = {};
    GetWindowTextW(pDis->hwndItem, btnText, static_cast<int>(std::size(btnText)));

    SetBkMode(hdcMem, TRANSPARENT);
    SetTextColor(hdcMem, textColor);
    HFONT hFont = isPrimary ? m_hFontBold : m_hFontRegular;
    HGDIOBJ oldFont = SelectObject(hdcMem, hFont);

    RECT rcText = rcLocal;
    if (isPressed && !isDisabled) {
        int off = std::max(1, dp(1));
        rcText.left += off;
        rcText.top += off;
        rcText.right += off;
        rcText.bottom += off;
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
    wchar_t dohBuf[512] = {};
    wchar_t descBuf[256] = {};

    GetWindowTextW(m_hEditName, nameBuf, static_cast<int>(std::size(nameBuf)));
    GetWindowTextW(m_hEditPri,  priBuf,  static_cast<int>(std::size(priBuf)));
    GetWindowTextW(m_hEditSec,  secBuf,  static_cast<int>(std::size(secBuf)));
    GetWindowTextW(m_hEditDoh,  dohBuf,  static_cast<int>(std::size(dohBuf)));
    GetWindowTextW(m_hEditDesc, descBuf, static_cast<int>(std::size(descBuf)));

    std::wstring nameStr = nameBuf;
    std::wstring priStr  = priBuf;
    std::wstring secStr  = secBuf;
    std::wstring dohStr  = dohBuf;
    std::wstring descStr = descBuf;

    auto trim = [](std::wstring& s) {
        while (!s.empty() && std::iswspace(static_cast<wint_t>(s.front()))) s.erase(s.begin());
        while (!s.empty() && std::iswspace(static_cast<wint_t>(s.back()))) s.pop_back();
    };
    trim(nameStr);
    trim(priStr);
    trim(secStr);
    trim(dohStr);
    trim(descStr);

    m_fieldErrors.fill(false);

    if (nameStr.empty()) {
        m_fieldErrors[static_cast<size_t>(FieldIndex::Name)] = true;
        invalidate_field_box(static_cast<size_t>(FieldIndex::Name));
        MessageBoxW(m_hWnd, L"Пожалуйста, введите название профиля.", L"Ошибка ввода", MB_OK | MB_ICONWARNING);
        SetFocus(m_hEditName);
        return;
    }

    if (priStr.empty() || !quickdns::DnsManager::is_valid_ipv4(priStr)) {
        m_fieldErrors[static_cast<size_t>(FieldIndex::PrimaryIpv4)] = true;
        invalidate_field_box(static_cast<size_t>(FieldIndex::PrimaryIpv4));
        MessageBoxW(m_hWnd, L"Пожалуйста, укажите корректный основной IPv4 адрес (например, 194.87.196.13 или 94.140.14.14).", L"Некорректный IP", MB_OK | MB_ICONWARNING);
        SetFocus(m_hEditPri);
        return;
    }

    if (!secStr.empty() && !quickdns::DnsManager::is_valid_ipv4(secStr)) {
        m_fieldErrors[static_cast<size_t>(FieldIndex::SecondaryIpv4)] = true;
        invalidate_field_box(static_cast<size_t>(FieldIndex::SecondaryIpv4));
        MessageBoxW(m_hWnd, L"Вторичный адрес должен быть корректным IPv4 адресом (или оставьте это поле пустым).", L"Некорректный IP", MB_OK | MB_ICONWARNING);
        SetFocus(m_hEditSec);
        return;
    }

    if (!dohStr.empty() && !quickdns::DnsManager::is_valid_doh_template(dohStr)) {
        m_fieldErrors[static_cast<size_t>(FieldIndex::DohTemplate)] = true;
        invalidate_field_box(static_cast<size_t>(FieldIndex::DohTemplate));
        MessageBoxW(m_hWnd, L"Некорректный адрес DoH-шаблона. Шаблон должен начинаться с https:// и содержать корректный URL.", L"Ошибка ввода", MB_OK | MB_ICONWARNING);
        SetFocus(m_hEditDoh);
        return;
    }

    if (secStr == priStr) {
        secStr.clear();
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

    m_result = std::move(profile);
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
    case DM_GETDEFID:
        return MAKELONG(IDC_BTN_SAVE_PROFILE, DC_HASDEFID);

    case DM_SETDEFID:
        return TRUE;

    case WM_GETMINMAXINFO: {
        auto* pMMI = reinterpret_cast<MINMAXINFO*>(lParam);
        RECT rcMin = { 0, 0, MulDiv(480, static_cast<int>(m_dpi), 96), MulDiv(530, static_cast<int>(m_dpi), 96) };
        adjust_window_rect_for_dpi(&rcMin, static_cast<DWORD>(GetWindowLongW(hWnd, GWL_STYLE)),
                                   FALSE, static_cast<DWORD>(GetWindowLongW(hWnd, GWL_EXSTYLE)), m_dpi);
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
            for (const auto& rcBox : m_fieldRects) {
                if (PtInRect(&rcBox, pt)) {
                    SetCursor(LoadCursorW(nullptr, IDC_IBEAM));
                    return TRUE;
                }
            }
            if (PtInRect(&m_smartCardRect, pt)) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
        }
        break;
    }

    case WM_MOUSEMOVE: {
        if (!m_trackingMouseLeave) {
            m_trackingMouseLeave = true;
            TRACKMOUSEEVENT tme{};
            tme.cbSize = sizeof(TRACKMOUSEEVENT);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hWnd;
            TrackMouseEvent(&tme);
        }
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        int newHovered = -1;
        for (size_t i = 0; i < m_fieldRects.size(); ++i) {
            if (PtInRect(&m_fieldRects[i], pt)) {
                newHovered = static_cast<int>(i);
                break;
            }
        }
        if (newHovered != m_hoveredContainerField) {
            int prev = m_hoveredContainerField;
            m_hoveredContainerField = newHovered;
            if (prev >= 0) invalidate_field_box(static_cast<size_t>(prev));
            if (newHovered >= 0) invalidate_field_box(static_cast<size_t>(newHovered));
        }
        break;
    }

    case WM_MOUSELEAVE: {
        m_trackingMouseLeave = false;
        if (m_hoveredContainerField >= 0) {
            int prev = m_hoveredContainerField;
            m_hoveredContainerField = -1;
            invalidate_field_box(static_cast<size_t>(prev));
        }
        break;
    }

    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        HWND edits[] = { m_hEditName, m_hEditPri, m_hEditSec, m_hEditDoh, m_hEditDesc };
        for (size_t i = 0; i < m_fieldRects.size(); ++i) {
            if (PtInRect(&m_fieldRects[i], pt)) {
                if (edits[i] && IsWindow(edits[i])) {
                    SetFocus(edits[i]);
                }
                return 0;
            }
        }
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
            const float s = lm.scale;

            Gdiplus::FontFamily ffRegular(L"Segoe UI");
            Gdiplus::FontFamily ffDisplay(L"Segoe UI Variable Display");
            const Gdiplus::FontFamily* pDisplay = ffDisplay.IsAvailable() ? &ffDisplay : &ffRegular;

            Gdiplus::Font fontHeader(pDisplay, 18.0f * s, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
            Gdiplus::Font fontSub(&ffRegular, 12.0f * s, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
            Gdiplus::Font fontLabel(&ffRegular, 12.3f * s, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
            Gdiplus::Font fontHint(&ffRegular, 11.2f * s, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
            Gdiplus::Font fontBadge(&ffRegular, 10.8f * s, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);

            Gdiplus::SolidBrush textPriBr(Gdiplus::Color(255, GetRValue(pal.textPrimary), GetGValue(pal.textPrimary), GetBValue(pal.textPrimary)));
            Gdiplus::SolidBrush textSecBr(Gdiplus::Color(255, GetRValue(pal.textSecondary), GetGValue(pal.textSecondary), GetBValue(pal.textSecondary)));
            Gdiplus::SolidBrush textMutedBr(Gdiplus::Color(255, GetRValue(pal.textMuted), GetGValue(pal.textMuted), GetBValue(pal.textMuted)));
            Gdiplus::SolidBrush accentBlueBr(Gdiplus::Color(255, GetRValue(pal.accentBlue), GetGValue(pal.accentBlue), GetBValue(pal.accentBlue)));

            // 1. Top Header Bar (Title + Live Mode Pill + Subtitle + Bottom Separator)
            g.DrawString(HEADER_TITLE_TEXT, -1, &fontHeader,
                Gdiplus::PointF(static_cast<float>(lm.marginX), static_cast<float>(lm.headerTitleY)), &textPriBr);

            {
                const wchar_t* modeBadgeLabel = m_isSmartDns ? L"SmartDNS профиль" : L"Кастомный DNS";
                Gdiplus::StringFormat sfCenter(Gdiplus::StringFormat::GenericTypographic());
                sfCenter.SetAlignment(Gdiplus::StringAlignmentCenter);
                sfCenter.SetLineAlignment(Gdiplus::StringAlignmentCenter);

                Gdiplus::RectF titleBounds;
                g.MeasureString(HEADER_TITLE_TEXT, -1, &fontHeader, Gdiplus::PointF(0.0f, 0.0f), &titleBounds);

                Gdiplus::RectF badgeBounds;
                g.MeasureString(modeBadgeLabel, -1, &fontBadge, Gdiplus::PointF(0.0f, 0.0f), &sfCenter, &badgeBounds);

                float pillW = badgeBounds.Width + 20.0f * s;
                float pillH = 22.0f * s;
                float pillX = static_cast<float>(lm.marginX + lm.contentW) - pillW;
                float pillY = static_cast<float>(lm.headerTitleY) + 1.0f * s;

                if (pillX > static_cast<float>(lm.marginX) + titleBounds.Width + 12.0f * s) {
                    Gdiplus::RectF pillRc(pillX, pillY, pillW, pillH);
                    Gdiplus::GraphicsPath pillPath;
                    quickdns::AddRoundedRect(pillPath, pillRc, 5.0f * s);

                    Gdiplus::Color pFill = m_isSmartDns
                        ? (m_isDarkMode ? Gdiplus::Color(46, 34, 197, 94) : Gdiplus::Color(30, 22, 163, 74))
                        : (m_isDarkMode ? Gdiplus::Color(38, 59, 130, 246) : Gdiplus::Color(24, 37, 99, 235));
                    Gdiplus::Color pBorder = m_isSmartDns
                        ? (m_isDarkMode ? Gdiplus::Color(150, 34, 197, 94) : Gdiplus::Color(140, 22, 163, 74))
                        : (m_isDarkMode ? Gdiplus::Color(130, 96, 165, 250) : Gdiplus::Color(115, 52, 98, 150));
                    Gdiplus::Color pText = m_isSmartDns
                        ? (m_isDarkMode ? Gdiplus::Color(255, 74, 222, 128) : Gdiplus::Color(255, 21, 128, 61))
                        : Gdiplus::Color(255, GetRValue(pal.accentBlue), GetGValue(pal.accentBlue), GetBValue(pal.accentBlue));

                    Gdiplus::SolidBrush pFillBr(pFill);
                    Gdiplus::Pen pPen(pBorder, 1.0f);
                    Gdiplus::SolidBrush pTxtBr(pText);
                    g.FillPath(&pFillBr, &pillPath);
                    g.DrawPath(&pPen, &pillPath);
                    g.DrawString(modeBadgeLabel, -1, &fontBadge, pillRc, &sfCenter, &pTxtBr);
                }
            }

            Gdiplus::StringFormat sfSingle;
            sfSingle.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
            sfSingle.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);

            g.DrawString(HEADER_SUB_TEXT, -1, &fontSub,
                Gdiplus::RectF(static_cast<float>(lm.marginX), static_cast<float>(lm.headerSubY),
                               static_cast<float>(lm.contentW), static_cast<float>(lm.headerSubH)),
                &sfSingle, &textSecBr);

            Gdiplus::Pen sepPen(Gdiplus::Color(255, GetRValue(pal.separator), GetGValue(pal.separator), GetBValue(pal.separator)), 1.0f);
            g.DrawLine(&sepPen,
                static_cast<float>(lm.marginX), static_cast<float>(lm.headerSepY) + 0.5f,
                static_cast<float>(lm.marginX + lm.contentW), static_cast<float>(lm.headerSepY) + 0.5f);

            // 2. Main Configuration GDI+ Card
            Gdiplus::RectF mainCardRc(
                static_cast<float>(lm.marginX) + 0.5f,
                static_cast<float>(lm.cardTop) + 0.5f,
                static_cast<float>(lm.contentW) - 1.0f,
                static_cast<float>(lm.cardH) - 1.0f);
            Gdiplus::GraphicsPath mainCardPath;
            quickdns::AddRoundedRect(mainCardPath, mainCardRc, 10.0f * s);

            Gdiplus::SolidBrush cardBgBr(Gdiplus::Color(255, GetRValue(pal.cardBg), GetGValue(pal.cardBg), GetBValue(pal.cardBg)));
            Gdiplus::Pen cardBorderPen(Gdiplus::Color(255, GetRValue(pal.cardBorder), GetGValue(pal.cardBorder), GetBValue(pal.cardBorder)), 1.0f);
            g.FillPath(&cardBgBr, &mainCardPath);
            g.DrawPath(&cardBorderPen, &mainCardPath);

            // Subtle inner horizontal divider separating IPv4 addresses from DoH & Description
            Gdiplus::Pen innerSepPen(Gdiplus::Color(85, GetRValue(pal.separator), GetGValue(pal.separator), GetBValue(pal.separator)), 1.0f);
            g.DrawLine(&innerSepPen,
                static_cast<float>(lm.innerX), static_cast<float>(lm.cardInnerSepY) + 0.5f,
                static_cast<float>(lm.innerX + lm.innerW), static_cast<float>(lm.cardInnerSepY) + 0.5f);

            Gdiplus::SolidBrush inputBgBr(Gdiplus::Color(255, GetRValue(pal.bg), GetGValue(pal.bg), GetBValue(pal.bg)));
            Gdiplus::StringFormat sfRight;
            sfRight.SetAlignment(Gdiplus::StringAlignmentFar);
            sfRight.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
            sfRight.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);

            auto drawInputBox = [&](FieldIndex idx, const wchar_t* label, const wchar_t* rightHint, bool isRequired) {
                const size_t i = static_cast<size_t>(idx);
                const RECT& box = m_fieldRects[i];
                const float bx = static_cast<float>(box.left);
                const float by = static_cast<float>(box.top);
                const float bw = static_cast<float>(box.right - box.left);
                const float bh = static_cast<float>(box.bottom - box.top);

                const float lblY = by - static_cast<float>(lm.labelOffsetY);
                g.DrawString(label, -1, &fontLabel, Gdiplus::PointF(bx + 2.0f * s, lblY), &textPriBr);

                if (rightHint && rightHint[0] != L'\0') {
                    Gdiplus::RectF lblBounds;
                    g.MeasureString(label, -1, &fontLabel, Gdiplus::PointF(0.0f, 0.0f), &lblBounds);
                    float hintLeft = bx + lblBounds.Width + 8.0f * s;
                    float hintW = (bx + bw - 2.0f * s) - hintLeft;
                    if (hintW > 40.0f * s) {
                        const Gdiplus::Brush* pHintBr = isRequired ? &accentBlueBr : &textMutedBr;
                        g.DrawString(rightHint, -1, &fontHint,
                            Gdiplus::RectF(hintLeft, lblY + 1.0f * s, hintW, 18.0f * s),
                            &sfRight, pHintBr);
                    }
                }

                const bool isFocused = m_fieldFocused[i];
                const bool isHovered = m_fieldEditHovered[i] || (m_hoveredContainerField == static_cast<int>(i));
                const bool hasError  = m_fieldErrors[i];

                // Soft outer focus / validation glow
                if (hasError || isFocused) {
                    COLORREF glowRef = hasError ? pal.accentRed : pal.accentBlue;
                    Gdiplus::RectF glowRc(bx - 1.5f * s, by - 1.5f * s, bw + 3.0f * s, bh + 3.0f * s);
                    Gdiplus::GraphicsPath glowPath;
                    quickdns::AddRoundedRect(glowPath, glowRc, 7.5f * s);
                    Gdiplus::Pen glowPen(Gdiplus::Color(m_isDarkMode ? 45 : 32, GetRValue(glowRef), GetGValue(glowRef), GetBValue(glowRef)), 2.5f * s);
                    g.DrawPath(&glowPen, &glowPath);
                }

                Gdiplus::RectF rcInput(bx + 0.5f, by + 0.5f, bw - 1.0f, bh - 1.0f);
                Gdiplus::GraphicsPath inputPath;
                quickdns::AddRoundedRect(inputPath, rcInput, 6.0f * s);
                g.FillPath(&inputBgBr, &inputPath);

                COLORREF borderCol = pal.cardBorder;
                float borderW = 1.0f;
                if (hasError) {
                    borderCol = pal.accentRed;
                    borderW = 1.5f * s;
                } else if (isFocused) {
                    borderCol = m_isDarkMode ? RGB(96, 165, 250) : RGB(37, 99, 235);
                    borderW = 1.5f * s;
                } else if (isHovered) {
                    borderCol = m_isDarkMode ? RGB(78, 94, 122) : RGB(168, 186, 214);
                    borderW = 1.2f * s;
                }

                Gdiplus::Pen borderPen(Gdiplus::Color(255, GetRValue(borderCol), GetGValue(borderCol), GetBValue(borderCol)), borderW);
                g.DrawPath(&borderPen, &inputPath);
            };

            drawInputBox(FieldIndex::Name,          L"Название профиля",           L"Обязательно",            true);
            drawInputBox(FieldIndex::PrimaryIpv4,   L"Основной DNS IPv4",          L"Обязательно",            true);
            drawInputBox(FieldIndex::SecondaryIpv4, L"Вторичный DNS IPv4",         L"Необязательно",          false);
            drawInputBox(FieldIndex::DohTemplate,   L"DNS over HTTPS (DoH) шаблон", L"Необязательно \u2022 Win11+", false);
            drawInputBox(FieldIndex::Description,   L"Описание или заметка",       L"Необязательно",          false);

            // 3. Footer Action Bar Separator + Keyboard Shortcut Hint
            g.DrawLine(&sepPen,
                static_cast<float>(lm.marginX), static_cast<float>(lm.footerSepY) + 0.5f,
                static_cast<float>(lm.marginX + lm.contentW), static_cast<float>(lm.footerSepY) + 0.5f);

            const int saveX = lm.marginX + lm.contentW - lm.btnSaveW;
            const int cancelX = saveX - lm.btnGap - lm.btnCancelW;
            const int availHintW = cancelX - lm.marginX - static_cast<int>(12.0f * s);
            if (availHintW >= static_cast<int>(165.0f * s)) {
                Gdiplus::StringFormat sfVCenter;
                sfVCenter.SetLineAlignment(Gdiplus::StringAlignmentCenter);
                sfVCenter.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
                sfVCenter.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
                g.DrawString(L"Enter \u2014 сохранить  \u2022  Esc \u2014 отмена", -1, &fontHint,
                    Gdiplus::RectF(static_cast<float>(lm.marginX), static_cast<float>(lm.yButtons),
                                   static_cast<float>(availHintW), static_cast<float>(lm.btnH)),
                    &sfVCenter, &textMutedBr);
            }
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

        if (code == EN_CHANGE) {
            int errIdx = -1;
            if (id == IDC_EDIT_NAME)     errIdx = static_cast<int>(FieldIndex::Name);
            if (id == IDC_EDIT_PRI_IPV4) errIdx = static_cast<int>(FieldIndex::PrimaryIpv4);
            if (id == IDC_EDIT_SEC_IPV4) errIdx = static_cast<int>(FieldIndex::SecondaryIpv4);
            if (id == IDC_EDIT_DOH)      errIdx = static_cast<int>(FieldIndex::DohTemplate);
            if (id == IDC_EDIT_DESC)     errIdx = static_cast<int>(FieldIndex::Description);
            if (errIdx >= 0 && m_fieldErrors[static_cast<size_t>(errIdx)]) {
                m_fieldErrors[static_cast<size_t>(errIdx)] = false;
                invalidate_field_box(static_cast<size_t>(errIdx));
            }
            return 0;
        }

        if (id == IDC_CHECK_SMARTDNS && code == BN_CLICKED) {
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
        auto pal = quickdns::GetPalette(m_isDarkMode);
        SetTextColor(hdcStatic, pal.textPrimary);
        SetBkMode(hdcStatic, TRANSPARENT);
        return reinterpret_cast<INT_PTR>(m_hbrBg);
    }

    case WM_CTLCOLOREDIT: {
        HDC hdcEdit = reinterpret_cast<HDC>(wParam);
        auto pal = quickdns::GetPalette(m_isDarkMode);
        SetBkMode(hdcEdit, OPAQUE);
        SetTextColor(hdcEdit, pal.textPrimary);
        SetBkColor(hdcEdit, pal.bg);
        return reinterpret_cast<INT_PTR>(m_hbrInput);
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

    default:
        break;
    }

    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}
