#pragma once

#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <memory>
#include "WebViewManager.hpp"

namespace UltraLight {

class MainWindow {
public:
    MainWindow();
    ~MainWindow();

    bool Create(HINSTANCE hInstance, int nCmdShow);
    HWND GetHwnd() const { return m_hWnd; }

private:
    static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK AddressBarSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData);

    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    void CreateToolbarControls();
    void UpdateLayout(int width, int height);
    void UpdateDpiScaling(UINT dpi);
    void ApplyModernTheme();
    void ShowExtensionsMenu();
    void HandleExtensionMenuCommand(WORD id);

    HWND m_hWnd = nullptr;
    HINSTANCE m_hInstance = nullptr;
    UINT m_dpi = 96;

    // Controls
    HWND m_hBtnBack = nullptr;
    HWND m_hBtnForward = nullptr;
    HWND m_hBtnReload = nullptr;
    HWND m_hEditAddress = nullptr;
    HWND m_hBtnBlocker = nullptr;
    HWND m_hBtnExtensions = nullptr;

    HFONT m_hUiFont = nullptr;
    int m_topbarHeight = 44;

    std::unique_ptr<WebViewManager> m_webViewManager;
};

} // namespace UltraLight
