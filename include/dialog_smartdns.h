#pragma once

#include <windows.h>
#include <commctrl.h>
#include <string>
#include <vector>
#include <optional>
#include <memory>
#include "config_manager.h"
#include "ui_palette.h"

namespace quickdns {

struct SmartDnsCategory {
    std::wstring name;
    std::vector<std::wstring> services;
};

struct SmartDnsEntry {
    std::string profile_id;
    std::wstring name;
    std::wstring website_url;
    std::wstring primary_ipv4;
    std::wstring secondary_ipv4;
    std::wstring doh_template;
    std::wstring location;
    std::wstring description;
    std::vector<SmartDnsCategory> categories;
    bool is_active = false;
    std::wstring doh_status;
    int ping_ms = -1;
    bool is_verified = false;
};

class SmartDnsDialog {
public:
    /**
     * @brief Displays the SmartDNS Catalog & Unblocked Services modal dialog.
     * @param hParent Parent window handle.
     * @param hInstance Application instance.
     * @param profiles Loaded DNS profiles.
     * @param currentActiveId ID of currently active profile.
     * @param isDarkMode Whether dark mode is enabled.
     * @param pConfig Optional pointer to ConfigManager for dynamic IP updates.
     * @return Selected profile ID if user clicked "Apply", std::nullopt otherwise.
     */
    static std::optional<std::string> show(
        HWND hParent,
        HINSTANCE hInstance,
        const std::vector<DnsProfile>& profiles,
        const std::string& currentActiveId = "",
        bool isDarkMode = true,
        ConfigManager* pConfig = nullptr
    );

private:
    SmartDnsDialog(
        HWND hParent,
        HINSTANCE hInstance,
        const std::vector<DnsProfile>& profiles,
        const std::string& currentActiveId,
        bool isDarkMode,
        ConfigManager* pConfig = nullptr
    );
    ~SmartDnsDialog();

    bool create_dialog_window();
    void apply_theme();
    void setup_fonts();
    void create_controls();
    void populate_smartdns_data(const std::vector<DnsProfile>& profiles);
    void update_filtered_list();
    void on_apply_selected();
    void on_open_website(size_t index);
    void on_verify_and_scrape_ips();
    void on_close();

    static LRESULT CALLBACK dialog_proc_thunk(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK list_proc_thunk(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK search_edit_subclass(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData);

    LRESULT handle_message(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    LRESULT handle_list_message(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

    void reposition_controls();
    void recalculate_item_layouts();
    int calculate_item_height(const SmartDnsEntry& entry, int cardWidth) const;
    int get_item_at_point(int mouseY, int& outItemY) const;
    void draw_card_item(Gdiplus::Graphics& g, size_t index, int y, int width, int height, bool isSelected, bool isHovered);
    void draw_custom_button(DRAWITEMSTRUCT* pDis);
    void attach_button_effects(HWND hBtn, UINT_PTR subclassId);

    HWND m_hParent = nullptr;
    HINSTANCE m_hInstance = nullptr;
    HWND m_hWnd = nullptr;
    HWND m_hListWnd = nullptr;
    HWND m_hSearchEdit = nullptr;
    HWND m_hBtnApply = nullptr;
    HWND m_hBtnWebsite = nullptr;
    HWND m_hBtnVerify = nullptr;
    HWND m_hBtnClose = nullptr;

    ConfigManager* m_pConfig = nullptr;
    bool m_isVerifying = false;

    std::wstring m_statusText;
    bool m_searchEditFocused = false;
    bool m_searchEditHovered = false;

    bool m_isDarkMode = true;
    HBRUSH m_hbrBg = nullptr;
    HBRUSH m_hbrCard = nullptr;

    HFONT m_hFontRegular = nullptr;
    HFONT m_hFontBold = nullptr;
    HFONT m_hFontHeader = nullptr;
    HFONT m_hFontMonospace = nullptr;
    HFONT m_hFontSmall = nullptr;
    HFONT m_hFontSmallBold = nullptr;

    std::vector<SmartDnsEntry> m_allEntries;
    std::vector<size_t> m_filteredIndices; // indices into m_allEntries
    std::vector<int> m_itemHeights;
    std::vector<int> m_itemYPositions;
    int m_totalContentHeight = 0;
    std::string m_currentActiveId;
    std::wstring m_searchQuery;

    int m_selectedIndex = 0;
    int m_hoveredIndex = -1;
    int m_scrollOffset = 0;
    bool m_isDraggingScroll = false;
    bool m_scrollThumbHovered = false;
    int m_dragStartY = 0;
    int m_dragStartOffset = 0;

    std::optional<std::string> m_selectedProfileId;
    bool m_finished = false;
};

} // namespace quickdns
