#include "MainWindow.hpp"
#include "Config.hpp"
#include "ElementBlocker.hpp"
#include "ExtensionManager.hpp"
#include "PowerManager.hpp"
#include <windowsx.h>
#include <uxtheme.h>
#include <shellapi.h>

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif

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
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_LEFT,
        0, 0, 0, 0, m_hWnd, reinterpret_cast<HMENU>(IDC_EDIT_ADDRESS), m_hInstance, nullptr
    );
    SendMessageW(m_hEditAddress, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(6, 6));
    SendMessageW(m_hEditAddress, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"输入网址或搜索内容，按 Enter 访问"));

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

            if (_wcsicmp(input.c_str(), L"chrome://extensions") == 0 ||
                _wcsicmp(input.c_str(), L"edge://extensions") == 0 ||
                _wcsicmp(input.c_str(), L"about:extensions") == 0) {
                if (self) {
                    ExtensionManager::Instance().ShowExtensionsDialog(self->GetHwnd());
                }
                return 0;
            }

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

    case WM_DROPFILES: {
        HDROP hDrop = reinterpret_cast<HDROP>(wParam);
        UINT fileCount = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < fileCount; ++i) {
            wchar_t filePath[MAX_PATH]{};
            if (DragQueryFileW(hDrop, i, filePath, MAX_PATH)) {
                std::filesystem::path p(filePath);
                if (std::filesystem::is_directory(p)) {
                    if (std::filesystem::exists(p / "manifest.json")) {
                        std::wstring msgText = L"检测到未打包的 Chrome 扩展程序：\n" + p.wstring() + L"\n\n是否立即加载？";
                        if (MessageBoxW(m_hWnd, msgText.c_str(), L"加载未打包的扩展程序", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                            ExtensionManager::Instance().LoadUnpackedExtension(p, [this](bool success, const std::wstring& resMsg) {
                                MessageBoxW(m_hWnd, resMsg.c_str(), success ? L"加载成功" : L"加载失败", MB_OK | (success ? MB_ICONINFORMATION : MB_ICONERROR));
                            });
                        }
                    } else {
                        MessageBoxW(m_hWnd, L"所拖入的文件夹不包含 manifest.json 文件！\n请拖入包含 manifest.json 的扩展程序根目录。", L"缺少清单文件", MB_OK | MB_ICONWARNING);
                    }
                } else if (p.extension() == L".crx") {
                    std::wstring msgText = L"检测到 Chrome 扩展程序安装包：\n" + p.wstring() + L"\n\n是否立即安装？";
                    if (MessageBoxW(m_hWnd, msgText.c_str(), L"安装 CRX 扩展程序", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                        ExtensionManager::Instance().InstallCrx(p, [this](bool success, const std::wstring& resMsg) {
                            MessageBoxW(m_hWnd, resMsg.c_str(), success ? L"安装成功" : L"安装失败", MB_OK | (success ? MB_ICONINFORMATION : MB_ICONERROR));
                        });
                    }
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
            ElementBlocker::Instance().TogglePickerMode(m_webViewManager->GetWebView());
            break;
        case IDC_BTN_EXTENSIONS: {
            ShowExtensionsMenu();
            break;
        }
        case IDM_EXT_LOAD_UNPACKED:
            ExtensionManager::Instance().PromptLoadUnpackedExtension(m_hWnd);
            break;
        case IDM_EXT_INSTALL_CRX:
            ExtensionManager::Instance().PromptInstallCrx(m_hWnd);
            break;
        case IDM_EXT_OPEN_DIR:
            ExtensionManager::Instance().OpenExtensionsDirectory();
            break;
        case IDM_EXT_MANAGE:
            ExtensionManager::Instance().ShowExtensionsDialog(m_hWnd);
            break;
        default:
            if (id >= IDM_EXT_ITEM_BASE) {
                HandleExtensionMenuCommand(id);
            }
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

void MainWindow::ShowExtensionsMenu() {
    HMENU hMenu = CreatePopupMenu();

    AppendMenuW(hMenu, MF_STRING, IDM_EXT_LOAD_UNPACKED, L"📂  加载未打包的扩展程序...");
    AppendMenuW(hMenu, MF_STRING, IDM_EXT_INSTALL_CRX, L"📦  安装 .CRX 扩展程序...");
    AppendMenuW(hMenu, MF_STRING, IDM_EXT_OPEN_DIR, L"📁  打开扩展程序根目录");
    AppendMenuW(hMenu, MF_STRING, IDM_EXT_MANAGE, L"⚙  扩展程序管理中心...");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

    const auto& exts = ExtensionManager::Instance().GetExtensions();
    if (exts.empty()) {
        AppendMenuW(hMenu, MF_STRING | MF_GRAYED, 0, L"    (暂无已安装的扩展程序)");
    } else {
        for (size_t i = 0; i < exts.size(); ++i) {
            const auto& ext = exts[i];
            HMENU hSub = CreatePopupMenu();
            UINT_PTR baseCmd = IDM_EXT_ITEM_BASE + (i * 10);

            std::wstring statusStr = ext.isEnabled ? L"状态: [已启用] - 点击禁用" : L"状态: [已禁用] - 点击启用";
            AppendMenuW(hSub, MF_STRING, baseCmd + 1, statusStr.c_str());

            if (!ext.defaultPopup.empty()) {
                AppendMenuW(hSub, MF_STRING, baseCmd + 2, L"打开扩展弹窗界面");
                AppendMenuW(hSub, MF_STRING, baseCmd + 5, L"在主标签页中打开扩展页面");
            }

            AppendMenuW(hSub, MF_STRING, baseCmd + 3, L"⟳ 重新加载扩展");
            AppendMenuW(hSub, MF_STRING, baseCmd + 4, L"🗑 移除此扩展程序");

            std::wstring nameW = StringUtils::Utf8ToWide(ext.name);
            std::wstring itemTitle = (ext.isEnabled ? L"🧩  " : L"⚪  ") + (nameW.empty() ? L"未命名扩展" : nameW);
            if (!ext.version.empty()) {
                std::wstring verW = StringUtils::Utf8ToWide(ext.version);
                itemTitle += L" (v" + verW + L")";
            }

            AppendMenuW(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hSub), itemTitle.c_str());
        }
    }

    RECT rect;
    GetWindowRect(m_hBtnExtensions, &rect);
    TrackPopupMenu(hMenu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON, rect.left, rect.bottom, 0, m_hWnd, nullptr);
    DestroyMenu(hMenu);
}

void MainWindow::HandleExtensionMenuCommand(WORD id) {
    size_t extIndex = (id - IDM_EXT_ITEM_BASE) / 10;
    WORD action = (id - IDM_EXT_ITEM_BASE) % 10;

    const auto& exts = ExtensionManager::Instance().GetExtensions();
    if (extIndex >= exts.size()) return;

    const auto& ext = exts[extIndex];
    switch (action) {
    case 1: // Toggle enabled
        ExtensionManager::Instance().SetExtensionEnabled(ext.id, !ext.isEnabled);
        break;
    case 2: { // Open popup
        RECT rect;
        GetWindowRect(m_hBtnExtensions, &rect);
        POINT pt{ rect.left, rect.bottom };
        ExtensionManager::Instance().ShowExtensionPopup(m_hWnd, ext, pt);
        break;
    }
    case 3: // Reload
        ExtensionManager::Instance().ReloadExtension(ext.id, [this](bool ok) {
            MessageBoxW(m_hWnd, ok ? L"扩展程序已成功重新加载！" : L"重新加载扩展失败！", L"重新加载", MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONERROR));
        });
        break;
    case 4: { // Remove
        std::wstring nameW = StringUtils::Utf8ToWide(ext.name);
        std::wstring prompt = L"确定要移除扩展程序 [" + (nameW.empty() ? L"未命名扩展" : nameW) + L"] 吗？";
        if (MessageBoxW(m_hWnd, prompt.c_str(), L"移除扩展程序", MB_YESNO | MB_ICONQUESTION) == IDYES) {
            ExtensionManager::Instance().RemoveExtension(ext.id);
        }
        break;
    }
    case 5: { // Open in main tab
        if (!ext.defaultPopup.empty() && m_webViewManager) {
            std::wstring idW = StringUtils::Utf8ToWide(ext.id);
            std::wstring popupW = StringUtils::Utf8ToWide(ext.defaultPopup);
            std::wstring extUrl = L"chrome-extension://" + idW + L"/" + popupW;
            m_webViewManager->Navigate(extUrl);
        }
        break;
    }
    default:
        break;
    }
}

} // namespace UltraLight
