#include "ExtensionManager.hpp"
#include "StringUtils.hpp"
#include "Config.hpp"
#include <shobjidl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <unordered_set>
#include <iostream>

#if __has_include(<nlohmann/json.hpp>)
#include <nlohmann/json.hpp>
using json = nlohmann::json;
#endif

using namespace Microsoft::WRL;

namespace UltraLight {

namespace {

enum ExtDlgCtrlID : int {
    IDC_EXTDLG_LIST            = 3001,
    IDC_EXTDLG_BTN_LOAD        = 3002,
    IDC_EXTDLG_BTN_REFRESH     = 3003,
    IDC_EXTDLG_BTN_TOGGLE      = 3004,
    IDC_EXTDLG_BTN_REMOVE      = 3005,
    IDC_EXTDLG_BTN_OPTIONS     = 3006,
    IDC_EXTDLG_BTN_OPEN_DIR    = 3007,
    IDC_EXTDLG_BTN_COPY_ID     = 3008,
    IDC_EXTDLG_CHK_PRESERVE    = 3009,
    IDC_EXTDLG_EDIT_ID         = 3010,
    IDC_EXTDLG_EDIT_PATH       = 3011,
    IDC_EXTDLG_STATIC_DESC     = 3012,
    IDC_EXTDLG_STATIC_NAME_VER = 3013,
    IDC_EXTDLG_BTN_CLOSE       = 3014,
    IDC_EXTDLG_STATIC_ENV      = 3015
};

struct ExtDlgContext {
    HWND hDlg = nullptr;
    HWND hList = nullptr;
    HWND hBtnLoad = nullptr;
    HWND hBtnRefresh = nullptr;
    HWND hBtnToggle = nullptr;
    HWND hBtnRemove = nullptr;
    HWND hBtnOptions = nullptr;
    HWND hBtnOpenFolder = nullptr;
    HWND hBtnCopyId = nullptr;
    HWND hChkPreserve = nullptr;
    HWND hEditId = nullptr;
    HWND hEditPath = nullptr;
    HWND hStaticDesc = nullptr;
    HWND hStaticNameVer = nullptr;
    HWND hBtnClose = nullptr;

    HFONT hFont = nullptr;
    HFONT hFontBold = nullptr;
    HFONT hFontMono = nullptr;
    HBRUSH hBrushBg = nullptr;
    HBRUSH hBrushEdit = nullptr;

    std::wstring selectedId = L"";
};

void CopyTextToClipboard(HWND hWndOwner, const std::wstring& text) {
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

void UpdateDialogControls(ExtDlgContext* ctx) {
    if (!ctx || !ctx->hList) return;

    ListView_DeleteAllItems(ctx->hList);

    auto exts = ExtensionManager::Instance().GetExtensions();
    int selectRow = -1;

    for (size_t i = 0; i < exts.size(); ++i) {
        const auto& ext = exts[i];

        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = static_cast<int>(i);
        item.iSubItem = 0;
        item.pszText = const_cast<wchar_t*>(ext.name.c_str());
        item.lParam = static_cast<LPARAM>(i);
        ListView_InsertItem(ctx->hList, &item);

        std::wstring statusStr = ext.isEnabled ? L"已启用" : L"已禁用";
        ListView_SetItemText(ctx->hList, static_cast<int>(i), 1, const_cast<wchar_t*>(statusStr.c_str()));
        ListView_SetItemText(ctx->hList, static_cast<int>(i), 2, const_cast<wchar_t*>(ext.version.c_str()));
        ListView_SetItemText(ctx->hList, static_cast<int>(i), 3, const_cast<wchar_t*>(ext.id.c_str()));
        ListView_SetItemText(ctx->hList, static_cast<int>(i), 4, const_cast<wchar_t*>(ext.folderPath.c_str()));

        if (!ctx->selectedId.empty() && ext.id == ctx->selectedId) {
            selectRow = static_cast<int>(i);
        }
    }

    if (selectRow >= 0) {
        ListView_SetItemState(ctx->hList, selectRow, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(ctx->hList, selectRow, FALSE);
    } else if (!exts.empty()) {
        ListView_SetItemState(ctx->hList, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ctx->selectedId = exts[0].id;
    } else {
        ctx->selectedId = L"";
    }

    // Refresh details panel
    const ExtensionInfo* cur = ExtensionManager::Instance().FindExtension(ctx->selectedId);
    if (cur) {
        std::wstring nameVer = L"扩展名称: " + cur->name + L" (版本: " + cur->version + L")";
        SetWindowTextW(ctx->hStaticNameVer, nameVer.c_str());
        SetWindowTextW(ctx->hEditId, cur->id.c_str());
        SetWindowTextW(ctx->hEditPath, cur->folderPath.c_str());

        std::wstring desc = cur->description.empty() ? L"暂无描述信息。" : cur->description;
        SetWindowTextW(ctx->hStaticDesc, desc.c_str());

        SetWindowTextW(ctx->hBtnToggle, cur->isEnabled ? L"⏸ 禁用扩展" : L"▶ 启用扩展");

        EnableWindow(ctx->hBtnToggle, TRUE);
        EnableWindow(ctx->hBtnRemove, TRUE);
        EnableWindow(ctx->hBtnCopyId, TRUE);
        EnableWindow(ctx->hBtnOpenFolder, !cur->folderPath.empty());
        EnableWindow(ctx->hBtnOptions, !cur->optionsPage.empty());
    } else {
        SetWindowTextW(ctx->hStaticNameVer, L"请在上方列表选择扩展程序查看详细信息");
        SetWindowTextW(ctx->hEditId, L"");
        SetWindowTextW(ctx->hEditPath, L"");
        SetWindowTextW(ctx->hStaticDesc, L"");
        SetWindowTextW(ctx->hBtnToggle, L"启用 / 禁用");

        EnableWindow(ctx->hBtnToggle, FALSE);
        EnableWindow(ctx->hBtnRemove, FALSE);
        EnableWindow(ctx->hBtnCopyId, FALSE);
        EnableWindow(ctx->hBtnOpenFolder, FALSE);
        EnableWindow(ctx->hBtnOptions, FALSE);
    }
}

LRESULT CALLBACK ExtDlgWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* ctx = reinterpret_cast<ExtDlgContext*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));

    switch (msg) {
    case WM_NCCREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        ctx = reinterpret_cast<ExtDlgContext*>(cs->lpCreateParams);
        ctx->hDlg = hWnd;
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));
        return TRUE;
    }

