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
    // macOS Safari Dark Palette
    m_hBrTopBarBg = CreateSolidBrush(RGB(36, 36, 39));
    m_hBrAddressBg = CreateSolidBrush(RGB(48, 48, 52));
    m_hPenAddressBorder = CreatePen(PS_SOLID, 1, RGB(62, 62, 68));
    m_hPenAddressBorderFocus = CreatePen(PS_SOLID, 1, RGB(10, 132, 255));
    m_hPenSeparator = CreatePen(PS_SOLID, 1, RGB(24, 24, 26));
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

    // Extend frame into client area so DWM retains drop shadows and rounded corners
    MARGINS margins{ 0, 0, 1, 0 };
    DwmExtendFrameIntoClientArea(m_hWnd, &margins);
    SetWindowPos(m_hWnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);

    DragAcceptFiles(m_hWnd, TRUE);
    ShowWindow(m_hWnd, nCmdShow);
    UpdateWindow(m_hWnd);

    return true;
}

void MainWindow::ApplyModernTheme() {
    BOOL darkMode = TRUE;
    DwmSetWindowAttribute(m_hWnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));

    DWORD cornerPref = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(m_hWnd, DWMWA_WINDOW_CORNER_PREFERENCE, &cornerPref, sizeof(cornerPref));

    DWORD backdropType = 2; // Mica
    DwmSetWindowAttribute(m_hWnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdropType, sizeof(backdropType));
}

void MainWindow::UpdateDpiScaling(UINT dpi) {
    m_dpi = dpi;
    m_topbarHeight = MulDiv(52, dpi, 96);

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

    // Apply fonts to child controls
    if (m_hBtnBack) SendMessageW(m_hBtnBack, WM_SETFONT, reinterpret_cast<WPARAM>(m_hNavFont), TRUE);
    if (m_hBtnForward) SendMessageW(m_hBtnForward, WM_SETFONT, reinterpret_cast<WPARAM>(m_hNavFont), TRUE);
    if (m_hEditAddress) SendMessageW(m_hEditAddress, WM_SETFONT, reinterpret_cast<WPARAM>(m_hAddressFont), TRUE);
    if (m_hBtnReload) SendMessageW(m_hBtnReload, WM_SETFONT, reinterpret_cast<WPARAM>(m_hUiFont), TRUE);
    if (m_hBtnShare) SendMessageW(m_hBtnShare, WM_SETFONT, reinterpret_cast<WPARAM>(m_hUiFont), TRUE);
    if (m_hBtnSound) SendMessageW(m_hBtnSound, WM_SETFONT, reinterpret_cast<WPARAM>(m_hUiFont), TRUE);
    if (m_hBtnBlocker) SendMessageW(m_hBtnBlocker, WM_SETFONT, reinterpret_cast<WPARAM>(m_hUiFont), TRUE);
    if (m_hBtnDns) SendMessageW(m_hBtnDns, WM_SETFONT, reinterpret_cast<WPARAM>(m_hUiFont), TRUE);
    if (m_hBtnZoom) SendMessageW(m_hBtnZoom, WM_SETFONT, reinterpret_cast<WPARAM>(m_hUiFont), TRUE);
    if (m_hBtnNewTab) SendMessageW(m_hBtnNewTab, WM_SETFONT, reinterpret_cast<WPARAM>(m_hUiFont), TRUE);
}

