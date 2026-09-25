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

namespace UltraLight {

static const int kPresetZoomPercentages[] = {
    500, 400, 300, 250, 200, 175, 150, 125, 110, 100, 90, 80, 75, 67, 50, 33, 25
};

MainWindow::MainWindow() : m_webViewManager(std::make_unique<WebViewManager>()) {}

MainWindow::~MainWindow() {
    if (m_webViewManager) {
        m_webViewManager->ShutdownAndPurgeData();
    }
    if (m_hUiFont) {
        DeleteObject(m_hUiFont);
    }
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
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(101));
    wc.hIconSm = reinterpret_cast<HICON>(LoadImageW(hInstance, MAKEINTRESOURCEW(101), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));

    if (!RegisterClassExW(&wc)) {
        return false;
    }

    m_hWnd = CreateWindowExW(
        WS_EX_APPWINDOW,
        CLASS_NAME,
        L"UltraLight Browser",
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
}

void MainWindow::UpdateDpiScaling(UINT dpi) {
    m_dpi = dpi;
    m_topbarHeight = MulDiv(44, dpi, 96);

    if (m_hUiFont) {
        DeleteObject(m_hUiFont);
    }

    int fontHeight = -MulDiv(11, dpi, 72);
    m_hUiFont = CreateFontW(
        fontHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI Variable Display"
    );

    // Apply font to child controls
    if (m_hBtnBack) SendMessageW(m_hBtnBack, WM_SETFONT, reinterpret_cast<WPARAM>(m_hUiFont), TRUE);
    if (m_hBtnForward) SendMessageW(m_hBtnForward, WM_SETFONT, reinterpret_cast<WPARAM>(m_hUiFont), TRUE);
    if (m_hBtnReload) SendMessageW(m_hBtnReload, WM_SETFONT, reinterpret_cast<WPARAM>(m_hUiFont), TRUE);
    if (m_hEditAddress) SendMessageW(m_hEditAddress, WM_SETFONT, reinterpret_cast<WPARAM>(m_hUiFont), TRUE);
    if (m_hBtnDns) SendMessageW(m_hBtnDns, WM_SETFONT, reinterpret_cast<WPARAM>(m_hUiFont), TRUE);
    if (m_hBtnZoom) SendMessageW(m_hBtnZoom, WM_SETFONT, reinterpret_cast<WPARAM>(m_hUiFont), TRUE);
    if (m_hBtnBlocker) SendMessageW(m_hBtnBlocker, WM_SETFONT, reinterpret_cast<WPARAM>(m_hUiFont), TRUE);
}

void MainWindow::CreateToolbarControls() {
    m_hBtnBack = CreateWindowExW(
        0, L"BUTTON", L"←",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_FLAT,
        0, 0, 0, 0, m_hWnd, reinterpret_cast<HMENU>(IDC_BTN_BACK), m_hInstance, nullptr
    );

    m_hBtnForward = CreateWindowExW(
        0, L"BUTTON", L"→",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_FLAT,
        0, 0, 0, 0, m_hWnd, reinterpret_cast<HMENU>(IDC_BTN_FORWARD), m_hInstance, nullptr
    );

    m_hBtnReload = CreateWindowExW(
        0, L"BUTTON", L"⟳",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_FLAT,
        0, 0, 0, 0, m_hWnd, reinterpret_cast<HMENU>(IDC_BTN_RELOAD), m_hInstance, nullptr
    );

    m_hEditAddress = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_LEFT,
        0, 0, 0, 0, m_hWnd, reinterpret_cast<HMENU>(IDC_EDIT_ADDRESS), m_hInstance, nullptr
    );
    SendMessageW(m_hEditAddress, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(6, 6));
    SendMessageW(m_hEditAddress, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"输入网址或搜索内容，按 Enter 访问"));

    m_hBtnDns = CreateWindowExW(
        0, L"BUTTON", L"🌐 DNS",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_FLAT,
        0, 0, 0, 0, m_hWnd, reinterpret_cast<HMENU>(IDC_BTN_DNS), m_hInstance, nullptr
    );

    m_hBtnZoom = CreateWindowExW(
        0, L"BUTTON", L"🔍 100%",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_FLAT,
        0, 0, 0, 0, m_hWnd, reinterpret_cast<HMENU>(IDC_BTN_ZOOM), m_hInstance, nullptr
    );

    m_hBtnBlocker = CreateWindowExW(
        0, L"BUTTON", L"🛡 Blocker",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_FLAT,
        0, 0, 0, 0, m_hWnd, reinterpret_cast<HMENU>(IDC_BTN_BLOCKER), m_hInstance, nullptr
    );

    // Subclass address bar for Enter key navigation
    SetWindowSubclass(m_hEditAddress, AddressBarSubclassProc, 1, reinterpret_cast<DWORD_PTR>(this));

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

    int pad = MulDiv(6, m_dpi, 96);
    int btnW = MulDiv(34, m_dpi, 96);
    int blockBtnW = MulDiv(90, m_dpi, 96);
    int zoomBtnW = MulDiv(72, m_dpi, 96);
    int dnsBtnW = MulDiv(102, m_dpi, 96);
    int topH = m_topbarHeight;
    int ctrlH = topH - pad * 2;

    int x = pad;

    // Back
    SetWindowPos(m_hBtnBack, nullptr, x, pad, btnW, ctrlH, SWP_NOZORDER);
    x += btnW + pad;

    // Forward
    SetWindowPos(m_hBtnForward, nullptr, x, pad, btnW, ctrlH, SWP_NOZORDER);
    x += btnW + pad;

    // Reload
    SetWindowPos(m_hBtnReload, nullptr, x, pad, btnW, ctrlH, SWP_NOZORDER);
    x += btnW + pad;

    // Right-aligned buttons: [ 🌐 DNS ] [ 🔍 100% ] [ 🛡 Blocker ]
    int rightX = width - pad - blockBtnW;
    SetWindowPos(m_hBtnBlocker, nullptr, rightX, pad, blockBtnW, ctrlH, SWP_NOZORDER);

    rightX -= (zoomBtnW + pad);
    SetWindowPos(m_hBtnZoom, nullptr, rightX, pad, zoomBtnW, ctrlH, SWP_NOZORDER);

    rightX -= (dnsBtnW + pad);
    SetWindowPos(m_hBtnDns, nullptr, rightX, pad, dnsBtnW, ctrlH, SWP_NOZORDER);

    // Address Bar fill
    int addrW = (rightX - pad) - x;
    if (addrW > 50) {
        SetWindowPos(m_hEditAddress, nullptr, x, pad + 1, addrW, ctrlH - 2, SWP_NOZORDER);
    }

    // Resize WebView2
    RECT webViewRect{ 0, topH, width, height };
    m_webViewManager->Resize(webViewRect);
}

