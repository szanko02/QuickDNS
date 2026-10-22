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
#include <algorithm>
#include <cmath>
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

    int total_queries = 0;
    int success_queries = 0;
    int min_ping = -1;
    int max_ping = -1;
    int avg_ping = -1;
    float jitter = 0.0f;
    float reliability = 0.0f;

    bool is_fastest = false;
    bool is_most_stable = false;
    bool is_fastest_smart = false;
    bool is_most_stable_smart = false;

    void reset_stats() {
        current_ping = -1;
        latency_history.clear();
        total_queries = 0;
        success_queries = 0;
        min_ping = -1;
        max_ping = -1;
        avg_ping = -1;
        jitter = 0.0f;
        reliability = 0.0f;
        is_fastest = false;
        is_most_stable = false;
        is_fastest_smart = false;
        is_most_stable_smart = false;
        quality = DnsQuality::Moderate;
        status_text = L"Ожидание...";
        quality_label = L"Ожидание";
    }

    void recalculate_stats() {
        std::vector<int> valid;
        for (int p : latency_history) {
            if (p > 0) valid.push_back(p);
        }
        if (valid.empty()) {
            min_ping = -1;
            max_ping = -1;
            avg_ping = -1;
            jitter = 0.0f;
            reliability = (total_queries > 0) ? (static_cast<float>(success_queries) * 100.0f / total_queries) : 0.0f;
            return;
        }
        min_ping = *std::min_element(valid.begin(), valid.end());
        max_ping = *std::max_element(valid.begin(), valid.end());
        long long sum = 0;
        for (int v : valid) sum += v;
        avg_ping = static_cast<int>(sum / valid.size());

        float diffSum = 0.0f;
        for (int v : valid) {
            diffSum += std::abs(static_cast<float>(v) - static_cast<float>(avg_ping));
        }
        jitter = diffSum / static_cast<float>(valid.size());
        reliability = (total_queries > 0) ? (static_cast<float>(success_queries) * 100.0f / total_queries) : 100.0f;
    }
};

enum class BenchmarkDurationMode {
    Timed30s = 0,
    Timed10s,
    Timed60s,
    Continuous
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

    void start_timed_test();
    void stop_timed_test(bool finishedNaturally = false);
    void reset_all_stats();
    void update_verdict();
    void update_filtered_indices();
    void set_duration_mode(BenchmarkDurationMode mode);
    void set_filter_smart_only(bool smartOnly);
    void show_ai_presets_menu();

    static LRESULT CALLBACK dialog_proc_thunk(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK list_proc_thunk(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    LRESULT handle_message(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    LRESULT handle_list_message(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

    void draw_list_item(Gdiplus::Graphics& g, size_t filteredIdx, int y, int width, int height, bool isSelected, bool isHovered);
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
    HFONT m_hFontSmallBold = nullptr;

    // Cached GDI+ Fonts for 60 FPS smooth rendering
    std::unique_ptr<Gdiplus::FontFamily> m_gpFontFamily;
    std::unique_ptr<Gdiplus::FontFamily> m_gpMonoFamily;
    std::unique_ptr<Gdiplus::Font> m_gpFontTitle;
    std::unique_ptr<Gdiplus::Font> m_gpFontSub;
    std::unique_ptr<Gdiplus::Font> m_gpFontBadge;
    std::unique_ptr<Gdiplus::Font> m_gpFontPing;

    // Domain & Presets Controls
    HWND m_hEditDomain = nullptr;
    HWND m_hBtnAiPresets = nullptr;

    // Filter Chips
    HWND m_hBtnFilterAll = nullptr;
    HWND m_hBtnFilterSmart = nullptr;

    // Duration Chips
    HWND m_hBtnDur10 = nullptr;
    HWND m_hBtnDur30 = nullptr;
    HWND m_hBtnDur60 = nullptr;
    HWND m_hBtnDurCont = nullptr;

    // Action Controls
    HWND m_hBtnStartStop = nullptr;
    HWND m_hBtnReset = nullptr;
    HWND m_hBtnApply = nullptr;
    HWND m_hBtnApplySmart = nullptr;
    HWND m_hBtnPauseResume = nullptr;
    HWND m_hBtnClose = nullptr;

    std::vector<BenchmarkItem> m_items;
    std::vector<size_t> m_filteredIndices;
    std::string m_currentActiveId;
    std::mutex m_itemsMutex;

    int m_selectedIndex = 0;
    int m_hoveredIndex = -1;
    int m_scrollOffset = 0;
    bool m_isDraggingScroll = false;
    bool m_scrollThumbHovered = false;
    int m_dragStartY = 0;
    int m_dragStartOffset = 0;

    std::atomic<bool> m_workerRunning{false};
    std::atomic<bool> m_isPaused{false};
    std::thread m_workerThread;

    std::string m_targetDomain = "google.com";
    bool m_filterOnlySmartDns = false;
    BenchmarkDurationMode m_durationMode = BenchmarkDurationMode::Timed30s;
    std::atomic<bool> m_isTimedRunning{false};
    int m_targetDurationSec = 30;
    int m_elapsedSec = 0;
    std::chrono::steady_clock::time_point m_timedStartTime;
    bool m_timedFinished = false;

    // Overall Winners
    int m_fastestIndex = -1;
    int m_mostStableIndex = -1;
    std::wstring m_verdictFastest;
    std::wstring m_verdictStable;

    // SmartDNS Winners
    int m_fastestSmartIndex = -1;
    int m_mostStableSmartIndex = -1;
    std::wstring m_verdictSmartFastest;
    std::wstring m_verdictSmartStable;

    std::optional<std::string> m_selectedProfileId;
    bool m_finished = false;
};

} // namespace quickdns
