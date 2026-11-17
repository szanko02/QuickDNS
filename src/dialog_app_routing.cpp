#include "dialog_app_routing.h"
#include "encoding_utils.h"
#include "subnet_routes.h"
#include "split_routing.h"
#include <commdlg.h>
#include <commctrl.h>
#include <objbase.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <utility>
#include <algorithm>

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

std::filesystem::path colocated_engine() {
    std::wstring executable(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (!length || length >= executable.size()) return {};
    executable.resize(length);
    const auto engine = std::filesystem::path(executable).parent_path() / L"sing-box.exe";
    std::error_code error;
    return std::filesystem::is_regular_file(engine, error) ? engine : std::filesystem::path{};
}

std::vector<int> selected_rules(HWND list, bool multiple) {
    if (!multiple) {
        const auto index = SendMessageW(list, LB_GETCURSEL, 0, 0);
        return index < 0 ? std::vector<int>{} : std::vector<int>{static_cast<int>(index)};
    }
    const auto count = SendMessageW(list, LB_GETSELCOUNT, 0, 0);
    if (count <= 0) return {};
    std::vector<int> rows(static_cast<size_t>(count));
    const auto actual = SendMessageW(list, LB_GETSELITEMS, count, reinterpret_cast<LPARAM>(rows.data()));
    if (actual <= 0) return {};
    rows.resize(static_cast<size_t>(actual));
    return rows;
}

void select_rule(HWND list, bool multiple, size_t row) {
    if (multiple) SendMessageW(list, LB_SETSEL, TRUE, static_cast<LPARAM>(row));
    else SendMessageW(list, LB_SETCURSEL, row, 0);
}

void draw_surface(HDC dc, const RECT& bounds, float radius, COLORREF top, COLORREF bottom, COLORREF border) {
    const auto color = [](COLORREF value) {
        return Gdiplus::Color(255, GetRValue(value), GetGValue(value), GetBValue(value));
    };
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    const Gdiplus::RectF rect(bounds.left + 0.5f, bounds.top + 0.5f,
                             static_cast<float>(bounds.right - bounds.left) - 1.0f,
                             static_cast<float>(bounds.bottom - bounds.top) - 1.0f);
    Gdiplus::GraphicsPath path;
    AddRoundedRect(path, rect, radius);
    Gdiplus::LinearGradientBrush fill(rect, color(top), color(bottom), Gdiplus::LinearGradientModeVertical);
    Gdiplus::Pen outline(color(border), 1.0f);
    graphics.FillPath(&fill, &path);
    graphics.DrawPath(&outline, &path);
}
}