    case WM_CREATE: {
        BOOL darkMode = TRUE;
        DwmSetWindowAttribute(hWnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));

        ctx->hFont = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        ctx->hFontBold = CreateFontW(-13, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        ctx->hFontMono = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");

        ctx->hBrushBg = CreateSolidBrush(RGB(32, 32, 32));
        ctx->hBrushEdit = CreateSolidBrush(RGB(45, 45, 45));

        // 1. Top action buttons
        ctx->hBtnLoad = CreateWindowExW(0, L"BUTTON", L"📂 加载未打包扩展...",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            20, 16, 175, 32, hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EXTDLG_BTN_LOAD)), nullptr, nullptr);
        SendMessageW(ctx->hBtnLoad, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontBold), TRUE);

        ctx->hBtnRefresh = CreateWindowExW(0, L"BUTTON", L"🔄 刷新列表",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            205, 16, 110, 32, hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EXTDLG_BTN_REFRESH)), nullptr, nullptr);
        SendMessageW(ctx->hBtnRefresh, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFont), TRUE);

        HWND hStaticEnv = CreateWindowExW(0, L"STATIC", L"🛡️ WebView2 Chrome 扩展运行环境: [已就绪 (支持 MV2 / MV3 核心)]",
            WS_CHILD | WS_VISIBLE | SS_RIGHT,
            330, 22, 385, 20, hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EXTDLG_STATIC_ENV)), nullptr, nullptr);
        SendMessageW(hStaticEnv, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFont), TRUE);

        // 2. Main ListView
        ctx->hList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | WS_BORDER | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
            20, 58, 695, 210, hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EXTDLG_LIST)), nullptr, nullptr);
        SendMessageW(ctx->hList, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFont), TRUE);

        ListView_SetExtendedListViewStyle(ctx->hList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);

        LVCOLUMNW lvc{};
        lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;

        lvc.iSubItem = 0;
        lvc.pszText = const_cast<wchar_t*>(L"扩展程序名称");
        lvc.cx = 200;
        ListView_InsertColumn(ctx->hList, 0, &lvc);

        lvc.iSubItem = 1;
        lvc.pszText = const_cast<wchar_t*>(L"状态");
        lvc.cx = 75;
        ListView_InsertColumn(ctx->hList, 1, &lvc);

        lvc.iSubItem = 2;
        lvc.pszText = const_cast<wchar_t*>(L"版本");
        lvc.cx = 65;
        ListView_InsertColumn(ctx->hList, 2, &lvc);

        lvc.iSubItem = 3;
        lvc.pszText = const_cast<wchar_t*>(L"扩展 ID");
        lvc.cx = 160;
        ListView_InsertColumn(ctx->hList, 3, &lvc);

        lvc.iSubItem = 4;
        lvc.pszText = const_cast<wchar_t*>(L"解压目录路径");
        lvc.cx = 190;
        ListView_InsertColumn(ctx->hList, 4, &lvc);

        // 3. Details GroupBox
        HWND hGrpDetails = CreateWindowExW(0, L"BUTTON", L"扩展程序详细信息与控制",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            20, 278, 695, 185, hWnd, nullptr, nullptr, nullptr);
        SendMessageW(hGrpDetails, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontBold), TRUE);

        ctx->hStaticNameVer = CreateWindowExW(0, L"STATIC", L"请在上方列表选择扩展程序",
            WS_CHILD | WS_VISIBLE,
            35, 302, 660, 20, hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EXTDLG_STATIC_NAME_VER)), nullptr, nullptr);
        SendMessageW(ctx->hStaticNameVer, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontBold), TRUE);

        // ID row
        HWND hLblId = CreateWindowExW(0, L"STATIC", L"扩展 ID:", WS_CHILD | WS_VISIBLE,
            35, 328, 70, 20, hWnd, nullptr, nullptr, nullptr);
        SendMessageW(hLblId, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFont), TRUE);

        ctx->hEditId = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_READONLY | ES_AUTOHSCROLL,
            110, 326, 470, 22, hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EXTDLG_EDIT_ID)), nullptr, nullptr);
        SendMessageW(ctx->hEditId, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontMono), TRUE);

        ctx->hBtnCopyId = CreateWindowExW(0, L"BUTTON", L"复制 ID",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            590, 325, 110, 24, hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EXTDLG_BTN_COPY_ID)), nullptr, nullptr);
        SendMessageW(ctx->hBtnCopyId, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFont), TRUE);

        // Path row
        HWND hLblPath = CreateWindowExW(0, L"STATIC", L"解压路径:", WS_CHILD | WS_VISIBLE,
            35, 358, 70, 20, hWnd, nullptr, nullptr, nullptr);
        SendMessageW(hLblPath, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFont), TRUE);

        ctx->hEditPath = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_READONLY | ES_AUTOHSCROLL,
            110, 356, 470, 22, hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EXTDLG_EDIT_PATH)), nullptr, nullptr);
        SendMessageW(ctx->hEditPath, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontMono), TRUE);

        ctx->hBtnOpenFolder = CreateWindowExW(0, L"BUTTON", L"📂 打开目录",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            590, 355, 110, 24, hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EXTDLG_BTN_OPEN_DIR)), nullptr, nullptr);
        SendMessageW(ctx->hBtnOpenFolder, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFont), TRUE);

        // Description
        ctx->hStaticDesc = CreateWindowExW(0, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE,
            35, 386, 665, 36, hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EXTDLG_STATIC_DESC)), nullptr, nullptr);
        SendMessageW(ctx->hStaticDesc, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFont), TRUE);

        // Action buttons inside group
        ctx->hBtnToggle = CreateWindowExW(0, L"BUTTON", L"⏸ 禁用扩展",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            35, 426, 120, 28, hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EXTDLG_BTN_TOGGLE)), nullptr, nullptr);
        SendMessageW(ctx->hBtnToggle, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontBold), TRUE);

        ctx->hBtnOptions = CreateWindowExW(0, L"BUTTON", L"🌐 打开选项页",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            165, 426, 120, 28, hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EXTDLG_BTN_OPTIONS)), nullptr, nullptr);
        SendMessageW(ctx->hBtnOptions, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFont), TRUE);

        ctx->hBtnRemove = CreateWindowExW(0, L"BUTTON", L"🗑️ 移除此扩展",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            295, 426, 120, 28, hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EXTDLG_BTN_REMOVE)), nullptr, nullptr);
        SendMessageW(ctx->hBtnRemove, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFont), TRUE);

        // 4. Bottom bar
        ctx->hChkPreserve = CreateWindowExW(0, L"BUTTON", L"退出时保留扩展程序配置与解压加载项 (推荐)",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            20, 475, 400, 24, hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EXTDLG_CHK_PRESERVE)), nullptr, nullptr);
        SendMessageW(ctx->hChkPreserve, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFont), TRUE);
        SendMessageW(ctx->hChkPreserve, BM_SETCHECK, Config::Instance().GetSettings().preserveExtensionData ? BST_CHECKED : BST_UNCHECKED, 0);

        ctx->hBtnClose = CreateWindowExW(0, L"BUTTON", L"关闭",
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            615, 472, 100, 30, hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EXTDLG_BTN_CLOSE)), nullptr, nullptr);
        SendMessageW(ctx->hBtnClose, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontBold), TRUE);

        UpdateDialogControls(ctx);
        return 0;
    }

    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(240, 240, 240));
        return reinterpret_cast<INT_PTR>(ctx ? ctx->hBrushBg : GetStockObject(BLACK_BRUSH));
    }

    case WM_CTLCOLORBTN: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(240, 240, 240));
        return reinterpret_cast<INT_PTR>(ctx ? ctx->hBrushBg : GetStockObject(BLACK_BRUSH));
    }

    case WM_CTLCOLOREDIT: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetBkMode(hdc, OPAQUE);
        SetBkColor(hdc, RGB(45, 45, 45));
        SetTextColor(hdc, RGB(220, 220, 220));
        return reinterpret_cast<INT_PTR>(ctx ? ctx->hBrushEdit : GetStockObject(GRAY_BRUSH));
    }

    case WM_NOTIFY: {
        auto* pnmhdr = reinterpret_cast<NMHDR*>(lParam);
        if (pnmhdr->idFrom == IDC_EXTDLG_LIST) {
            if (pnmhdr->code == LVN_ITEMCHANGED) {
                auto* pnmv = reinterpret_cast<NMLISTVIEW*>(lParam);
                if ((pnmv->uChanged & LVIF_STATE) && (pnmv->uNewState & LVIS_SELECTED)) {
                    wchar_t idBuf[128]{};
                    ListView_GetItemText(ctx->hList, pnmv->iItem, 3, idBuf, 128);
                    ctx->selectedId = idBuf;

                    const ExtensionInfo* cur = ExtensionManager::Instance().FindExtension(ctx->selectedId);
                    if (cur) {
                        std::wstring nameVer = L"扩展名称: " + cur->name + L" (版本: " + cur->version + L")";
                        SetWindowTextW(ctx->hStaticNameVer, nameVer.c_str());
                        SetWindowTextW(ctx->hEditId, cur->id.c_str());
                        SetWindowTextW(ctx->hEditPath, cur->folderPath.c_str());

                        std::wstring desc = cur->description.empty() ? L"暂无描述信息。" : cur->description;
                        SetWindowTextW(ctx->hStaticDesc, desc.c_str());

                        SetWindowTextW(ctx->hBtnToggle, cur->isEnabled ? L"⏸ 禁用扩展" : L"▶ 启用扩展");

                        EnableWindow(ctx->hBtnToggle, TRUE);
                        EnableWindow(ctx->hBtnRemove, TRUE);
                        EnableWindow(ctx->hBtnCopyId, TRUE);
                        EnableWindow(ctx->hBtnOpenFolder, !cur->folderPath.empty());
                        EnableWindow(ctx->hBtnOptions, !cur->optionsPage.empty());
                    }
                }
            } else if (pnmhdr->code == NM_DBLCLK) {
                if (!ctx->selectedId.empty()) {
                    SendMessageW(hWnd, WM_COMMAND, MAKEWPARAM(IDC_EXTDLG_BTN_TOGGLE, 0), 0);
                }
            }
        }
        break;
    }

    case WM_COMMAND: {
        WORD id = LOWORD(wParam);
        switch (id) {
        case IDC_EXTDLG_BTN_LOAD: {
            ExtensionManager::Instance().LoadUnpackedExtension(hWnd, [ctx](bool ok, const std::wstring& msg) {
                MessageBoxW(ctx->hDlg, msg.c_str(), ok ? L"加载扩展程序" : L"加载失败", ok ? (MB_OK | MB_ICONINFORMATION) : (MB_OK | MB_ICONERROR));
                UpdateDialogControls(ctx);
            });
            break;
        }

        case IDC_EXTDLG_BTN_REFRESH: {
            ExtensionManager::Instance().RefreshExtensions([ctx](bool) {
                UpdateDialogControls(ctx);
            });
            break;
        }

        case IDC_EXTDLG_BTN_TOGGLE: {
            if (ctx->selectedId.empty()) break;
            ExtensionManager::Instance().ToggleExtension(ctx->selectedId, [ctx](bool) {
                UpdateDialogControls(ctx);
            });
            break;
        }

        case IDC_EXTDLG_BTN_REMOVE: {
            if (ctx->selectedId.empty()) break;
            int ret = MessageBoxW(hWnd, L"确定要移除选中的扩展程序吗？\n移除后该扩展将不再运行。", L"确认移除扩展", MB_YESNO | MB_ICONQUESTION);
            if (ret == IDYES) {
                ExtensionManager::Instance().RemoveExtension(ctx->selectedId, [ctx](bool) {
                    ctx->selectedId = L"";
                    UpdateDialogControls(ctx);
                });
            }
            break;
        }

        case IDC_EXTDLG_BTN_OPTIONS: {
            if (ctx->selectedId.empty()) break;
            ExtensionManager::Instance().OpenExtensionOptions(ctx->selectedId, nullptr);
            DestroyWindow(hWnd);
            break;
        }

        case IDC_EXTDLG_BTN_OPEN_DIR: {
            if (!ctx->selectedId.empty()) {
                ExtensionManager::Instance().OpenExtensionFolder(ctx->selectedId);
            }
            break;
        }

        case IDC_EXTDLG_BTN_COPY_ID: {
            if (!ctx->selectedId.empty()) {
                CopyTextToClipboard(hWnd, ctx->selectedId);
                MessageBoxW(hWnd, L"扩展 ID 已复制到剪贴板！", L"复制成功", MB_OK | MB_ICONINFORMATION);
            }
            break;
        }

        case IDC_EXTDLG_CHK_PRESERVE: {
            LRESULT checked = SendMessageW(ctx->hChkPreserve, BM_GETCHECK, 0, 0);
            auto& settings = Config::Instance().GetSettings();
            settings.preserveExtensionData = (checked == BST_CHECKED);
            Config::Instance().Save();
            break;
        }

        case IDC_EXTDLG_BTN_CLOSE:
        case IDCANCEL:
            DestroyWindow(hWnd);
            break;

        default:
            break;
        }
        return 0;
    }

    case WM_CLOSE: {
        DestroyWindow(hWnd);
        return 0;
    }

    case WM_DESTROY: {
        if (ctx) {
            if (ctx->hFont) DeleteObject(ctx->hFont);
            if (ctx->hFontBold) DeleteObject(ctx->hFontBold);
            if (ctx->hFontMono) DeleteObject(ctx->hFontMono);
            if (ctx->hBrushBg) DeleteObject(ctx->hBrushBg);
            if (ctx->hBrushEdit) DeleteObject(ctx->hBrushEdit);
        }
        return 0;
    }

    default:
        break;
    }

    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

} // namespace