LRESULT CALLBACK MainWindow::SafariButtonSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR /*uIdSubclass*/, DWORD_PTR dwRefData) {
    auto* self = reinterpret_cast<MainWindow*>(dwRefData);

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

        // Fill background with toolbar color
        HBRUSH hBrBar = self ? self->m_hBrTopBarBg : nullptr;
        if (!hBrBar) hBrBar = GetSysColorBrush(COLOR_BTNFACE);
        FillRect(memDC, &rc, hBrBar);

        bool enabled = IsWindowEnabled(hWnd) != FALSE;
        int radius = self ? MulDiv(6, self->m_dpi, 96) : 6;

        // Render rounded background on hover or press
        if (enabled && isPressed) {
            HBRUSH hBrPress = CreateSolidBrush(RGB(68, 68, 76));
            HPEN hPenPress = CreatePen(PS_SOLID, 1, RGB(82, 82, 90));
            HGDIOBJ oldBrush = SelectObject(memDC, hBrPress);
            HGDIOBJ oldPen = SelectObject(memDC, hPenPress);
            RoundRect(memDC, rc.left, rc.top, rc.right, rc.bottom, radius * 2, radius * 2);
            SelectObject(memDC, oldBrush);
            SelectObject(memDC, oldPen);
            DeleteObject(hBrPress);
            DeleteObject(hPenPress);
        } else if (enabled && isHovered) {
            HBRUSH hBrHover = CreateSolidBrush(RGB(52, 52, 58));
            HPEN hPenHover = CreatePen(PS_SOLID, 1, RGB(66, 66, 72));
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
            ? (isHovered ? RGB(255, 255, 255) : RGB(232, 232, 237))
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
    // 1. Navigation Chevrons
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

    // 3. Central Smart Search Field (Edit box & Reload button)
    m_hEditAddress = CreateWindowExW(
        0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_LEFT,
        0, 0, 0, 0, m_hWnd, reinterpret_cast<HMENU>(IDC_EDIT_ADDRESS), m_hInstance, nullptr
    );
    SendMessageW(m_hEditAddress, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(6, 6));
    SendMessageW(m_hEditAddress, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"搜索或输入网站名称"));
    SetWindowSubclass(m_hEditAddress, AddressBarSubclassProc, 1, reinterpret_cast<DWORD_PTR>(this));

    m_hBtnReload = CreateWindowExW(
        0, L"BUTTON", L"↻",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 0, 0, m_hWnd, reinterpret_cast<HMENU>(IDC_BTN_RELOAD), m_hInstance, nullptr
    );
    SetWindowSubclass(m_hBtnReload, SafariButtonSubclassProc, 1, reinterpret_cast<DWORD_PTR>(this));

    // 4. Right Safari Action Buttons
    m_hBtnShare = CreateWindowExW(
        0, L"BUTTON", L"↥",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 0, 0, m_hWnd, reinterpret_cast<HMENU>(IDC_BTN_SHARE), m_hInstance, nullptr
    );
    SetWindowSubclass(m_hBtnShare, SafariButtonSubclassProc, 1, reinterpret_cast<DWORD_PTR>(this));

    m_hBtnSound = CreateWindowExW(
        0, L"BUTTON", L"🎧 环绕 [标]",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 0, 0, m_hWnd, reinterpret_cast<HMENU>(IDC_BTN_SOUND), m_hInstance, nullptr
    );
    SetWindowSubclass(m_hBtnSound, SafariButtonSubclassProc, 1, reinterpret_cast<DWORD_PTR>(this));

    m_hBtnBlocker = CreateWindowExW(
        0, L"BUTTON", L"🛡️",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 0, 0, m_hWnd, reinterpret_cast<HMENU>(IDC_BTN_BLOCKER), m_hInstance, nullptr
    );
    SetWindowSubclass(m_hBtnBlocker, SafariButtonSubclassProc, 1, reinterpret_cast<DWORD_PTR>(this));

    m_hBtnDns = CreateWindowExW(
        0, L"BUTTON", L"🌐 DNS",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 0, 0, m_hWnd, reinterpret_cast<HMENU>(IDC_BTN_DNS), m_hInstance, nullptr
    );
    SetWindowSubclass(m_hBtnDns, SafariButtonSubclassProc, 1, reinterpret_cast<DWORD_PTR>(this));

    m_hBtnZoom = CreateWindowExW(
        0, L"BUTTON", L"100%",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 0, 0, m_hWnd, reinterpret_cast<HMENU>(IDC_BTN_ZOOM), m_hInstance, nullptr
    );
    SetWindowSubclass(m_hBtnZoom, SafariButtonSubclassProc, 1, reinterpret_cast<DWORD_PTR>(this));

    m_hBtnNewTab = CreateWindowExW(
        0, L"BUTTON", L"+",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 0, 0, m_hWnd, reinterpret_cast<HMENU>(IDC_BTN_NEWTAB), m_hInstance, nullptr
    );
    SetWindowSubclass(m_hBtnNewTab, SafariButtonSubclassProc, 1, reinterpret_cast<DWORD_PTR>(this));

    UpdateDpiScaling(GetDpiForWindow(m_hWnd));
    UpdateDnsDisplay();
    UpdateSoundDisplay();
}

