#include "MainWindow.hpp"
#include "Config.hpp"
#include "DnsManager.hpp"
#include "ElementBlocker.hpp"
#include "NativeRequestFilter.hpp"
#include "PowerManager.hpp"
#include "StringUtils.hpp"
#include <windowsx.h>
#include <uxtheme.h>
#include <shellapi.h>
#include <cmath>

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif

#ifndef DWMWA_SYSTEMBACKDROP_TYPE
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#endif

namespace UltraLight {

static const int kPresetZoomPercentages[] = {
    500, 400, 300, 250, 200, 175, 150, 125, 110, 100, 90, 80, 75, 67, 50, 33, 25
};

static void CopyTextToClipboard(HWND hWndOwner, const std::wstring& text) {
    if (!OpenClipboard(hWndOwner)) return;
    EmptyClipboard();
    size_t bytes = (text.length() + 1) * sizeof(wchar_t);
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (hMem) {
        void* pMem = GlobalLock(hMem);
        if (pMem) {
            memcpy(pMem, text.c_str(), bytes);
            GlobalUnlock(hMem);
            SetClipboardData(CF_UNICODETEXT, hMem);
        } else {
            GlobalFree(hMem);
        }
    }
    CloseClipboard();
}

MainWindow::MainWindow() : m_webViewManager(std::make_unique<WebViewManager>()) {
    m_hBrTopBarBg = CreateSolidBrush(RGB(30, 30, 32));
    m_hBrAddressBg = CreateSolidBrush(RGB(44, 44, 48));
    m_hPenAddressBorder = CreatePen(PS_SOLID, 1, RGB(65, 65, 72));
    m_hPenAddressBorderFocus = CreatePen(PS_SOLID, 1, RGB(10, 132, 255));
    m_hPenSeparator = CreatePen(PS_SOLID, 1, RGB(48, 48, 52));
}

MainWindow::~MainWindow() {
    if (m_webViewManager) {
        m_webViewManager->ShutdownAndPurgeData();
    }
    if (m_hUiFont) DeleteObject(m_hUiFont);
    if (m_hNavFont) DeleteObject(m_hNavFont);
    if (m_hAddressFont) DeleteObject(m_hAddressFont);
    if (m_hBrTopBarBg) DeleteObject(m_hBrTopBarBg);
    if (m_hBrAddressBg) DeleteObject(m_hBrAddressBg);
    if (m_hPenAddressBorder) DeleteObject(m_hPenAddressBorder);
    if (m_hPenAddressBorderFocus) DeleteObject(m_hPenAddressBorderFocus);
    if (m_hPenSeparator) DeleteObject(m_hPenSeparator);
}

bool MainWindow::Create(HINSTANCE hInstance, int nCmdShow) {
    m_hInstance = hInstance;

    const wchar_t CLASS_NAME[] = L"UltraLightBrowserMainWindow";

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = MainWindow::WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = m_hBrTopBarBg;
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(101));
    wc.hIconSm = reinterpret_cast<HICON>(LoadImageW(hInstance, MAKEINTRESOURCEW(101), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));

    if (!RegisterClassExW(&wc)) {
        return false;
    }

    m_hWnd = CreateWindowExW(
        WS_EX_APPWINDOW,
        CLASS_NAME,
        L"Safari",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 1280, 800,
        nullptr,
        nullptr,
        hInstance,
        this
    );

    if (!m_hWnd) {
        return false;
    }

    if (wc.hIcon) {
        SendMessageW(m_hWnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(wc.hIcon));
    }
    if (wc.hIconSm) {
        SendMessageW(m_hWnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(wc.hIconSm));
    }

    ApplyModernTheme();
    DragAcceptFiles(m_hWnd, TRUE);
    ShowWindow(m_hWnd, nCmdShow);
    UpdateWindow(m_hWnd);

    return true;
}

void MainWindow::ApplyModernTheme() {
    // Windows 11 Immersive Dark Mode
    BOOL darkMode = TRUE;
    DwmSetWindowAttribute(m_hWnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));

    // Windows 11 Rounded Corners Preference
    DWORD cornerPref = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(m_hWnd, DWMWA_WINDOW_CORNER_PREFERENCE, &cornerPref, sizeof(cornerPref));

    // Optional: Acrylic or Mica backdrop on Win11 22H2+
    DWORD backdropType = 2; // 2 = Mica
    DwmSetWindowAttribute(m_hWnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdropType, sizeof(backdropType));
}

void MainWindow::UpdateDpiScaling(UINT dpi) {
    m_dpi = dpi;
    m_topbarHeight = MulDiv(46, dpi, 96);

    if (m_hUiFont) DeleteObject(m_hUiFont);
    if (m_hNavFont) DeleteObject(m_hNavFont);
    if (m_hAddressFont) DeleteObject(m_hAddressFont);

    int fontHeight = -MulDiv(10, dpi, 72);
    m_hUiFont = CreateFontW(
        fontHeight, 0, 0, 0, FW_MEDIUM, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI Variable Text"
    );

    int navFontHeight = -MulDiv(14, dpi, 72);
    m_hNavFont = CreateFontW(
        navFontHeight, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI"
    );

    int addressFontHeight = -MulDiv(10, dpi, 72);
    m_hAddressFont = CreateFontW(
        addressFontHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI Variable Text"
    );

    // Apply fonts to controls
    if (m_hBtnBack) SendMessageW(m_hBtnBack, WM_SETFONT, reinterpret_cast<WPARAM>(m_hNavFont), TRUE);
    if (m_hBtnForward) SendMessageW(m_hBtnForward, WM_SETFONT, reinterpret_cast<WPARAM>(m_hNavFont), TRUE);
    if (m_hBtnReload) SendMessageW(m_hBtnReload, WM_SETFONT, reinterpret_cast<WPARAM>(m_hUiFont), TRUE);
    if (m_hEditAddress) SendMessageW(m_hEditAddress, WM_SETFONT, reinterpret_cast<WPARAM>(m_hAddressFont), TRUE);
    if (m_hBtnShare) SendMessageW(m_hBtnShare, WM_SETFONT, reinterpret_cast<WPARAM>(m_hUiFont), TRUE);
    if (m_hBtnDns) SendMessageW(m_hBtnDns, WM_SETFONT, reinterpret_cast<WPARAM>(m_hUiFont), TRUE);
    if (m_hBtnZoom) SendMessageW(m_hBtnZoom, WM_SETFONT, reinterpret_cast<WPARAM>(m_hUiFont), TRUE);
    if (m_hBtnBlocker) SendMessageW(m_hBtnBlocker, WM_SETFONT, reinterpret_cast<WPARAM>(m_hUiFont), TRUE);
}

