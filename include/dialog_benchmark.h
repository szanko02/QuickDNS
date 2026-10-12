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
#include "dns_benchmark.h"
#include "config_manager.h"
#include "ui_palette.h"

namespace quickdns {

struct BenchmarkItem {
    DnsProfile profile;
    int current_ping = -1;
    DnsQuality quality = DnsQuality::SlowOrDown;
    std::wstring status_text = L"Ожидание...";
    std::wstring quality_label = L"Ожидание";
    std::vector<int> latency_history;
};

class BenchmarkDialog {
public:
    static std::optional<std::string> show(
        HWND hParent,
        HINSTANCE hInstance,
        const std::vector<DnsProfile>& profiles,
        const std::string& currentActiveId = "",
        bool isDarkMode = true
    );

private:
    BenchmarkDialog(HWND hParent, HINSTANCE hInstance, const std::vector<DnsProfile>& profiles, const std::string& currentActiveId, bool isDarkMode);
    ~BenchmarkDialog();

    bool create_dialog_window();
    void apply_theme();
    void setup_fonts();
    void create_controls();
    void start_ping_worker();
    void stop_ping_worker();
    void toggle_pause();
    void on_apply();
    void on_close();

    static LRESULT CALLBACK dialog_proc_thunk(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK list_proc_thunk(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    LRESULT handle_message(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    LRESULT handle_list_message(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

    void draw_list_item(Gdiplus::Graphics& g, int index, int y, int width, int height, bool isSelected, bool isHovered);
    void draw_custom_button(DRAWITEMSTRUCT* pDis);
    void attach_button_effects(HWND hBtn, UINT_PTR subclassId);

    HWND m_hParent = nullptr;
    HINSTANCE m_hInstance = nullptr;
    HWND m_hWnd = nullptr;
    HWND m_hListWnd = nullptr;

    bool m_isDarkMode = true;
    HBRUSH m_hbrBg = nullptr;
    HBRUSH m_hbrCard = nullptr;

    HFONT m_hFontRegular = nullptr;
    HFONT m_hFontBold = nullptr;
    HFONT m_hFontHeader = nullptr;
    HFONT m_hFontMonospace = nullptr;
    HFONT m_hFontSmall = nullptr;

    HWND m_hStatusText = nullptr;
    HWND m_hBtnApply = nullptr;
    HWND m_hBtnPauseResume = nullptr;
    HWND m_hBtnClose = nullptr;

    std::vector<BenchmarkItem> m_items;
    std::string m_currentActiveId;
    std::mutex m_itemsMutex;

    int m_selectedIndex = 0;
    int m_hoveredIndex = -1;
    int m_scrollOffset = 0;

    std::atomic<bool> m_workerRunning{false};
    std::atomic<bool> m_isPaused{false};
    std::thread m_workerThread;

    std::optional<std::string> m_selectedProfileId;
    bool m_finished = false;
};

} // namespace quickdns