void MainWindow::UpdateLayout(int width, int height) {
    if (width <= 0 || height <= 0) return;

    if (m_isFullScreen) {
        RECT fsRect{ 0, 0, width, height };
        if (m_webViewManager) {
            m_webViewManager->Resize(fsRect);
        }
        return;
    }

    int topH = m_topbarHeight;
    int ctrlH = MulDiv(28, m_dpi, 96);
    int btnY = (topH - ctrlH) / 2;

    // 1. macOS Traffic Lights Bounds
    int trafficCenterY = topH / 2;
    int trafficR = MulDiv(6, m_dpi, 96);
    int circleD = trafficR * 2;
    int trafficGap = MulDiv(8, m_dpi, 96);

    int closeX = MulDiv(20, m_dpi, 96);
    int minX = closeX + circleD + trafficGap;
    int maxX = minX + circleD + trafficGap;

    m_rcTrafficClose = { closeX - trafficR, trafficCenterY - trafficR, closeX + trafficR, trafficCenterY + trafficR };
    m_rcTrafficMin   = { minX - trafficR, trafficCenterY - trafficR, minX + trafficR, trafficCenterY + trafficR };
    m_rcTrafficMax   = { maxX - trafficR, trafficCenterY - trafficR, maxX + trafficR, trafficCenterY + trafficR };
    m_rcTrafficGroup = { m_rcTrafficClose.left - 4, m_rcTrafficClose.top - 4, m_rcTrafficMax.right + 4, m_rcTrafficMax.bottom + 4 };

    // 2. Left Action Group (Back, Forward)
    int navBtnW = MulDiv(28, m_dpi, 96);
    int pad = MulDiv(6, m_dpi, 96);

    int leftX = m_rcTrafficMax.right + MulDiv(18, m_dpi, 96);

    SetWindowPos(m_hBtnBack, nullptr, leftX, btnY, navBtnW, ctrlH, SWP_NOZORDER);
    leftX += navBtnW + MulDiv(2, m_dpi, 96);

    SetWindowPos(m_hBtnForward, nullptr, leftX, btnY, navBtnW, ctrlH, SWP_NOZORDER);
    leftX += navBtnW + MulDiv(12, m_dpi, 96);

    int leftGroupEnd = leftX;

    // 3. Right Action Group (Share, Blocker, Sound, DNS, Zoom, New Tab)
    int newTabBtnW = MulDiv(28, m_dpi, 96);
    int zoomBtnW = MulDiv(58, m_dpi, 96);
    int dnsBtnW = MulDiv(80, m_dpi, 96);
    int soundBtnW = MulDiv(78, m_dpi, 96);
    int blockerBtnW = MulDiv(32, m_dpi, 96);
    int shareBtnW = MulDiv(30, m_dpi, 96);

    int rightMargin = MulDiv(14, m_dpi, 96);
    int rightX = width - rightMargin - newTabBtnW;

    SetWindowPos(m_hBtnNewTab, nullptr, rightX, btnY, newTabBtnW, ctrlH, SWP_NOZORDER);

    rightX -= (zoomBtnW + pad);
    SetWindowPos(m_hBtnZoom, nullptr, rightX, btnY, zoomBtnW, ctrlH, SWP_NOZORDER);

    rightX -= (dnsBtnW + pad);
    SetWindowPos(m_hBtnDns, nullptr, rightX, btnY, dnsBtnW, ctrlH, SWP_NOZORDER);

    rightX -= (soundBtnW + pad);
    SetWindowPos(m_hBtnSound, nullptr, rightX, btnY, soundBtnW, ctrlH, SWP_NOZORDER);

    rightX -= (blockerBtnW + pad);
    SetWindowPos(m_hBtnBlocker, nullptr, rightX, btnY, blockerBtnW, ctrlH, SWP_NOZORDER);

    rightX -= (shareBtnW + pad);
    SetWindowPos(m_hBtnShare, nullptr, rightX, btnY, shareBtnW, ctrlH, SWP_NOZORDER);

    int rightGroupStart = rightX - MulDiv(12, m_dpi, 96);

    // 4. Centered Smart Search Capsule Layout
    int availW = rightGroupStart - leftGroupEnd;
    int maxCapsuleW = MulDiv(680, m_dpi, 96);
    int capsuleW = availW;
    int capsuleX = leftGroupEnd;

    if (availW > maxCapsuleW) {
        int idealX = (width - maxCapsuleW) / 2;
        if (idealX >= leftGroupEnd && (idealX + maxCapsuleW) <= rightGroupStart) {
            capsuleX = idealX;
            capsuleW = maxCapsuleW;
        } else {
            capsuleW = availW;
        }
    }

    if (capsuleW > MulDiv(120, m_dpi, 96)) {
        int capsuleH = MulDiv(30, m_dpi, 96);
        int capsuleY = (topH - capsuleH) / 2;
        m_rcAddressCapsule = { capsuleX, capsuleY, capsuleX + capsuleW, capsuleY + capsuleH };

        // Integrated Reload/Stop Button at right edge of the capsule
        int reloadBtnW = MulDiv(22, m_dpi, 96);
        int reloadBtnH = MulDiv(22, m_dpi, 96);
        int reloadX = capsuleX + capsuleW - reloadBtnW - MulDiv(4, m_dpi, 96);
        int reloadY = capsuleY + (capsuleH - reloadBtnH) / 2;
        SetWindowPos(m_hBtnReload, nullptr, reloadX, reloadY, reloadBtnW, reloadBtnH, SWP_NOZORDER);

        // Edit control between SSL icon on left and Reload button on right
        int iconOffset = MulDiv(26, m_dpi, 96);
        int editX = capsuleX + iconOffset;
        int editW = (reloadX - editX) - MulDiv(4, m_dpi, 96);
        int editH = MulDiv(20, m_dpi, 96);
        int editY = capsuleY + (capsuleH - editH) / 2;

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
        if (m_hEditAddress) ShowWindow(m_hEditAddress, SW_HIDE);
        if (m_hBtnReload) ShowWindow(m_hBtnReload, SW_HIDE);
        if (m_hBtnShare) ShowWindow(m_hBtnShare, SW_HIDE);
        if (m_hBtnBlocker) ShowWindow(m_hBtnBlocker, SW_HIDE);
        if (m_hBtnSound) ShowWindow(m_hBtnSound, SW_HIDE);
        if (m_hBtnDns) ShowWindow(m_hBtnDns, SW_HIDE);
        if (m_hBtnZoom) ShowWindow(m_hBtnZoom, SW_HIDE);
        if (m_hBtnNewTab) ShowWindow(m_hBtnNewTab, SW_HIDE);

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

        int showCmd = SW_SHOW;
        if (m_hBtnBack) ShowWindow(m_hBtnBack, showCmd);
        if (m_hBtnForward) ShowWindow(m_hBtnForward, showCmd);
        if (m_hEditAddress) ShowWindow(m_hEditAddress, showCmd);
        if (m_hBtnReload) ShowWindow(m_hBtnReload, showCmd);
        if (m_hBtnShare) ShowWindow(m_hBtnShare, showCmd);
        if (m_hBtnBlocker) ShowWindow(m_hBtnBlocker, showCmd);
        if (m_hBtnSound) ShowWindow(m_hBtnSound, showCmd);
        if (m_hBtnDns) ShowWindow(m_hBtnDns, showCmd);
        if (m_hBtnZoom) ShowWindow(m_hBtnZoom, showCmd);
        if (m_hBtnNewTab) ShowWindow(m_hBtnNewTab, showCmd);

        RECT client;
        GetClientRect(m_hWnd, &client);
        UpdateLayout(client.right, client.bottom);
    }
}

