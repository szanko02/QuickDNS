#pragma once

#include <windows.h>
#include <commctrl.h>
#include <string>
#include <vector>
#include <optional>
#include <memory>
#include <atomic>
#include <mutex>
#include <thread>
#include "config_manager.h"
#include "ai_checker.h"
#include "ui_palette.h"

namespace quickdns {

struct AiCheckRowItem {
    DnsProfile profile;
    ProfileAiCheckResult result;
    bool is_completed = false;
    bool is_best = false;
};

class AiCheckDialog {
public:
    static std::optional<std::string> show(
        HWND hParent,
        HINSTANCE hInstance,
        const std::vector<DnsProfile>& profiles,
        const std::string& currentActiveId = "",
        bool isDarkMode = true
    );

private:
    AiCheckDialog(
        HWND hParent,
        HINSTANCE hInstance,
        const std::vector<DnsProfile>& profiles,
        const std::string& currentActiveId,
        bool isDarkMode
    );
    ~AiCheckDialog();

    bool create_dialog_window();
    void apply_theme();
    void setup_fonts();
    void create_controls();
    void reposition_controls();

    void start_check();
    void stop_check();
    void update_best_verdict_locked();
    void sort_rows_locked();

    void on_apply();
    void on_recheck();
    void on_close();

    static LRESULT CALLBACK dialog_proc_thunk(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK list_proc_thunk(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    LRESULT handle_message(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    LRESULT handle_list_message(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

    void draw_list_item(Gdiplus::Graphics& g, size_t rowIdx, int y, int width, int height, bool isSelected, bool isHovered);
    void draw_service_pill(Gdiplus::Graphics& g, const AiServiceCheckResult& svc, float x, float y, float w, float h);
    void draw_custom_button(DRAWITEMSTRUCT* pDis);
    void attach_button_effects(HWND hBtn, UINT_PTR subclassId);

    HWND m_hParent = nullptr;
    HINSTANCE m_hInstance = nullptr;
    HWND m_hWnd = nullptr;
    HWND m_hListWnd = nullptr;

    HWND m_hBtnApply = nullptr;
    HWND m_hBtnRecheck = nullptr;
    HWND m_hBtnClose = nullptr;

    bool m_isDarkMode = true;
    HBRUSH m_hbrBg = nullptr;
    HBRUSH m_hbrCard = nullptr;

    HFONT m_hFontRegular = nullptr;
    HFONT m_hFontBold = nullptr;
    HFONT m_hFontHeader = nullptr;
    HFONT m_hFontMonospace = nullptr;
    HFONT m_hFontSmall = nullptr;
    HFONT m_hFontSmallBold = nullptr;

    std::unique_ptr<Gdiplus::FontFamily> m_gpFontFamily;
    std::unique_ptr<Gdiplus::FontFamily> m_gpMonoFamily;
    std::unique_ptr<Gdiplus::Font> m_gpFontTitle;
    std::unique_ptr<Gdiplus::Font> m_gpFontSub;
    std::unique_ptr<Gdiplus::Font> m_gpFontBadge;
    std::unique_ptr<Gdiplus::Font> m_gpFontPill;
    std::unique_ptr<Gdiplus::Font> m_gpFontTotal;

    std::vector<AiCheckRowItem> m_rows;
    std::string m_currentActiveId;
    ClientIpInfo m_clientIpInfo;
    bool m_clientIpFetched = false;

    std::wstring m_verdictTitle;
    std::wstring m_verdictSub;
    int m_completedCount = 0;
    int m_bestRowIndex = -1;

    mutable std::mutex m_mutex;
    std::thread m_checkThread;
    std::atomic<bool> m_cancelFlag{false};
    std::atomic<bool> m_isChecking{false};

    int m_selectedIndex = 0;
    int m_hoveredIndex = -1;
    int m_scrollOffset = 0;
    bool m_isDraggingScroll = false;
    bool m_scrollThumbHovered = false;
    int m_dragStartY = 0;
    int m_dragStartOffset = 0;
    bool m_userChangedSelection = false;

    std::optional<std::string> m_selectedProfileId;
    bool m_finished = false;
};

} // namespace quickdns
