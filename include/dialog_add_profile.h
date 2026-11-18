#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <array>
#include <optional>
#include <string>
#include "config_manager.h"
#include "ui_palette.h"

class AddProfileDialog {
public:
    enum class FieldIndex : size_t {
        Name = 0,
        PrimaryIpv4 = 1,
        SecondaryIpv4 = 2,
        DohTemplate = 3,
        Description = 4,
        Count = 5
    };

    static std::optional<DnsProfile> show(HWND hParent, HINSTANCE hInstance, bool isDarkMode = true,
                                          const std::wstring& defaultPriIp = L"",
                                          const std::wstring& defaultSecIp = L"");

    HFONT get_font_regular() const { return m_hFontRegular; }
    HFONT get_font_bold() const { return m_hFontBold; }
    HFONT get_font_small() const { return m_hFontSmall; }
    HFONT get_font_small_bold() const { return m_hFontSmallBold; }
    bool is_dark_mode() const { return m_isDarkMode; }
    bool is_smart_dns() const { return m_isSmartDns; }
    void set_smart_dns(bool enabled);
    void toggle_smart_dns();
    void set_field_focused(size_t index, bool focused);
    void set_field_hovered(size_t index, bool hovered);
    void on_save();
    void on_cancel();
    int dp(int px) const;
    float dpf(float px) const;

private:
    struct LayoutMetrics {
        float scale = 1.0f;
        int marginX = 20;
        int contentW = 560;

        int headerTitleY = 14;
        int headerSubY = 39;
        int headerSubH = 20;
        int headerSepY = 66;

        int cardTop = 80;
        int cardPadX = 16;
        int innerX = 36;
        int innerW = 528;
        int colGap = 14;
        int colW = 257;

        int fieldH = 36;
        int innerEditH = 20;
        int innerEditOffsetY = 8;
        int innerEditPadX = 12;
        int labelOffsetY = 20;

        int yName = 114;
        int yIpRow = 184;
        int cardInnerSepY = 236;
        int yDoh = 270;
        int yDesc = 340;
        int cardH = 312;

        int ySmart = 406;
        int smartH = 78;

        int footerSepY = 500;
        int yButtons = 514;
        int btnH = 38;
        int btnCancelW = 118;
        int btnSaveW = 196;
        int btnGap = 10;
    };

    AddProfileDialog(HWND hParent, HINSTANCE hInstance, bool isDarkMode,
                     const std::wstring& defaultPriIp = L"",
                     const std::wstring& defaultSecIp = L"");
    ~AddProfileDialog();

    bool create_dialog_window();
    void setup_fonts(float scale);
    void apply_fonts_to_controls();
    void apply_theme();
    void create_controls();
    void reposition_controls();
    LayoutMetrics compute_layout(int clientW, int clientH) const;
    void invalidate_field_box(size_t index);

    void draw_custom_button(DRAWITEMSTRUCT* pDis);
    void attach_button_effects(HWND hBtn, UINT_PTR subclassId);
    void attach_edit_effects(HWND hEdit, size_t fieldIndex, UINT_PTR subclassId);

    static LRESULT CALLBACK dialog_proc_thunk(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    LRESULT handle_message(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

    HWND m_hParent = nullptr;
    HINSTANCE m_hInstance = nullptr;
    HWND m_hWnd = nullptr;
    UINT m_dpi = 96;
    float m_activeFontScale = 0.0f;
    bool m_isDarkMode = true;
    bool m_isSmartDns = false;
    bool m_trackingMouseLeave = false;
    int m_hoveredContainerField = -1;

    RECT m_smartCardRect{};
    std::array<RECT, static_cast<size_t>(FieldIndex::Count)> m_fieldRects{};
    std::array<bool, static_cast<size_t>(FieldIndex::Count)> m_fieldFocused{};
    std::array<bool, static_cast<size_t>(FieldIndex::Count)> m_fieldEditHovered{};
    std::array<bool, static_cast<size_t>(FieldIndex::Count)> m_fieldErrors{};

    HBRUSH m_hbrBg = nullptr;
    HBRUSH m_hbrCard = nullptr;
    HBRUSH m_hbrInput = nullptr;

    HFONT m_hFontRegular = nullptr;
    HFONT m_hFontBold = nullptr;
    HFONT m_hFontHeader = nullptr;
    HFONT m_hFontSmall = nullptr;
    HFONT m_hFontSmallBold = nullptr;

    HWND m_hEditName = nullptr;
    HWND m_hEditPri = nullptr;
    HWND m_hEditSec = nullptr;
    HWND m_hEditDoh = nullptr;
    HWND m_hEditDesc = nullptr;
    HWND m_hCheckSmartDns = nullptr;
    HWND m_hBtnCancel = nullptr;
    HWND m_hBtnSave = nullptr;

    std::wstring m_defaultPriIp;
    std::wstring m_defaultSecIp;

    std::optional<DnsProfile> m_result;
    bool m_finished = false;
};
