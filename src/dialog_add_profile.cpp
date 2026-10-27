#include "dialog_add_profile.h"
#include "dns_manager.h"
#include "encoding_utils.h"
#include "ui_palette.h"
#include "resource.h"
#include <dwmapi.h>
#include <windowsx.h>
#include <commctrl.h>
#include <cmath>

using QuickDns::Encoding::wide_to_utf8;

namespace {
    constexpr const wchar_t* DIALOG_CLASS_NAME = L"QuickDNS_AddProfileDialogClass";
    constexpr int DIALOG_WIDTH = 580;
    constexpr int DIALOG_HEIGHT = 650;

    constexpr int ID_EDIT_NAME = 3001;
    constexpr int ID_EDIT_PRI  = 3002;
    constexpr int ID_EDIT_SEC  = 3003;
    constexpr int ID_EDIT_DESC = 3004;
    constexpr int ID_EDIT_DOH  = 3005;
    constexpr int ID_BTN_SAVE  = 3006;
    constexpr int ID_BTN_CANCEL= 3007;
    constexpr int ID_CHECK_SMARTDNS = 3008;

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
        if (!state) return DefSubclassProc(hWnd, uMsg, wParam, lParam);

        switch (uMsg) {
        case WM_SETCURSOR: {
            if (LOWORD(lParam) == HTCLIENT && IsWindowEnabled(hWnd)) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            break;
        }
        case WM_MOUSEMOVE: {
            if (!state->isHovered) {
                state->isHovered = true;
                TRACKMOUSEEVENT tme = { sizeof(TRACKMOUSEEVENT), TME_LEAVE, hWnd, 0 };
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
        case WM_LBUTTONDOWN: {
            state->isPressed = true;
            SetCapture(hWnd);
            InvalidateRect(hWnd, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONUP: {
            if (state->isPressed) {
                state->isPressed = false;
                ReleaseCapture();
                RECT rc;
                GetClientRect(hWnd, &rc);
                POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
                if (PtInRect(&rc, pt)) {
                    bool isChecked = (SendMessageW(hWnd, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    SendMessageW(hWnd, BM_SETCHECK, isChecked ? BST_UNCHECKED : BST_CHECKED, 0);
                }
                InvalidateRect(hWnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_KEYUP: {
            if (wParam == VK_SPACE) {
                bool isChecked = (SendMessageW(hWnd, BM_GETCHECK, 0, 0) == BST_CHECKED);
                SendMessageW(hWnd, BM_SETCHECK, isChecked ? BST_UNCHECKED : BST_CHECKED, 0);
                InvalidateRect(hWnd, nullptr, FALSE);
                return 0;
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

            bool isDark = state->pDlg->is_dark_mode();
            auto pal = quickdns::GetPalette(isDark);

            HBRUSH hbrCard = CreateSolidBrush(pal.cardBg);
            FillRect(hdcMem, &rc, hbrCard);
            DeleteObject(hbrCard);

            bool isChecked = (SendMessageW(hWnd, BM_GETCHECK, 0, 0) == BST_CHECKED);

            {
                Gdiplus::Graphics g(hdcMem);
                g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

                int boxSize = 18;
                int boxX = 2;
                int boxY = (h - boxSize) / 2;
                Gdiplus::RectF boxRc(static_cast<float>(boxX) + 0.5f, static_cast<float>(boxY) + 0.5f, static_cast<float>(boxSize) - 1.0f, static_cast<float>(boxSize) - 1.0f);
                Gdiplus::GraphicsPath boxPath;
                quickdns::AddRoundedRect(boxPath, boxRc, 4.0f);

                if (isChecked) {
                    Gdiplus::SolidBrush checkBgBr(Gdiplus::Color(255, 34, 197, 94));
                    g.FillPath(&checkBgBr, &boxPath);

                    Gdiplus::Pen checkPen(Gdiplus::Color(255, 255, 255, 255), 2.2f);
                    checkPen.SetStartCap(Gdiplus::LineCapRound);
                    checkPen.SetEndCap(Gdiplus::LineCapRound);
                    g.DrawLine(&checkPen, boxX + 4.5f, boxY + 9.5f, boxX + 7.5f, boxY + 13.0f);
                    g.DrawLine(&checkPen, boxX + 7.5f, boxY + 13.0f, boxX + 13.5f, boxY + 5.5f);
                } else {
                    Gdiplus::SolidBrush uncheckBgBr(Gdiplus::Color(255, 22, 25, 31));
                    g.FillPath(&uncheckBgBr, &boxPath);
                    COLORREF borderCol = state->isHovered ? RGB(96, 165, 250) : pal.cardBorder;
                    Gdiplus::Pen uncheckPen(Gdiplus::Color(255, GetRValue(borderCol), GetGValue(borderCol), GetBValue(borderCol)), 1.5f);
                    g.DrawPath(&uncheckPen, &boxPath);
                }
            }

            RECT rcText = { 28, 0, w, h };
            SetBkMode(hdcMem, TRANSPARENT);
            SetTextColor(hdcMem, pal.textPrimary);
            HGDIOBJ oldFont = SelectObject(hdcMem, state->pDlg->get_font_bold());
            DrawTextW(hdcMem, L"Профиль SmartDNS (разблокировка сервисов и медиа)", -1, &rcText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
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

std::optional<DnsProfile> AddProfileDialog::show(HWND hParent, HINSTANCE hInstance, bool isDarkMode) {
    AddProfileDialog dlg(hParent, hInstance, isDarkMode);
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

    return dlg.m_result;
}

AddProfileDialog::AddProfileDialog(HWND hParent, HINSTANCE hInstance, bool isDarkMode)
    : m_hParent(hParent), m_hInstance(hInstance), m_isDarkMode(isDarkMode)
{
    auto pal = quickdns::GetPalette(m_isDarkMode);
    m_hbrBg = CreateSolidBrush(pal.bg);
    m_hbrCard = CreateSolidBrush(pal.cardBg);

    m_hFontRegular = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    m_hFontBold = CreateFontW(-14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    m_hFontHeader = CreateFontW(-18, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable Display");

    m_hFontSmall = CreateFontW(-11, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    m_hFontSmallBold = CreateFontW(-11, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
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

bool AddProfileDialog::create_dialog_window() {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &AddProfileDialog::dialog_proc_thunk;
    wc.hInstance = m_hInstance;
    wc.hIcon = LoadIconW(m_hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
    if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm = LoadIconW(m_hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr; // Handled in WM_ERASEBKGND / WM_PAINT
    wc.lpszClassName = DIALOG_CLASS_NAME;
    RegisterClassExW(&wc);

    RECT wr = { 0, 0, DIALOG_WIDTH, DIALOG_HEIGHT };
    DWORD dwStyle = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MAXIMIZEBOX | WS_MINIMIZEBOX | WS_VISIBLE;
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

    HICON hIcon = LoadIconW(m_hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
    if (hIcon) {
        SendMessageW(m_hWnd, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
        SendMessageW(m_hWnd, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);
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

void AddProfileDialog::create_controls() {
    RECT rcClient = {};
    GetClientRect(m_hWnd, &rcClient);
    int clientWidth = rcClient.right - rcClient.left;
    int marginX = 24;
    int contentWidth = clientWidth - (2 * marginX);

    int fieldH = 38;
    int innerEditH = 24;
    int innerOffset = 7;
    int editW = contentWidth - 20;

    // 1. Name Edit Box (Y = 100)
    int yName = 100;
    m_hEditName = CreateWindowExW(0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        marginX + 10, yName + innerOffset, editW, innerEditH,
        m_hWnd, (HMENU)ID_EDIT_NAME, m_hInstance, nullptr);
    SendMessageW(m_hEditName, WM_SETFONT, (WPARAM)m_hFontRegular, TRUE);
    SendMessageW(m_hEditName, EM_SETCUEBANNER, TRUE, (LPARAM)L"Например: AdGuard DNS, Мой DNS, Офисный сервер");

    // 2. Primary IPv4 Edit Box (Y = 170)
    int yPri = 170;
    m_hEditPri = CreateWindowExW(0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        marginX + 10, yPri + innerOffset, editW, innerEditH,
        m_hWnd, (HMENU)ID_EDIT_PRI, m_hInstance, nullptr);
    SendMessageW(m_hEditPri, WM_SETFONT, (WPARAM)m_hFontRegular, TRUE);
    SendMessageW(m_hEditPri, EM_SETCUEBANNER, TRUE, (LPARAM)L"Например: 94.140.14.14 или 77.88.8.8");

    // 3. Secondary IPv4 Edit Box (Y = 240)
    int ySec = 240;
    m_hEditSec = CreateWindowExW(0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        marginX + 10, ySec + innerOffset, editW, innerEditH,
        m_hWnd, (HMENU)ID_EDIT_SEC, m_hInstance, nullptr);
    SendMessageW(m_hEditSec, WM_SETFONT, (WPARAM)m_hFontRegular, TRUE);
    SendMessageW(m_hEditSec, EM_SETCUEBANNER, TRUE, (LPARAM)L"Например: 94.140.15.15 (необязательно)");

    // 4. Description Edit Box (Y = 310)
    int yDesc = 310;
    m_hEditDesc = CreateWindowExW(0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        marginX + 10, yDesc + innerOffset, editW, innerEditH,
        m_hWnd, (HMENU)ID_EDIT_DESC, m_hInstance, nullptr);
    SendMessageW(m_hEditDesc, WM_SETFONT, (WPARAM)m_hFontRegular, TRUE);
    SendMessageW(m_hEditDesc, EM_SETCUEBANNER, TRUE, (LPARAM)L"Краткое описание или назначение (необязательно)");

    // 5. DoH URL Edit Box (Y = 380)
    int yDoh = 380;
    m_hEditDoh = CreateWindowExW(0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        marginX + 10, yDoh + innerOffset, editW, innerEditH,
        m_hWnd, (HMENU)ID_EDIT_DOH, m_hInstance, nullptr);
    SendMessageW(m_hEditDoh, WM_SETFONT, (WPARAM)m_hFontRegular, TRUE);
    SendMessageW(m_hEditDoh, EM_SETCUEBANNER, TRUE, (LPARAM)L"https://dns.adguard-dns.com/dns-query (необязательно)");

    // 6. SmartDNS Option Checkbox inside Card (Y = 450, H = 68)
    int ySmart = 450;
    m_hCheckSmartDns = CreateWindowExW(0, WC_BUTTONW, L"Профиль SmartDNS (разблокировка сервисов и медиа)",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        marginX + 14, ySmart + 8, contentWidth - 28, 26,
        m_hWnd, (HMENU)ID_CHECK_SMARTDNS, m_hInstance, nullptr);

    auto* chkState = new CheckboxAnimState();
    chkState->pDlg = this;
    SetPropW(m_hCheckSmartDns, L"QuickDnsCheckAnim", reinterpret_cast<HANDLE>(chkState));
    SetWindowSubclass(m_hCheckSmartDns, CustomCheckboxSubclassProc, 401, reinterpret_cast<DWORD_PTR>(chkState));

    // 7. Action Buttons (Y = 538)
    int yButtons = 538;
    int btnWidth = (contentWidth - 14) / 2;
    int btnHeight = 40;

    m_hBtnSave = CreateWindowExW(0, WC_BUTTONW, L"Сохранить профиль",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        marginX, yButtons, btnWidth, btnHeight,
        m_hWnd, (HMENU)ID_BTN_SAVE, m_hInstance, nullptr);

    m_hBtnCancel = CreateWindowExW(0, WC_BUTTONW, L"Отмена",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        marginX + btnWidth + 14, yButtons, btnWidth, btnHeight,
        m_hWnd, (HMENU)ID_BTN_CANCEL, m_hInstance, nullptr);

    attach_button_effects(m_hBtnSave, 301);
    attach_button_effects(m_hBtnCancel, 302);
}

void AddProfileDialog::reposition_controls() {
    RECT rcClient = {};
    GetClientRect(m_hWnd, &rcClient);
    int clientWidth = rcClient.right - rcClient.left;
    int marginX = 24;
    int contentWidth = clientWidth - (2 * marginX);
    if (contentWidth < 200) return;

    int innerEditH = 24;
    int innerOffset = 7;
    int editW = contentWidth - 20;

    int yName = 100;
    if (m_hEditName) SetWindowPos(m_hEditName, nullptr, marginX + 10, yName + innerOffset, editW, innerEditH, SWP_NOZORDER);

    int yPri = 170;
    if (m_hEditPri) SetWindowPos(m_hEditPri, nullptr, marginX + 10, yPri + innerOffset, editW, innerEditH, SWP_NOZORDER);

    int ySec = 240;
    if (m_hEditSec) SetWindowPos(m_hEditSec, nullptr, marginX + 10, ySec + innerOffset, editW, innerEditH, SWP_NOZORDER);

    int yDesc = 310;
    if (m_hEditDesc) SetWindowPos(m_hEditDesc, nullptr, marginX + 10, yDesc + innerOffset, editW, innerEditH, SWP_NOZORDER);

    int yDoh = 380;
    if (m_hEditDoh) SetWindowPos(m_hEditDoh, nullptr, marginX + 10, yDoh + innerOffset, editW, innerEditH, SWP_NOZORDER);

    int ySmart = 450;
    if (m_hCheckSmartDns) SetWindowPos(m_hCheckSmartDns, nullptr, marginX + 14, ySmart + 8, contentWidth - 28, 26, SWP_NOZORDER);

    int yButtons = 538;
    int btnWidth = (contentWidth - 14) / 2;
    int btnHeight = 40;

    if (m_hBtnSave) SetWindowPos(m_hBtnSave, nullptr, marginX, yButtons, btnWidth, btnHeight, SWP_NOZORDER);
    if (m_hBtnCancel) SetWindowPos(m_hBtnCancel, nullptr, marginX + btnWidth + 14, yButtons, btnWidth, btnHeight, SWP_NOZORDER);

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

    auto pal = quickdns::GetPalette(m_isDarkMode);
    bool isPrimary = (pDis->CtlID == ID_BTN_SAVE);

    auto* anim = reinterpret_cast<ButtonAnimationState*>(GetPropW(pDis->hwndItem, L"QuickDnsAnim"));
    float hoverVal = anim ? anim->hover : 0.0f;
    bool isPressed = (pDis->itemState & ODS_SELECTED) || (anim && anim->isPressed);

    COLORREF baseBg = isPrimary ? pal.btnPrimaryBg : pal.btnUtilBg;
    COLORREF hoverBg = isPrimary ? pal.btnPrimaryHover : pal.btnUtilHover;
    COLORREF pressBg = isPrimary ? pal.btnPrimaryPress : pal.btnUtilPress;

    COLORREF baseBorder = isPrimary ? pal.btnPrimaryBorder : pal.btnUtilBorder;
    COLORREF hoverBorder = isPrimary ? pal.btnPrimaryHover : (m_isDarkMode ? RGB(70, 85, 110) : RGB(180, 195, 220));

    COLORREF curBg = isPressed ? pressBg : quickdns::LerpColor(baseBg, hoverBg, hoverVal);
    COLORREF curBorder = quickdns::LerpColor(baseBorder, hoverBorder, hoverVal);
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
        quickdns::AddRoundedRect(path, Gdiplus::RectF(0.5f, 0.5f, static_cast<float>(w) - 1.0f, static_cast<float>(h) - 1.0f), 7.0f);

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
        while (!s.empty() && iswspace(s.front())) s.erase(s.begin());
        while (!s.empty() && iswspace(s.back())) s.pop_back();
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

    bool isSmart = (Button_GetCheck(m_hCheckSmartDns) == BST_CHECKED);

    DnsProfile profile;
    profile.id = "custom_" + std::to_string(GetTickCount64());
    profile.name = wide_to_utf8(nameStr);
    profile.primary_ipv4 = wide_to_utf8(priStr);
    profile.secondary_ipv4 = wide_to_utf8(secStr);
    profile.doh_template = wide_to_utf8(dohStr);
    profile.is_smart_dns = isSmart;
    if (descStr.empty()) {
        profile.description = isSmart ? ("Пользовательский SmartDNS: " + profile.primary_ipv4)
                                      : ("Пользовательский DNS: " + profile.primary_ipv4);
    } else {
        profile.description = wide_to_utf8(descStr);
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
        RECT rcMin = { 0, 0, 500, 600 };
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
        return 1; // Double buffered paint

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

        auto pal = quickdns::GetPalette(m_isDarkMode);

        // Fill background
        HBRUSH hbrBg = CreateSolidBrush(pal.bg);
        FillRect(hdcMem, &rc, hbrBg);
        DeleteObject(hbrBg);

        {
            Gdiplus::Graphics g(hdcMem);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
            g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

            int marginX = 24;
            int contentW = w - (2 * marginX);

            Gdiplus::FontFamily ffDisplay(L"Segoe UI Variable Display");
            Gdiplus::FontFamily ffRegular(L"Segoe UI");
            Gdiplus::Font fontHeader(&ffDisplay, 14.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);
            Gdiplus::Font fontLabel(&ffRegular, 9.5f, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);
            Gdiplus::Font fontSub(&ffRegular, 9.0f, Gdiplus::FontStyleRegular, Gdiplus::UnitPoint);

            Gdiplus::SolidBrush textPriBr(Gdiplus::Color(255, GetRValue(pal.textPrimary), GetGValue(pal.textPrimary), GetBValue(pal.textPrimary)));
            Gdiplus::SolidBrush textSecBr(Gdiplus::Color(255, GetRValue(pal.textSecondary), GetGValue(pal.textSecondary), GetBValue(pal.textSecondary)));

            // Header Title and Subtitle with clear separation
            g.DrawString(L"Добавить свой DNS сервер", -1, &fontHeader, Gdiplus::PointF(static_cast<float>(marginX), 18.0f), &textPriBr);
            g.DrawString(L"Создайте собственный профиль DNS с поддержкой IPv4, DoH и SmartDNS", -1, &fontSub, Gdiplus::PointF(static_cast<float>(marginX), 46.0f), &textSecBr);

            // Container card brush & pen
            Gdiplus::SolidBrush cardBgBr(Gdiplus::Color(255, GetRValue(pal.cardBg), GetGValue(pal.cardBg), GetBValue(pal.cardBg)));
            Gdiplus::Pen cardBorderPen(Gdiplus::Color(255, GetRValue(pal.cardBorder), GetGValue(pal.cardBorder), GetBValue(pal.cardBorder)), 1.0f);

            // Helper lambda to draw rounded input field container card and its label
            auto drawFieldCard = [&](const wchar_t* label, int y, int fieldH) {
                g.DrawString(label, -1, &fontLabel, Gdiplus::PointF(static_cast<float>(marginX), static_cast<float>(y - 22)), &textPriBr);
                Gdiplus::GraphicsPath p;
                quickdns::AddRoundedRect(p, Gdiplus::RectF(static_cast<float>(marginX), static_cast<float>(y), static_cast<float>(contentW), static_cast<float>(fieldH)), 6.0f);
                g.FillPath(&cardBgBr, &p);
                g.DrawPath(&cardBorderPen, &p);
            };

            drawFieldCard(L"Название профиля (обязательно):", 100, 38);
            drawFieldCard(L"Основной DNS IPv4 (обязательно):", 170, 38);
            drawFieldCard(L"Дополнительный DNS IPv4 (необязательно):", 240, 38);
            drawFieldCard(L"Описание / заметка (необязательно):", 310, 38);
            drawFieldCard(L"DNS over HTTPS (DoH) URL (необязательно):", 380, 38);

            // SmartDNS card container (Y = 450, H = 68)
            Gdiplus::GraphicsPath smartCardPath;
            quickdns::AddRoundedRect(smartCardPath, Gdiplus::RectF(static_cast<float>(marginX), 450.0f, static_cast<float>(contentW), 68.0f), 8.0f);
            g.FillPath(&cardBgBr, &smartCardPath);
            g.DrawPath(&cardBorderPen, &smartCardPath);

            Gdiplus::RectF rcSmartSub(static_cast<float>(marginX + 16), 488.0f, static_cast<float>(contentW - 32), 22.0f);
            Gdiplus::StringFormat sfSmartSub;
            sfSmartSub.SetTrimming(Gdiplus::StringTrimmingWord);
            g.DrawString(L"Отметьте, если сервер предоставляет функцию обхода блокировок и стриминга",
                -1, &fontSub, rcSmartSub, &sfSmartSub, &textSecBr);
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
        if (id == ID_BTN_SAVE || id == IDOK) {
            on_save();
            return 0;
        }
        if (id == ID_BTN_CANCEL || id == IDCANCEL) {
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
        HDC hdcStatic = (HDC)wParam;
        HWND hwndStatic = (HWND)lParam;
        auto pal = quickdns::GetPalette(m_isDarkMode);
        SetTextColor(hdcStatic, pal.textPrimary);
        SetBkMode(hdcStatic, TRANSPARENT);
        if (hwndStatic == m_hCheckSmartDns) {
            return (INT_PTR)m_hbrCard;
        }
        return (INT_PTR)m_hbrBg;
    }

    case WM_CTLCOLOREDIT: {
        HDC hdcEdit = (HDC)wParam;
        auto pal = quickdns::GetPalette(m_isDarkMode);
        SetBkMode(hdcEdit, OPAQUE);
        SetTextColor(hdcEdit, pal.textPrimary);
        SetBkColor(hdcEdit, pal.cardBg);
        return (INT_PTR)m_hbrCard;
    }

    case WM_CLOSE: {
        on_cancel();
        return 0;
    }
    }

    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}
