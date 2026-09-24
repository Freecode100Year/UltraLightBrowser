#include "MainWindow.hpp"
#include "Config.hpp"
#include "ElementBlocker.hpp"
#include "ExtensionManager.hpp"
#include "PowerManager.hpp"
#include <windowsx.h>
#include <uxtheme.h>

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif

enum ControlID : WORD {
    IDC_BTN_BACK = 1001,
    IDC_BTN_FORWARD = 1002,
    IDC_BTN_RELOAD = 1003,
    IDC_EDIT_ADDRESS = 1004,
    IDC_BTN_BLOCKER = 1005,
    IDC_BTN_EXTENSIONS = 1006
};

namespace UltraLight {

MainWindow::MainWindow() : m_webViewManager(std::make_unique<WebViewManager>()) {}

MainWindow::~MainWindow() {
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

    ApplyModernTheme();
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
    if (m_hBtnBlocker) SendMessageW(m_hBtnBlocker, WM_SETFONT, reinterpret_cast<WPARAM>(m_hUiFont), TRUE);
    if (m_hBtnExtensions) SendMessageW(m_hBtnExtensions, WM_SETFONT, reinterpret_cast<WPARAM>(m_hUiFont), TRUE);
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
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_LEFT,
        0, 0, 0, 0, m_hWnd, reinterpret_cast<HMENU>(IDC_EDIT_ADDRESS), m_hInstance, nullptr
    );

    m_hBtnBlocker = CreateWindowExW(
        0, L"BUTTON", L"🛡 Blocker",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_FLAT,
        0, 0, 0, 0, m_hWnd, reinterpret_cast<HMENU>(IDC_BTN_BLOCKER), m_hInstance, nullptr
    );

    m_hBtnExtensions = CreateWindowExW(
        0, L"BUTTON", L"🧩 Extensions",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_FLAT,
        0, 0, 0, 0, m_hWnd, reinterpret_cast<HMENU>(IDC_BTN_EXTENSIONS), m_hInstance, nullptr
    );

    // Subclass address bar for Enter key navigation
    SetWindowSubclass(m_hEditAddress, AddressBarSubclassProc, 1, reinterpret_cast<DWORD_PTR>(this));

    UpdateDpiScaling(GetDpiForWindow(m_hWnd));
}

void MainWindow::UpdateLayout(int width, int height) {
    if (width <= 0 || height <= 0) return;

    int pad = MulDiv(6, m_dpi, 96);
    int btnW = MulDiv(34, m_dpi, 96);
    int extBtnW = MulDiv(105, m_dpi, 96);
    int blockBtnW = MulDiv(90, m_dpi, 96);
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

    // Right-aligned buttons
    int rightX = width - pad - extBtnW;
    SetWindowPos(m_hBtnExtensions, nullptr, rightX, pad, extBtnW, ctrlH, SWP_NOZORDER);

    rightX -= (blockBtnW + pad);
    SetWindowPos(m_hBtnBlocker, nullptr, rightX, pad, blockBtnW, ctrlH, SWP_NOZORDER);

    // Address Bar fill
    int addrW = (rightX - pad) - x;
    if (addrW > 50) {
        SetWindowPos(m_hEditAddress, nullptr, x, pad + 1, addrW, ctrlH - 2, SWP_NOZORDER);
    }

    // Resize WebView2
    RECT webViewRect{ 0, topH, width, height };
    m_webViewManager->Resize(webViewRect);
}

LRESULT CALLBACK MainWindow::AddressBarSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR /*uIdSubclass*/, DWORD_PTR dwRefData) {
    auto* self = reinterpret_cast<MainWindow*>(dwRefData);

    if (uMsg == WM_KEYDOWN && wParam == VK_RETURN) {
        wchar_t buffer[2048]{};
        GetWindowTextW(hWnd, buffer, static_cast<int>(std::size(buffer)));
        if (self && self->m_webViewManager) {
            self->m_webViewManager->Navigate(buffer);
        }
        return 0;
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
            SetWindowTextW(m_hEditAddress, uri.c_str());
        });

        // Initialize WebView2
        m_webViewManager->Initialize(m_hWnd, [this]() {
            m_webViewManager->Navigate(Config::Instance().GetSettings().startUrl);
        });

        return 0;
    }

    case WM_SIZE: {
        int w = LOWORD(lParam);
        int h = HIWORD(lParam);
        UpdateLayout(w, h);
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
        case IDC_EDIT_ADDRESS:
            SetFocus(m_hEditAddress);
            SendMessageW(m_hEditAddress, EM_SETSEL, 0, -1);
            break;
        case IDC_BTN_BLOCKER:
            ElementBlocker::Instance().TogglePickerMode(m_webViewManager->GetWebView());
            break;
        case IDC_BTN_EXTENSIONS: {
            RECT rect;
            GetWindowRect(m_hBtnExtensions, &rect);
            const auto& exts = ExtensionManager::Instance().GetExtensions();
            if (!exts.empty()) {
                POINT pt{ rect.left, rect.bottom };
                ExtensionManager::Instance().ShowExtensionPopup(m_hWnd, exts.front(), pt);
            } else {
                MessageBoxW(m_hWnd, L"Drop unpacked extensions or .crx files in %LOCALAPPDATA%\\UltraLightBrowser\\Extensions\\", L"Extensions", MB_OK | MB_ICONINFORMATION);
            }
            break;
        }
        default:
            break;
        }
        return 0;
    }

    case WM_SYSCOMMAND: {
        if ((wParam & 0xFFF0) == SC_MINIMIZE) {
            PowerManager::Instance().HandleWindowMinimize(m_webViewManager->GetWebView());
        } else if ((wParam & 0xFFF0) == SC_RESTORE) {
            PowerManager::Instance().HandleWindowRestore(m_webViewManager->GetWebView());
        }
        break;
    }

    case WM_ACTIVATE: {
        if (LOWORD(wParam) != WA_INACTIVE) {
            PowerManager::Instance().HandleWindowRestore(m_webViewManager->GetWebView());
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

} // namespace UltraLight