LRESULT CALLBACK MainWindow::SafariButtonSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR /*uIdSubclass*/, DWORD_PTR dwRefData) {
    auto* self = reinterpret_cast<MainWindow*>(dwRefData);
    static bool s_isHovered = false;
    static bool s_isPressed = false;

    // Use window props to store per-button state
    bool isHovered = GetPropW(hWnd, L"SafariBtnHover") != nullptr;
    bool isPressed = GetPropW(hWnd, L"SafariBtnPressed") != nullptr;

    switch (uMsg) {
    case WM_MOUSEMOVE: {
        if (!isHovered) {
            SetPropW(hWnd, L"SafariBtnHover", reinterpret_cast<HANDLE>(1));
            TRACKMOUSEEVENT tme{ sizeof(TRACKMOUSEEVENT), TME_LEAVE, hWnd, 0 };
            TrackMouseEvent(&tme);
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        break;
    }
    case WM_MOUSELEAVE: {
        RemovePropW(hWnd, L"SafariBtnHover");
        RemovePropW(hWnd, L"SafariBtnPressed");
        InvalidateRect(hWnd, nullptr, FALSE);
        break;
    }
    case WM_LBUTTONDOWN: {
        SetPropW(hWnd, L"SafariBtnPressed", reinterpret_cast<HANDLE>(1));
        InvalidateRect(hWnd, nullptr, FALSE);
        break;
    }
    case WM_LBUTTONUP: {
        RemovePropW(hWnd, L"SafariBtnPressed");
        InvalidateRect(hWnd, nullptr, FALSE);
        break;
    }
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc;
        GetClientRect(hWnd, &rc);

        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP memBmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
        HGDIOBJ oldBmp = SelectObject(memDC, memBmp);

        // Fill background with toolbar dark tone
        HBRUSH hBrBar = self ? self->m_hBrTopBarBg : nullptr;
        if (!hBrBar) hBrBar = GetSysColorBrush(COLOR_BTNFACE);
        FillRect(memDC, &rc, hBrBar);

        bool enabled = IsWindowEnabled(hWnd) != FALSE;
        int radius = self ? MulDiv(6, self->m_dpi, 96) : 6;

        // Render rounded pill background for hover/pressed states
        if (enabled && isPressed) {
            HBRUSH hBrPress = CreateSolidBrush(RGB(68, 68, 74));
            HPEN hPenPress = CreatePen(PS_SOLID, 1, RGB(80, 80, 88));
            HGDIOBJ oldBrush = SelectObject(memDC, hBrPress);
            HGDIOBJ oldPen = SelectObject(memDC, hPenPress);
            RoundRect(memDC, rc.left, rc.top, rc.right, rc.bottom, radius * 2, radius * 2);
            SelectObject(memDC, oldBrush);
            SelectObject(memDC, oldPen);
            DeleteObject(hBrPress);
            DeleteObject(hPenPress);
        } else if (enabled && isHovered) {
            HBRUSH hBrHover = CreateSolidBrush(RGB(50, 50, 56));
            HPEN hPenHover = CreatePen(PS_SOLID, 1, RGB(65, 65, 72));
            HGDIOBJ oldBrush = SelectObject(memDC, hBrHover);
            HGDIOBJ oldPen = SelectObject(memDC, hPenHover);
            RoundRect(memDC, rc.left, rc.top, rc.right, rc.bottom, radius * 2, radius * 2);
            SelectObject(memDC, oldBrush);
            SelectObject(memDC, oldPen);
            DeleteObject(hBrHover);
            DeleteObject(hPenHover);
        }

        // Draw button label / glyph
        wchar_t text[128]{};
        GetWindowTextW(hWnd, text, static_cast<int>(std::size(text)));

        COLORREF textColor = enabled
            ? (isHovered ? RGB(255, 255, 255) : RGB(225, 225, 230))
            : RGB(105, 105, 110);

        SetBkMode(memDC, TRANSPARENT);
        SetTextColor(memDC, textColor);

        HFONT hFont = reinterpret_cast<HFONT>(SendMessageW(hWnd, WM_GETFONT, 0, 0));
        HGDIOBJ oldFont = hFont ? SelectObject(memDC, hFont) : nullptr;

        DrawTextW(memDC, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        if (oldFont) SelectObject(memDC, oldFont);

        BitBlt(hdc, 0, 0, rc.right, rc.bottom, memDC, 0, 0, SRCCOPY);

        SelectObject(memDC, oldBmp);
        DeleteObject(memBmp);
        DeleteDC(memDC);

        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_NCDESTROY:
        RemovePropW(hWnd, L"SafariBtnHover");
        RemovePropW(hWnd, L"SafariBtnPressed");
        break;
    default:
        break;
    }

    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

void MainWindow::CreateToolbarControls() {
    m_hBtnBack = CreateWindowExW(
        0, L"BUTTON", L"‹",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 0, 0, m_hWnd, reinterpret_cast<HMENU>(IDC_BTN_BACK), m_hInstance, nullptr
    );
    SetWindowSubclass(m_hBtnBack, SafariButtonSubclassProc, 1, reinterpret_cast<DWORD_PTR>(this));

    m_hBtnForward = CreateWindowExW(
        0, L"BUTTON", L"›",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 0, 0, m_hWnd, reinterpret_cast<HMENU>(IDC_BTN_FORWARD), m_hInstance, nullptr
    );
    SetWindowSubclass(m_hBtnForward, SafariButtonSubclassProc, 1, reinterpret_cast<DWORD_PTR>(this));

    m_hBtnReload = CreateWindowExW(
        0, L"BUTTON", L"↻",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 0, 0, m_hWnd, reinterpret_cast<HMENU>(IDC_BTN_RELOAD), m_hInstance, nullptr
    );
    SetWindowSubclass(m_hBtnReload, SafariButtonSubclassProc, 1, reinterpret_cast<DWORD_PTR>(this));

    // Address Bar - Sleek borderless edit embedded in Safari pill
    m_hEditAddress = CreateWindowExW(
        0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_LEFT,
        0, 0, 0, 0, m_hWnd, reinterpret_cast<HMENU>(IDC_EDIT_ADDRESS), m_hInstance, nullptr
    );
    SendMessageW(m_hEditAddress, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(8, 8));
    SendMessageW(m_hEditAddress, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"搜索或输入网站名称"));
    SetWindowSubclass(m_hEditAddress, AddressBarSubclassProc, 1, reinterpret_cast<DWORD_PTR>(this));

    // Right-aligned Safari Toolbar Buttons
    m_hBtnShare = CreateWindowExW(
        0, L"BUTTON", L"↥",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 0, 0, m_hWnd, reinterpret_cast<HMENU>(IDC_BTN_SHARE), m_hInstance, nullptr
    );
    SetWindowSubclass(m_hBtnShare, SafariButtonSubclassProc, 1, reinterpret_cast<DWORD_PTR>(this));

    m_hBtnDns = CreateWindowExW(
        0, L"BUTTON", L"🌐 DNS",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 0, 0, m_hWnd, reinterpret_cast<HMENU>(IDC_BTN_DNS), m_hInstance, nullptr
    );
    SetWindowSubclass(m_hBtnDns, SafariButtonSubclassProc, 1, reinterpret_cast<DWORD_PTR>(this));

    m_hBtnZoom = CreateWindowExW(
        0, L"BUTTON", L"🔍 100%",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 0, 0, m_hWnd, reinterpret_cast<HMENU>(IDC_BTN_ZOOM), m_hInstance, nullptr
    );
    SetWindowSubclass(m_hBtnZoom, SafariButtonSubclassProc, 1, reinterpret_cast<DWORD_PTR>(this));

    m_hBtnBlocker = CreateWindowExW(
        0, L"BUTTON", L"🛡️ 隐私保护",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 0, 0, m_hWnd, reinterpret_cast<HMENU>(IDC_BTN_BLOCKER), m_hInstance, nullptr
    );
    SetWindowSubclass(m_hBtnBlocker, SafariButtonSubclassProc, 1, reinterpret_cast<DWORD_PTR>(this));

    UpdateDpiScaling(GetDpiForWindow(m_hWnd));
    UpdateDnsDisplay();
}

void MainWindow::UpdateLayout(int width, int height) {
    if (width <= 0 || height <= 0) return;

    if (m_isFullScreen || m_isImmersiveMode) {
        RECT fsRect{ 0, 0, width, height };
        if (m_webViewManager) {
            m_webViewManager->Resize(fsRect);
        }
        return;
    }

    int pad = MulDiv(8, m_dpi, 96);
    int navBtnW = MulDiv(32, m_dpi, 96);
    int shareBtnW = MulDiv(34, m_dpi, 96);
    int zoomBtnW = MulDiv(70, m_dpi, 96);
    int dnsBtnW = MulDiv(86, m_dpi, 96);
    int blockBtnW = MulDiv(88, m_dpi, 96);

    int topH = m_topbarHeight;
    int ctrlH = MulDiv(30, m_dpi, 96);
    int btnY = (topH - ctrlH) / 2;

    int x = pad;

    // Left Navigation: [ ‹ ] [ › ] [ ↻ ]
    SetWindowPos(m_hBtnBack, nullptr, x, btnY, navBtnW, ctrlH, SWP_NOZORDER);
    x += navBtnW + MulDiv(4, m_dpi, 96);

    SetWindowPos(m_hBtnForward, nullptr, x, btnY, navBtnW, ctrlH, SWP_NOZORDER);
    x += navBtnW + MulDiv(4, m_dpi, 96);

    SetWindowPos(m_hBtnReload, nullptr, x, btnY, navBtnW, ctrlH, SWP_NOZORDER);
    x += navBtnW + pad;

    int leftGroupEnd = x;

    // Right Action Buttons: [ ↥ ] [ 🌐 DNS ] [ 🔍 100% ] [ 🛡️ 隐私保护 ]
    int rightX = width - pad - blockBtnW;
    SetWindowPos(m_hBtnBlocker, nullptr, rightX, btnY, blockBtnW, ctrlH, SWP_NOZORDER);

    rightX -= (zoomBtnW + MulDiv(4, m_dpi, 96));
    SetWindowPos(m_hBtnZoom, nullptr, rightX, btnY, zoomBtnW, ctrlH, SWP_NOZORDER);

    rightX -= (dnsBtnW + MulDiv(4, m_dpi, 96));
    SetWindowPos(m_hBtnDns, nullptr, rightX, btnY, dnsBtnW, ctrlH, SWP_NOZORDER);

    rightX -= (shareBtnW + MulDiv(6, m_dpi, 96));
    SetWindowPos(m_hBtnShare, nullptr, rightX, btnY, shareBtnW, ctrlH, SWP_NOZORDER);

    int rightGroupStart = rightX;

    // Safari Centered Smart Search Capsule Layout
    int availableW = (rightGroupStart - pad) - (leftGroupEnd + pad);
    int maxCapsuleW = MulDiv(680, m_dpi, 96);
    int capsuleW = availableW;
    int capsuleX = leftGroupEnd + pad;

    if (availableW > maxCapsuleW) {
        int idealCenteredX = (width - maxCapsuleW) / 2;
        if (idealCenteredX >= leftGroupEnd + pad && (idealCenteredX + maxCapsuleW) <= rightGroupStart - pad) {
            capsuleX = idealCenteredX;
            capsuleW = maxCapsuleW;
        } else {
            capsuleW = availableW;
        }
    }

    if (capsuleW > MulDiv(100, m_dpi, 96)) {
        m_rcAddressCapsule = { capsuleX, btnY, capsuleX + capsuleW, btnY + ctrlH };

        // Position edit control inside capsule, leaving room on the left for lock icon
        int iconOffset = MulDiv(26, m_dpi, 96);
        int editX = capsuleX + iconOffset;
        int editW = capsuleW - iconOffset - MulDiv(8, m_dpi, 96);
        int editH = MulDiv(20, m_dpi, 96);
        int editY = btnY + (ctrlH - editH) / 2;

        SetWindowPos(m_hEditAddress, nullptr, editX, editY, editW, editH, SWP_NOZORDER);
    }

    // Refresh toolbar region
    RECT rcTop{ 0, 0, width, topH };
    InvalidateRect(m_hWnd, &rcTop, FALSE);

    // Resize WebView2
    RECT webViewRect{ 0, topH, width, height };
    if (m_webViewManager) {
        m_webViewManager->Resize(webViewRect);
    }
}

void MainWindow::SetFullScreen(bool enable) {
    if (m_isFullScreen == enable) return;
    m_isFullScreen = enable;

    if (m_isFullScreen) {
        m_wpPrev.length = sizeof(WINDOWPLACEMENT);
        GetWindowPlacement(m_hWnd, &m_wpPrev);
        m_dwStylePrev = static_cast<DWORD>(GetWindowLongW(m_hWnd, GWL_STYLE));

        // Hide toolbar controls
        if (m_hBtnBack) ShowWindow(m_hBtnBack, SW_HIDE);
        if (m_hBtnForward) ShowWindow(m_hBtnForward, SW_HIDE);
        if (m_hBtnReload) ShowWindow(m_hBtnReload, SW_HIDE);
        if (m_hEditAddress) ShowWindow(m_hEditAddress, SW_HIDE);
        if (m_hBtnShare) ShowWindow(m_hBtnShare, SW_HIDE);
        if (m_hBtnDns) ShowWindow(m_hBtnDns, SW_HIDE);
        if (m_hBtnZoom) ShowWindow(m_hBtnZoom, SW_HIDE);
        if (m_hBtnBlocker) ShowWindow(m_hBtnBlocker, SW_HIDE);

        HMONITOR hMon = MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{ sizeof(MONITORINFO) };
        GetMonitorInfoW(hMon, &mi);

        SetWindowLongW(m_hWnd, GWL_STYLE, m_dwStylePrev & ~(WS_CAPTION | WS_THICKFRAME));
        int monW = mi.rcMonitor.right - mi.rcMonitor.left;
        int monH = mi.rcMonitor.bottom - mi.rcMonitor.top;
        SetWindowPos(
            m_hWnd, HWND_TOP,
            mi.rcMonitor.left, mi.rcMonitor.top,
            monW, monH,
            SWP_NOOWNERZORDER | SWP_FRAMECHANGED
        );

        if (m_webViewManager) {
            RECT fsRect{ 0, 0, monW, monH };
            m_webViewManager->Resize(fsRect);
        }
    } else {
        SetWindowLongW(m_hWnd, GWL_STYLE, m_dwStylePrev);
        SetWindowPlacement(m_hWnd, &m_wpPrev);
        SetWindowPos(
            m_hWnd, nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED
        );

        int showCmd = m_isImmersiveMode ? SW_HIDE : SW_SHOW;
        if (m_hBtnBack) ShowWindow(m_hBtnBack, showCmd);
        if (m_hBtnForward) ShowWindow(m_hBtnForward, showCmd);
        if (m_hBtnReload) ShowWindow(m_hBtnReload, showCmd);
        if (m_hEditAddress) ShowWindow(m_hEditAddress, showCmd);
        if (m_hBtnShare) ShowWindow(m_hBtnShare, showCmd);
        if (m_hBtnDns) ShowWindow(m_hBtnDns, showCmd);
        if (m_hBtnZoom) ShowWindow(m_hBtnZoom, showCmd);
        if (m_hBtnBlocker) ShowWindow(m_hBtnBlocker, showCmd);

        RECT client;
        GetClientRect(m_hWnd, &client);
        UpdateLayout(client.right, client.bottom);
    }
}

void MainWindow::ToggleFullScreen() {
    SetFullScreen(!m_isFullScreen);
}

void MainWindow::SetImmersiveMode(bool enable) {
    if (m_isImmersiveMode == enable) return;
    m_isImmersiveMode = enable;

    int showCmd = (enable || m_isFullScreen) ? SW_HIDE : SW_SHOW;
    if (m_hBtnBack) ShowWindow(m_hBtnBack, showCmd);
    if (m_hBtnForward) ShowWindow(m_hBtnForward, showCmd);
    if (m_hBtnReload) ShowWindow(m_hBtnReload, showCmd);
    if (m_hEditAddress) ShowWindow(m_hEditAddress, showCmd);
    if (m_hBtnShare) ShowWindow(m_hBtnShare, showCmd);
    if (m_hBtnDns) ShowWindow(m_hBtnDns, showCmd);
    if (m_hBtnZoom) ShowWindow(m_hBtnZoom, showCmd);
    if (m_hBtnBlocker) ShowWindow(m_hBtnBlocker, showCmd);

    RECT client;
    GetClientRect(m_hWnd, &client);
    UpdateLayout(client.right, client.bottom);
}

void MainWindow::ToggleImmersiveMode() {
    SetImmersiveMode(!m_isImmersiveMode);
}

LRESULT CALLBACK MainWindow::AddressBarSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR /*uIdSubclass*/, DWORD_PTR dwRefData) {
    auto* self = reinterpret_cast<MainWindow*>(dwRefData);
    static bool s_needSelectAllOnMouseUp = false;

    switch (uMsg) {
    case WM_SETFOCUS:
        s_needSelectAllOnMouseUp = true;
        if (self) {
            self->m_isAddressFocused = true;
            InvalidateRect(self->m_hWnd, &self->m_rcAddressCapsule, FALSE);
        }
        break;

    case WM_KILLFOCUS:
        s_needSelectAllOnMouseUp = false;
        if (self) {
            self->m_isAddressFocused = false;
            InvalidateRect(self->m_hWnd, &self->m_rcAddressCapsule, FALSE);
        }
        break;

    case WM_LBUTTONUP: {
        LRESULT res = DefSubclassProc(hWnd, uMsg, wParam, lParam);
        if (s_needSelectAllOnMouseUp) {
            s_needSelectAllOnMouseUp = false;
            SendMessageW(hWnd, EM_SETSEL, 0, -1);
        }
        return res;
    }

    case WM_KEYDOWN:
        if (wParam == VK_RETURN) {
            wchar_t buffer[2048]{};
            GetWindowTextW(hWnd, buffer, static_cast<int>(std::size(buffer)));
            std::wstring input = buffer;
            while (!input.empty() && iswspace(input.front())) input.erase(input.begin());
            while (!input.empty() && iswspace(input.back())) input.pop_back();

            if (input.empty()) return 0;

            if (self && self->m_webViewManager) {
                self->m_webViewManager->Navigate(input);
                if (self->m_webViewManager->GetController()) {
                    self->m_webViewManager->GetController()->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
                }
            }
            return 0;
        } else if (wParam == 'A' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            SendMessageW(hWnd, EM_SETSEL, 0, -1);
            return 0;
        } else if (wParam == VK_ESCAPE) {
            if (self && self->m_webViewManager && self->m_webViewManager->GetWebView()) {
                wil::unique_cotaskmem_string uri;
                if (SUCCEEDED(self->m_webViewManager->GetWebView()->get_Source(&uri)) && uri.get()) {
                    SetWindowTextW(hWnd, uri.get());
                }
                if (self->m_webViewManager->GetController()) {
                    self->m_webViewManager->GetController()->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
                }
            }
            return 0;
        }
        break;

    case WM_CHAR:
        if (wParam == VK_RETURN) {
            return 0; // Suppress beep
        }
        break;

    default:
        break;
    }

    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

LRESULT MainWindow::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        CreateToolbarControls();

        // Bind callbacks to WebView
        m_webViewManager->SetTitleChangedCallback([this](const std::wstring& title) {
            if (title.empty()) {
                SetWindowTextW(m_hWnd, L"Safari");
            } else {
                SetWindowTextW(m_hWnd, (title + L" — Safari").c_str());
            }
        });

        m_webViewManager->SetSourceChangedCallback([this](const std::wstring& uri) {
            if (GetFocus() != m_hEditAddress) {
                SetWindowTextW(m_hEditAddress, uri.c_str());
            }
        });

        m_webViewManager->SetFullScreenCallback([this](bool fs) {
            SetFullScreen(fs);
        });

        m_webViewManager->SetZoomFactorChangedCallback([this](double zoom) {
            UpdateZoomDisplay(zoom);
        });

        m_webViewManager->SetUserActivityCallback([this]() {
            m_lastInteractionTick = GetTickCount64();
            if (m_webViewManager && m_webViewManager->GetWebView() &&
                (PowerManager::Instance().IsSuspended() || PowerManager::Instance().IsAudioPlaybackBackgrounded())) {
                PowerManager::Instance().HandleActivityResume(
                    m_webViewManager->GetController(),
                    m_webViewManager->GetWebView()
                );
            }
        });

        m_lastInteractionTick = GetTickCount64();
        SetTimer(m_hWnd, IDT_INACTIVITY_CHECK, 15000, nullptr);

        // Initialize WebView2
        m_webViewManager->Initialize(m_hWnd, [this]() {
            RECT client;
            GetClientRect(m_hWnd, &client);
            UpdateLayout(client.right, client.bottom);
            UpdateZoomDisplay(m_webViewManager->GetZoomFactor());
            m_webViewManager->Navigate(Config::Instance().GetSettings().startUrl);
        });

        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(m_hWnd, &ps);

        RECT client;
        GetClientRect(m_hWnd, &client);
        int w = client.right;
        int topH = m_topbarHeight;

        if (!m_isFullScreen && !m_isImmersiveMode && w > 0 && topH > 0) {
            HDC memDC = CreateCompatibleDC(hdc);
            HBITMAP memBmp = CreateCompatibleBitmap(hdc, w, topH);
            HGDIOBJ oldBmp = SelectObject(memDC, memBmp);

            // Fill toolbar background
            RECT rcTop{ 0, 0, w, topH };
            FillRect(memDC, &rcTop, m_hBrTopBarBg);

            // Draw subtle bottom separator line
            HGDIOBJ oldPen = SelectObject(memDC, m_hPenSeparator);
            MoveToEx(memDC, 0, topH - 1, nullptr);
            LineTo(memDC, w, topH - 1);

            // Draw Safari Centered Address Bar Capsule
            if (m_rcAddressCapsule.right > m_rcAddressCapsule.left) {
                HPEN activePen = m_isAddressFocused ? m_hPenAddressBorderFocus : m_hPenAddressBorder;
                SelectObject(memDC, activePen);
                HGDIOBJ oldBrush = SelectObject(memDC, m_hBrAddressBg);

                int radius = MulDiv(8, m_dpi, 96);
                RoundRect(memDC, m_rcAddressCapsule.left, m_rcAddressCapsule.top, m_rcAddressCapsule.right, m_rcAddressCapsule.bottom, radius * 2, radius * 2);
                SelectObject(memDC, oldBrush);

                // Draw Safari Privacy / SSL Lock Icon
                RECT rcLock{
                    m_rcAddressCapsule.left + MulDiv(7, m_dpi, 96),
                    m_rcAddressCapsule.top,
                    m_rcAddressCapsule.left + MulDiv(24, m_dpi, 96),
                    m_rcAddressCapsule.bottom
                };
                SetBkMode(memDC, TRANSPARENT);
                SetTextColor(memDC, m_isAddressFocused ? RGB(10, 132, 255) : RGB(140, 140, 145));
                HGDIOBJ oldFont = SelectObject(memDC, m_hUiFont);
                DrawTextW(memDC, L"🔒", -1, &rcLock, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                SelectObject(memDC, oldFont);
            }

            SelectObject(memDC, oldPen);

            BitBlt(hdc, 0, 0, w, topH, memDC, 0, 0, SRCCOPY);
            SelectObject(memDC, oldBmp);
            DeleteObject(memBmp);
            DeleteDC(memDC);
        }

        EndPaint(m_hWnd, &ps);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC: {
        HWND hTarget = reinterpret_cast<HWND>(lParam);
        if (hTarget == m_hEditAddress) {
            HDC hdcEdit = reinterpret_cast<HDC>(wParam);
            SetTextColor(hdcEdit, RGB(245, 245, 247));
            SetBkColor(hdcEdit, RGB(44, 44, 48));
            return reinterpret_cast<LRESULT>(m_hBrAddressBg);
        }
        break;
    }

    case WM_TIMER: {
        if (wParam == IDT_INACTIVITY_CHECK) {
            ULONGLONG now = GetTickCount64();
            HWND hFore = GetForegroundWindow();
            bool isUnfocused = (hFore != m_hWnd) || IsIconic(m_hWnd);
            if (isUnfocused && (now - m_lastInteractionTick >= 300000)) {
                if (m_webViewManager && m_webViewManager->GetWebView()) {
                    bool isPlayingAudio = m_webViewManager->IsDocumentPlayingAudio();
                    PowerManager::Instance().HandleInactivitySuspend(
                        m_webViewManager->GetController(),
                        m_webViewManager->GetWebView(),
                        isPlayingAudio
                    );
                }
            }
            return 0;
        }
        break;
    }

    case WM_ACTIVATE: {
        if (LOWORD(wParam) != WA_INACTIVE) {
            m_lastInteractionTick = GetTickCount64();
            if (m_webViewManager && m_webViewManager->GetWebView() &&
                (PowerManager::Instance().IsSuspended() || PowerManager::Instance().IsAudioPlaybackBackgrounded())) {
                PowerManager::Instance().HandleActivityResume(
                    m_webViewManager->GetController(),
                    m_webViewManager->GetWebView()
                );
            }
        }
        break;
    }

    case WM_SETFOCUS: {
        m_lastInteractionTick = GetTickCount64();
        if (m_webViewManager && m_webViewManager->GetWebView() &&
            (PowerManager::Instance().IsSuspended() || PowerManager::Instance().IsAudioPlaybackBackgrounded())) {
            PowerManager::Instance().HandleActivityResume(
                m_webViewManager->GetController(),
                m_webViewManager->GetWebView()
            );
        }
        break;
    }

    case WM_SIZE: {
        if (wParam == SIZE_MINIMIZED) {
            if (m_webViewManager && m_webViewManager->GetWebView()) {
                bool isPlayingAudio = m_webViewManager->IsDocumentPlayingAudio();
                PowerManager::Instance().HandleWindowMinimize(
                    m_webViewManager->GetController(),
                    m_webViewManager->GetWebView(),
                    isPlayingAudio
                );
            }
            return 0;
        } else if (wParam == SIZE_RESTORED || wParam == SIZE_MAXIMIZED) {
            if (m_webViewManager && m_webViewManager->GetWebView() &&
                (PowerManager::Instance().IsSuspended() || PowerManager::Instance().IsAudioPlaybackBackgrounded())) {
                PowerManager::Instance().HandleWindowRestore(
                    m_webViewManager->GetController(),
                    m_webViewManager->GetWebView()
                );
            }
        }
        int w = LOWORD(lParam);
        int h = HIWORD(lParam);
        UpdateLayout(w, h);
        return 0;
    }

    case WM_MOVE: {
        if (m_webViewManager) {
            m_webViewManager->NotifyParentWindowPositionChanged();
        }
        break;
    }

    case WM_KEYDOWN: {
        m_lastInteractionTick = GetTickCount64();
        if (wParam == VK_F9) {
            ToggleImmersiveMode();
            return 0;
        }
        if (wParam == VK_F11) {
            ToggleFullScreen();
            return 0;
        }
        if (wParam == VK_ESCAPE) {
            if (m_isFullScreen) {
                SetFullScreen(false);
                return 0;
            }
            if (m_isImmersiveMode) {
                SetImmersiveMode(false);
                return 0;
            }
        }
        break;
    }

    case WM_DROPFILES: {
        HDROP hDrop = reinterpret_cast<HDROP>(wParam);
        UINT fileCount = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
        if (fileCount > 0) {
            wchar_t filePath[MAX_PATH]{};
            if (DragQueryFileW(hDrop, 0, filePath, MAX_PATH)) {
                if (m_webViewManager) {
                    m_webViewManager->Navigate(filePath);
                }
            }
        }
        DragFinish(hDrop);
        return 0;
    }

    case WM_COMMAND: {
        WORD id = LOWORD(wParam);
        switch (id) {
        case IDC_BTN_BACK:
            m_webViewManager->GoBack();
            break;
        case IDC_BTN_FORWARD:
            m_webViewManager->GoForward();
            break;
        case IDC_BTN_RELOAD:
            m_webViewManager->Reload();
            break;
        case IDM_FOCUS_ADDRESS_BAR:
            SetFocus(m_hEditAddress);
            SendMessageW(m_hEditAddress, EM_SETSEL, 0, -1);
            break;
        case IDM_TOGGLE_FULLSCREEN:
            ToggleFullScreen();
            break;
        case IDM_EXIT_FULLSCREEN:
            if (m_isFullScreen) SetFullScreen(false);
            if (m_isImmersiveMode) SetImmersiveMode(false);
            break;
        case IDM_TOGGLE_IMMERSIVE:
            ToggleImmersiveMode();
            break;
        case IDM_ZOOM_IN:
            if (m_webViewManager) m_webViewManager->ZoomIn();
            break;
        case IDM_ZOOM_OUT:
            if (m_webViewManager) m_webViewManager->ZoomOut();
            break;
        case IDM_ZOOM_RESET:
            if (m_webViewManager) m_webViewManager->ZoomReset();
            break;
        case IDC_BTN_ZOOM:
            ShowZoomMenu();
            break;
        case IDC_BTN_SHARE:
            ShowShareMenu();
            break;
        case IDM_SHARE_COPY_URL: {
            if (m_webViewManager && m_webViewManager->GetWebView()) {
                wil::unique_cotaskmem_string uri;
                if (SUCCEEDED(m_webViewManager->GetWebView()->get_Source(&uri)) && uri.get()) {
                    CopyTextToClipboard(m_hWnd, uri.get());
                    MessageBoxW(m_hWnd, (L"已拷贝当前网页链接到剪贴板:\n" + std::wstring(uri.get())).c_str(), L"Safari 分享", MB_OK | MB_ICONINFORMATION);
                }
            }
            break;
        }
        case IDM_SHARE_OPEN_DEFAULT: {
            if (m_webViewManager && m_webViewManager->GetWebView()) {
                wil::unique_cotaskmem_string uri;
                if (SUCCEEDED(m_webViewManager->GetWebView()->get_Source(&uri)) && uri.get()) {
                    ShellExecuteW(m_hWnd, L"open", uri.get(), nullptr, nullptr, SW_SHOWNORMAL);
                }
            }
            break;
        }
        case IDC_BTN_DNS:
            ShowDnsMenu();
            break;
        case IDM_DNS_TOGGLE_ENABLE: {
            auto& settings = Config::Instance().GetSettings();
            settings.enablePublicDns = !settings.enablePublicDns;
            Config::Instance().Save();
            DnsManager::Instance().ApplySettings();
            UpdateDnsDisplay();
            std::wstring infoMsg = settings.enablePublicDns
                ? L"已开启公共安全 DNS (DoH 加密解析)！\n建议刷新网页以使新设置生效。"
                : L"已关闭公共 DNS，恢复系统默认解析。";
            MessageBoxW(m_hWnd, infoMsg.c_str(), L"安全 DNS 设置", MB_OK | MB_ICONINFORMATION);
            break;
        }
        case IDM_DNS_OPEN_SETTINGS:
            DnsManager::Instance().ShowDnsDialog(m_hWnd);
            UpdateDnsDisplay();
            break;
        case IDC_EDIT_ADDRESS: {
            WORD notify = HIWORD(wParam);
            if (notify == EN_KILLFOCUS) {
                if (m_webViewManager && m_webViewManager->GetWebView()) {
                    wil::unique_cotaskmem_string uri;
                    if (SUCCEEDED(m_webViewManager->GetWebView()->get_Source(&uri)) && uri.get()) {
                        SetWindowTextW(m_hEditAddress, uri.get());
                    }
                }
            }
            break;
        }
        case IDC_BTN_BLOCKER:
            ShowBlockerMenu();
            break;
        case IDM_BLOCKER_PICKER:
            ElementBlocker::Instance().TogglePickerMode(m_webViewManager->GetWebView());
            break;
        case IDM_BLOCKER_TOGGLE_NATIVE: {
            bool nextState = !NativeRequestFilter::Instance().IsEnabled();
            NativeRequestFilter::Instance().SetEnabled(nextState);
            std::wstring infoMsg = nextState
                ? L"原生网络请求拦截已开启！\n广告与恶意跟踪器将直接在网络底层阻断，网页加载提速 40%+。"
                : L"原生网络请求拦截已关闭。";
            MessageBoxW(m_hWnd, infoMsg.c_str(), L"原生请求拦截", MB_OK | MB_ICONINFORMATION);
            break;
        }
        case IDM_BLOCKER_CLEAR_RULES: {
            std::string host = StringUtils::WideToUtf8(ElementBlocker::Instance().GetCurrentHost());
            if (!host.empty()) {
                Config::Instance().ClearBlockRulesForHost(host);
                ElementBlocker::Instance().UpdateRulesScript(m_webViewManager->GetWebView());
                m_webViewManager->Reload();
                std::wstring infoMsg = L"已清空网站 [" + ElementBlocker::Instance().GetCurrentHost() + L"] 的全部元素屏蔽规则并刷新。";
                MessageBoxW(m_hWnd, infoMsg.c_str(), L"清空规则", MB_OK | MB_ICONINFORMATION);
            } else {
                MessageBoxW(m_hWnd, L"当前页面未识别到有效域名。", L"清空规则", MB_OK | MB_ICONINFORMATION);
            }
            break;
        }
        default:
            if (id >= IDM_ZOOM_SET_BASE && id < IDM_ZOOM_SET_BASE + static_cast<WORD>(std::size(kPresetZoomPercentages))) {
                size_t idx = id - IDM_ZOOM_SET_BASE;
                if (m_webViewManager) {
                    m_webViewManager->SetZoomFactor(kPresetZoomPercentages[idx] / 100.0);
                }
            } else if (id >= IDM_DNS_SELECT_BASE && id < IDM_DNS_SELECT_BASE + 50) {
                size_t pIdx = id - IDM_DNS_SELECT_BASE;
                const auto& providers = DnsManager::Instance().GetProviders();
                if (pIdx < providers.size()) {
                    auto& settings = Config::Instance().GetSettings();
                    settings.enablePublicDns = true;
                    settings.selectedDnsProvider = providers[pIdx].id;
                    Config::Instance().Save();
                    DnsManager::Instance().ApplySettings();
                    UpdateDnsDisplay();
                    std::wstring infoMsg = L"已切换至安全 DNS: 【" + providers[pIdx].name + L"】\n\n新策略已生效，建议刷新网页。";
                    MessageBoxW(m_hWnd, infoMsg.c_str(), L"安全 DNS 已更新", MB_OK | MB_ICONINFORMATION);
                }
            }
            break;
        }
        return 0;
    }

    case WM_SYSCOMMAND: {
        if ((wParam & 0xFFF0) == SC_MINIMIZE) {
            if (m_webViewManager && m_webViewManager->GetWebView()) {
                bool isPlayingAudio = m_webViewManager->IsDocumentPlayingAudio();
                PowerManager::Instance().HandleWindowMinimize(
                    m_webViewManager->GetController(),
                    m_webViewManager->GetWebView(),
                    isPlayingAudio
                );
            }
        } else if ((wParam & 0xFFF0) == SC_RESTORE) {
            if (m_webViewManager && m_webViewManager->GetWebView()) {
                PowerManager::Instance().HandleWindowRestore(
                    m_webViewManager->GetController(),
                    m_webViewManager->GetWebView()
                );
            }
        }
        break;
    }

    case WM_DPICHANGED: {
        auto* lprc = reinterpret_cast<RECT*>(lParam);
        SetWindowPos(m_hWnd, nullptr, lprc->left, lprc->top, lprc->right - lprc->left, lprc->bottom - lprc->top, SWP_NOZORDER | SWP_NOACTIVATE);
        UpdateDpiScaling(HIWORD(wParam));
        RECT client;
        GetClientRect(m_hWnd, &client);
        UpdateLayout(client.right, client.bottom);
        return 0;
    }

    case WM_CLOSE: {
        ShowWindow(m_hWnd, SW_HIDE);
        if (m_webViewManager) {
            m_webViewManager->ShutdownAndPurgeData();
        }
        DestroyWindow(m_hWnd);
        return 0;
    }

    case WM_DESTROY: {
        PostQuitMessage(0);
        return 0;
    }

    default:
        break;
    }

    return DefWindowProcW(m_hWnd, msg, wParam, lParam);
}

LRESULT CALLBACK MainWindow::WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    MainWindow* pThis = nullptr;

    if (msg == WM_NCCREATE) {
        auto* pCreate = reinterpret_cast<CREATESTRUCTW*>(lParam);
        pThis = reinterpret_cast<MainWindow*>(pCreate->lpCreateParams);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pThis));
        pThis->m_hWnd = hWnd;
    } else {
        pThis = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    }

    if (pThis) {
        return pThis->HandleMessage(msg, wParam, lParam);
    }

    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