ExtensionManager& ExtensionManager::Instance() {
    static ExtensionManager s_instance;
    return s_instance;
}

void ExtensionManager::Initialize(ICoreWebView2* webView, ICoreWebView2Environment* env) {
    m_webView = webView;
    m_environment = env;

    if (!m_webView) return;

    wil::com_ptr<ICoreWebView2_13> webView13;
    if (SUCCEEDED(m_webView->QueryInterface(IID_PPV_ARGS(&webView13))) && webView13) {
        wil::com_ptr<ICoreWebView2Profile> profile;
        if (SUCCEEDED(webView13->get_Profile(&profile)) && profile) {
            profile->QueryInterface(IID_PPV_ARGS(&m_profile7));
        }
    }

    if (m_profile7) {
        RestoreSavedExtensions();
    }
}

void ExtensionManager::RestoreSavedExtensions() {
    if (!m_profile7) return;

    auto savedConfigs = Config::Instance().GetInstalledExtensions();
    if (savedConfigs.empty()) {
        RefreshExtensions();
        return;
    }

    m_profile7->GetBrowserExtensions(
        Callback<ICoreWebView2ProfileGetBrowserExtensionsCompletedHandler>(
            [this, savedConfigs](HRESULT hr, ICoreWebView2BrowserExtensionList* list) -> HRESULT {
                std::unordered_set<std::string> loadedIds;
                if (SUCCEEDED(hr) && list) {
                    UINT32 count = 0;
                    list->get_Count(&count);
                    for (UINT32 i = 0; i < count; ++i) {
                        wil::com_ptr<ICoreWebView2BrowserExtension> ext;
                        if (SUCCEEDED(list->GetValueAtIndex(i, &ext)) && ext) {
                            wil::unique_cotaskmem_string idStr;
                            if (SUCCEEDED(ext->get_Id(&idStr)) && idStr.get()) {
                                loadedIds.insert(StringUtils::WideToUtf8(idStr.get()));
                            }
                        }
                    }
                }

                // Restore any missing extension from its folder
                for (const auto& cfg : savedConfigs) {
                    if (!cfg.folderPath.empty() && std::filesystem::exists(cfg.folderPath)) {
                        if (loadedIds.find(cfg.id) == loadedIds.end()) {
                            m_profile7->AddBrowserExtension(
                                cfg.folderPath.c_str(),
                                Callback<ICoreWebView2ProfileAddBrowserExtensionCompletedHandler>(
                                    [this, cfg](HRESULT addHr, ICoreWebView2BrowserExtension* ext) -> HRESULT {
                                        if (SUCCEEDED(addHr) && ext) {
                                            if (!cfg.enabled) {
                                                ext->Enable(FALSE, nullptr);
                                            }
                                        }
                                        return S_OK;
                                    }
                                ).Get()
                            );
                        }
                    }
                }

                RefreshExtensions();
                return S_OK;
            }
        ).Get()
    );
}