void MainWindow::ToggleFullScreen() {
    SetFullScreen(!m_isFullScreen);
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
    case WM_NCCALCSIZE: {
        if (wParam == TRUE) {
            if (IsZoomed(m_hWnd)) {
                auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
                HMONITOR hMon = MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTONEAREST);
                MONITORINFO mi{ sizeof(MONITORINFO) };
                if (GetMonitorInfoW(hMon, &mi)) {
                    params->rgrc[0] = mi.rcWork;
                }
            }
            return 0; // Remove standard Windows caption and frame
        }
        break;
    }

    case WM_NCHITTEST: {
        POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        RECT rcWin;
        GetWindowRect(m_hWnd, &rcWin);

        // Native resize borders
        if (!IsZoomed(m_hWnd) && !m_isFullScreen) {
            int b = MulDiv(6, m_dpi, 96);
            bool left = (pt.x >= rcWin.left && pt.x < rcWin.left + b);
            bool right = (pt.x < rcWin.right && pt.x >= rcWin.right - b);
            bool top = (pt.y >= rcWin.top && pt.y < rcWin.top + b);
            bool bottom = (pt.y < rcWin.bottom && pt.y >= rcWin.bottom - b);

            if (top && left) return HTTOPLEFT;
            if (top && right) return HTTOPRIGHT;
            if (bottom && left) return HTBOTTOMLEFT;
            if (bottom && right) return HTBOTTOMRIGHT;
            if (left) return HTLEFT;
            if (right) return HTRIGHT;
            if (top) return HTTOP;
            if (bottom) return HTBOTTOM;
        }

        POINT clientPt = pt;
        ScreenToClient(m_hWnd, &clientPt);

        if (clientPt.y >= 0 && clientPt.y < m_topbarHeight && !m_isFullScreen) {
            // Check macOS Traffic Lights
            if (PtInRect(&m_rcTrafficGroup, clientPt)) {
                return HTCLIENT;
            }
            // Check child controls
            HWND hChild = ChildWindowFromPointEx(m_hWnd, clientPt, CWP_SKIPINVISIBLE | CWP_SKIPDISABLED);
            if (hChild && hChild != m_hWnd) {
                return HTCLIENT;
            }
            // Draggable empty toolbar area
            return HTCAPTION;
        }

        return HTCLIENT;
    }

    case WM_CREATE: {
        CreateToolbarControls();

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

        m_webViewManager->SetNavigationStateCallback([this](bool isLoading) {
            m_isLoading = isLoading;
            if (m_hBtnReload) {
                SetWindowTextW(m_hBtnReload, isLoading ? L"✕" : L"↻");
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

        if (!m_isFullScreen && w > 0 && topH > 0) {
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

            // 1. Render macOS Traffic Lights (Red, Yellow, Green)
            int trafficR = MulDiv(6, m_dpi, 96);
            int circleD = trafficR * 2;
            int trafficCenterY = topH / 2;

            int closeX = MulDiv(20, m_dpi, 96);
            int minX = closeX + circleD + MulDiv(8, m_dpi, 96);
            int maxX = minX + circleD + MulDiv(8, m_dpi, 96);

            // Close (Red)
            COLORREF colClose = (m_isWindowActive || m_isTrafficGroupHovered)
                ? (m_trafficPressedBtn == 1 ? RGB(214, 69, 61) : RGB(255, 95, 86))
                : RGB(72, 72, 76);
            COLORREF penClose = (m_isWindowActive || m_isTrafficGroupHovered) ? RGB(224, 68, 62) : RGB(60, 60, 64);
            HBRUSH hBrClose = CreateSolidBrush(colClose);
            HPEN hPenClose = CreatePen(PS_SOLID, 1, penClose);
            SelectObject(memDC, hBrClose);
            SelectObject(memDC, hPenClose);
            Ellipse(memDC, closeX - trafficR, trafficCenterY - trafficR, closeX + trafficR, trafficCenterY + trafficR);
            DeleteObject(hBrClose);
            DeleteObject(hPenClose);

            // Minimize (Yellow)
            COLORREF colMin = (m_isWindowActive || m_isTrafficGroupHovered)
                ? (m_trafficPressedBtn == 2 ? RGB(214, 153, 30) : RGB(255, 189, 46))
                : RGB(72, 72, 76);
            COLORREF penMin = (m_isWindowActive || m_isTrafficGroupHovered) ? RGB(222, 161, 35) : RGB(60, 60, 64);
            HBRUSH hBrMin = CreateSolidBrush(colMin);
            HPEN hPenMin = CreatePen(PS_SOLID, 1, penMin);
            SelectObject(memDC, hBrMin);
            SelectObject(memDC, hPenMin);
            Ellipse(memDC, minX - trafficR, trafficCenterY - trafficR, minX + trafficR, trafficCenterY + trafficR);
            DeleteObject(hBrMin);
            DeleteObject(hPenMin);

            // Zoom / Maximize (Green)
            COLORREF colMax = (m_isWindowActive || m_isTrafficGroupHovered)
                ? (m_trafficPressedBtn == 3 ? RGB(30, 160, 48) : RGB(39, 201, 63))
                : RGB(72, 72, 76);
            COLORREF penMax = (m_isWindowActive || m_isTrafficGroupHovered) ? RGB(26, 171, 41) : RGB(60, 60, 64);
            HBRUSH hBrMax = CreateSolidBrush(colMax);
            HPEN hPenMax = CreatePen(PS_SOLID, 1, penMax);
            SelectObject(memDC, hBrMax);
            SelectObject(memDC, hPenMax);
            Ellipse(memDC, maxX - trafficR, trafficCenterY - trafficR, maxX + trafficR, trafficCenterY + trafficR);
            DeleteObject(hBrMax);
            DeleteObject(hPenMax);

            // Hover symbols inside Traffic Lights
            if (m_isTrafficGroupHovered) {
                int arm = MulDiv(3, m_dpi, 96);

                // Red '✕'
                HPEN hPenSymClose = CreatePen(PS_SOLID, 1, RGB(77, 0, 0));
                SelectObject(memDC, hPenSymClose);
                MoveToEx(memDC, closeX - arm, trafficCenterY - arm, nullptr);
                LineTo(memDC, closeX + arm + 1, trafficCenterY + arm + 1);
                MoveToEx(memDC, closeX - arm, trafficCenterY + arm, nullptr);
                LineTo(memDC, closeX + arm + 1, trafficCenterY - arm - 1);
                DeleteObject(hPenSymClose);

                // Yellow '–'
                HPEN hPenSymMin = CreatePen(PS_SOLID, 1, RGB(153, 87, 0));
                SelectObject(memDC, hPenSymMin);
                MoveToEx(memDC, minX - arm, trafficCenterY, nullptr);
                LineTo(memDC, minX + arm + 1, trafficCenterY);
                DeleteObject(hPenSymMin);

                // Green '⤢'
                HPEN hPenSymMax = CreatePen(PS_SOLID, 1, RGB(0, 100, 0));
                SelectObject(memDC, hPenSymMax);
                MoveToEx(memDC, maxX + arm, trafficCenterY - arm, nullptr);
                LineTo(memDC, maxX + 1, trafficCenterY - arm);
                MoveToEx(memDC, maxX + arm, trafficCenterY - arm, nullptr);
                LineTo(memDC, maxX + arm, trafficCenterY);
                MoveToEx(memDC, maxX - arm, trafficCenterY + arm, nullptr);
                LineTo(memDC, maxX - 1, trafficCenterY + arm);
                MoveToEx(memDC, maxX - arm, trafficCenterY + arm, nullptr);
                LineTo(memDC, maxX - arm, trafficCenterY);
                DeleteObject(hPenSymMax);
            }

            // 2. Render macOS Safari Centered Address Bar Capsule
            if (m_rcAddressCapsule.right > m_rcAddressCapsule.left) {
                HPEN activePen = m_isAddressFocused ? m_hPenAddressBorderFocus : m_hPenAddressBorder;
                SelectObject(memDC, activePen);
                HGDIOBJ oldBrush = SelectObject(memDC, m_hBrAddressBg);

                int radius = MulDiv(9, m_dpi, 96);
                RoundRect(memDC, m_rcAddressCapsule.left, m_rcAddressCapsule.top, m_rcAddressCapsule.right, m_rcAddressCapsule.bottom, radius * 2, radius * 2);
                SelectObject(memDC, oldBrush);

                // Draw SSL Lock Icon on left inside capsule
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

    case WM_MOUSEMOVE: {
        POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (PtInRect(&m_rcTrafficGroup, pt)) {
            if (!m_isTrafficGroupHovered) {
                m_isTrafficGroupHovered = true;
                TRACKMOUSEEVENT tme{ sizeof(TRACKMOUSEEVENT), TME_LEAVE, m_hWnd, 0 };
                TrackMouseEvent(&tme);
                InvalidateRect(m_hWnd, &m_rcTrafficGroup, FALSE);
            }

            int prevHover = m_trafficHoveredBtn;
            if (PtInRect(&m_rcTrafficClose, pt)) m_trafficHoveredBtn = 1;
            else if (PtInRect(&m_rcTrafficMin, pt)) m_trafficHoveredBtn = 2;
            else if (PtInRect(&m_rcTrafficMax, pt)) m_trafficHoveredBtn = 3;
            else m_trafficHoveredBtn = 0;

            if (prevHover != m_trafficHoveredBtn) {
                InvalidateRect(m_hWnd, &m_rcTrafficGroup, FALSE);
            }
        } else if (m_isTrafficGroupHovered) {
            m_isTrafficGroupHovered = false;
            m_trafficHoveredBtn = 0;
            InvalidateRect(m_hWnd, &m_rcTrafficGroup, FALSE);
        }
        break;
    }

    case WM_MOUSELEAVE: {
        if (m_isTrafficGroupHovered) {
            m_isTrafficGroupHovered = false;
            m_trafficHoveredBtn = 0;
            m_trafficPressedBtn = 0;
            InvalidateRect(m_hWnd, &m_rcTrafficGroup, FALSE);
        }
        break;
    }

    case WM_LBUTTONDOWN: {
        POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (PtInRect(&m_rcTrafficGroup, pt)) {
            if (PtInRect(&m_rcTrafficClose, pt)) m_trafficPressedBtn = 1;
            else if (PtInRect(&m_rcTrafficMin, pt)) m_trafficPressedBtn = 2;
            else if (PtInRect(&m_rcTrafficMax, pt)) m_trafficPressedBtn = 3;

            if (m_trafficPressedBtn != 0) {
                SetCapture(m_hWnd);
                InvalidateRect(m_hWnd, &m_rcTrafficGroup, FALSE);
                return 0;
            }
        }
        break;
    }

    case WM_LBUTTONUP: {
        if (m_trafficPressedBtn != 0) {
            ReleaseCapture();
            POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            int pressed = m_trafficPressedBtn;
            m_trafficPressedBtn = 0;
            InvalidateRect(m_hWnd, &m_rcTrafficGroup, FALSE);

            if (pressed == 1 && PtInRect(&m_rcTrafficClose, pt)) {
                PostMessageW(m_hWnd, WM_CLOSE, 0, 0);
            } else if (pressed == 2 && PtInRect(&m_rcTrafficMin, pt)) {
                ShowWindow(m_hWnd, SW_MINIMIZE);
            } else if (pressed == 3 && PtInRect(&m_rcTrafficMax, pt)) {
                ShowWindow(m_hWnd, IsZoomed(m_hWnd) ? SW_RESTORE : SW_MAXIMIZE);
            }
            return 0;
        }
        break;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC: {
        HWND hTarget = reinterpret_cast<HWND>(lParam);
        if (hTarget == m_hEditAddress) {
            HDC hdcEdit = reinterpret_cast<HDC>(wParam);
            SetTextColor(hdcEdit, RGB(255, 255, 255));
            SetBkColor(hdcEdit, RGB(48, 48, 52));
            return reinterpret_cast<LRESULT>(m_hBrAddressBg);
        }
        break;
    }

    case WM_TIMER: {
        if (wParam == IDT_INACTIVITY_CHECK) {
            KillTimer(m_hWnd, IDT_INACTIVITY_CHECK);
            if (!m_isWindowActive || IsIconic(m_hWnd)) {
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
        m_isWindowActive = (LOWORD(wParam) != WA_INACTIVE);
        InvalidateRect(m_hWnd, &m_rcTrafficGroup, FALSE);

        if (m_isWindowActive) {
            // Cancel background idle timer immediately on window focus
            KillTimer(m_hWnd, IDT_INACTIVITY_CHECK);
            m_lastInteractionTick = GetTickCount64();
            if (m_webViewManager && m_webViewManager->GetWebView() &&
                (PowerManager::Instance().IsSuspended() || PowerManager::Instance().IsAudioPlaybackBackgrounded())) {
                PowerManager::Instance().HandleActivityResume(
                    m_webViewManager->GetController(),
                    m_webViewManager->GetWebView()
                );
            }
        } else {
            // Start a single 5-minute timer to suspend if user remains unfocused
            SetTimer(m_hWnd, IDT_INACTIVITY_CHECK, 300000, nullptr);
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
        if (wParam == VK_F11) {
            ToggleFullScreen();
            return 0;
        }
        if (wParam == VK_ESCAPE) {
            if (m_isFullScreen) {
                SetFullScreen(false);
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
            if (m_isLoading) {
                m_webViewManager->Stop();
            } else {
                m_webViewManager->Reload();
            }
            break;
        case IDC_BTN_NEWTAB:
            m_webViewManager->Navigate(Config::Instance().GetSettings().startUrl);
            SetFocus(m_hEditAddress);
            SendMessageW(m_hEditAddress, EM_SETSEL, 0, -1);
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
        case IDC_BTN_SOUND:
            ShowSoundMenu();
            break;
        case IDM_SURROUND_TOGGLE: {
            auto& settings = Config::Instance().GetSettings();
            settings.enableSurroundSound = !settings.enableSurroundSound;
            Config::Instance().Save();
            if (m_webViewManager) {
                m_webViewManager->SetSurroundSound(settings.enableSurroundSound, settings.surroundSoundMode);
            }
            UpdateSoundDisplay();
            break;
        }
        case IDM_SURROUND_MODE_LIGHT: {
            auto& settings = Config::Instance().GetSettings();
            settings.enableSurroundSound = true;
            settings.surroundSoundMode = "light";
            Config::Instance().Save();
            if (m_webViewManager) {
                m_webViewManager->SetSurroundSound(settings.enableSurroundSound, settings.surroundSoundMode);
            }
            UpdateSoundDisplay();
            break;
        }
        case IDM_SURROUND_MODE_STANDARD: {
            auto& settings = Config::Instance().GetSettings();
            settings.enableSurroundSound = true;
            settings.surroundSoundMode = "standard";
            Config::Instance().Save();
            if (m_webViewManager) {
                m_webViewManager->SetSurroundSound(settings.enableSurroundSound, settings.surroundSoundMode);
            }
            UpdateSoundDisplay();
            break;
        }
        case IDM_SURROUND_MODE_CINEMA: {
            auto& settings = Config::Instance().GetSettings();
            settings.enableSurroundSound = true;
            settings.surroundSoundMode = "cinema";
            Config::Instance().Save();
            if (m_webViewManager) {
                m_webViewManager->SetSurroundSound(settings.enableSurroundSound, settings.surroundSoundMode);
            }
            UpdateSoundDisplay();
            break;
        }
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
    swprintf_s(buf, L"%d%%", percent);
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

void MainWindow::UpdateSoundDisplay() {
    if (!m_hBtnSound) return;
    const auto& settings = Config::Instance().GetSettings();
    std::wstring label;
    if (!settings.enableSurroundSound) {
        label = L"🎧 环绕 [关]";
    } else if (settings.surroundSoundMode == "light") {
        label = L"🎧 环绕 [轻]";
    } else if (settings.surroundSoundMode == "cinema") {
        label = L"🎧 环绕 [影]";
    } else {
        label = L"🎧 环绕 [标]";
    }
    SetWindowTextW(m_hBtnSound, label.c_str());
    InvalidateRect(m_hBtnSound, nullptr, TRUE);
}

void MainWindow::ShowSoundMenu() {
    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) return;

    const auto& settings = Config::Instance().GetSettings();

    UINT toggleFlags = MF_STRING | (settings.enableSurroundSound ? MF_CHECKED : MF_UNCHECKED);
    AppendMenuW(hMenu, toggleFlags, IDM_SURROUND_TOGGLE, L"✔  开启 2 声道虚拟环绕立体声 (DSP)");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

    UINT lightFlags = MF_STRING | ((settings.enableSurroundSound && settings.surroundSoundMode == "light") ? MF_CHECKED : MF_UNCHECKED);
    AppendMenuW(hMenu, lightFlags, IDM_SURROUND_MODE_LIGHT, L"🍃  轻柔模式 (自然声场加宽，适合人声播客)");

    UINT stdFlags = MF_STRING | ((settings.enableSurroundSound && settings.surroundSoundMode == "standard") ? MF_CHECKED : MF_UNCHECKED);
    AppendMenuW(hMenu, stdFlags, IDM_SURROUND_MODE_STANDARD, L"🎧  标准模式 (Bauer交叉反馈+房间反射，默认)");

    UINT cinemaFlags = MF_STRING | ((settings.enableSurroundSound && settings.surroundSoundMode == "cinema") ? MF_CHECKED : MF_UNCHECKED);
    AppendMenuW(hMenu, cinemaFlags, IDM_SURROUND_MODE_CINEMA, L"🎬  影院模式 (极限声场沉浸+低频饱满，影视爆棚)");

    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING | MF_DISABLED | MF_GRAYED, 0, L"💡 针对耳机优化，带动态防破音压限器 (实时生效)");

    RECT btnRect{};
    GetWindowRect(m_hBtnSound, &btnRect);

    TrackPopupMenu(
        hMenu,
        TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON,
        btnRect.left, btnRect.bottom,
        0, m_hWnd, nullptr
    );

    DestroyMenu(hMenu);
}

} // namespace UltraLight