void MainWindow::UpdateZoomDisplay(double zoom) {
    if (!m_hBtnZoom) return;
    int percent = static_cast<int>(std::round(zoom * 100.0));
    wchar_t buf[32]{};
    swprintf_s(buf, L"🔍 %d%%", percent);
    SetWindowTextW(m_hBtnZoom, buf);
}

void MainWindow::ShowZoomMenu() {
    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) return;

    AppendMenuW(hMenu, MF_STRING, IDM_ZOOM_IN, L"放大页面\tCtrl + +");
    AppendMenuW(hMenu, MF_STRING, IDM_ZOOM_OUT, L"缩小页面\tCtrl + -");
    AppendMenuW(hMenu, MF_STRING, IDM_ZOOM_RESET, L"实际大小 (100%)\tCtrl + 0");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

    double currentZoom = m_webViewManager ? m_webViewManager->GetZoomFactor() : 1.0;
    int currentPercent = static_cast<int>(std::round(currentZoom * 100.0));

    int closestIdx = -1;
    int minDiff = 10000;
    for (size_t i = 0; i < std::size(kPresetZoomPercentages); ++i) {
        int diff = std::abs(kPresetZoomPercentages[i] - currentPercent);
        if (diff < minDiff) {
            minDiff = diff;
            closestIdx = static_cast<int>(i);
        }
    }

    for (size_t i = 0; i < std::size(kPresetZoomPercentages); ++i) {
        wchar_t itemText[32]{};
        swprintf_s(itemText, L"%d%%", kPresetZoomPercentages[i]);
        UINT flags = MF_STRING;
        if (static_cast<int>(i) == closestIdx && minDiff <= 3) {
            flags |= MF_CHECKED;
        }
        AppendMenuW(hMenu, flags, IDM_ZOOM_SET_BASE + static_cast<WORD>(i), itemText);
    }

    RECT btnRect{};
    GetWindowRect(m_hBtnZoom, &btnRect);

    TrackPopupMenu(
        hMenu,
        TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON,
        btnRect.left, btnRect.bottom,
        0, m_hWnd, nullptr
    );

    DestroyMenu(hMenu);
}

