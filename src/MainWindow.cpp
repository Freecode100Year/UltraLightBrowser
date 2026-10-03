#include "MainWindow.hpp"
#include "AppShell.hpp"
#include "Config.hpp"
#include "InternalPages.hpp"
#include "PowerManager.hpp"
#include "StringUtils.hpp"
#include "WindowGeometry.hpp"

#include <windowsx.h>
#include <uxtheme.h>
#include <shellapi.h>
#include <cmath>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

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

namespace {

constexpr wchar_t kClassName[] = L"UltraLightBrowserMainWindow";
constexpr wchar_t kSuggestClass[] = L"UltraLightBrowserSuggest";
constexpr wchar_t kToastClass[] = L"UltraLightBrowserToast";

void CopyTextToClipboard(HWND hWndOwner, const std::wstring& text) {
    if (!OpenClipboard(hWndOwner)) return;
    EmptyClipboard();
    const size_t bytes = (text.length() + 1) * sizeof(wchar_t);
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

std::wstring Trim(std::wstring s) {
    while (!s.empty() && iswspace(s.front())) s.erase(s.begin());
    while (!s.empty() && iswspace(s.back())) s.pop_back();
    return s;
}

} // namespace

MainWindow::MainWindow(bool isPrivate) : m_private(isPrivate) {
    m_hBrAddressBg = CreateSolidBrush(RGB(30, 32, 37));
}

MainWindow::~MainWindow() {
    for (auto& t : m_tabs) {
        if (t->view) AppShell::Instance().Retire(std::move(t->view));
    }
    m_tabs.clear();
    if (m_spare && m_spare->view) AppShell::Instance().Retire(std::move(m_spare->view));
    if (m_paintBitmap) DeleteObject(m_paintBitmap);
    if (m_sidebar.controller) m_sidebar.controller->Close();
    if (m_overview.controller) m_overview.controller->Close();
    if (m_hSuggest) DestroyWindow(m_hSuggest);
    if (m_hToast) DestroyWindow(m_hToast);
    if (m_hUiFont) DeleteObject(m_hUiFont);
    if (m_hAddressFont) DeleteObject(m_hAddressFont);
    if (m_hSmallFont) DeleteObject(m_hSmallFont);
    if (m_hBrAddressBg) DeleteObject(m_hBrAddressBg);
}

bool MainWindow::Create(HINSTANCE hInstance, int nCmdShow, const std::vector<std::wstring>& initialUrls) {
    m_hInstance = hInstance;
    m_initialUrls = initialUrls;

    static bool s_registered = false;
    if (!s_registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.lpfnWndProc = MainWindow::WndProc;
        wc.hInstance = hInstance;
        wc.lpszClassName = kClassName;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(101));
        wc.hIconSm = reinterpret_cast<HICON>(LoadImageW(hInstance, MAKEINTRESOURCEW(101), IMAGE_ICON,
            GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
        if (!RegisterClassExW(&wc)) return false;

        WNDCLASSEXW sc{};
        sc.cbSize = sizeof(sc);
        sc.lpfnWndProc = MainWindow::SuggestWndProc;
        sc.hInstance = hInstance;
        sc.lpszClassName = kSuggestClass;
        sc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        sc.style = CS_DROPSHADOW;
        RegisterClassExW(&sc);

        WNDCLASSEXW tc{};
        tc.cbSize = sizeof(tc);
        tc.lpfnWndProc = MainWindow::ToastWndProc;
        tc.hInstance = hInstance;
        tc.lpszClassName = kToastClass;
        tc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassExW(&tc);
        s_registered = true;
    }

    // Cascade additional windows.
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    if (AppShell::Instance().WindowCount() > 0) {
        if (HWND fg = GetForegroundWindow()) {
            RECT r{};
            if (GetWindowRect(fg, &r)) { x = r.left + 32; y = r.top + 32; }
        }
    }

    m_hWnd = CreateWindowExW(WS_EX_APPWINDOW, kClassName, L"UltraLightBrowser",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, 1280, 820, nullptr, nullptr, hInstance, this);
    if (!m_hWnd) return false;

    ApplyModernTheme();
    MARGINS margins{0, 0, 1, 0};
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
    DWORD cornerPref = 2;  // DWMWCP_ROUND
    DwmSetWindowAttribute(m_hWnd, DWMWA_WINDOW_CORNER_PREFERENCE, &cornerPref, sizeof(cornerPref));
    DWORD backdropType = 2;  // Mica
    DwmSetWindowAttribute(m_hWnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdropType, sizeof(backdropType));
}

void MainWindow::UpdateDpiScaling(UINT dpi) {
    m_dpi = dpi ? dpi : 96;
    m_topbarHeight = S(52);
    if (m_hUiFont) DeleteObject(m_hUiFont);
    if (m_hAddressFont) DeleteObject(m_hAddressFont);
    if (m_hSmallFont) DeleteObject(m_hSmallFont);
    // GDI+ only accepts installed TrueType faces with regular/bold weights; Segoe UI
    // Variable exists on Windows 11 only, so fall back to Segoe UI elsewhere.
    static const wchar_t* face = [] {
        LOGFONTW lf{};
        lf.lfCharSet = DEFAULT_CHARSET;
        wcscpy_s(lf.lfFaceName, L"Segoe UI Variable Text");
        bool found = false;
        HDC screen = GetDC(nullptr);
        EnumFontFamiliesExW(screen, &lf, [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM p) -> int {
            *reinterpret_cast<bool*>(p) = true;
            return 0;
        }, reinterpret_cast<LPARAM>(&found), 0);
        ReleaseDC(nullptr, screen);
        return found ? L"Segoe UI Variable Text" : L"Segoe UI";
    }();
    auto font = [this](int pt, int weight) {
        return CreateFontW(-MulDiv(pt, static_cast<int>(m_dpi), 72), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
    };
    m_gpUiFont.reset();
    m_gpAddressFont.reset();
    m_gpSmallFont.reset();
    m_hUiFont = font(9, FW_NORMAL);
    m_hAddressFont = font(10, FW_NORMAL);
    m_hSmallFont = font(8, FW_BOLD);
    if (m_hEditAddress) SendMessageW(m_hEditAddress, WM_SETFONT, reinterpret_cast<WPARAM>(m_hAddressFont), TRUE);
    if (m_hFindEdit) SendMessageW(m_hFindEdit, WM_SETFONT, reinterpret_cast<WPARAM>(m_hUiFont), TRUE);
}

void MainWindow::EnsurePaintFonts(HDC hdc) {
    if (!m_gpUiFont) m_gpUiFont = std::make_unique<Gdiplus::Font>(hdc, m_hUiFont);
    if (!m_gpAddressFont) m_gpAddressFont = std::make_unique<Gdiplus::Font>(hdc, m_hAddressFont);
    if (!m_gpSmallFont) m_gpSmallFont = std::make_unique<Gdiplus::Font>(hdc, m_hSmallFont);
}

// ------------------------------------------------------------------ public hooks

ICoreWebView2* MainWindow::ActiveWebView() const {
    const Tab* t = ActiveTab();
    if (t && t->view && t->view->GetWebView()) return t->view->GetWebView();
    for (const auto& tab : m_tabs) {
        if (tab->view && tab->view->GetWebView()) return tab->view->GetWebView();
    }
    return nullptr;
}

UINT32 MainWindow::BrowserProcessId() const {
    for (const auto& t : m_tabs) {
        if (t->view) {
            if (const UINT32 pid = t->view->BrowserProcessId()) return pid;
        }
    }
    return 0;
}

void MainWindow::OpenInNewTab(const std::wstring& url, bool activate) {
    NewTab(url, activate);
}

void MainWindow::FocusWindow() {
    if (IsIconic(m_hWnd)) ShowWindow(m_hWnd, SW_RESTORE);
    SetForegroundWindow(m_hWnd);
}

// ------------------------------------------------------------------ fullscreen

void MainWindow::SetFullScreen(bool enable) {
    if (m_isFullScreen == enable) return;
    m_isFullScreen = enable;
    if (m_isAddressFocused) EndAddressEdit(false);
    HideSuggestions();
    if (enable) {
        m_wpPrev.length = sizeof(WINDOWPLACEMENT);
        GetWindowPlacement(m_hWnd, &m_wpPrev);
        m_dwStylePrev = static_cast<DWORD>(GetWindowLongW(m_hWnd, GWL_STYLE));
        HMONITOR hMon = MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{sizeof(MONITORINFO)};
        GetMonitorInfoW(hMon, &mi);
        // Fullscreen must not inherit maximized work-area sizing or DWM corners.
        const DWORD fullscreenStyle = (m_dwStylePrev & ~(WS_OVERLAPPEDWINDOW | WS_MAXIMIZE)) | WS_POPUP;
        SetWindowLongW(m_hWnd, GWL_STYLE, fullscreenStyle);
        const MARGINS margins{};
        DwmExtendFrameIntoClientArea(m_hWnd, &margins);
        const DWORD cornerPreference = 1;  // DWMWCP_DONOTROUND
        DwmSetWindowAttribute(m_hWnd, DWMWA_WINDOW_CORNER_PREFERENCE, &cornerPreference, sizeof(cornerPreference));
        const COLORREF borderColor = 0xFFFFFFFE;  // DWMWA_COLOR_NONE
        DwmSetWindowAttribute(m_hWnd, 34 /* DWMWA_BORDER_COLOR */, &borderColor, sizeof(borderColor));
        SetWindowPos(m_hWnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    } else {
        SetWindowLongW(m_hWnd, GWL_STYLE, m_dwStylePrev);
        const MARGINS margins{0, 0, 1, 0};
        DwmExtendFrameIntoClientArea(m_hWnd, &margins);
        const DWORD cornerPreference = 2;
        DwmSetWindowAttribute(m_hWnd, DWMWA_WINDOW_CORNER_PREFERENCE, &cornerPreference, sizeof(cornerPreference));
        const COLORREF borderColor = 0xFFFFFFFF;  // DWMWA_COLOR_DEFAULT
        DwmSetWindowAttribute(m_hWnd, 34, &borderColor, sizeof(borderColor));
        SetWindowPlacement(m_hWnd, &m_wpPrev);
        SetWindowPos(m_hWnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    UpdateLayout();
}

void MainWindow::ToggleFullScreen() {
    SetFullScreen(!m_isFullScreen);
}

// ------------------------------------------------------------------ address bar

void MainWindow::BeginAddressEdit(const std::wstring& text, bool selectAll) {
    if (m_isFullScreen || !ActiveTab()) return;
    if (m_overviewVisible) HideOverview();
    m_isAddressFocused = true;
    m_suppressSuggest = true;
    SetWindowTextW(m_hEditAddress, text.c_str());
    m_suppressSuggest = false;
    UpdateLayout();
    SetFocus(m_hEditAddress);
    if (selectAll) SendMessageW(m_hEditAddress, EM_SETSEL, 0, -1);
    else SendMessageW(m_hEditAddress, EM_SETSEL, static_cast<WPARAM>(text.size()), static_cast<LPARAM>(text.size()));
    InvalidateToolbar();
}

void MainWindow::EndAddressEdit(bool focusPage) {
    if (!m_isAddressFocused) return;
    m_isAddressFocused = false;
    HideSuggestions();
    ShowWindow(m_hEditAddress, SW_HIDE);
    UpdateLayout();
    if (focusPage) {
        if (Tab* t = ActiveTab(); t && t->view && t->view->GetController()) {
            t->view->GetController()->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        }
    }
}

void MainWindow::CommitAddress() {
    std::wstring input;
    if (m_hSuggest && IsWindowVisible(m_hSuggest) && m_suggestSel > 0 && m_suggestSel <= static_cast<int>(m_suggestions.size())) {
        input = StringUtils::Utf8ToWide(m_suggestions[static_cast<size_t>(m_suggestSel - 1)].url);
    } else {
        const int len = GetWindowTextLengthW(m_hEditAddress);
        std::wstring buf(static_cast<size_t>(len) + 1, L'\0');
        GetWindowTextW(m_hEditAddress, buf.data(), len + 1);
        buf.resize(static_cast<size_t>(len));
        input = Trim(buf);
    }
    if (input.empty()) return;
    const bool shiftEnter = (GetKeyState(VK_MENU) & 0x8000) != 0;  // Alt+Enter: new tab
    EndAddressEdit(true);
    if (shiftEnter) NewTab(WebViewManager::ResolveInput(input), true, m_activeId);
    else NavigateActive(input);
}

void MainWindow::UpdateSuggestions() {
    if (m_suppressSuggest || !m_isAddressFocused) return;
    const int len = GetWindowTextLengthW(m_hEditAddress);
    std::wstring text(static_cast<size_t>(len) + 1, L'\0');
    GetWindowTextW(m_hEditAddress, text.data(), len + 1);
    text.resize(static_cast<size_t>(len));
    text = Trim(text);
    if (text.empty()) {
        HideSuggestions();
        return;
    }
    m_suggestQuery = text;
    m_suggestions = m_private ? std::vector<Suggestion>{}
                              : AppShell::Instance().Lib().Suggest(StringUtils::WideToUtf8(text), 7);
    m_suggestSel = 0;
    m_suggestHover = -1;
    if (!m_hSuggest) {
        m_hSuggest = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, kSuggestClass, L"",
                                     WS_POPUP, 0, 0, 10, 10, m_hWnd, nullptr, m_hInstance, this);
    }
    if (!m_hSuggest) return;
    const int rowH = S(34);
    const int rows = 1 + static_cast<int>(m_suggestions.size());
    RECT anchor = m_rcActive;
    const int width = (std::max)(static_cast<int>(anchor.right - anchor.left), S(460));
    POINT pt{anchor.left, anchor.bottom + S(4)};
    ClientToScreen(m_hWnd, &pt);
    SetWindowPos(m_hSuggest, HWND_TOPMOST, pt.x, pt.y, width, rows * rowH + S(12), SWP_NOACTIVATE | SWP_SHOWWINDOW);
    HRGN rgn = CreateRoundRectRgn(0, 0, width + 1, rows * rowH + S(12) + 1, S(16), S(16));
    SetWindowRgn(m_hSuggest, rgn, TRUE);
    InvalidateRect(m_hSuggest, nullptr, FALSE);
}

void MainWindow::HideSuggestions() {
    if (m_hSuggest) ShowWindow(m_hSuggest, SW_HIDE);
    m_suggestSel = 0;
}

void MainWindow::MoveSuggestion(int delta) {
    if (!m_hSuggest || !IsWindowVisible(m_hSuggest)) return;
    const int count = 1 + static_cast<int>(m_suggestions.size());
    m_suggestSel = ((m_suggestSel + delta) % count + count) % count;
    m_suppressSuggest = true;
    const std::wstring text = m_suggestSel == 0 ? m_suggestQuery
        : StringUtils::Utf8ToWide(m_suggestions[static_cast<size_t>(m_suggestSel - 1)].url);
    SetWindowTextW(m_hEditAddress, text.c_str());
    SendMessageW(m_hEditAddress, EM_SETSEL, text.size(), text.size());
    m_suppressSuggest = false;
    InvalidateRect(m_hSuggest, nullptr, FALSE);
}

LRESULT CALLBACK MainWindow::SuggestWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    if (!self) return DefWindowProcW(hWnd, msg, wParam, lParam);
    const int rowH = self->S(34);
    const int pad = self->S(6);
    switch (msg) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE: {
        const int row = (GET_Y_LPARAM(lParam) - pad) / rowH;
        if (row != self->m_suggestHover) {
            self->m_suggestHover = row;
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hWnd, 0};
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE:
        self->m_suggestHover = -1;
        InvalidateRect(hWnd, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN: {
        const int row = (GET_Y_LPARAM(lParam) - pad) / rowH;
        if (row >= 0 && row <= static_cast<int>(self->m_suggestions.size())) {
            self->m_suggestSel = row;
            if (row == 0) {
                self->m_suppressSuggest = true;
                SetWindowTextW(self->m_hEditAddress, self->m_suggestQuery.c_str());
                self->m_suppressSuggest = false;
            }
            self->CommitAddress();
        }
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc{};
        GetClientRect(hWnd, &rc);
        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
        HGDIOBJ old = SelectObject(mem, bmp);
        {
            Gdiplus::Graphics g(mem);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
            Gdiplus::SolidBrush bg(Gdiplus::Color(255, 40, 42, 48));
            g.FillRectangle(&bg, 0, 0, rc.right, rc.bottom);
            Gdiplus::Font font(mem, self->m_hAddressFont);
            Gdiplus::Font smallFont2(mem, self->m_hUiFont);
            Gdiplus::StringFormat fmt(Gdiplus::StringFormatFlagsNoWrap);
            fmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
            fmt.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
            const int count = 1 + static_cast<int>(self->m_suggestions.size());
            for (int i = 0; i < count; ++i) {
                const Gdiplus::REAL y = static_cast<Gdiplus::REAL>(pad + i * rowH);
                const bool sel = i == self->m_suggestSel;
                const bool hover = i == self->m_suggestHover;
                if (sel || hover) {
                    Gdiplus::SolidBrush hb(sel ? Gdiplus::Color(255, 61, 111, 240) : Gdiplus::Color(255, 56, 59, 66));
                    g.FillRectangle(&hb, static_cast<Gdiplus::REAL>(pad), y, static_cast<Gdiplus::REAL>(rc.right - 2 * pad), static_cast<Gdiplus::REAL>(rowH));
                }
                const Gdiplus::Color fg = sel ? Gdiplus::Color(255, 255, 255, 255) : Gdiplus::Color(255, 232, 233, 236);
                const Gdiplus::Color dim = sel ? Gdiplus::Color(255, 220, 228, 255) : Gdiplus::Color(255, 140, 143, 150);
                const Gdiplus::RectF iconBox(static_cast<Gdiplus::REAL>(pad + self->S(10)), y + (rowH - self->S(16)) / 2.0f,
                                             static_cast<Gdiplus::REAL>(self->S(16)), static_cast<Gdiplus::REAL>(self->S(16)));
                const Gdiplus::REAL textX = iconBox.X + self->S(28);
                const Gdiplus::REAL textW = rc.right - textX - pad - self->S(10);
                Gdiplus::SolidBrush fgBrush(fg), dimBrush(dim);
                if (i == 0) {
                    Icons::Draw(g, Icon::Search, iconBox, dim);
                    const std::wstring label = L"搜索“" + self->m_suggestQuery + L"”";
                    g.DrawString(label.c_str(), -1, &font, Gdiplus::RectF(textX, y, textW, static_cast<Gdiplus::REAL>(rowH)), &fmt, &fgBrush);
                } else {
                    const auto& s = self->m_suggestions[static_cast<size_t>(i - 1)];
                    Icons::Draw(g, s.bookmark ? Icon::Bookmark : Icon::Clock, iconBox, dim);
                    const std::wstring title = StringUtils::Utf8ToWide(s.title.empty() ? s.url : s.title);
                    const std::wstring url = StringUtils::Utf8ToWide(s.url);
                    const Gdiplus::REAL titleW = textW * 0.58f;
                    g.DrawString(title.c_str(), -1, &font, Gdiplus::RectF(textX, y, titleW, static_cast<Gdiplus::REAL>(rowH)), &fmt, &fgBrush);
                    g.DrawString(url.c_str(), -1, &smallFont2, Gdiplus::RectF(textX + titleW + self->S(12), y, textW - titleW - self->S(12), static_cast<Gdiplus::REAL>(rowH)), &fmt, &dimBrush);
                }
            }
        }
        BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hWnd, &ps);
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

LRESULT CALLBACK MainWindow::AddressBarSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR dwRefData) {
    auto* self = reinterpret_cast<MainWindow*>(dwRefData);
    switch (uMsg) {
    case WM_KILLFOCUS: {
        const LRESULT res = DefSubclassProc(hWnd, uMsg, wParam, lParam);
        if (self) self->EndAddressEdit(false);
        return res;
    }
    case WM_KEYDOWN:
        if (!self) break;
        if (wParam == VK_RETURN) {
            self->CommitAddress();
            return 0;
        }
        if (wParam == VK_ESCAPE) {
            self->EndAddressEdit(true);
            return 0;
        }
        if (wParam == VK_DOWN) { self->MoveSuggestion(1); return 0; }
        if (wParam == VK_UP) { self->MoveSuggestion(-1); return 0; }
        if (wParam == 'A' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            SendMessageW(hWnd, EM_SETSEL, 0, -1);
            return 0;
        }
        break;
    case WM_CHAR:
        if (wParam == VK_RETURN || wParam == VK_ESCAPE || wParam == 1 /* Ctrl+A */) return 0;  // no beep
        break;
    default:
        break;
    }
    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

// ------------------------------------------------------------------ find bar

void MainWindow::ShowFindBar() {
    if (m_isFullScreen) return;
    if (!m_findVisible) {
        m_findVisible = true;
        UpdateLayout();
    }
    SetFocus(m_hFindEdit);
    SendMessageW(m_hFindEdit, EM_SETSEL, 0, -1);
    InvalidateRect(m_hWnd, &m_rcFindBar, FALSE);
}

void MainWindow::HideFindBar() {
    if (!m_findVisible) return;
    m_findVisible = false;
    if (Tab* t = ActiveTab(); t && t->view && t->view->GetWebView()) {
        t->view->GetWebView()->ExecuteScript(InternalPages::FindScript(L"clear", L"").c_str(), nullptr);
    }
    m_findQuery.clear();
    m_findCount = m_findIndex = 0;
    UpdateLayout();
    if (Tab* t = ActiveTab(); t && t->view && t->view->GetController()) {
        t->view->GetController()->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
    }
}

void MainWindow::RunFind(const wchar_t* action) {
    Tab* t = ActiveTab();
    if (!t || !t->view || !t->view->GetWebView() || !m_hFindEdit) return;
    const int len = GetWindowTextLengthW(m_hFindEdit);
    std::wstring q(static_cast<size_t>(len) + 1, L'\0');
    GetWindowTextW(m_hFindEdit, q.data(), len + 1);
    q.resize(static_cast<size_t>(len));
    m_findQuery = q;
    if (q.empty()) {
        m_findCount = m_findIndex = 0;
        t->view->GetWebView()->ExecuteScript(InternalPages::FindScript(L"clear", L"").c_str(), nullptr);
        InvalidateRect(m_hWnd, &m_rcFindBar, FALSE);
        return;
    }
    const HWND hwnd = m_hWnd;
    t->view->GetWebView()->ExecuteScript(InternalPages::FindScript(action, q).c_str(),
        Microsoft::WRL::Callback<ICoreWebView2ExecuteScriptCompletedHandler>([this, hwnd](HRESULT hr, LPCWSTR result) -> HRESULT {
            if (!IsWindow(hwnd) || m_destroyed) return S_OK;
            m_findCount = m_findIndex = 0;
            if (SUCCEEDED(hr) && result) {
                try {
                    const auto r = json::parse(StringUtils::WideToUtf8(result));
                    if (r.is_object()) {
                        m_findCount = r.value("count", 0);
                        m_findIndex = r.value("index", 0);
                    }
                } catch (...) {}
            }
            InvalidateRect(m_hWnd, &m_rcFindBar, FALSE);
            return S_OK;
        }).Get());
}

LRESULT CALLBACK MainWindow::FindEditSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR dwRefData) {
    auto* self = reinterpret_cast<MainWindow*>(dwRefData);
    if (self) {
        if (uMsg == WM_KEYDOWN) {
            if (wParam == VK_RETURN) {
                self->RunFind((GetKeyState(VK_SHIFT) & 0x8000) ? L"prev" : L"next");
                return 0;
            }
            if (wParam == VK_ESCAPE) {
                self->HideFindBar();
                return 0;
            }
        } else if (uMsg == WM_CHAR && (wParam == VK_RETURN || wParam == VK_ESCAPE)) {
            return 0;
        } else if (uMsg == WM_SETFOCUS || uMsg == WM_KILLFOCUS) {
            InvalidateRect(self->m_hWnd, &self->m_rcFindBar, FALSE);
        }
    }
    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

// ------------------------------------------------------------------ toast

LRESULT CALLBACK MainWindow::ToastWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (msg == WM_ERASEBKGND) return 1;
    if (msg == WM_PAINT && self) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc{};
        GetClientRect(hWnd, &rc);
        HBRUSH bg = CreateSolidBrush(RGB(24, 25, 29));
        FillRect(hdc, &rc, bg);
        DeleteObject(bg);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(240, 240, 244));
        HGDIOBJ old = SelectObject(hdc, self->m_hUiFont);
        DrawTextW(hdc, self->m_toastText.c_str(), -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(hdc, old);
        EndPaint(hWnd, &ps);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

void MainWindow::ShowToast(const std::wstring& text) {
    m_toastText = text;
    if (!m_hToast) {
        m_hToast = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT, kToastClass, L"",
                                   WS_POPUP, 0, 0, 10, 10, m_hWnd, nullptr, m_hInstance, this);
        if (!m_hToast) return;
        SetLayeredWindowAttributes(m_hToast, 0, 235, LWA_ALPHA);
    }
    HDC hdc = GetDC(m_hToast);
    HGDIOBJ old = SelectObject(hdc, m_hUiFont);
    SIZE size{};
    GetTextExtentPoint32W(hdc, text.c_str(), static_cast<int>(text.size()), &size);
    SelectObject(hdc, old);
    ReleaseDC(m_hToast, hdc);
    const int w = size.cx + S(36), h = S(36);
    const RECT content = ContentRect();
    POINT pt{(content.left + content.right - w) / 2, content.top + S(18)};
    ClientToScreen(m_hWnd, &pt);
    SetWindowPos(m_hToast, HWND_TOPMOST, pt.x, pt.y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    SetWindowRgn(m_hToast, CreateRoundRectRgn(0, 0, w + 1, h + 1, S(14), S(14)), TRUE);
    InvalidateRect(m_hToast, nullptr, TRUE);
    SetTimer(m_hWnd, IDT_TOAST, 1800, nullptr);
}

// ------------------------------------------------------------------ commands

void MainWindow::OnCommand(WORD id) {
    Tab* tab = ActiveTab();
    m_lastInteractionTick = GetTickCount64();
    switch (id) {
    case IDM_NEW_TAB:
        if (m_overviewVisible) HideOverview();
        NewTab(NewTabUrl(), true);
        BeginAddressEdit(L"", false);
        return;
    case IDM_NEW_WINDOW: AppShell::Instance().OpenWindow(false); return;
    case IDM_NEW_PRIVATE_WINDOW: AppShell::Instance().OpenWindow(true); return;
    case IDM_CLOSE_TAB: if (tab) CloseTab(tab->id); return;
    case IDM_CLOSE_WINDOW: PostMessageW(m_hWnd, WM_CLOSE, 0, 0); return;
    case IDM_REOPEN_TAB: ReopenClosedTab(); return;
    case IDM_NEXT_TAB:
    case IDM_PREV_TAB: {
        if (m_tabs.size() < 2) return;
        const int n = static_cast<int>(m_tabs.size());
        const int idx = (IndexOf(m_activeId) + (id == IDM_NEXT_TAB ? 1 : n - 1)) % n;
        ActivateTab(m_tabs[static_cast<size_t>(idx)]->id);
        return;
    }
    case IDM_BACK: if (tab && tab->view) tab->view->GoBack(); return;
    case IDM_FORWARD: if (tab && tab->view) tab->view->GoForward(); return;
    case IDM_RELOAD: if (tab && tab->view) tab->view->Reload(); return;
    case IDM_STOP: if (tab && tab->view) tab->view->Stop(); return;
    case IDM_HOME: NavigateActive(Config::Instance().GetSettings().startUrl); return;
    case IDM_FOCUS_ADDRESS_BAR:
        if (tab) BeginAddressEdit(DisplayUrl(*tab), true);
        return;
    case IDM_TOGGLE_FULLSCREEN: ToggleFullScreen(); return;
    case IDM_EXIT_FULLSCREEN:
        if (m_overviewVisible) HideOverview();
        if (m_isFullScreen) SetFullScreen(false);
        return;
    case IDM_ZOOM_IN: if (tab && tab->view) tab->view->ZoomIn(); return;
    case IDM_ZOOM_OUT: if (tab && tab->view) tab->view->ZoomOut(); return;
    case IDM_ZOOM_RESET: if (tab && tab->view) tab->view->ZoomReset(); return;
    case IDM_BOOKMARKS: OpenInternalPage(L"bookmarks.html", true); return;
    case IDM_HISTORY: OpenInternalPage(L"history.html", true); return;
    case IDM_SETTINGS: OpenInternalPage(L"settings.html", true); return;
    case IDM_READING_LIST: ToggleSidebar(L"reading"); return;
    case IDM_SIDEBAR: ToggleSidebar(); return;
    case IDM_OVERVIEW: ToggleOverview(); return;
    case IDM_DOWNLOADS:
        if (tab && tab->view && tab->view->GetWebView()) {
            wil::com_ptr<ICoreWebView2_9> wv9;
            if (SUCCEEDED(tab->view->GetWebView()->QueryInterface(IID_PPV_ARGS(&wv9))) && wv9) {
                BOOL open = FALSE;
                wv9->get_IsDefaultDownloadDialogOpen(&open);
                if (open) wv9->CloseDefaultDownloadDialog();
                else wv9->OpenDefaultDownloadDialog();
            }
        }
        return;
    case IDM_PRINT:
        if (tab && tab->view && tab->view->GetWebView()) {
            wil::com_ptr<ICoreWebView2_16> wv16;
            if (SUCCEEDED(tab->view->GetWebView()->QueryInterface(IID_PPV_ARGS(&wv16))) && wv16) {
                wv16->ShowPrintUI(COREWEBVIEW2_PRINT_DIALOG_KIND_BROWSER);
            }
        }
        return;
    case IDM_READER: ToggleReader(); return;
    case IDM_FIND: ShowFindBar(); return;
    case IDM_FIND_NEXT:
        if (!m_findVisible) ShowFindBar();
        else RunFind(L"next");
        return;
    case IDM_FIND_PREV:
        if (!m_findVisible) ShowFindBar();
        else RunFind(L"prev");
        return;
    case IDM_FIND_CLOSE: HideFindBar(); return;
    case IDM_ADD_BOOKMARK: AddBookmark(kBookmarksFolder); return;
    case IDM_ADD_FAVORITE: AddBookmark(kFavoritesFolder); return;
    case IDM_ADD_READING: AddToReadingList(); return;
    case IDM_MENU: ShowMainMenu(); return;
    case IDM_SHARE: ShowShareMenu(); return;
    case IDM_SHARE_COPY_URL:
        if (tab && !DisplayUrl(*tab).empty()) {
            CopyTextToClipboard(m_hWnd, DisplayUrl(*tab));
            ShowToast(L"已拷贝链接");
        }
        return;
    case IDM_SHARE_OPEN_DEFAULT:
        if (tab && !DisplayUrl(*tab).empty()) ShellExecuteW(m_hWnd, L"open", DisplayUrl(*tab).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    default:
        break;
    }
    if (id > IDM_SELECT_TAB_BASE && id <= IDM_SELECT_TAB_BASE + 9) {
        const int n = id - IDM_SELECT_TAB_BASE;
        if (m_tabs.empty()) return;
        // Ctrl+9 always means the last tab.
        const size_t idx = n == 9 ? m_tabs.size() - 1 : static_cast<size_t>(n - 1);
        if (idx < m_tabs.size()) ActivateTab(m_tabs[idx]->id);
        return;
    }
    HandleMenuCommand(id);
}

void MainWindow::OnClose() {
    if (m_closing) return;
    m_closing = true;
    KillTimer(m_hWnd, IDT_TAB_SUSPEND);
    KillTimer(m_hWnd, IDT_LIBRARY_SAVE);
    if (AppShell::Instance().IsLastWindow(this)) {
        AppShell::Instance().PrepareExit(this);
    }
    ShowWindow(m_hWnd, SW_HIDE);
    if (m_hSuggest) ShowWindow(m_hSuggest, SW_HIDE);
    if (m_hToast) ShowWindow(m_hToast, SW_HIDE);
    for (auto& t : m_tabs) {
        if (t->view) AppShell::Instance().Retire(std::move(t->view));
    }
    m_tabs.clear();
    if (m_spare && m_spare->view) AppShell::Instance().Retire(std::move(m_spare->view));
    m_spare.reset();
    if (m_sidebar.controller) { m_sidebar.controller->Close(); m_sidebar = {}; }
    if (m_overview.controller) { m_overview.controller->Close(); m_overview = {}; }
    DestroyWindow(m_hWnd);
}

// ------------------------------------------------------------------ window procedure

LRESULT CALLBACK MainWindow::WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    MainWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = reinterpret_cast<MainWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->m_hWnd = hWnd;
    } else {
        self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    }
    if (self) return self->HandleMessage(msg, wParam, lParam);
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

LRESULT MainWindow::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_NCCALCSIZE:
        if (wParam == TRUE) {
            if (m_isFullScreen || IsZoomed(m_hWnd)) {
                auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
                HMONITOR hMon = MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTONEAREST);
                MONITORINFO mi{sizeof(MONITORINFO)};
                if (GetMonitorInfoW(hMon, &mi)) {
                    params->rgrc[0] = CalculateClientBounds(params->rgrc[0], mi.rcMonitor, mi.rcWork,
                                                            m_isFullScreen, IsZoomed(m_hWnd) != FALSE);
                }
            }
            return 0;  // Remove standard Windows caption and frame
        }
        break;

    case WM_NCHITTEST: {
        POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        RECT rcWin{};
        GetWindowRect(m_hWnd, &rcWin);
        if (!IsZoomed(m_hWnd) && !m_isFullScreen) {
            const int b = S(6);
            const bool left = pt.x >= rcWin.left && pt.x < rcWin.left + b;
            const bool right = pt.x < rcWin.right && pt.x >= rcWin.right - b;
            const bool top = pt.y >= rcWin.top && pt.y < rcWin.top + b;
            const bool bottom = pt.y < rcWin.bottom && pt.y >= rcWin.bottom - b;
            if (top && left) return HTTOPLEFT;
            if (top && right) return HTTOPRIGHT;
            if (bottom && left) return HTBOTTOMLEFT;
            if (bottom && right) return HTBOTTOMRIGHT;
            if (left) return HTLEFT;
            if (right) return HTRIGHT;
            if (top) return HTTOP;
            if (bottom) return HTBOTTOM;
        }
        POINT client = pt;
        ScreenToClient(m_hWnd, &client);
        if (!m_isFullScreen && client.y >= 0 && client.y < m_topbarHeight) {
            if (HitTest(client).kind != Hit::None) return HTCLIENT;
            if (m_isAddressFocused && PtInRect(&m_rcActive, client)) return HTCLIENT;
            return HTCAPTION;  // drag the window from empty toolbar space
        }
        return HTCLIENT;
    }

    // The title bar is custom drawn, but DefWindowProc still hot-tracks the invisible
    // system caption buttons at the top-right corner and shows their tooltips
    // ("向上还原", "向下还原", ...), which then stick. Non-client mouse moves carry
    // no behaviour we need (dragging uses WM_NCLBUTTONDOWN), so swallow them.
    case WM_NCMOUSEMOVE:
    case WM_NCMOUSEHOVER:
    case WM_NCMOUSELEAVE:
        if (m_hover.kind != Hit::None || m_isTrafficGroupHovered) {
            m_hover = {};
            m_isTrafficGroupHovered = false;
            m_trafficHoveredBtn = 0;
            InvalidateToolbar();
        }
        return 0;

    case WM_CREATE: {
        UpdateDpiScaling(GetDpiForWindow(m_hWnd));
        m_hEditAddress = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL | ES_LEFT,
                                         0, 0, 0, 0, m_hWnd, reinterpret_cast<HMENU>(IDC_EDIT_ADDRESS), m_hInstance, nullptr);
        SendMessageW(m_hEditAddress, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"搜索或输入网站名称"));
        SendMessageW(m_hEditAddress, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(2, 2));
        SetWindowSubclass(m_hEditAddress, AddressBarSubclassProc, 1, reinterpret_cast<DWORD_PTR>(this));
        m_hFindEdit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL | ES_LEFT,
                                      0, 0, 0, 0, m_hWnd, reinterpret_cast<HMENU>(IDC_EDIT_FIND), m_hInstance, nullptr);
        SendMessageW(m_hFindEdit, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"在页面中查找"));
        SetWindowSubclass(m_hFindEdit, FindEditSubclassProc, 1, reinterpret_cast<DWORD_PTR>(this));
        UpdateDpiScaling(m_dpi);

        m_lastInteractionTick = GetTickCount64();
        SetTimer(m_hWnd, IDT_TAB_SUSPEND, 30000, nullptr);
        SetTimer(m_hWnd, IDT_LIBRARY_SAVE, 30000, nullptr);
        m_sidebarVisible = Config::Instance().GetSettings().sidebarVisible;

        // Initial tabs
        if (m_initialUrls.empty()) {
            NewTab(m_private ? InternalPages::Url(L"start.html") : NewTabUrl(), true);
        } else {
            bool first = true;
            for (const auto& url : m_initialUrls) {
                NewTab(url, first);
                first = false;
            }
        }
        if (m_sidebarVisible) {
            m_sidebarVisible = false;
            ToggleSidebar();
        }
        UpdateWindowTitle();
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(m_hWnd, &ps);
        RECT client{};
        GetClientRect(m_hWnd, &client);
        const int w = client.right;
        const int h = m_isFullScreen ? 0 : m_topbarHeight + (m_findVisible ? S(40) : 0);
        if (w > 0 && h > 0) {
            // Reuse one back buffer instead of allocating a bitmap on every paint.
            if (!m_paintBitmap || m_paintSize.cx < w || m_paintSize.cy < h) {
                if (m_paintBitmap) DeleteObject(m_paintBitmap);
                m_paintSize = {(std::max)(static_cast<LONG>(w), m_paintSize.cx), (std::max)(static_cast<LONG>(h), m_paintSize.cy)};
                m_paintBitmap = CreateCompatibleBitmap(hdc, m_paintSize.cx, m_paintSize.cy);
            }
            HDC mem = CreateCompatibleDC(hdc);
            HGDIOBJ old = SelectObject(mem, m_paintBitmap);
            EnsurePaintFonts(mem);
            RECT all{0, 0, w, h};
            HBRUSH bg = CreateSolidBrush(RGB(30, 32, 37));
            FillRect(mem, &all, bg);
            DeleteObject(bg);
            PaintToolbar(mem, w);
            PaintFindBar(mem);
            BitBlt(hdc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left,
                   (std::min)(static_cast<LONG>(h), ps.rcPaint.bottom) - ps.rcPaint.top, mem, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
            SelectObject(mem, old);
            DeleteDC(mem);
        }
        // Area under not-yet-ready web views
        RECT rest{0, h, client.right, client.bottom};
        if (rest.bottom > rest.top) {
            HBRUSH bg = CreateSolidBrush(RGB(30, 32, 37));
            FillRect(hdc, &rest, bg);
            DeleteObject(bg);
        }
        EndPaint(m_hWnd, &ps);
        return 0;
    }

    case WM_MOUSEMOVE:
        OnToolbarMouseMove({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
        return 0;

    case WM_MOUSELEAVE:
        m_trackingMouse = false;
        if (m_hover.kind != Hit::None || m_isTrafficGroupHovered) {
            m_hover = {};
            m_isTrafficGroupHovered = false;
            m_trafficHoveredBtn = 0;
            InvalidateToolbar();
            if (m_findVisible) InvalidateRect(m_hWnd, &m_rcFindBar, FALSE);
        }
        return 0;

    case WM_LBUTTONDOWN:
        OnToolbarButtonDown({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
        return 0;

    case WM_LBUTTONUP:
        OnToolbarButtonUp({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
        return 0;

    case WM_LBUTTONDBLCLK:
        return 0;

    case WM_MBUTTONUP:
        OnToolbarMiddleUp({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
        return 0;

    case WM_RBUTTONUP:
        OnToolbarRightUp({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
        return 0;

    case WM_CTLCOLOREDIT: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetTextColor(hdc, RGB(242, 242, 246));
        SetBkColor(hdc, RGB(30, 32, 37));
        return reinterpret_cast<LRESULT>(m_hBrAddressBg);
    }

    case WM_ACTIVATE: {
        m_isWindowActive = LOWORD(wParam) != WA_INACTIVE;
        InvalidateToolbar();
        if (m_isWindowActive) {
            KillTimer(m_hWnd, IDT_INACTIVITY_CHECK);
            m_lastInteractionTick = GetTickCount64();
            Tab* t = ActiveTab();
            if (m_backgrounded && t && t->view && t->view->GetWebView()) {
                m_backgrounded = false;
                PowerManager::Instance().HandleActivityResume(t->view->GetController(), t->view->GetWebView());
            }
        } else {
            HideSuggestions();
            // Start a single 5-minute timer to suspend if the user stays away
            SetTimer(m_hWnd, IDT_INACTIVITY_CHECK, 300000, nullptr);
        }
        break;
    }

    case WM_TIMER: {
        const UINT_PTR timer = wParam;
        Tab* active = ActiveTab();
        if (timer == IDT_INACTIVITY_CHECK) {
            KillTimer(m_hWnd, IDT_INACTIVITY_CHECK);
            // Only a minimized window may be hidden and suspended; an unfocused but visible
            // window (e.g. on a second monitor) must keep rendering.
            if (IsIconic(m_hWnd) && active && active->view && active->view->GetWebView()) {
                m_backgrounded = true;
                PowerManager::Instance().HandleInactivitySuspend(active->view->GetController(), active->view->GetWebView(),
                                                                 active->view->IsDocumentPlayingAudio());
            }
            return 0;
        }
        if (timer == IDT_AUDIO_STOP_GRACE) {
            KillTimer(m_hWnd, IDT_AUDIO_STOP_GRACE);
            if (active && active->view && active->view->GetWebView() && m_backgrounded && !active->view->IsDocumentPlayingAudio()) {
                PowerManager::Instance().HandleWindowMinimize(active->view->GetController(), active->view->GetWebView(), false);
            }
            return 0;
        }
        if (timer == IDT_TAB_SUSPEND) {
            SuspendBackgroundTabs(false);
            return 0;
        }
        if (timer == IDT_LIBRARY_SAVE) {
            AppShell::Instance().SaveLibrarySoon();
            return 0;
        }
        if (timer == IDT_PROGRESS) {
            if (!active || !active->loading) KillTimer(m_hWnd, IDT_PROGRESS);
            if (m_rcActive.right > m_rcActive.left) InvalidateRect(m_hWnd, &m_rcActive, FALSE);
            return 0;
        }
        if (timer == IDT_FIND_DEBOUNCE) {
            KillTimer(m_hWnd, IDT_FIND_DEBOUNCE);
            RunFind(L"search");
            return 0;
        }
        if (timer == IDT_TOAST) {
            KillTimer(m_hWnd, IDT_TOAST);
            if (m_hToast) ShowWindow(m_hToast, SW_HIDE);
            return 0;
        }
        if (timer == IDT_PANELS) {
            KillTimer(m_hWnd, IDT_PANELS);
            FlushPanels();
            return 0;
        }
        if (timer == IDT_SPARE) {
            KillTimer(m_hWnd, IDT_SPARE);
            // Building the pre-rendered new tab waits until the visible page is loaded.
            if (active && active->loading) SetTimer(m_hWnd, IDT_SPARE, 1000, nullptr);
            else PrepareSpareTab();
            return 0;
        }
        if (timer == IDT_IDLE_WORK) {
            RunIdleWork();
            return 0;
        }
        if (timer == IDT_PREFETCH) {
            UpdateAddressPrefetch();
            return 0;
        }
        if (timer >= 6000 && timer < 6000 + 100000) {
            KillTimer(m_hWnd, timer);
            const int tabId = static_cast<int>(timer - 6000);
            Tab* t = FindTab(tabId);
            if (t && tabId == m_activeId && t->readerSource.empty() && t->readerAvailable) ToggleReader();
            return 0;
        }
        break;
    }

    case WM_SIZE: {
        Tab* active = ActiveTab();
        if (wParam == SIZE_MINIMIZED) {
            HideSuggestions();
            if (active && active->view && active->view->GetWebView()) {
                if (AppShell::Instance().VisibleWindowCount() == 0) {
                    m_backgrounded = true;
                    PowerManager::Instance().HandleWindowMinimize(active->view->GetController(), active->view->GetWebView(),
                                                                  active->view->IsDocumentPlayingAudio());
                } else if (!active->audio) {
                    // Other windows stay in the foreground: suspend just this tab, keep EcoQoS off.
                    active->view->SetVisible(false);
                }
            }
            return 0;
        }
        if ((wParam == SIZE_RESTORED || wParam == SIZE_MAXIMIZED) && active && active->view && active->view->GetWebView()) {
            if (m_backgrounded) {
                m_backgrounded = false;
                PowerManager::Instance().HandleWindowRestore(active->view->GetController(), active->view->GetWebView());
            }
            if (!m_overviewVisible) active->view->SetVisible(true);
        }
        UpdateLayout();
        return 0;
    }

    case WM_MOVE:
        for (auto& t : m_tabs) {
            if (t->view) t->view->NotifyParentWindowPositionChanged();
        }
        if (m_sidebar.controller) m_sidebar.controller->NotifyParentWindowPositionChanged();
        if (m_overview.controller) m_overview.controller->NotifyParentWindowPositionChanged();
        HideSuggestions();
        break;

    case WM_DROPFILES: {
        HDROP hDrop = reinterpret_cast<HDROP>(wParam);
        wchar_t filePath[MAX_PATH]{};
        if (DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0) > 0 && DragQueryFileW(hDrop, 0, filePath, MAX_PATH)) {
            NavigateActive(filePath);
        }
        DragFinish(hDrop);
        return 0;
    }

    case WM_COMMAND: {
        const WORD id = LOWORD(wParam);
        const WORD notify = HIWORD(wParam);
        if (id == IDC_EDIT_ADDRESS) {
            if (notify == EN_CHANGE) {
                UpdateSuggestions();
                SetTimer(m_hWnd, IDT_PREFETCH, 250, nullptr);
            }
            return 0;
        }
        if (id == IDC_EDIT_FIND) {
            if (notify == EN_CHANGE) SetTimer(m_hWnd, IDT_FIND_DEBOUNCE, 160, nullptr);
            return 0;
        }
        OnCommand(id);
        return 0;
    }

    case WM_SYSCOMMAND:
        if ((wParam & 0xFFF0) == SC_RESTORE) {
            if (Tab* t = ActiveTab(); t && t->view && t->view->GetWebView() && m_backgrounded) {
                m_backgrounded = false;
                PowerManager::Instance().HandleWindowRestore(t->view->GetController(), t->view->GetWebView());
            }
        }
        break;

    case WM_DPICHANGED: {
        auto* rc = reinterpret_cast<RECT*>(lParam);
        UpdateDpiScaling(HIWORD(wParam));
        SetWindowPos(m_hWnd, nullptr, rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top, SWP_NOZORDER | SWP_NOACTIVATE);
        UpdateLayout();
        return 0;
    }

    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        mmi->ptMinTrackSize = {S(560), S(360)};
        return 0;
    }

    case WM_APP_THUMBNAIL: {
        std::unique_ptr<ThumbnailMessage> r(reinterpret_cast<ThumbnailMessage*>(lParam));
        if (r && !m_closing) {
            OnThumbnailReady(r->tabId, std::move(r->dataUrl));
            if (r->done) r->done();
        }
        return 0;
    }

    case WM_CLOSE:
        OnClose();
        return 0;

    case WM_NCDESTROY: {
        m_destroyed = true;
        SetWindowLongPtrW(m_hWnd, GWLP_USERDATA, 0);
        const LRESULT r = DefWindowProcW(m_hWnd, msg, wParam, lParam);
        AppShell::Instance().OnWindowDestroyed(this);
        return r;
    }

    default:
        break;
    }
    return DefWindowProcW(m_hWnd, msg, wParam, lParam);
}

} // namespace UltraLight
