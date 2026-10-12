#include "dialog_benchmark.h"
#include "encoding_utils.h"
#include "resource.h"
#include <dwmapi.h>
#include <windowsx.h>
#include <commctrl.h>
#include <thread>

#pragma comment(lib, "comctl32.lib")

using QuickDns::Encoding::wide_to_utf8;

namespace {
    constexpr const wchar_t* DIALOG_CLASS_NAME = L"QuickDNS_BenchmarkDialogClass";
    constexpr int DIALOG_WIDTH = 640;
    constexpr int DIALOG_HEIGHT = 540;

    constexpr COLORREF COLOR_BG_DARK = RGB(30, 30, 30);
    constexpr COLORREF COLOR_TEXT_LIGHT = RGB(240, 240, 240);

    constexpr UINT WM_APP_BENCHMARK_PROGRESS = WM_APP + 10;
    constexpr UINT WM_APP_BENCHMARK_DONE     = WM_APP + 11;
}

namespace quickdns {

std::optional<std::string> BenchmarkDialog::show(
    HWND hParent,
    HINSTANCE hInstance,
    const std::vector<DnsProfile>& profiles,
    const std::string& currentActiveId)
{
    BenchmarkDialog dlg(hParent, hInstance, profiles, currentActiveId);
    if (!dlg.create_dialog_window()) {
        return std::nullopt;
    }

    EnableWindow(hParent, FALSE);

    dlg.start_benchmark();

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

BenchmarkDialog::BenchmarkDialog(
    HWND hParent,
    HINSTANCE hInstance,
    const std::vector<DnsProfile>& profiles,
    const std::string& currentActiveId)
    : m_hParent(hParent)
    , m_hInstance(hInstance)
    , m_profiles(profiles)
    , m_currentActiveId(currentActiveId)
{
    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icex.dwICC = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icex);

    setup_fonts();
}

BenchmarkDialog::~BenchmarkDialog() {
    if (m_hFontRegular) DeleteObject(m_hFontRegular);
    if (m_hFontBold) DeleteObject(m_hFontBold);
    if (m_hFontHeader) DeleteObject(m_hFontHeader);
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
}

bool BenchmarkDialog::create_dialog_window() {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &BenchmarkDialog::dialog_proc_thunk;
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
        L"Тест скорости и доступности DNS (Пинг)",
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
    return true;
}

void BenchmarkDialog::apply_theme() {
    BOOL useDarkMode = TRUE;
    DwmSetWindowAttribute(m_hWnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &useDarkMode, sizeof(useDarkMode));
    DWORD cornerPref = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(m_hWnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &cornerPref, sizeof(cornerPref));
}

void BenchmarkDialog::create_controls() {
    RECT rcClient = {};
    GetClientRect(m_hWnd, &rcClient);
    int clientWidth = rcClient.right - rcClient.left;
    int marginX = 20;
    int contentWidth = clientWidth - (marginX * 2);
    int curY = 16;

    HWND hTitle = CreateWindowExW(0, L"STATIC", L"Тест скорости и отклика DNS",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        marginX, curY, contentWidth, 24,
        m_hWnd, nullptr, m_hInstance, nullptr);
    SendMessageW(hTitle, WM_SETFONT, (WPARAM)m_hFontHeader, TRUE);
    curY += 28;

    HWND hSub = CreateWindowExW(0, L"STATIC", L"Проверка задержки (пинг) на пуле доменов: ChatGPT, Claude, Google.",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        marginX, curY, contentWidth, 18,
        m_hWnd, nullptr, m_hInstance, nullptr);
    SendMessageW(hSub, WM_SETFONT, (WPARAM)m_hFontRegular, TRUE);
    curY += 22;

    m_hStatusText = CreateWindowExW(0, L"STATIC", L"Инициализация тестирования...",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        marginX, curY, contentWidth, 18,
        m_hWnd, (HMENU)IDC_STATUS_BENCHMARK, m_hInstance, nullptr);
    SendMessageW(m_hStatusText, WM_SETFONT, (WPARAM)m_hFontBold, TRUE);
    curY += 26;

    // ListView Control
    int listHeight = 330;
    m_hListView = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        WC_LISTVIEWW,
        L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        marginX, curY, contentWidth, listHeight,
        m_hWnd, (HMENU)IDC_LIST_BENCHMARK, m_hInstance, nullptr
    );
    SendMessageW(m_hListView, WM_SETFONT, (WPARAM)m_hFontRegular, TRUE);
    ListView_SetExtendedListViewStyle(m_hListView, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);

    // Setup Columns
    LVCOLUMNW col = {};
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;

    col.pszText = const_cast<LPWSTR>(L"Статус");
    col.cx = 48;
    ListView_InsertColumn(m_hListView, 0, &col);

    col.pszText = const_cast<LPWSTR>(L"Профиль DNS");
    col.cx = 160;
    ListView_InsertColumn(m_hListView, 1, &col);

    col.pszText = const_cast<LPWSTR>(L"Пинг (Отклик)");
    col.cx = 110;
    ListView_InsertColumn(m_hListView, 2, &col);

    col.pszText = const_cast<LPWSTR>(L"Оценка");
    col.cx = 90;
    ListView_InsertColumn(m_hListView, 3, &col);

    col.pszText = const_cast<LPWSTR>(L"Основной IP");
    col.cx = 115;
    ListView_InsertColumn(m_hListView, 4, &col);

    curY += listHeight + 14;