void MainWindow::UpdateDnsDisplay() {
    if (!m_hBtnDns) return;
    const auto& settings = Config::Instance().GetSettings();
    if (!settings.enablePublicDns) {
        SetWindowTextW(m_hBtnDns, L"🌐 DNS [系统]");
    } else {
        if (settings.selectedDnsProvider == "custom") {
            SetWindowTextW(m_hBtnDns, L"🌐 DNS [自定义]");
        } else {
            const auto* p = DnsManager::Instance().GetActiveProvider();
            if (p) {
                std::wstring label;
                if (p->id == "alidns") label = L"🌐 阿里 DNS";
                else if (p->id == "dnspod") label = L"🌐 腾讯 DNS";
                else if (p->id == "baidu") label = L"🌐 百度 DNS";
                else if (p->id == "114") label = L"🌐 114 DNS";
                else if (p->id == "cloudflare") label = L"🌐 1.1.1.1";
                else if (p->id == "google") label = L"🌐 8.8.8.8";
                else if (p->id == "quad9") label = L"🌐 Quad9";
                else if (p->id == "opendns") label = L"🌐 OpenDNS";
                else if (p->id == "cnnic") label = L"🌐 CNNIC";
                else label = L"🌐 " + p->name;
                SetWindowTextW(m_hBtnDns, label.c_str());
            } else {
                SetWindowTextW(m_hBtnDns, L"🌐 DNS [开]");
            }
        }
    }
}

