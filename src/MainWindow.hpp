#pragma once

#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <memory>
#include "WebViewManager.hpp"

namespace UltraLight {

enum ControlID : WORD {
    IDC_BTN_BACK        = 1001,
    IDC_BTN_FORWARD     = 1002,
    IDC_BTN_RELOAD      = 1003,
    IDC_EDIT_ADDRESS    = 1004,
    IDC_BTN_DNS         = 1005,
    IDC_BTN_ZOOM        = 1006,
    IDC_BTN_BLOCKER     = 1007,
    IDC_BTN_SHARE       = 1008,
    IDC_BTN_SOUND       = 1009,
    IDC_BTN_NEWTAB      = 1010
};

enum AppCommandID : WORD {
    IDM_FOCUS_ADDRESS_BAR   = 2001,
    IDM_TOGGLE_FULLSCREEN   = 2002,
    IDM_EXIT_FULLSCREEN     = 2003,
    IDM_ZOOM_IN             = 2004,
    IDM_ZOOM_OUT            = 2005,
    IDM_ZOOM_RESET          = 2006,
    IDM_DNS_TOGGLE_ENABLE   = 2010,
    IDM_DNS_OPEN_SETTINGS   = 2011,
    IDM_BLOCKER_PICKER      = 2012,
    IDM_BLOCKER_TOGGLE_NATIVE = 2013,
    IDM_BLOCKER_CLEAR_RULES = 2014,
    IDM_SHARE_COPY_URL      = 2015,
    IDM_SHARE_OPEN_DEFAULT  = 2016,
    IDM_SURROUND_TOGGLE     = 2017,
    IDM_SURROUND_MODE_LIGHT = 2018,
    IDM_SURROUND_MODE_STANDARD = 2019,
    IDM_SURROUND_MODE_CINEMA = 2020,
    IDM_SURROUND_VOCAL_BOOST = 2021,
    IDM_SURROUND_BOOST_100  = 2022,
    IDM_SURROUND_BOOST_150  = 2023,
    IDM_SURROUND_BOOST_200  = 2024,
    IDM_SURROUND_BOOST_300  = 2025,
    IDM_SURROUND_MONO_DOWNMIX = 2026,
    IDM_SURROUND_DEV_AUTO   = 2027,
    IDM_SURROUND_DEV_HEADPHONES = 2028,
    IDM_SURROUND_DEV_SPEAKERS = 2029,
    IDM_ZOOM_SET_BASE       = 2030,
    IDM_DNS_SELECT_BASE     = 2100
};

class MainWindow {
public:
    MainWindow();
    ~MainWindow();

    bool Create(HINSTANCE hInstance, int nCmdShow);
    HWND GetHwnd() const { return m_hWnd; }
    void SetFullScreen(bool enable);
    void ToggleFullScreen();
    bool IsFullScreen() const { return m_isFullScreen; }

private:
    static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK AddressBarSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData);
    static LRESULT CALLBACK SafariButtonSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData);

    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    void CreateToolbarControls();
    void UpdateLayout(int width, int height);
    void UpdateDpiScaling(UINT dpi);
    void ApplyModernTheme();
    void ShowZoomMenu();
    void UpdateZoomDisplay(double zoom);
    void ShowDnsMenu();
    void UpdateDnsDisplay();
    void ShowSoundMenu();
    void UpdateSoundDisplay();
    void ShowBlockerMenu();
    void ShowShareMenu();

    HWND m_hWnd = nullptr;
    HINSTANCE m_hInstance = nullptr;
    UINT m_dpi = 96;

    // Controls
    HWND m_hBtnBack = nullptr;
    HWND m_hBtnForward = nullptr;
    HWND m_hEditAddress = nullptr;
    HWND m_hBtnReload = nullptr;
    HWND m_hBtnShare = nullptr;
    HWND m_hBtnSound = nullptr;
    HWND m_hBtnBlocker = nullptr;
    HWND m_hBtnDns = nullptr;
    HWND m_hBtnZoom = nullptr;
    HWND m_hBtnNewTab = nullptr;

    // macOS Traffic Lights
    RECT m_rcTrafficClose{};
    RECT m_rcTrafficMin{};
    RECT m_rcTrafficMax{};
    RECT m_rcTrafficGroup{};
    int m_trafficHoveredBtn = 0; // 0: none, 1: close, 2: min, 3: max
    int m_trafficPressedBtn = 0;
    bool m_isTrafficGroupHovered = false;
    bool m_isWindowActive = true;

    // Safari Styling Resources
    HFONT m_hUiFont = nullptr;
    HFONT m_hNavFont = nullptr;
    HFONT m_hAddressFont = nullptr;
    HBRUSH m_hBrTopBarBg = nullptr;
    HBRUSH m_hBrAddressBg = nullptr;
    HPEN m_hPenAddressBorder = nullptr;
    HPEN m_hPenAddressBorderFocus = nullptr;
    HPEN m_hPenSeparator = nullptr;

    RECT m_rcAddressCapsule{};
    bool m_isAddressFocused = false;
    bool m_isLoading = false;
    int m_topbarHeight = 52;

    bool m_isFullScreen = false;
    WINDOWPLACEMENT m_wpPrev{ sizeof(WINDOWPLACEMENT) };
    DWORD m_dwStylePrev = 0;

    ULONGLONG m_lastInteractionTick = 0;
    static constexpr UINT_PTR IDT_INACTIVITY_CHECK = 5001;

    std::unique_ptr<WebViewManager> m_webViewManager;
};

} // namespace UltraLight