    // Buttons
    int btnWidth = 180;
    m_hBtnApply = CreateWindowExW(0, WC_BUTTONW, L"Применить этот DNS",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        marginX, curY, btnWidth, 34,
        m_hWnd, (HMENU)IDC_BTN_BENCHMARK_APPLY, m_hInstance, nullptr);
    SendMessageW(m_hBtnApply, WM_SETFONT, (WPARAM)m_hFontBold, TRUE);
    EnableWindow(m_hBtnApply, FALSE);

    m_hBtnRetest = CreateWindowExW(0, WC_BUTTONW, L"Повторить тест",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        marginX + btnWidth + 10, curY, 140, 34,
        m_hWnd, (HMENU)IDC_BTN_BENCHMARK_RETEST, m_hInstance, nullptr);
    SendMessageW(m_hBtnRetest, WM_SETFONT, (WPARAM)m_hFontRegular, TRUE);

    m_hBtnClose = CreateWindowExW(0, WC_BUTTONW, L"Закрыть",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        marginX + contentWidth - 100, curY, 100, 34,
        m_hWnd, (HMENU)IDC_BTN_BENCHMARK_CLOSE, m_hInstance, nullptr);
    SendMessageW(m_hBtnClose, WM_SETFONT, (WPARAM)m_hFontRegular, TRUE);
}

void BenchmarkDialog::start_benchmark() {
    if (m_isTesting) return;
    m_isTesting = true;

    EnableWindow(m_hBtnRetest, FALSE);
    EnableWindow(m_hBtnApply, FALSE);
    SetWindowTextW(m_hStatusText, L"Тестирование серверов... Подождите 1-2 секунды.");

    ListView_DeleteAllItems(m_hListView);

    std::vector<DnsProfile> profilesToTest = m_profiles;
    HWND hWnd = m_hWnd;

    std::thread([this, profilesToTest, hWnd]() {
        auto results = DnsBenchmark::run_benchmark(profilesToTest, nullptr);
        m_results = results;
        m_isTesting = false;

        PostMessageW(hWnd, WM_APP_BENCHMARK_DONE, 0, 0);
    }).detach();
}

void BenchmarkDialog::update_list_with_results() {
    ListView_DeleteAllItems(m_hListView);

    int fastestIdx = -1;
    for (size_t i = 0; i < m_results.size(); ++i) {
        const auto& r = m_results[i];

        LVITEMW item = {};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(i);
        item.iSubItem = 0;

        std::wstring dot;
        if (r.quality == DnsQuality::Fast) {
            dot = L"[OK]";
            if (fastestIdx == -1 && r.latency_ms > 0) fastestIdx = static_cast<int>(i);
        } else if (r.quality == DnsQuality::Moderate) {
            dot = L"[MID]";
        } else {
            dot = L"[ERR]";
        }

        item.pszText = const_cast<LPWSTR>(dot.c_str());
        ListView_InsertItem(m_hListView, &item);

        ListView_SetItemText(m_hListView, static_cast<int>(i), 1, const_cast<LPWSTR>(r.profile_name.c_str()));
        ListView_SetItemText(m_hListView, static_cast<int>(i), 2, const_cast<LPWSTR>(r.status_text.c_str()));
        ListView_SetItemText(m_hListView, static_cast<int>(i), 3, const_cast<LPWSTR>(r.quality_label.c_str()));

        std::wstring ipText = r.primary_ip.empty() ? L"(DHCP)" : r.primary_ip;
        ListView_SetItemText(m_hListView, static_cast<int>(i), 4, const_cast<LPWSTR>(ipText.c_str()));
    }

    if (fastestIdx != -1) {
        ListView_SetItemState(m_hListView, fastestIdx, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        EnableWindow(m_hBtnApply, TRUE);
    } else if (!m_results.empty()) {
        ListView_SetItemState(m_hListView, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        EnableWindow(m_hBtnApply, TRUE);
    }

    SetWindowTextW(m_hStatusText, L"Тестирование завершено! Серверы отсортированы по скорости.");
    EnableWindow(m_hBtnRetest, TRUE);
}

void BenchmarkDialog::on_apply() {
    int sel = ListView_GetNextItem(m_hListView, -1, LVNI_SELECTED);
    if (sel >= 0 && sel < static_cast<int>(m_results.size())) {
        m_selectedProfileId = m_results[sel].profile_id;
        m_finished = true;
    }
}

void BenchmarkDialog::on_close() {
    m_finished = true;
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

    case WM_APP_BENCHMARK_DONE: {
        update_list_with_results();
        return 0;
    }

    case WM_NOTIFY: {
        auto* pNmhdr = reinterpret_cast<NMHDR*>(lParam);
        if (pNmhdr->idFrom == IDC_LIST_BENCHMARK) {
            if (pNmhdr->code == NM_DBLCLK) {
                on_apply();
                return 0;
            } else if (pNmhdr->code == LVN_ITEMCHANGED) {
                int sel = ListView_GetNextItem(m_hListView, -1, LVNI_SELECTED);
                EnableWindow(m_hBtnApply, sel >= 0);
            }
        }
        break;
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (id == IDC_BTN_BENCHMARK_APPLY) {
            on_apply();
            return 0;
        }
        if (id == IDC_BTN_BENCHMARK_RETEST) {
            start_benchmark();
            return 0;
        }
        if (id == IDC_BTN_BENCHMARK_CLOSE || id == IDCANCEL) {
            on_close();
            return 0;
        }
        break;
    }

    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, COLOR_TEXT_LIGHT);
        SetBkMode(hdc, TRANSPARENT);
        static HBRUSH hbr = CreateSolidBrush(COLOR_BG_DARK);
        return (INT_PTR)hbr;
    }

    case WM_CLOSE: {
        on_close();
        return 0;
    }
    }

    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

} // namespace quickdns