void MainWindow::ShowDnsMenu() {
    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) return;

    const auto& settings = Config::Instance().GetSettings();
    const auto& providers = DnsManager::Instance().GetProviders();

    UINT toggleFlags = MF_STRING | (settings.enablePublicDns ? MF_CHECKED : MF_UNCHECKED);
    AppendMenuW(hMenu, toggleFlags, IDM_DNS_TOGGLE_ENABLE, L"✔  启用公共安全 DNS (DoH 隐私加密)");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

    for (size_t i = 0; i < providers.size(); ++i) {
        const auto& p = providers[i];
        std::wstring itemText = p.name;
        if (!p.ipv6Primary.empty() && p.ipv6Primary != L"(暂无)") {
            itemText += L"  [IPv4/IPv6]";
        } else {
            itemText += L"  [IPv4]";
        }
        UINT pFlags = MF_STRING;
        if (settings.enablePublicDns && settings.selectedDnsProvider == p.id) {
            pFlags |= MF_CHECKED;
        }
        AppendMenuW(hMenu, pFlags, IDM_DNS_SELECT_BASE + static_cast<WORD>(i), itemText.c_str());
    }

    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, IDM_DNS_OPEN_SETTINGS, L"⚙  公共 DNS 详细 IPv4/IPv6 与高级设置...");

    RECT btnRect{};
    GetWindowRect(m_hBtnDns, &btnRect);

    TrackPopupMenu(
        hMenu,
        TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON,
        btnRect.left, btnRect.bottom,
        0, m_hWnd, nullptr
    );

    DestroyMenu(hMenu);
}

