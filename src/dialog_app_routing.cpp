#include "dialog_app_routing.h"
#include "encoding_utils.h"
#include <commdlg.h>
#include <objbase.h>
#include <utility>

namespace quickdns {
namespace {
constexpr wchar_t class_name[] = L"QuickDNS_AppRoutingDraftDialog";
constexpr wchar_t inactive[] = L"Черновики не активны. Маршрутизация и блокировка не применяются.";
std::wstring text(HWND control) {
    const int size = GetWindowTextLengthW(control);
    std::wstring value(static_cast<size_t>(size) + 1, L'\0');
    value.resize(static_cast<size_t>(GetWindowTextW(control, value.data(), size + 1)));
    return value;
}
HWND control(HWND parent, const wchar_t* cls, const wchar_t* label, DWORD style,
             int x, int y, int width, int height, int id = 0) {
    HWND result = CreateWindowExW(0, cls, label, WS_CHILD | WS_VISIBLE | style, x, y, width, height,
                                  parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                  reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(parent, GWLP_HINSTANCE)), nullptr);
    SendMessageW(result, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    return result;
}
}
AppRoutingDialog::AppRoutingDialog(std::filesystem::path path, std::vector<RoutingAdapter> adapters,
                                   std::optional<std::vector<InstalledPackage>> packages)
    : store_(std::move(path)), adapters_(std::move(adapters)), packages_(std::move(packages)) {}
AppRoutingDialog::~AppRoutingDialog() { if (window_) DestroyWindow(window_); }
bool AppRoutingDialog::create(HINSTANCE instance, HWND parent) {
    if (window_ || !instance || !store_.load()) return false;
    drafts_ = store_.rules(); instance_ = instance; parent_ = parent;
    WNDCLASSW wc{};
    wc.lpfnWndProc = procedure; wc.hInstance = instance; wc.lpszClassName = class_name;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    return CreateWindowExW(WS_EX_DLGMODALFRAME, class_name, L"Маршрутизация приложений — черновик",
                           WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT,
                           720, 520, parent, nullptr, instance, this) != nullptr;
}
void AppRoutingDialog::show_modal() {
    if (!window_) return;
    if (parent_) EnableWindow(parent_, FALSE);
    ShowWindow(window_, SW_SHOW);
    MSG msg{};
    int status = 1;
    while (window_ && (status = GetMessageW(&msg, nullptr, 0, 0)) > 0)
        if (!IsDialogMessageW(window_, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    if (status == 0) PostQuitMessage(static_cast<int>(msg.wParam));
    if (parent_) { EnableWindow(parent_, TRUE); SetActiveWindow(parent_); }
}
LRESULT CALLBACK AppRoutingDialog::procedure(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<AppRoutingDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        self = static_cast<AppRoutingDialog*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->window_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    return self ? self->dispatch(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}
LRESULT AppRoutingDialog::dispatch(UINT msg, WPARAM wp, LPARAM lp) {
    const auto get = [this](int id) { return GetDlgItem(window_, id); };
    const auto refresh = [this, &get]() {
        HWND list = get(RoutingList); SendMessageW(list, LB_RESETCONTENT, 0, 0);
        for (const auto& rule : drafts_) {
            std::wstring identity = QuickDns::Encoding::utf8_to_wide(rule.package_family.empty() ? rule.application_path : rule.package_family);
            std::wstring adapter = QuickDns::Encoding::utf8_to_wide(rule.adapter_id);
            for (const auto& candidate : adapters_) {
                if (CompareStringOrdinal(candidate.id.c_str(), -1, adapter.c_str(), -1, TRUE) == CSTR_EQUAL) {
                    adapter = candidate.name + (candidate.connected ? L" (подключён)" : L" (отключён)"); break;
                }
            }
            const std::wstring row = identity + L"  →  " + adapter;
            SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(row.c_str()));
        }
    };
    switch (msg) {
    case WM_CREATE: {
        control(window_, L"STATIC", inactive, 0, 16, 14, 665, 35, RoutingStatus);
        control(window_, L"STATIC", L"Тип приложения:", 0, 16, 56, 180, 22);
        HWND kind = control(window_, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 16, 80, 180, 160, RoutingKind);
        SendMessageW(kind, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Файл EXE"));
        SendMessageW(kind, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Семейство MSIX"));
        SendMessageW(kind, CB_SETCURSEL, 0, 0);
        HWND packages = control(window_, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 210, 80, 475, 180, RoutingPackages);
        ShowWindow(packages, SW_HIDE);
        control(window_, L"STATIC", L"Путь EXE или имя семейства пакета:", 0, 16, 116, 400, 22);
        control(window_, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 16, 140, 540, 25, RoutingIdentity);
        control(window_, L"BUTTON", L"Обзор…", BS_PUSHBUTTON | WS_TABSTOP, 565, 140, 120, 26, RoutingBrowse);
        control(window_, L"STATIC", L"Сетевой адаптер:", 0, 16, 177, 200, 22);
        HWND combo = control(window_, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 16, 201, 540, 200, RoutingAdapterCombo);
        for (const auto& adapter : adapters_) {
            const std::wstring label = adapter.name + (adapter.connected ? L" (подключён)" : L" (отключён)");
            SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        }
        if (!adapters_.empty()) SendMessageW(combo, CB_SETCURSEL, 0, 0);
        control(window_, L"BUTTON", L"Добавить", BS_PUSHBUTTON | WS_TABSTOP, 565, 201, 120, 27, RoutingAdd);
        control(window_, L"LISTBOX", L"", WS_BORDER | LBS_NOTIFY | WS_VSCROLL | WS_TABSTOP, 16, 245, 540, 170, RoutingList);
        control(window_, L"BUTTON", L"Удалить", BS_PUSHBUTTON | WS_TABSTOP, 565, 245, 120, 27, RoutingRemove);
        control(window_, L"BUTTON", L"Сохранить", BS_PUSHBUTTON | WS_TABSTOP, 565, 389, 120, 27, RoutingSave);
        refresh(); return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case RoutingKind: {
            if (HIWORD(wp) != CBN_SELCHANGE) return 0;
            const bool is_package = SendMessageW(get(RoutingKind), CB_GETCURSEL, 0, 0) == 1;
            ShowWindow(get(RoutingPackages), is_package ? SW_SHOW : SW_HIDE);
            EnableWindow(get(RoutingBrowse), !is_package);
            SetWindowTextW(get(RoutingIdentity), L"");
            if (is_package) {
                if (!packages_) packages_ = enumerate_installed_packages();
                HWND packages = get(RoutingPackages);
                SendMessageW(packages, CB_RESETCONTENT, 0, 0);
                for (const auto& package : *packages_) {
                    const std::wstring label = package.name + L" — " + package.family;
                    SendMessageW(packages, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
                }
            }
            return 0;
        }
        case RoutingPackages: {
            if (HIWORD(wp) != CBN_SELCHANGE || !packages_) return 0;
            const auto index = SendMessageW(get(RoutingPackages), CB_GETCURSEL, 0, 0);
            if (index >= 0 && static_cast<size_t>(index) < packages_->size())
                SetWindowTextW(get(RoutingIdentity), (*packages_)[static_cast<size_t>(index)].family.c_str());
            return 0;
        }
        case RoutingBrowse: {
            if (SendMessageW(get(RoutingKind), CB_GETCURSEL, 0, 0) != 0) return 0;
            wchar_t path[32768]{}; OPENFILENAMEW ofn{};
            ofn.lStructSize = sizeof(ofn); ofn.hwndOwner = window_; ofn.lpstrFile = path;
            ofn.nMaxFile = static_cast<DWORD>(std::size(path));
            ofn.lpstrFilter = L"Исполняемые файлы (*.exe)\0*.exe\0Все файлы\0*.*\0\0";
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
            if (GetOpenFileNameW(&ofn)) SetWindowTextW(get(RoutingIdentity), path);
            return 0;
        }
        case RoutingAdd: {
            const auto index = SendMessageW(get(RoutingAdapterCombo), CB_GETCURSEL, 0, 0);
            if (index < 0 || static_cast<size_t>(index) >= adapters_.size()) { SetWindowTextW(get(RoutingStatus), L"Выберите адаптер. Правила не активны."); return 0; }
            GUID guid{}; wchar_t id[40]{};
            if (FAILED(CoCreateGuid(&guid)) || !StringFromGUID2(guid, id, static_cast<int>(std::size(id)))) { SetWindowTextW(get(RoutingStatus), L"Ошибка создания идентификатора."); return 0; }
            AppRoutingRule rule; rule.id = QuickDns::Encoding::wide_to_utf8(id);
            rule.adapter_id = QuickDns::Encoding::wide_to_utf8(adapters_[static_cast<size_t>(index)].id);
            const auto identity = QuickDns::Encoding::wide_to_utf8(text(get(RoutingIdentity)));
            if (SendMessageW(get(RoutingKind), CB_GETCURSEL, 0, 0) == 1) rule.package_family = identity;
            else rule.application_path = identity;
            auto next = drafts_; next.push_back(rule);
            if (!store_.replace_rules(next)) { SetWindowTextW(get(RoutingStatus), L"Неверное или повторяющееся правило. Черновики не активны."); return 0; }
            drafts_ = std::move(next); refresh(); SetWindowTextW(get(RoutingStatus), inactive); return 0;
        }
        case RoutingRemove: {
            const auto index = SendMessageW(get(RoutingList), LB_GETCURSEL, 0, 0);
            if (index < 0 || static_cast<size_t>(index) >= drafts_.size()) return 0;
            auto next = drafts_; next.erase(next.begin() + index);
            if (store_.replace_rules(next)) { drafts_ = std::move(next); refresh(); }
            return 0;
        }
        case RoutingSave:
            SetWindowTextW(get(RoutingStatus), store_.save() ? L"Черновики сохранены. Правила не активны." : L"Ошибка сохранения. Правила не активны."); return 0;
        }
        break;
    case WM_CLOSE: DestroyWindow(window_); return 0;
    case WM_NCDESTROY: { HWND old = window_; window_ = nullptr; SetWindowLongPtrW(old, GWLP_USERDATA, 0); return DefWindowProcW(old, msg, wp, lp); }
    }
    return DefWindowProcW(window_, msg, wp, lp);
}
}
