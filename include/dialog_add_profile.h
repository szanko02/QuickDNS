#pragma once

#include <windows.h>
#include <string>
#include <optional>
#include "config_manager.h"

class AddProfileDialog {
public:
    static std::optional<DnsProfile> show(HWND hParent, HINSTANCE hInstance);

private:
    AddProfileDialog(HWND hParent, HINSTANCE hInstance);
    ~AddProfileDialog();

    bool create_dialog_window();
    void apply_theme();
    void create_controls();
    void on_save();
    void on_cancel();

    static LRESULT CALLBACK dialog_proc_thunk(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    LRESULT handle_message(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

    HWND m_hParent = nullptr;
    HINSTANCE m_hInstance = nullptr;
    HWND m_hWnd = nullptr;

    HFONT m_hFontRegular = nullptr;
    HFONT m_hFontBold = nullptr;
    HFONT m_hFontHeader = nullptr;

    HWND m_hEditName = nullptr;
    HWND m_hEditPri = nullptr;
    HWND m_hEditSec = nullptr;
    HWND m_hEditDesc = nullptr;
    HWND m_hEditDoh = nullptr;
    HWND m_hBtnSave = nullptr;
    HWND m_hBtnCancel = nullptr;

    std::optional<DnsProfile> m_result;
    bool m_finished = false;
};
