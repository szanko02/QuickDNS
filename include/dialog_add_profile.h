#pragma once

#include <windows.h>
#include <string>
#include <optional>
#include "config_manager.h"
#include "ui_palette.h"

class AddProfileDialog {
public:
    static std::optional<DnsProfile> show(HWND hParent, HINSTANCE hInstance, bool isDarkMode = true,
                                          const std::wstring& defaultPriIp = L"",
                                          const std::wstring& defaultSecIp = L"");

    HFONT get_font_bold() const { return m_hFontBold; }
    bool is_dark_mode() const { return m_isDarkMode; }
    bool is_smart_dns() const { return m_isSmartDns; }
    void set_smart_dns(bool enabled);
    void toggle_smart_dns();
    int dp(int px) const;
    float dpf(float px) const;

private:
    struct LayoutMetrics {
        float scale = 1.0f;
        int marginX = 24;
        int contentW = 532;
        int headerY = 18;
        int subY = 46;
        int subH = 38;
        int fieldH = 36;
        int innerEditH = 22;
        int innerOffset = 7;
        int labelOffsetY = 23;
        int yName = 104;
        int yPri = 182;
        int ySec = 260;
        int yDesc = 338;
        int yDoh = 416;
        int ySmart = 472;
        int smartH = 70;
        int checkH = 26;
        int smartSubTopPad = 38;
        int yButtons = 558;
        int btnH = 40;
        int btnGap = 14;
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
    void on_save();
    void on_cancel();

    void draw_custom_button(DRAWITEMSTRUCT* pDis);
    void attach_button_effects(HWND hBtn, UINT_PTR subclassId);

    static LRESULT CALLBACK dialog_proc_thunk(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    LRESULT handle_message(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

    HWND m_hParent = nullptr;
    HINSTANCE m_hInstance = nullptr;
    HWND m_hWnd = nullptr;
    UINT m_dpi = 96;
    float m_activeFontScale = 0.0f;
    bool m_isDarkMode = true;
    bool m_isSmartDns = false;
    RECT m_smartCardRect{};

    HBRUSH m_hbrBg = nullptr;
    HBRUSH m_hbrCard = nullptr;

    HFONT m_hFontRegular = nullptr;
    HFONT m_hFontBold = nullptr;
    HFONT m_hFontHeader = nullptr;
    HFONT m_hFontSmall = nullptr;
    HFONT m_hFontSmallBold = nullptr;

    HWND m_hEditName = nullptr;
    HWND m_hEditPri = nullptr;
    HWND m_hEditSec = nullptr;
    HWND m_hEditDesc = nullptr;
    HWND m_hEditDoh = nullptr;
    HWND m_hCheckSmartDns = nullptr;
    HWND m_hBtnSave = nullptr;
    HWND m_hBtnCancel = nullptr;

    std::wstring m_defaultPriIp;
    std::wstring m_defaultSecIp;

    std::optional<DnsProfile> m_result;
    bool m_finished = false;
};