bool ExtensionManager::ParseManifest(const std::filesystem::path& folderPath, ExtensionInfo& outInfo, std::string& outError) {
    std::error_code ec;
    if (!std::filesystem::exists(folderPath, ec) || !std::filesystem::is_directory(folderPath, ec)) {
        outError = "指定的路径不是有效目录。";
        return false;
    }

    std::filesystem::path manifestPath = folderPath / "manifest.json";
    if (!std::filesystem::exists(manifestPath, ec) || !std::filesystem::is_regular_file(manifestPath, ec)) {
        outError = "目录中未找到 manifest.json 扩展清单文件。";
        return false;
    }

    std::ifstream file(manifestPath);
    if (!file.is_open()) {
        outError = "无法打开 manifest.json 文件进行读取。";
        return false;
    }

#if __has_include(<nlohmann/json.hpp>)
    try {
        json manifest;
        file >> manifest;

        if (!manifest.contains("manifest_version")) {
            outError = "manifest.json 缺少必需的 'manifest_version' 字段。";
            return false;
        }

        std::string rawName = "";
        if (manifest.contains("name") && manifest["name"].is_string()) {
            rawName = manifest["name"].get<std::string>();
        }
        if (rawName.empty()) {
            rawName = folderPath.filename().string();
        }

        std::string rawVersion = "1.0.0";
        if (manifest.contains("version") && manifest["version"].is_string()) {
            rawVersion = manifest["version"].get<std::string>();
        }

        std::string rawDesc = "";
        if (manifest.contains("description") && manifest["description"].is_string()) {
            rawDesc = manifest["description"].get<std::string>();
        }

        // Localization helper (__MSG_xxx__)
        auto resolveLocaleMsg = [&](const std::string& key) -> std::string {
            if (key.rfind("__MSG_", 0) == 0 && key.length() > 8 && key.substr(key.length() - 2) == "__") {
                std::string msgKey = key.substr(6, key.length() - 8);
                static const std::vector<std::string> localeDirs = { "zh_CN", "zh", "en", "en_US", "en_GB" };
                for (const auto& loc : localeDirs) {
                    std::filesystem::path locFile = folderPath / "_locales" / loc / "messages.json";
                    if (std::filesystem::exists(locFile, ec)) {
                        try {
                            std::ifstream lf(locFile);
                            if (lf.is_open()) {
                                json locJson;
                                lf >> locJson;
                                if (locJson.contains(msgKey) && locJson[msgKey].contains("message")) {
                                    return locJson[msgKey]["message"].get<std::string>();
                                }
                            }
                        } catch (...) {}
                    }
                }
            }
            return key;
        };

        rawName = resolveLocaleMsg(rawName);
        rawDesc = resolveLocaleMsg(rawDesc);

        std::string optionsPage = "";
        if (manifest.contains("options_ui") && manifest["options_ui"].is_object()) {
            if (manifest["options_ui"].contains("page") && manifest["options_ui"]["page"].is_string()) {
                optionsPage = manifest["options_ui"]["page"].get<std::string>();
            }
        }
        if (optionsPage.empty() && manifest.contains("options_page") && manifest["options_page"].is_string()) {
            optionsPage = manifest["options_page"].get<std::string>();
        }

        outInfo.name = StringUtils::Utf8ToWide(rawName);
        outInfo.version = StringUtils::Utf8ToWide(rawVersion);
        outInfo.description = StringUtils::Utf8ToWide(rawDesc);
        outInfo.folderPath = folderPath.wstring();
        outInfo.optionsPage = StringUtils::Utf8ToWide(optionsPage);
        return true;
    } catch (const std::exception& e) {
        outError = std::string("解析 manifest.json 格式错误: ") + e.what();
        return false;
    } catch (...) {
        outError = "解析 manifest.json 遇到未知异常。";
        return false;
    }
#else
    outInfo.name = folderPath.filename().wstring();
    outInfo.version = L"1.0.0";
    outInfo.description = L"";
    outInfo.folderPath = folderPath.wstring();
    outInfo.optionsPage = L"";
    return true;
#endif
}