void MainWindow::ShowBlockerMenu() {
    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) return;

    bool nativeEnabled = NativeRequestFilter::Instance().IsEnabled();
    uint64_t blockedCount = NativeRequestFilter::Instance().GetBlockedCount();

    AppendMenuW(hMenu, MF_STRING, IDM_BLOCKER_PICKER, L"🎯 选取网页元素屏蔽 (Ctrl + Shift + H)");

    std::wstring nativeStr = nativeEnabled ? L"⚡ 原生请求拦截: [已开启]" : L"⚡ 原生请求拦截: [已关闭]";
    AppendMenuW(hMenu, MF_STRING | (nativeEnabled ? MF_CHECKED : MF_UNCHECKED), IDM_BLOCKER_TOGGLE_NATIVE, nativeStr.c_str());

    std::wstring countStr = L"📊 已阻断请求: " + std::to_wstring(blockedCount) + L" 个 (提速40%+)";
    AppendMenuW(hMenu, MF_STRING | MF_DISABLED | MF_GRAYED, 0, countStr.c_str());

    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, IDM_TOGGLE_IMMERSIVE, m_isImmersiveMode ? L"🌌 退出无 UI 沉浸模式 (F9)" : L"🌌 切换无 UI 沉浸模式 (F9)");

    std::wstring currentHost = ElementBlocker::Instance().GetCurrentHost();
    std::wstring clearStr = currentHost.empty()
        ? L"🗑️ 清空当前网站元素规则"
        : (L"🗑️ 清空 " + currentHost + L" 元素规则");
    AppendMenuW(hMenu, MF_STRING, IDM_BLOCKER_CLEAR_RULES, clearStr.c_str());

    RECT btnRect{};
    GetWindowRect(m_hBtnBlocker, &btnRect);

    TrackPopupMenu(
        hMenu,
        TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON,
        btnRect.left, btnRect.bottom,
        0, m_hWnd, nullptr
    );

    DestroyMenu(hMenu);
}

void MainWindow::ShowShareMenu() {
    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) return;

    AppendMenuW(hMenu, MF_STRING, IDM_SHARE_COPY_URL, L"📋  拷贝当前网页链接 (Copy URL)");
    AppendMenuW(hMenu, MF_STRING, IDM_SHARE_OPEN_DEFAULT, L"🌐  在系统默认浏览器中打开");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, IDM_TOGGLE_FULLSCREEN, m_isFullScreen ? L"🖥️  退出全屏视图 (F11)" : L"🖥️  全屏视图 (F11)");
    AppendMenuW(hMenu, MF_STRING, IDM_TOGGLE_IMMERSIVE, m_isImmersiveMode ? L"🌌  退出沉浸全景视图 (F9)" : L"🌌  沉浸全景视图 (F9)");

    RECT btnRect{};
    GetWindowRect(m_hBtnShare, &btnRect);

    TrackPopupMenu(
        hMenu,
        TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON,
        btnRect.left, btnRect.bottom,
        0, m_hWnd, nullptr
    );

    DestroyMenu(hMenu);
}

} // namespace UltraLight
