#include "dialog_add_profile.h"
#include "dns_manager.h"
#include "encoding_utils.h"
#include <dwmapi.h>
#include <windowsx.h>

using QuickDns::Encoding::wide_to_utf8;

namespace {
    constexpr const wchar_t* DIALOG_CLASS_NAME = L"QuickDNS_AddProfileDialogClass";
    constexpr int DIALOG_WIDTH = 440;
    constexpr int DIALOG_HEIGHT = 460;

    constexpr COLORREF COLOR_BG_DARK = RGB(32, 32, 32);
    constexpr COLORREF COLOR_TEXT_LIGHT = RGB(240, 240, 240);
    constexpr COLORREF COLOR_INPUT_BG = RGB(45, 45, 45);

    constexpr int ID_EDIT_NAME = 3001;
    constexpr int ID_EDIT_PRI  = 3002;
    constexpr int ID_EDIT_SEC  = 3003;
    constexpr int ID_EDIT_DESC = 3004;
    constexpr int ID_EDIT_DOH  = 3005;
    constexpr int ID_BTN_SAVE  = 3006;
    constexpr int ID_BTN_CANCEL= 3007;
}

std::optional<DnsProfile> AddProfileDialog::show(HWND hParent, HINSTANCE hInstance) {
    AddProfileDialog dlg(hParent, hInstance);
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

AddProfileDialog::AddProfileDialog(HWND hParent, HINSTANCE hInstance)
    : m_hParent(hParent), m_hInstance(hInstance)
{
    m_hFontRegular = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    m_hFontBold = CreateFontW(-14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    m_hFontHeader = CreateFontW(-18, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable Display");
}

AddProfileDialog::~AddProfileDialog() {
    if (m_hFontRegular) DeleteObject(m_hFontRegular);
    if (m_hFontBold) DeleteObject(m_hFontBold);
    if (m_hFontHeader) DeleteObject(m_hFontHeader);
}

bool AddProfileDialog::create_dialog_window() {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &AddProfileDialog::dialog_proc_thunk;
    wc.hInstance = m_hInstance;
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(COLOR_BG_DARK);
    wc.lpszClassName = DIALOG_CLASS_NAME;
    RegisterClassExW(&wc);

    RECT parentRect = {};
    GetWindowRect(m_hParent, &parentRect);
    int posX = parentRect.left + ((parentRect.right - parentRect.left) - DIALOG_WIDTH) / 2;
    int posY = parentRect.top + ((parentRect.bottom - parentRect.top) - DIALOG_HEIGHT) / 2;

    m_hWnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        DIALOG_CLASS_NAME,
        L"Добавить свой DNS сервер",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        posX, posY,
        DIALOG_WIDTH, DIALOG_HEIGHT,
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
    SetFocus(m_hEditName);
    return true;
}

void AddProfileDialog::apply_theme() {
    BOOL useDarkMode = TRUE;
    DwmSetWindowAttribute(m_hWnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &useDarkMode, sizeof(useDarkMode));
    DWORD cornerPref = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(m_hWnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &cornerPref, sizeof(cornerPref));
}

void AddProfileDialog::create_controls() {
    RECT rcClient = {};
    GetClientRect(m_hWnd, &rcClient);
    int clientWidth = rcClient.right - rcClient.left;
    int marginX = 20;
    int contentWidth = clientWidth - (2 * marginX);
    int curY = 16;

    HWND hTitle = CreateWindowExW(0, L"STATIC", L"Новый профиль DNS",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        marginX, curY, contentWidth, 24,
        m_hWnd, nullptr, m_hInstance, nullptr);
    SendMessageW(hTitle, WM_SETFONT, (WPARAM)m_hFontHeader, TRUE);
    curY += 32;

    // 1. Name
    HWND hLblName = CreateWindowExW(0, L"STATIC", L"Название (например: AdGuard DNS, Мой DNS):",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        marginX, curY, contentWidth, 18,
        m_hWnd, nullptr, m_hInstance, nullptr);
    SendMessageW(hLblName, WM_SETFONT, (WPARAM)m_hFontRegular, TRUE);
    curY += 22;

    m_hEditName = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        marginX, curY, contentWidth, 24,
        m_hWnd, (HMENU)ID_EDIT_NAME, m_hInstance, nullptr);
    SendMessageW(m_hEditName, WM_SETFONT, (WPARAM)m_hFontRegular, TRUE);
    curY += 34;

    // 2. Primary IPv4
    HWND hLblPri = CreateWindowExW(0, L"STATIC", L"Основной DNS IPv4 (обязательно):",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        marginX, curY, contentWidth, 18,
        m_hWnd, nullptr, m_hInstance, nullptr);
    SendMessageW(hLblPri, WM_SETFONT, (WPARAM)m_hFontRegular, TRUE);
    curY += 22;

    m_hEditPri = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        marginX, curY, contentWidth, 24,
        m_hWnd, (HMENU)ID_EDIT_PRI, m_hInstance, nullptr);
    SendMessageW(m_hEditPri, WM_SETFONT, (WPARAM)m_hFontRegular, TRUE);
    curY += 34;

    // 3. Secondary IPv4
    HWND hLblSec = CreateWindowExW(0, L"STATIC", L"Дополнительный DNS IPv4 (необязательно):",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        marginX, curY, contentWidth, 18,
        m_hWnd, nullptr, m_hInstance, nullptr);
    SendMessageW(hLblSec, WM_SETFONT, (WPARAM)m_hFontRegular, TRUE);
    curY += 22;

    m_hEditSec = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        marginX, curY, contentWidth, 24,
        m_hWnd, (HMENU)ID_EDIT_SEC, m_hInstance, nullptr);
    SendMessageW(m_hEditSec, WM_SETFONT, (WPARAM)m_hFontRegular, TRUE);
    curY += 34;

    // 4. Description
    HWND hLblDesc = CreateWindowExW(0, L"STATIC", L"Описание / заметка (необязательно):",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        marginX, curY, contentWidth, 18,
        m_hWnd, nullptr, m_hInstance, nullptr);
    SendMessageW(hLblDesc, WM_SETFONT, (WPARAM)m_hFontRegular, TRUE);
    curY += 22;

    m_hEditDesc = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        marginX, curY, contentWidth, 24,
        m_hWnd, (HMENU)ID_EDIT_DESC, m_hInstance, nullptr);
    SendMessageW(m_hEditDesc, WM_SETFONT, (WPARAM)m_hFontRegular, TRUE);
    curY += 34;

    // 5. DNS over HTTPS (DoH) URL
    HWND hLblDoh = CreateWindowExW(0, L"STATIC", L"DNS over HTTPS (DoH) URL (необязательно):",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        marginX, curY, contentWidth, 18,
        m_hWnd, nullptr, m_hInstance, nullptr);
    SendMessageW(hLblDoh, WM_SETFONT, (WPARAM)m_hFontRegular, TRUE);
    curY += 22;

    m_hEditDoh = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        marginX, curY, contentWidth, 24,
        m_hWnd, (HMENU)ID_EDIT_DOH, m_hInstance, nullptr);
    SendMessageW(m_hEditDoh, WM_SETFONT, (WPARAM)m_hFontRegular, TRUE);
    curY += 40;

    // 6. Action Buttons
    int btnWidth = (contentWidth - 12) / 2;
    m_hBtnSave = CreateWindowExW(0, L"BUTTON", L"Сохранить",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        marginX, curY, btnWidth, 34,
        m_hWnd, (HMENU)ID_BTN_SAVE, m_hInstance, nullptr);
    SendMessageW(m_hBtnSave, WM_SETFONT, (WPARAM)m_hFontBold, TRUE);

    m_hBtnCancel = CreateWindowExW(0, L"BUTTON", L"Отмена",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        marginX + btnWidth + 12, curY, btnWidth, 34,
        m_hWnd, (HMENU)ID_BTN_CANCEL, m_hInstance, nullptr);
    SendMessageW(m_hBtnCancel, WM_SETFONT, (WPARAM)m_hFontRegular, TRUE);
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

    // Trim whitespace
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

    if (priStr.empty() || !DnsManager::is_valid_ipv4(priStr)) {
        MessageBoxW(m_hWnd, L"Пожалуйста, укажите корректный основной IPv4 адрес (например, 77.88.8.8 или 94.140.14.14).", L"Некорректный IP", MB_OK | MB_ICONWARNING);
        SetFocus(m_hEditPri);
        return;
    }

    if (!secStr.empty() && !DnsManager::is_valid_ipv4(secStr)) {
        MessageBoxW(m_hWnd, L"Дополнительный адрес должен быть корректным IPv4 адресом (или оставьте это поле пустым).", L"Некорректный IP", MB_OK | MB_ICONWARNING);
        SetFocus(m_hEditSec);
        return;
    }

    DnsProfile profile;
    profile.id = "custom_" + std::to_string(GetTickCount64());
    profile.name = wide_to_utf8(nameStr);
    profile.primary_ipv4 = wide_to_utf8(priStr);
    profile.secondary_ipv4 = wide_to_utf8(secStr);
    profile.doh_template = wide_to_utf8(dohStr);
    profile.description = descStr.empty() ? ("Пользовательский DNS: " + profile.primary_ipv4) : wide_to_utf8(descStr);
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
    case WM_SETCURSOR: {
        HWND hTarget = (HWND)wParam;
        if (hTarget && IsWindow(hTarget)) {
            wchar_t cls[32] = {};
            GetClassNameW(hTarget, cls, 32);
            if (wcscmp(cls, WC_BUTTONW) == 0 && IsWindowEnabled(hTarget)) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
        }
        break;
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

    case WM_CTLCOLORSTATIC: {
        HDC hdcStatic = (HDC)wParam;
        SetTextColor(hdcStatic, COLOR_TEXT_LIGHT);
        SetBkMode(hdcStatic, TRANSPARENT);
        static HBRUSH hbrBg = CreateSolidBrush(COLOR_BG_DARK);
        return (INT_PTR)hbrBg;
    }

    case WM_CLOSE: {
        on_cancel();
        return 0;
    }
    }

    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}