void ExtensionManager::LoadUnpackedExtension(HWND hWndParent, std::function<void(bool, const std::wstring&)> callback) {
    if (!m_profile7) {
        if (callback) {
            callback(false, L"当前环境不支持 WebView2 扩展接口 (需要 Microsoft Edge Evergreen Runtime >= 118)。");
        }
        return;
    }

    std::wstring folderPath = L"";

    wil::com_ptr<IFileOpenDialog> pFileOpen;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pFileOpen));
    if (SUCCEEDED(hr)) {
        FILEOPENDIALOGOPTIONS opt = 0;
        pFileOpen->GetOptions(&opt);
        pFileOpen->SetOptions(opt | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        pFileOpen->SetTitle(L"选择包含 manifest.json 的已解压 Chrome 扩展程序目录");
        if (SUCCEEDED(pFileOpen->Show(hWndParent))) {
            wil::com_ptr<IShellItem> pItem;
            if (SUCCEEDED(pFileOpen->GetResult(&pItem))) {
                PWSTR pszFilePath = nullptr;
                if (SUCCEEDED(pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszFilePath)) && pszFilePath) {
                    folderPath = pszFilePath;
                    CoTaskMemFree(pszFilePath);
                }
            }
        }
    } else {
        BROWSEINFOW bi{};
        bi.hwndOwner = hWndParent;
        bi.lpszTitle = L"选择 Chrome 扩展程序解压目录 (包含 manifest.json)";
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_USENEWUI;
        PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
        if (pidl) {
            wchar_t path[MAX_PATH]{};
            if (SHGetPathFromIDListW(pidl, path)) {
                folderPath = path;
            }
            CoTaskMemFree(pidl);
        }
    }

    if (folderPath.empty()) {
        return;
    }

    AddExtensionFromPath(folderPath, callback);
}