HWND AppRoutingDialog::control(HWND parent, const wchar_t* cls, const wchar_t* label, DWORD style,
                               int x, int y, int width, int height, int id) {
    const bool button = wcscmp(cls, L"BUTTON") == 0;
    const bool combo = wcscmp(cls, L"COMBOBOX") == 0;
    if (button && (style & BS_TYPEMASK) == BS_PUSHBUTTON) style |= BS_OWNERDRAW;
    if (combo) style |= CBS_OWNERDRAWFIXED | CBS_HASSTRINGS;
    HWND result = CreateWindowExW(0, cls, label, WS_CHILD | WS_VISIBLE | style, x, y, width, height,
                                  parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                  instance_, nullptr);
    if (!result) return nullptr;
    if (id != RoutingAppsPage && id != RoutingSubnetsPage) {
        auto& page = creating_subnets_ ? subnet_controls_ : app_controls_;
        page.push_back(result);
    }
    const bool heading = id == SplitPrimaryLabel || id == SplitSecondaryLabel ||
                         id == SplitRulesLabel || id == SplitEngineLabel;
    const auto font = heading ? heading_ : font_;
    SendMessageW(result, WM_SETFONT, reinterpret_cast<WPARAM>(font ? font : GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    if (combo) {
        SendMessageW(result, CB_SETITEMHEIGHT, 0, 23);
        SendMessageW(result, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), 22);
        SendMessageW(result, CB_SETDROPPEDWIDTH, 370, 0);
        if (dark_) SetWindowTheme(result, L"DarkMode_Explorer", nullptr);
    }
    return result;
}

void AppRoutingDialog::select_page(bool subnets) {
    subnets_selected_ = subnets;
    for (HWND control : app_controls_) {
        const bool show = !subnets && (GetDlgCtrlID(control) != RoutingPackages ||
            SendMessageW(GetDlgItem(window_, RoutingKind), CB_GETCURSEL, 0, 0) == 1);
        ShowWindow(control, show ? SW_SHOWNA : SW_HIDE);
    }
    for (HWND control : subnet_controls_) ShowWindow(control, subnets ? SW_SHOWNA : SW_HIDE);
    RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

void AppRoutingDialog::draw_control(const DRAWITEMSTRUCT& item) const {
    const auto id = static_cast<int>(item.CtlID);
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const bool selected = (item.itemState & ODS_SELECTED) != 0;
    const bool focused = (item.itemState & ODS_FOCUS) != 0;
    RECT rect = item.rcItem;
    if (item.CtlType == ODT_BUTTON) {
        const bool primary = id == SplitStart || id == SubnetAdd ||
            (id == RoutingAppsPage && !subnets_selected_) || (id == RoutingSubnetsPage && subnets_selected_);
        const COLORREF fill = disabled ? palette_.cardBg : primary
            ? (selected ? palette_.btnPrimaryPress : focused ? palette_.btnPrimaryHover : palette_.btnPrimaryBg)
            : (selected ? palette_.btnUtilPress : focused ? palette_.btnUtilHover : palette_.btnUtilBg);
        const COLORREF border = primary && !disabled ? palette_.btnPrimaryBorder : palette_.btnUtilBorder;
        FillRect(item.hDC, &rect, id == RoutingAppsPage || id == RoutingSubnetsPage ? background_ : field_);
        const auto highlight = disabled || selected ? fill
            : LerpColor(fill, primary ? palette_.btnPrimaryBorder : palette_.btnUtilHover, 0.25f);
        draw_surface(item.hDC, rect, 8.0f, highlight, fill, border);
        auto label = text(item.hwndItem);
        SetBkMode(item.hDC, TRANSPARENT);
        SetTextColor(item.hDC, disabled ? palette_.textMuted : primary ? palette_.btnPrimaryText : palette_.btnUtilText);
        DrawTextW(item.hDC, label.c_str(), -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        if (focused) {
            InflateRect(&rect, -4, -4);
            DrawFocusRect(item.hDC, &rect);
        }
    } else if (item.CtlType == ODT_COMBOBOX) {
        SetBkColor(item.hDC, selected ? palette_.btnPrimaryBg : palette_.cardBg);
        SetTextColor(item.hDC, disabled ? palette_.textMuted : selected ? palette_.btnPrimaryText : palette_.textPrimary);
        ExtTextOutW(item.hDC, 0, 0, ETO_OPAQUE, &rect, nullptr, 0, nullptr);
        if (item.itemID != static_cast<UINT>(-1)) {
            const auto size = SendMessageW(item.hwndItem, CB_GETLBTEXTLEN, item.itemID, 0);
            if (size != CB_ERR) {
                std::wstring label(static_cast<size_t>(size) + 1, L'\0');
                SendMessageW(item.hwndItem, CB_GETLBTEXT, item.itemID, reinterpret_cast<LPARAM>(label.data()));
                rect.left += 7;
                rect.right -= 3;
                DrawTextW(item.hDC, label.c_str(), static_cast<int>(size), &rect,
                          DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
            }
        }
    }
}
AppRoutingDialog::AppRoutingDialog(std::filesystem::path path, std::vector<RoutingAdapter> adapters,
                                   std::optional<std::vector<InstalledPackage>> packages, SubnetRoutes* subnet_routes,
                                   SplitRoutingSession* split_routing, bool dark)
    : store_(std::move(path)), adapters_(std::move(adapters)), packages_(std::move(packages)),
      subnet_routes_(subnet_routes), split_routing_(split_routing), dark_(dark), palette_(GetPalette(dark)) {}
AppRoutingDialog::~AppRoutingDialog() { if (window_) DestroyWindow(window_); }
bool AppRoutingDialog::create(HINSTANCE instance, HWND parent) {
    if (window_ || !instance) return false;
    if (!store_.load() && !subnet_routes_) return false;
    drafts_ = store_.rules(); instance_ = instance; parent_ = parent;
    WNDCLASSW wc{};
    wc.lpfnWndProc = procedure; wc.hInstance = instance; wc.lpszClassName = class_name;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.hbrBackground = nullptr;
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    return CreateWindowExW(WS_EX_DLGMODALFRAME, class_name,
                           subnet_routes_ ? L"Маршрутизация" : L"Маршрутизация приложений — черновик",
                           WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT,
                           720, split_routing_ ? 661 : (subnet_routes_ ? 605 : 455), parent, nullptr, instance, this) != nullptr;
}
void AppRoutingDialog::show_modal() {
    if (!window_) return;
    if (parent_) {
        EnableWindow(parent_, FALSE);
        RECT bounds{};
        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        if (GetWindowRect(window_, &bounds) &&
            GetMonitorInfoW(MonitorFromWindow(parent_, MONITOR_DEFAULTTONEAREST), &monitor)) {
            const auto& area = monitor.rcWork;
            const int x = area.left + std::max<LONG>(0, (area.right - area.left - (bounds.right - bounds.left)) / 2);
            const int y = area.top + std::max<LONG>(0, (area.bottom - area.top - (bounds.bottom - bounds.top)) / 2);
            SetWindowPos(window_, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
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

void AppRoutingDialog::create_subnet_controls() {
    control(window_, L"STATIC", L"Маршруты по подсетям для всех приложений", 0, 29, 66, 665, 20);
    control(window_, L"STATIC", L"Подсеть (CIDR)", 0, 29, 91, 170, 18);
    control(window_, L"STATIC", L"Шлюз (по желанию)", 0, 209, 91, 170, 18);
    control(window_, L"STATIC", L"Сетевой адаптер", 0, 389, 91, 180, 18);
    HWND destination = control(window_, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP,
                               16, 112, 170, 24, SubnetDestination);
    SendMessageW(destination, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(L"10.20.30.0/24"));
    control(window_, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 209, 112, 170, 24, SubnetGateway);
    HWND combo = control(window_, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 389, 112, 166, 200, SubnetAdapter);
    for (const auto& adapter : adapters_) {
        const auto label = adapter.name + (adapter.connected ? L"" : L" (отключён)");
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    }
    control(window_, L"BUTTON", L"Применить", BS_PUSHBUTTON | WS_TABSTOP, 565, 112, 120, 26, SubnetAdd);
    control(window_, L"LISTBOX", L"", WS_BORDER | LBS_NOTIFY | WS_VSCROLL | WS_TABSTOP, 29, 146, 526, 75, SubnetList);
    control(window_, L"BUTTON", L"Удалить", BS_PUSHBUTTON | WS_TABSTOP, 565, 146, 120, 26, SubnetRemove);
    control(window_, L"STATIC", L"Маршруты действуют до выхода из QuickDNS.",
            0, 29, 228, 665, 33, SubnetStatus);
    refresh_subnets();
}

void AppRoutingDialog::refresh_subnets() {
    HWND list = GetDlgItem(window_, SubnetList);
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    const auto& routes = subnet_routes_->routes();
    for (size_t i = 0; i < routes.size(); ++i) {
        auto label = subnet_route_label(routes[i]);
        if (subnet_routes_->status(i) != NO_ERROR) label += L" — запись отсутствует или изменена";
        SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    }
}

void AppRoutingDialog::change_subnet(int command) {
    const auto get = [this](int id) { return GetDlgItem(window_, id); };
    const bool adding = command == SubnetAdd;
    const auto index = SendMessageW(get(adding ? SubnetAdapter : SubnetList),
                                    adding ? CB_GETCURSEL : LB_GETCURSEL, 0, 0);
    HWND status = get(SubnetStatus);
    if (adding && split_routing_ && split_routing_->running()) {
        SetWindowTextW(status, L"Сначала остановите маршрутизацию приложений.");
        return;
    }
    if (index < 0 || (adding && static_cast<size_t>(index) >= adapters_.size())) {
        SetWindowTextW(status, adding ? L"Выберите сетевой адаптер." : L"Выберите маршрут для удаления.");
        return;
    }
    const DWORD result = adding
        ? subnet_routes_->add(text(get(SubnetDestination)), text(get(SubnetGateway)), adapters_[static_cast<size_t>(index)])
        : subnet_routes_->remove(static_cast<size_t>(index));
    refresh_subnets();
    if (result == NO_ERROR) {
        SetWindowTextW(status, adding ? L"Маршрут добавлен в таблицу Windows." : L"Маршрут удалён.");
    } else if (result == ERROR_INVALID_PARAMETER && adding) {
        SetWindowTextW(status, L"Проверьте подсеть в формате CIDR и адрес шлюза той же версии IP.");
    } else if (result == ERROR_NOT_FOUND && !adding) {
        SetWindowTextW(status, L"Маршрут уже отсутствует в таблице Windows.");
    } else if (result == ERROR_REVISION_MISMATCH && !adding) {
        SetWindowTextW(status, L"Запись изменилась вне QuickDNS и оставлена в таблице Windows.");
    } else if (result == ERROR_NETWORK_UNREACHABLE) {
        SetWindowTextW(status, L"Адаптер отключён или выбранная версия IP недоступна.");
    } else if (result == ERROR_OBJECT_ALREADY_EXISTS || result == ERROR_ALREADY_EXISTS) {
        SetWindowTextW(status, L"Такой маршрут уже есть в таблице Windows.");
    } else {
        wchar_t details[256]{};
        FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, result, 0,
                       details, static_cast<DWORD>(std::size(details)), nullptr);
        const auto message = L"Ошибка Windows " + std::to_wstring(result) + L". " + details;
        SetWindowTextW(status, message.c_str());
    }
}
void AppRoutingDialog::create_primary_control() {
    control(window_, L"STATIC", L"Основная сеть", 0, 29, 64, 312, 24,
            SplitPrimaryLabel);
    HWND primary = control(window_, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP,
                           29, 89, 312, 180, SplitPrimary);
    const auto saved = split_routing_->primary_id.empty()
        ? QuickDns::Encoding::utf8_to_wide(store_.settings.primary_id) : split_routing_->primary_id;
    for (size_t i = 0; i < adapters_.size(); ++i) {
        const auto& adapter = adapters_[i];
        const auto label = adapter.name + (adapter.connected ? L"" : L" (отключён)");
        SendMessageW(primary, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        if (adapter.id == saved) SendMessageW(primary, CB_SETCURSEL, i, 0);
    }
    control(window_, L"STATIC", L"", SS_ENDELLIPSIS, 29, 116, 312, 20, SplitPrimaryDns);
    update_adapter_dns(SplitPrimary, SplitPrimaryDns);
}

void AppRoutingDialog::update_adapter_dns(int adapter_control, int dns_control) const {
    const auto selection = SendMessageW(GetDlgItem(window_, adapter_control), CB_GETCURSEL, 0, 0);
    std::wstring addresses;
    if (selection >= 0 && static_cast<size_t>(selection) < adapters_.size()) {
        const auto& adapter = adapters_[static_cast<size_t>(selection)];
        for (const auto* family : {&adapter.ipv4_dns, &adapter.ipv6_dns}) {
            for (const auto& address : *family) {
                if (!addresses.empty()) addresses += L", ";
                addresses += address;
            }
        }
        if (addresses.empty()) addresses = L"не указаны";
        addresses = L"DNS адаптера (справочно) — " + addresses;
    }
    SetWindowTextW(GetDlgItem(window_, dns_control), addresses.c_str());
}

void AppRoutingDialog::create_split_controls() {
    const bool embedded = has_embedded_routing_engine();
    control(window_, L"STATIC", L"Движок маршрутизации", 0, 29, 476, 648, 24, SplitEngineLabel);
    const auto engine = split_routing_->engine.empty() ? colocated_engine() : split_routing_->engine;
    const auto field = control(window_, L"EDIT", embedded ? L"sing-box встроен в QuickDNS" : engine.c_str(),
                               WS_BORDER | ES_AUTOHSCROLL | (embedded ? ES_READONLY : WS_TABSTOP),
                               29, 504, 526, 25, SplitEngine);
    if (!embedded)
        SendMessageW(field, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(L"Укажите путь к sing-box.exe"));
    control(window_, L"BUTTON", L"Обзор…", BS_PUSHBUTTON | (embedded ? WS_DISABLED : WS_TABSTOP),
            565, 504, 112, 26, SplitBrowse);
    control(window_, L"STATIC", L"Внешние сайты используют вторичную сеть. Строгая изоляция недоступна.",
            0, 29, 541, 645, 20, SplitNotice);
    control(window_, L"BUTTON", L"Запустить", BS_PUSHBUTTON | WS_TABSTOP, 435, 568, 120, 30, SplitStart);
    control(window_, L"BUTTON", L"Остановить", BS_PUSHBUTTON | WS_TABSTOP, 565, 568, 112, 30, SplitStop);
    control(window_, L"STATIC", L"", 0, 29, 568, 393, 42, SplitStatus);
    refresh_split_status();
    SetTimer(window_, SplitStatus, 500, nullptr);
}

void AppRoutingDialog::refresh_split_status() {
    const HWND status_control = GetDlgItem(window_, SplitStatus);
    const auto status = split_routing_->status();
    if (text(status_control) != status) SetWindowTextW(status_control, status.c_str());
    const bool running = split_routing_->running();
    for (int id : {SplitPrimary, RoutingAdapterCombo, SplitStart})
        EnableWindow(GetDlgItem(window_, id), !running);
    const bool can_choose_engine = !running && !has_embedded_routing_engine();
    EnableWindow(GetDlgItem(window_, SplitEngine), !running || has_embedded_routing_engine());
    EnableWindow(GetDlgItem(window_, SplitBrowse), can_choose_engine);
    EnableWindow(GetDlgItem(window_, SplitStop), running);
}

void AppRoutingDialog::start_split() {
    const auto get = [this](int id) { return GetDlgItem(window_, id); };
    const auto selected = selected_rules(get(RoutingList), true);
    const auto selected_primary = SendMessageW(get(SplitPrimary), CB_GETCURSEL, 0, 0);
    const auto set_error = [&get](const wchar_t* error) { SetWindowTextW(get(SplitStatus), error); };
    if (selected_primary < 0 || static_cast<size_t>(selected_primary) >= adapters_.size()) {
        set_error(L"Выберите основной адаптер.");
        return;
    }
    if (subnet_routes_ && !subnet_routes_->routes().empty()) {
        set_error(L"Удалите маршруты по подсетям перед запуском sing-box.");
        return;
    }
    const auto secondary_combo = SendMessageW(get(RoutingAdapterCombo), CB_GETCURSEL, 0, 0);
    if (secondary_combo < 0 || static_cast<size_t>(secondary_combo) >= adapters_.size()) {
        set_error(L"Выберите вторичную сеть.");
        return;
    }
    const auto adapter_id = adapters_[static_cast<size_t>(secondary_combo)].id;
    const auto secondary = std::find_if(adapters_.begin(), adapters_.end(), [&](const auto& a) {
        return CompareStringOrdinal(a.id.c_str(), -1, adapter_id.c_str(), -1, TRUE) == CSTR_EQUAL;
    });
    if (secondary == adapters_.end()) {
        set_error(L"Адаптер из правила недоступен.");
        return;
    }

    std::optional<std::vector<InstalledPackage>> installed;
    std::vector<SplitRoutingTarget> targets;
    for (const auto row : selected) {
        const auto& rule = drafts_[static_cast<size_t>(row)];
        const auto id = QuickDns::Encoding::utf8_to_wide(rule.adapter_id);
        if (CompareStringOrdinal(id.c_str(), -1, adapter_id.c_str(), -1, TRUE) != CSTR_EQUAL) {
            set_error(L"Выберите правила для текущей вторичной сети.");
            return;
        }
        auto family = QuickDns::Encoding::utf8_to_wide(rule.package_family);
        if (family.empty()) {
            targets.push_back({QuickDns::Encoding::utf8_to_wide(rule.application_path), {}});
            continue;
        }
        if (!installed) installed = enumerate_installed_packages();
        const auto package = std::find_if(installed->begin(), installed->end(), [&](const auto& p) {
            return CompareStringOrdinal(p.family.c_str(), -1, family.c_str(), -1, TRUE) == CSTR_EQUAL;
        });
        if (package == installed->end() || package->install_path.empty()) {
            set_error(L"Каталог одного из выбранных пакетов MSIX недоступен.");
            return;
        }
        targets.push_back({package->install_path, std::move(family)});
    }

    const bool embedded = has_embedded_routing_engine();
    const auto engine = embedded ? std::wstring{} : text(get(SplitEngine));
    if (!embedded && engine.empty()) {
        set_error(L"Выберите sing-box.exe.");
        return;
    }
    if (MessageBoxW(window_, embedded ? L"Запустить встроенный движок? Он создаст TUN и изменит маршруты Windows для этого сеанса."
                            : L"Запустить выбранный sing-box.exe с правами администратора? "
                              L"Он создаст TUN и изменит маршруты Windows для этого сеанса. "
                              L"Проверьте происхождение файла. Полная изоляция трафика не гарантируется.",
                    L"QuickDNS", MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) != IDOK) return;
    const auto result = split_routing_->start(engine, adapters_[static_cast<size_t>(selected_primary)],
                                              *secondary, targets, false, {}, true);
    if (result.empty()) {
        split_routing_->secondary_id = adapter_id;
        refresh_split_status();
    } else set_error(result.c_str());
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
        const BOOL use_dark = dark_;
        DwmSetWindowAttribute(window_, 20, &use_dark, sizeof(use_dark));
        background_ = CreateSolidBrush(palette_.bg);
        field_ = CreateSolidBrush(palette_.cardBg);
        font_ = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        heading_ = CreateFontW(-17, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI Variable Display");
        if (subnet_routes_) {
            control(window_, L"BUTTON", L"Приложения", BS_PUSHBUTTON | WS_TABSTOP, 17, 14, 142, 31, RoutingAppsPage);
            control(window_, L"BUTTON", L"Подсети", BS_PUSHBUTTON | WS_TABSTOP, 167, 14, 118, 31, RoutingSubnetsPage);
        }
        const int offset = split_routing_ ? 118 : subnet_routes_ ? 51 : 0;
        if (split_routing_) {
            create_primary_control();
            control(window_, L"STATIC", L"Вторичная сеть", 0, 365, 64, 312, 24, SplitSecondaryLabel);
            control(window_, L"STATIC", L"Правила маршрутизации", 0, 29, 144, 645, 24, SplitRulesLabel);
        }
        control(window_, L"STATIC", split_routing_ ? L"Ctrl+щелчок выбирает несколько правил." : inactive,
                SS_ENDELLIPSIS, 29, split_routing_ ? 437 : 18 + offset, 645, 20, RoutingStatus);
        HWND kind = control(window_, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP,
                            29, 48 + offset, 174, 160, RoutingKind);
        SendMessageW(kind, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Файл EXE"));
        SendMessageW(kind, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Семейство MSIX"));
        SendMessageW(kind, CB_SETCURSEL, 0, 0);
        control(window_, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL,
                213, 48 + offset, 464, 180, RoutingPackages);
        control(window_, L"STATIC", L"Путь EXE или имя семейства пакета", 0, 29, 82 + offset, 510, 20);
        control(window_, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP,
                29, 102 + offset, 526, 25, RoutingIdentity);
        control(window_, L"BUTTON", L"Обзор…", BS_PUSHBUTTON | WS_TABSTOP, 565, 102 + offset, 112, 26, RoutingBrowse);
        if (!split_routing_)
            control(window_, L"STATIC", L"Сеть приложения", 0, 29, 134 + offset, 526, 20);
        HWND combo = control(window_, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP,
                             split_routing_ ? 365 : 29, split_routing_ ? 89 : 154 + offset,
                             split_routing_ ? 312 : 526, 180, RoutingAdapterCombo);
        auto secondary_id = split_routing_ ? split_routing_->secondary_id : std::wstring{};
        if (split_routing_ && secondary_id.empty()) {
            secondary_id = QuickDns::Encoding::utf8_to_wide(store_.settings.secondary_id);
            if (secondary_id.empty() && drafts_.size() == 1)
                secondary_id = QuickDns::Encoding::utf8_to_wide(drafts_.front().adapter_id);
        }
        for (size_t i = 0; i < adapters_.size(); ++i) {
            const auto& adapter = adapters_[i];
            const std::wstring label = adapter.name + (adapter.connected ? L" (подключён)" : L" (отключён)");
            SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
            if (adapter.id == secondary_id) SendMessageW(combo, CB_SETCURSEL, i, 0);
        }
        if (!split_routing_ && !adapters_.empty()) SendMessageW(combo, CB_SETCURSEL, 0, 0);
        if (split_routing_) {
            control(window_, L"STATIC", L"", SS_ENDELLIPSIS, 365, 116, 312, 20, RoutingAdapterDns);
            update_adapter_dns(RoutingAdapterCombo, RoutingAdapterDns);
        }
        control(window_, L"BUTTON", L"Добавить правило", BS_PUSHBUTTON | WS_TABSTOP,
                545, split_routing_ ? 253 : 154 + offset, 132, 27, RoutingAdd);
        control(window_, L"LISTBOX", L"", WS_BORDER | LBS_NOTIFY | WS_VSCROLL | WS_TABSTOP |
                (split_routing_ ? LBS_EXTENDEDSEL | LBS_DISABLENOSCROLL : 0),
                29, split_routing_ ? 287 : 190 + offset, 506, split_routing_ ? 145 : 160, RoutingList);
        control(window_, L"BUTTON", L"Удалить", BS_PUSHBUTTON | WS_TABSTOP,
                545, split_routing_ ? 287 : 190 + offset, 132, 27, RoutingRemove);
        control(window_, L"BUTTON", L"Сохранить", BS_PUSHBUTTON | WS_TABSTOP,
                545, split_routing_ ? 326 : 323 + offset, 132, 27, RoutingSave);
        if (split_routing_) create_split_controls();
        if (subnet_routes_) {
            creating_subnets_ = true;
            create_subnet_controls();
            creating_subnets_ = false;
        }
        select_page(false);
        refresh();
        if (split_routing_ && drafts_.size() == 1)
            select_rule(get(RoutingList), true, 0);
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case SplitPrimary:
        case RoutingAdapterCombo:
            if (split_routing_ && HIWORD(wp) == CBN_SELCHANGE)
                update_adapter_dns(LOWORD(wp), LOWORD(wp) == SplitPrimary ? SplitPrimaryDns : RoutingAdapterDns);
            return 0;
        case RoutingAppsPage:
        case RoutingSubnetsPage:
            if (subnet_routes_ && HIWORD(wp) == BN_CLICKED) select_page(LOWORD(wp) == RoutingSubnetsPage);
            return 0;
        case SplitBrowse: {
            if (!split_routing_ || split_routing_->running() || has_embedded_routing_engine()) return 0;
            wchar_t path[32768]{};
            OPENFILENAMEW picker{};
            picker.lStructSize = sizeof(picker);
            picker.hwndOwner = window_;
            picker.lpstrFile = path;
            picker.nMaxFile = static_cast<DWORD>(std::size(path));
            picker.lpstrFilter = L"sing-box.exe\0sing-box.exe\0\0";
            picker.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
            if (GetOpenFileNameW(&picker)) SetWindowTextW(get(SplitEngine), path);
            return 0;
        }
        case SplitStart:
            if (split_routing_) start_split();
            return 0;
        case SplitStop:
            if (split_routing_) { split_routing_->stop(); refresh_split_status(); }
            return 0;
        case SubnetAdd:
        case SubnetRemove:
            if (subnet_routes_ && HIWORD(wp) == BN_CLICKED) change_subnet(LOWORD(wp));
            return 0;
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
                    // The OpenAI.Codex MSIX ships an executable named ChatGPT.exe.
                    const bool openai_codex = CompareStringOrdinal(package.family.c_str(), -1,
                        L"OpenAI.Codex_2p2nqsd0c76g0", -1, TRUE) == CSTR_EQUAL;
                    const std::wstring label = (openai_codex ? L"ChatGPT.exe (" + package.name + L")"
                                                             : package.name) + L" — " + package.family;
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
            const auto selected = selected_rules(get(RoutingList), split_routing_ != nullptr);
            drafts_ = std::move(next);
            refresh();
            if (split_routing_)
                for (const auto row : selected) select_rule(get(RoutingList), true, static_cast<size_t>(row));
            select_rule(get(RoutingList), split_routing_ != nullptr, drafts_.size() - 1);
            SetWindowTextW(get(RoutingStatus), split_routing_ ? L"Правило добавлено."
                                                            : inactive);
            return 0;
        }
        case RoutingRemove: {
            auto selected = selected_rules(get(RoutingList), split_routing_ != nullptr);
            if (selected.empty()) return 0;
            std::sort(selected.rbegin(), selected.rend());
            auto next = drafts_;
            for (const auto index : selected) next.erase(next.begin() + index);
            if (store_.replace_rules(next)) { drafts_ = std::move(next); refresh(); }
            return 0;
        }
        case RoutingSave: {
            if (split_routing_) {
                const auto adapter_id = [&](int id) -> std::string {
                    const auto row = SendMessageW(get(id), CB_GETCURSEL, 0, 0);
                    return row >= 0 && static_cast<size_t>(row) < adapters_.size()
                        ? QuickDns::Encoding::wide_to_utf8(adapters_[static_cast<size_t>(row)].id) : std::string{};
                };
                store_.settings = {adapter_id(SplitPrimary), adapter_id(RoutingAdapterCombo)};
            }
            const bool saved = store_.save();
            if (saved && split_routing_ && !split_routing_->running()) {
                split_routing_->primary_id = QuickDns::Encoding::utf8_to_wide(store_.settings.primary_id);
                split_routing_->secondary_id = QuickDns::Encoding::utf8_to_wide(store_.settings.secondary_id);
            }
            SetWindowTextW(get(RoutingStatus), saved ? L"Конфигурация сохранена." : L"Ошибка сохранения.");
            return 0;
        }
        }
        break;
    case WM_ERASEBKGND: {
        const HDC dc = reinterpret_cast<HDC>(wp);
        RECT client{};
        GetClientRect(window_, &client);
        FillRect(dc, &client, background_);
        const auto card = [&](RECT bounds) {
            draw_surface(dc, bounds, 12.0f, palette_.cardBg, palette_.cardBg, palette_.cardBorder);
        };
        if (subnets_selected_) card({16, 56, 692, 268});
        else if (split_routing_) {
            card({16, 55, 692, 136});
            card({16, 140, 692, 463});
            card({16, 469, 692, 614});
        } else card({16, subnet_routes_ ? 56 : 7, 692, subnet_routes_ ? 423 : 372});
        return 1;
    }
    case WM_CTLCOLORSTATIC: {
        const HDC dc = reinterpret_cast<HDC>(wp);
        const int id = GetDlgCtrlID(reinterpret_cast<HWND>(lp));
        const bool dns = id == SplitPrimaryDns || id == RoutingAdapterDns;
        SetTextColor(dc, id == SplitNotice ? palette_.accentAmber : dns ? palette_.textSecondary : palette_.textPrimary);
        SetBkMode(dc, TRANSPARENT);
        return reinterpret_cast<LRESULT>(field_);
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        const HDC dc = reinterpret_cast<HDC>(wp);
        SetTextColor(dc, palette_.textPrimary);
        SetBkColor(dc, palette_.cardBg);
        return reinterpret_cast<LRESULT>(field_);
    }
    case WM_DRAWITEM:
        draw_control(*reinterpret_cast<const DRAWITEMSTRUCT*>(lp));
        return TRUE;
    case WM_TIMER:
        if (wp == SplitStatus && split_routing_) {
            if (IsWindowEnabled(get(SplitStop)) || split_routing_->running()) refresh_split_status();
            return 0;
        }
        break;
    case WM_CLOSE: DestroyWindow(window_); return 0;
    case WM_NCDESTROY: {
        HWND old = window_;
        window_ = nullptr;
        app_controls_.clear();
        subnet_controls_.clear();
        for (auto brush : {background_, field_}) if (brush) DeleteObject(brush);
        background_ = field_ = nullptr;
        for (auto font : {font_, heading_}) if (font) DeleteObject(font);
        font_ = heading_ = nullptr;
        SetWindowLongPtrW(old, GWLP_USERDATA, 0);
        return DefWindowProcW(old, msg, wp, lp);
    }
    }
    return DefWindowProcW(window_, msg, wp, lp);
}
}
