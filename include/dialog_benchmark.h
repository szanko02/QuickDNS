#pragma once

#include <windows.h>
#include <commctrl.h>
#include <string>
#include <vector>
#include <optional>
#include <memory>
#include <atomic>
#include "dns_benchmark.h"
#include "config_manager.h"

namespace quickdns {

class BenchmarkDialog {
public:
    static std::optional<std::string> show(
        HWND hParent,
        HINSTANCE hInstance,
        const std::vector<DnsProfile>& profiles,
        const std::string& currentActiveId = ""
    );

private:
    BenchmarkDialog(HWND hParent, HINSTANCE hInstance, const std::vector<DnsProfile>& profiles, const std::string& currentActiveId);
    ~BenchmarkDialog();

    bool create_dialog_window();
    void apply_theme();
    void setup_fonts();
    void create_controls();
    void start_benchmark();
    void update_list_with_results();
    void on_apply();
    void on_close();

    static LRESULT CALLBACK dialog_proc_thunk(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    LRESULT handle_message(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

    HWND m_hParent = nullptr;
    HINSTANCE m_hInstance = nullptr;
    HWND m_hWnd = nullptr;

    HFONT m_hFontRegular = nullptr;
    HFONT m_hFontBold = nullptr;
    HFONT m_hFontHeader = nullptr;

    HWND m_hStatusText = nullptr;
    HWND m_hListView = nullptr;
    HWND m_hBtnApply = nullptr;
    HWND m_hBtnRetest = nullptr;
    HWND m_hBtnClose = nullptr;

    std::vector<DnsProfile> m_profiles;
    std::string m_currentActiveId;
    std::vector<DnsBenchmarkResult> m_results;

    std::atomic<bool> m_isTesting{false};
    std::optional<std::string> m_selectedProfileId;
    bool m_finished = false;
};

} // namespace quickdns