void ExtensionManager::AddExtensionFromPath(const std::wstring& folderPath, std::function<void(bool, const std::wstring&)> callback) {
    if (!m_profile7) {
        if (callback) {
            callback(false, L"当前环境不支持 WebView2 扩展接口。");
        }
        return;
    }

    std::filesystem::path fPath(folderPath);
    std::error_code ec;
    fPath = std::filesystem::weakly_canonical(fPath, ec);

    std::wstring pStr = fPath.wstring();
    std::wstring lowerP = pStr;
    std::transform(lowerP.begin(), lowerP.end(), lowerP.begin(), ::towlower);
    if (lowerP.find(L":\\windows") != std::wstring::npos ||
        lowerP.find(L":\\program files") != std::wstring::npos ||
        fPath.root_path() == fPath) {
        if (callback) {
            callback(false, L"出于安全考虑，禁止将系统核心目录或驱动器根目录作为扩展加载！");
        }
        return;
    }

    ExtensionInfo parsedInfo;
    std::string parseErr;
    if (!ParseManifest(fPath, parsedInfo, parseErr)) {
        if (callback) {
            callback(false, L"扩展清单验证失败: " + StringUtils::Utf8ToWide(parseErr));
        }
        return;
    }

    m_profile7->AddBrowserExtension(
        fPath.c_str(),
        Callback<ICoreWebView2ProfileAddBrowserExtensionCompletedHandler>(
            [this, parsedInfo, fPath, callback](HRESULT errorCode, ICoreWebView2BrowserExtension* extension) -> HRESULT {
                if (FAILED(errorCode) || !extension) {
                    wchar_t hexCode[32]{};
                    swprintf_s(hexCode, L"0x%08X", static_cast<unsigned int>(errorCode));
                    std::wstring errMsg = L"WebView2 加载扩展程序失败 (错误代码: " + std::wstring(hexCode) + L")。\n请检查扩展是否兼容或已存在。";
                    if (callback) {
                        callback(false, errMsg);
                    }
                    return S_OK;
                }

                wil::unique_cotaskmem_string idStr;
                wil::unique_cotaskmem_string nameStr;
                BOOL isEnabled = TRUE;
                extension->get_Id(&idStr);
                extension->get_Name(&nameStr);
                extension->get_IsEnabled(&isEnabled);

                std::wstring extId = idStr.get() ? idStr.get() : L"";
                std::wstring extName = (nameStr.get() && wcslen(nameStr.get()) > 0) ? nameStr.get() : parsedInfo.name;

                ExtensionInfo info = parsedInfo;
                info.id = extId;
                info.name = extName;
                info.isEnabled = (isEnabled != FALSE);
                info.comExtension = extension;

                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    bool updated = false;
                    for (auto& existing : m_extensions) {
                        if (existing.id == info.id || existing.folderPath == info.folderPath) {
                            existing = info;
                            updated = true;
                            break;
                        }
                    }
                    if (!updated) {
                        m_extensions.push_back(info);
                    }
                }

                ExtensionConfigItem cfgItem;
                cfgItem.id = StringUtils::WideToUtf8(info.id);
                cfgItem.name = StringUtils::WideToUtf8(info.name);
                cfgItem.folderPath = info.folderPath;
                cfgItem.enabled = info.isEnabled;
                Config::Instance().AddOrUpdateExtensionConfig(cfgItem);

                std::wstring successMsg = L"扩展程序【" + info.name + L"】加载成功！\nID: " + info.id;
                if (callback) {
                    callback(true, successMsg);
                }
                return S_OK;
            }
        ).Get()
    );
}