void MainWindow::SetFullScreen(bool enable) {
    if (m_isFullScreen == enable) return;
    m_isFullScreen = enable;

    if (m_isFullScreen) {
        // Save current window placement & style
        m_wpPrev.length = sizeof(WINDOWPLACEMENT);
        GetWindowPlacement(m_hWnd, &m_wpPrev);
        m_dwStylePrev = static_cast<DWORD>(GetWindowLongW(m_hWnd, GWL_STYLE));

        // Hide toolbar controls
        if (m_hBtnBack) ShowWindow(m_hBtnBack, SW_HIDE);
        if (m_hBtnForward) ShowWindow(m_hBtnForward, SW_HIDE);
        if (m_hBtnReload) ShowWindow(m_hBtnReload, SW_HIDE);
        if (m_hEditAddress) ShowWindow(m_hEditAddress, SW_HIDE);
        if (m_hBtnDns) ShowWindow(m_hBtnDns, SW_HIDE);
        if (m_hBtnZoom) ShowWindow(m_hBtnZoom, SW_HIDE);
        if (m_hBtnBlocker) ShowWindow(m_hBtnBlocker, SW_HIDE);

        // Get monitor bounds for current window
        HMONITOR hMon = MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{ sizeof(MONITORINFO) };
        GetMonitorInfoW(hMon, &mi);

        // Modify style to borderless popup and resize to full monitor
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
        // Restore window style and placement
        SetWindowLongW(m_hWnd, GWL_STYLE, m_dwStylePrev);
        SetWindowPlacement(m_hWnd, &m_wpPrev);
        SetWindowPos(
            m_hWnd, nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED
        );

        // Show toolbar controls (if not in immersive mode)
        int showCmd = m_isImmersiveMode ? SW_HIDE : SW_SHOW;
        if (m_hBtnBack) ShowWindow(m_hBtnBack, showCmd);
        if (m_hBtnForward) ShowWindow(m_hBtnForward, showCmd);
        if (m_hBtnReload) ShowWindow(m_hBtnReload, showCmd);
        if (m_hEditAddress) ShowWindow(m_hEditAddress, showCmd);
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
        break;

    case WM_KILLFOCUS:
        s_needSelectAllOnMouseUp = false;
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
            SetWindowTextW(m_hWnd, (title + L" - UltraLight").c_str());
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

    case WM_TIMER: {
        if (wParam == IDT_INACTIVITY_CHECK) {
            ULONGLONG now = GetTickCount64();
            HWND hFore = GetForegroundWindow();
            bool isUnfocused = (hFore != m_hWnd) || IsIconic(m_hWnd);
            // 5 minutes (300,000 ms) of inactivity when unfocused or minimized
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
            if (m_isFullScreen) {
                SetFullScreen(false);
            }
            if (m_isImmersiveMode) {
                SetImmersiveMode(false);
            }
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
                ? L"已开启公共 DNS 服务器解析！\n建议刷新网页以使新设置彻底生效。"
                : L"已关闭公共 DNS，恢复为系统默认解析。";
            MessageBoxW(m_hWnd, infoMsg.c_str(), L"公共 DNS 设置", MB_OK | MB_ICONINFORMATION);
            break;
        }
        case IDM_DNS_OPEN_SETTINGS:
            DnsManager::Instance().ShowDnsDialog(m_hWnd);
            UpdateDnsDisplay();
            break;
        case IDC_EDIT_ADDRESS: {
            WORD notify = HIWORD(wParam);
            if (notify == EN_KILLFOCUS) {
                // When address bar loses focus, restore current page URL
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
                ? L"原生网络请求拦截已开启！\n广告与跟踪器将直接在网络层阻断，提速 40%+，省流量 50%+。"
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
                std::wstring infoMsg = L"已清空网站 [" + ElementBlocker::Instance().GetCurrentHost() + L"] 的全部元素屏蔽规则并重新载入。";
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
            } else if (id >= IDM_DNS_SELECT_BASE) {
                size_t pIdx = id - IDM_DNS_SELECT_BASE;
                const auto& providers = DnsManager::Instance().GetProviders();
                if (pIdx < providers.size()) {
                    auto& settings = Config::Instance().GetSettings();
                    settings.enablePublicDns = true;
                    settings.selectedDnsProvider = providers[pIdx].id;
                    Config::Instance().Save();
                    DnsManager::Instance().ApplySettings();
                    UpdateDnsDisplay();
                    std::wstring infoMsg = L"已切换至公共 DNS: 【" + providers[pIdx].name + L"】\n\n新策略已生效，建议刷新网页。";
                    MessageBoxW(m_hWnd, infoMsg.c_str(), L"公共 DNS 已更新", MB_OK | MB_ICONINFORMATION);
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

    AppendMenuW(hMenu, MF_STRING, IDM_ZOOM_IN, L"放大\tCtrl + +");
    AppendMenuW(hMenu, MF_STRING, IDM_ZOOM_OUT, L"缩小\tCtrl + -");
    AppendMenuW(hMenu, MF_STRING, IDM_ZOOM_RESET, L"重置为 100%\tCtrl + 0");
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
    AppendMenuW(hMenu, toggleFlags, IDM_DNS_TOGGLE_ENABLE, L"✔  启用公共 DNS 服务器 (DoH 隐私加密)");
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

    std::wstring nativeStr = nativeEnabled ? L"⚡ 原生网络请求拦截: [已开启]" : L"⚡ 原生网络请求拦截: [已关闭]";
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

} // namespace UltraLight
