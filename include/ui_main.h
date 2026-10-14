#pragma once

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include "dns_manager.h"
#include "config_manager.h"
#include "resource.h"

// Custom window messages
#define WM_APP_TRAYMSG          (WM_APP + 1)
#define WM_APP_DNS_APPLIED      (WM_APP + 2)
#define WM_APP_REFRESH_STATUS   (WM_APP + 3)
#define WM_APP_PING_UPDATED     (WM_APP + 4)

class MainWindow {
public:
    MainWindow();
    ~MainWindow();

    bool initialize(HINSTANCE hInstance, int nCmdShow);
    int run_message_loop();

    bool is_dark_mode() const { return m_isDarkMode; }
    HFONT get_font_regular() const { return m_hFontRegular; }

private:
    static LRESULT CALLBACK window_proc_thunk(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK graph_proc_thunk(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    LRESULT handle_message(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    LRESULT handle_graph_message(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

    // Initialization helpers
    void register_window_class();
    void create_controls();
    void apply_modern_theme();
    void setup_fonts();
    void setup_tray_icon();
    void remove_tray_icon();
    void show_tray_notification(const std::wstring& title, const std::wstring& message);

    // Background Real-Time Ping Monitor
    void start_ping_worker();
    void stop_ping_worker();
    void trigger_immediate_ping();

    // Data binding & UI updates
    void refresh_network_adapters();
    void populate_profiles_combobox();
    void update_status_card();
    void on_adapter_changed();
    void on_profile_selected();

    // Custom Graphical Rendering
    void draw_custom_button(DRAWITEMSTRUCT* pDis);
    void draw_theme_toggle_button(DRAWITEMSTRUCT* pDis);
    void draw_sparkline(HWND hWnd, HDC hdc);
    void draw_combobox_item(DRAWITEMSTRUCT* pDis);
    void attach_button_effects(HWND hBtn, UINT_PTR subclassId);
    void attach_combobox_effects(HWND hCombo, UINT_PTR subclassId);

    // Theme Management
    void apply_theme(bool isDark);
    void toggle_theme();

    // DNS Actions
    void apply_selected_dns();
    void reset_to_dhcp();
    void flush_dns_cache();
    void open_add_profile_dialog();
    void open_benchmark_dialog();
    void open_smartdns_catalog_dialog();
    void open_windows_network_settings();
    void copy_text_to_clipboard(const std::wstring& text);

    // Tray Menu
    void show_tray_context_menu();
    void on_tray_preset_selected(int commandId);

    // Members
    HINSTANCE m_hInstance = nullptr;
    HWND m_hWnd = nullptr;
    HFONT m_hFontRegular = nullptr;
    HFONT m_hFontBold = nullptr;
    HFONT m_hFontHeader = nullptr;
    HFONT m_hFontMonospace = nullptr;
    HFONT m_hFontSmall = nullptr;
    HFONT m_hFontGraphVal = nullptr;

    // Custom Theme Brushes
    HBRUSH m_hbrBg = nullptr;
    HBRUSH m_hbrCard = nullptr;
    bool m_isDarkMode = true;

    // Controls
    HWND m_hComboAdapter = nullptr;
    HWND m_hComboProfile = nullptr;
    HWND m_hBtnThemeToggle = nullptr;
    HWND m_hToolTip = nullptr;

    HWND m_hEditProfileIp = nullptr;
    HWND m_hBtnCopyIp = nullptr;
    HWND m_hEditProfileDoh = nullptr;
    HWND m_hBtnCopyDoh = nullptr;

    HWND m_hBtnApply = nullptr;
    HWND m_hBtnBenchmark = nullptr;
    HWND m_hBtnSmartDnsCatalog = nullptr;
    HWND m_hBtnResetDhcp = nullptr;
    HWND m_hBtnFlushDns = nullptr;
    HWND m_hBtnWinNetwork = nullptr;
    HWND m_hBtnAddProfile = nullptr;
    HWND m_hBtnDeleteProfile = nullptr;

    HWND m_hStatusAdapterName = nullptr;
    HWND m_hStatusDnsList = nullptr;
    HWND m_hStatusBadge = nullptr;
    HWND m_hStatusPing = nullptr;
    HWND m_hGraphWnd = nullptr;
    HWND m_hProfileDetails = nullptr;
    HWND m_hFooterVersion = nullptr;

    // System Tray
    NOTIFYICONDATAW m_nid = {};
    bool m_inTray = false;

    // Business Logic Engines
    std::unique_ptr<DnsManager> m_dnsManager;
    std::unique_ptr<ConfigManager> m_configManager;

    std::vector<NetworkAdapterInfo> m_adapters;
    std::vector<DnsProfile> m_profiles;
    int m_selectedAdapterIndex = -1;
    int m_selectedProfileIndex = -1;

    std::atomic<bool> m_isBusy = false;

    // Ping Monitor State
    std::thread m_pingWorker;
    std::atomic<bool> m_stopPingWorker{false};
    std::atomic<bool> m_pingImmediate{false};
    std::mutex m_historyMutex;
    std::vector<int> m_latencyHistory;
    int m_currentPingMs = -1;
};