void ExtensionManager::RefreshExtensions(std::function<void(bool)> onComplete) {
    if (!m_profile7) {
        if (onComplete) onComplete(false);
        return;
    }

    m_profile7->GetBrowserExtensions(
        Callback<ICoreWebView2ProfileGetBrowserExtensionsCompletedHandler>(
            [this, onComplete](HRESULT errorCode, ICoreWebView2BrowserExtensionList* extensionList) -> HRESULT {
                if (FAILED(errorCode) || !extensionList) {
                    if (onComplete) onComplete(false);
                    return S_OK;
                }

                UINT32 count = 0;
                extensionList->get_Count(&count);

                auto savedConfigs = Config::Instance().GetInstalledExtensions();
                std::vector<ExtensionInfo> currentList;

                for (UINT32 i = 0; i < count; ++i) {
                    wil::com_ptr<ICoreWebView2BrowserExtension> ext;
                    if (SUCCEEDED(extensionList->GetValueAtIndex(i, &ext)) && ext) {
                        wil::unique_cotaskmem_string idStr;
                        wil::unique_cotaskmem_string nameStr;
                        BOOL isEnabled = TRUE;
                        ext->get_Id(&idStr);
                        ext->get_Name(&nameStr);
                        ext->get_IsEnabled(&isEnabled);

                        ExtensionInfo info;
                        info.id = idStr.get() ? idStr.get() : L"";
                        info.name = nameStr.get() ? nameStr.get() : L"";
                        info.isEnabled = (isEnabled != FALSE);
                        info.comExtension = ext;

                        std::string idUtf8 = StringUtils::WideToUtf8(info.id);
                        for (const auto& cfg : savedConfigs) {
                            if (cfg.id == idUtf8) {
                                info.folderPath = cfg.folderPath;
                                break;
                            }
                        }

                        if (!info.folderPath.empty()) {
                            ExtensionInfo manifestInfo;
                            std::string err;
                            if (ParseManifest(std::filesystem::path(info.folderPath), manifestInfo, err)) {
                                if (info.name.empty()) info.name = manifestInfo.name;
                                info.version = manifestInfo.version;
                                info.description = manifestInfo.description;
                                info.optionsPage = manifestInfo.optionsPage;
                            }
                        }

                        if (info.version.empty()) info.version = L"1.0.0";
                        currentList.push_back(info);
                    }
                }

                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_extensions = currentList;
                }

                if (onComplete) onComplete(true);
                return S_OK;
            }
        ).Get()
    );
}

void ExtensionManager::ToggleExtension(const std::wstring& id, std::function<void(bool)> callback) {
    bool currentEnabled = true;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& item : m_extensions) {
            if (item.id == id) {
                currentEnabled = item.isEnabled;
                break;
            }
        }
    }
    SetExtensionEnabled(id, !currentEnabled, callback);
}

void ExtensionManager::SetExtensionEnabled(const std::wstring& id, bool enable, std::function<void(bool)> callback) {
    wil::com_ptr<ICoreWebView2BrowserExtension> ext;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& item : m_extensions) {
            if (item.id == id) {
                ext = item.comExtension;
                item.isEnabled = enable;
                break;
            }
        }
    }

    Config::Instance().SetExtensionConfigEnabled(StringUtils::WideToUtf8(id), enable);

    if (!ext) {
        if (callback) callback(false);
        return;
    }

    ext->Enable(
        enable ? TRUE : FALSE,
        Callback<ICoreWebView2BrowserExtensionEnableCompletedHandler>(
            [callback](HRESULT hr) -> HRESULT {
                if (callback) callback(SUCCEEDED(hr));
                return S_OK;
            }
        ).Get()
    );
}

void ExtensionManager::RemoveExtension(const std::wstring& id, std::function<void(bool)> callback) {
    wil::com_ptr<ICoreWebView2BrowserExtension> ext;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto it = m_extensions.begin(); it != m_extensions.end(); ++it) {
            if (it->id == id) {
                ext = it->comExtension;
                m_extensions.erase(it);
                break;
            }
        }
    }

    Config::Instance().RemoveExtensionConfig(StringUtils::WideToUtf8(id));

    if (!ext) {
        if (callback) callback(false);
        return;
    }

    ext->Remove(
        Callback<ICoreWebView2BrowserExtensionRemoveCompletedHandler>(
            [callback](HRESULT hr) -> HRESULT {
                if (callback) callback(SUCCEEDED(hr));
                return S_OK;
            }
        ).Get()
    );
}

void ExtensionManager::ReloadAllExtensions() {
    RefreshExtensions([this](bool) {
        RestoreSavedExtensions();
    });
}

void ExtensionManager::OpenExtensionOptions(const std::wstring& id, ICoreWebView2* webView) {
    ICoreWebView2* targetWebView = webView ? webView : m_webView.get();
    if (!targetWebView) return;

    std::wstring optionsPage = L"";
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& item : m_extensions) {
            if (item.id == id) {
                optionsPage = item.optionsPage;
                break;
            }
        }
    }

    if (optionsPage.empty()) {
        optionsPage = L"options.html";
    }

    std::wstring url = L"chrome-extension://" + id + L"/" + optionsPage;
    targetWebView->Navigate(url.c_str());
}

void ExtensionManager::OpenExtensionFolder(const std::wstring& id) {
    std::wstring folderPath = L"";
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& item : m_extensions) {
            if (item.id == id) {
                folderPath = item.folderPath;
                break;
            }
        }
    }

    if (!folderPath.empty()) {
        ShellExecuteW(nullptr, L"open", folderPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

std::vector<ExtensionInfo> ExtensionManager::GetExtensions() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_extensions;
}

bool ExtensionManager::HasExtension(const std::wstring& id) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& item : m_extensions) {
        if (item.id == id) return true;
    }
    return false;
}

const ExtensionInfo* ExtensionManager::FindExtension(const std::wstring& id) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& item : m_extensions) {
        if (item.id == id) return &item;
    }
    return nullptr;
}

size_t ExtensionManager::GetExtensionCount() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_extensions.size();
}

void ExtensionManager::ShowManageDialog(HWND hWndParent) {
    static const wchar_t* kDlgClassName = L"UltraLight_ExtensionManagerDialogClass";
    static bool classRegistered = false;

    HINSTANCE hInstance = GetModuleHandleW(nullptr);

    if (!classRegistered) {
        WNDCLASSEXW wc{ sizeof(WNDCLASSEXW) };
        wc.lpfnWndProc = ExtDlgWndProc;
        wc.hInstance = hInstance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = kDlgClassName;
        RegisterClassExW(&wc);
        classRegistered = true;
    }

    int dlgW = 745;
    int dlgH = 555;

    RECT parentRect{ 0, 0, 1024, 768 };
    if (hWndParent && IsWindow(hWndParent)) {
        GetWindowRect(hWndParent, &parentRect);
    }

    int posX = parentRect.left + ((parentRect.right - parentRect.left) - dlgW) / 2;
    int posY = parentRect.top + ((parentRect.bottom - parentRect.top) - dlgH) / 2;
    if (posX < 0) posX = 50;
    if (posY < 0) posY = 50;

    ExtDlgContext ctx;

    HWND hDlg = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
        kDlgClassName,
        L"🧩 扩展程序管理 (Chrome Extensions)",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        posX, posY, dlgW, dlgH,
        hWndParent, nullptr, hInstance, &ctx);

    if (!hDlg) return;

    if (hWndParent) {
        EnableWindow(hWndParent, FALSE);
    }

    MSG msg;
    while (IsWindow(hDlg) && GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_QUIT) {
            PostQuitMessage(static_cast<int>(msg.wParam));
            break;
        }
        if (!IsDialogMessageW(hDlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (hWndParent && IsWindow(hWndParent)) {
        EnableWindow(hWndParent, TRUE);
        SetForegroundWindow(hWndParent);
    }
}

} // namespace UltraLight
