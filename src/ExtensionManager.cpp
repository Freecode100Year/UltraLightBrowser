#include "ExtensionManager.hpp"
#include "StringUtils.hpp"
#include "Config.hpp"
#include <shobjidl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <uxtheme.h>
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
    IDC_EXTDLG_STATIC_ENV      = 3004,
    IDC_EXTDLG_HEADER_TITLE    = 3005,
    IDC_EXTDLG_HEADER_BADGE    = 3006,
    IDC_EXTDLG_STATIC_DESC     = 3007,
    IDC_EXTDLG_LBL_ID          = 3008,
    IDC_EXTDLG_EDIT_ID         = 3009,
    IDC_EXTDLG_BTN_COPY_ID     = 3010,
    IDC_EXTDLG_LBL_PATH        = 3011,
    IDC_EXTDLG_EDIT_PATH       = 3012,
    IDC_EXTDLG_BTN_OPEN_DIR    = 3013,
    IDC_EXTDLG_BTN_OPTIONS     = 3014,
    IDC_EXTDLG_BTN_TOGGLE      = 3015,
    IDC_EXTDLG_BTN_RELOAD      = 3016,
    IDC_EXTDLG_BTN_REMOVE      = 3017,
    IDC_EXTDLG_CHK_PRESERVE    = 3018,
    IDC_EXTDLG_STATIC_COUNT    = 3019,
    IDC_EXTDLG_BTN_CLOSE       = 3020
};

struct ExtDlgContext {
    HWND hDlg = nullptr;
    // Top
    HWND hBtnLoad = nullptr;
    HWND hBtnRefresh = nullptr;
    HWND hStaticEnv = nullptr;
    // Middle ListView
    HWND hList = nullptr;
    // Inspector Card Deck
    HWND hHeaderTitle = nullptr;
    HWND hHeaderBadge = nullptr;
    HWND hStaticDesc = nullptr;
    HWND hLblId = nullptr;
    HWND hEditId = nullptr;
    HWND hBtnCopyId = nullptr;
    HWND hLblPath = nullptr;
    HWND hEditPath = nullptr;
    HWND hBtnOpenFolder = nullptr;
    HWND hBtnOptions = nullptr;
    HWND hBtnToggle = nullptr;
    HWND hBtnReload = nullptr;
    HWND hBtnRemove = nullptr;
    // Bottom Bar
    HWND hChkPreserve = nullptr;
    HWND hStaticCount = nullptr;
    HWND hBtnClose = nullptr;

    // Fonts & GDI
    UINT dpi = 96;
    HFONT hFontNormal = nullptr;
    HFONT hFontBold = nullptr;
    HFONT hFontTitle = nullptr;
    HFONT hFontMono = nullptr;
    HFONT hFontSub = nullptr;

    HBRUSH hBrushBg = nullptr;      // RGB(24, 24, 24)
    HBRUSH hBrushCard = nullptr;    // RGB(34, 34, 34)
    HBRUSH hBrushEdit = nullptr;    // RGB(42, 42, 42)
    HPEN hPenBorder = nullptr;      // RGB(56, 56, 56)

    RECT cardRect{};
    std::wstring selectedId = L"";

    int Scale(int px) const {
        return MulDiv(px, static_cast<int>(dpi), 96);
    }
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

void CreateContextFonts(ExtDlgContext* ctx) {
    if (!ctx) return;
    if (ctx->hFontNormal) DeleteObject(ctx->hFontNormal);
    if (ctx->hFontBold) DeleteObject(ctx->hFontBold);
    if (ctx->hFontTitle) DeleteObject(ctx->hFontTitle);
    if (ctx->hFontMono) DeleteObject(ctx->hFontMono);
    if (ctx->hFontSub) DeleteObject(ctx->hFontSub);

    int normalH = -MulDiv(10, ctx->dpi, 72);
    int boldH   = -MulDiv(10, ctx->dpi, 72);
    int titleH  = -MulDiv(13, ctx->dpi, 72);
    int monoH   = -MulDiv(9,  ctx->dpi, 72);
    int subH    = -MulDiv(9,  ctx->dpi, 72);

    ctx->hFontNormal = CreateFontW(normalH, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable Display");
    ctx->hFontBold = CreateFontW(boldH, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable Display");
    ctx->hFontTitle = CreateFontW(titleH, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable Display");
    ctx->hFontMono = CreateFontW(monoH, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        FIXED_PITCH | FF_MODERN, L"Consolas");
    ctx->hFontSub = CreateFontW(subH, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

void UpdateLayout(ExtDlgContext* ctx, int clientW, int clientH) {
    if (!ctx || !ctx->hDlg || clientW <= 0 || clientH <= 0) return;

    HDWP hDwp = BeginDeferWindowPos(18);
    if (!hDwp) return;

    int padX = ctx->Scale(18);
    int padY = ctx->Scale(14);
    int topBtnH = ctx->Scale(32);

    // 1. Top action bar
    int loadBtnW = ctx->Scale(180);
    int refreshBtnW = ctx->Scale(110);
    hDwp = DeferWindowPos(hDwp, ctx->hBtnLoad, nullptr, padX, padY, loadBtnW, topBtnH, SWP_NOZORDER);
    hDwp = DeferWindowPos(hDwp, ctx->hBtnRefresh, nullptr, padX + loadBtnW + ctx->Scale(8), padY, refreshBtnW, topBtnH, SWP_NOZORDER);

    int envW = ctx->Scale(380);
    int envX = clientW - padX - envW;
    if (envX < padX + loadBtnW + refreshBtnW + ctx->Scale(10)) {
        envX = padX + loadBtnW + refreshBtnW + ctx->Scale(10);
        envW = clientW - padX - envX;
    }
    hDwp = DeferWindowPos(hDwp, ctx->hStaticEnv, nullptr, envX, padY + ctx->Scale(6), envW, ctx->Scale(22), SWP_NOZORDER);

    // 2. Vertical splits
    int bottomBarH = ctx->Scale(34);
    int deckH = ctx->Scale(220);
    int listTop = padY + topBtnH + ctx->Scale(10);
    int bottomY = clientH - padY - bottomBarH;
    int deckY = bottomY - ctx->Scale(10) - deckH;
    int listH = deckY - listTop - ctx->Scale(10);
    if (listH < ctx->Scale(90)) listH = ctx->Scale(90);

    int contentW = clientW - 2 * padX;

    // List View
    hDwp = DeferWindowPos(hDwp, ctx->hList, nullptr, padX, listTop, contentW, listH, SWP_NOZORDER);

    // Adjust list column widths
    int scrollbarW = GetSystemMetrics(SM_CXVSCROLL);
    int col0 = ctx->Scale(240);
    int col1 = ctx->Scale(90);
    int col2 = ctx->Scale(75);
    int col3 = ctx->Scale(160);
    int col4 = contentW - col0 - col1 - col2 - col3 - scrollbarW - ctx->Scale(4);
    if (col4 < ctx->Scale(160)) col4 = ctx->Scale(160);
    ListView_SetColumnWidth(ctx->hList, 0, col0);
    ListView_SetColumnWidth(ctx->hList, 1, col1);
    ListView_SetColumnWidth(ctx->hList, 2, col2);
    ListView_SetColumnWidth(ctx->hList, 3, col3);
    ListView_SetColumnWidth(ctx->hList, 4, col4);

    // 3. Card Deck
    ctx->cardRect = RECT{ padX, deckY, padX + contentW, deckY + deckH };
    int cardInnerPad = ctx->Scale(16);
    int cardInnerW = contentW - 2 * cardInnerPad;
    int curY = deckY + ctx->Scale(12);

    // Title & Badge row
    int badgeW = ctx->Scale(190);
    int titleW = cardInnerW - badgeW - ctx->Scale(10);
    if (titleW < ctx->Scale(100)) titleW = ctx->Scale(100);
    hDwp = DeferWindowPos(hDwp, ctx->hHeaderTitle, nullptr, padX + cardInnerPad, curY, titleW, ctx->Scale(26), SWP_NOZORDER);
    hDwp = DeferWindowPos(hDwp, ctx->hHeaderBadge, nullptr, padX + cardInnerPad + titleW + ctx->Scale(10), curY + ctx->Scale(3), badgeW, ctx->Scale(22), SWP_NOZORDER);

    curY += ctx->Scale(28);

    // Description
    hDwp = DeferWindowPos(hDwp, ctx->hStaticDesc, nullptr, padX + cardInnerPad, curY, cardInnerW, ctx->Scale(36), SWP_NOZORDER);

    curY += ctx->Scale(40);

    // ID row
    int lblW = ctx->Scale(70);
    int btnActionW = ctx->Scale(95);
    int editW = cardInnerW - lblW - btnActionW - ctx->Scale(14);
    hDwp = DeferWindowPos(hDwp, ctx->hLblId, nullptr, padX + cardInnerPad, curY + ctx->Scale(3), lblW, ctx->Scale(20), SWP_NOZORDER);
    hDwp = DeferWindowPos(hDwp, ctx->hEditId, nullptr, padX + cardInnerPad + lblW, curY, editW, ctx->Scale(24), SWP_NOZORDER);
    hDwp = DeferWindowPos(hDwp, ctx->hBtnCopyId, nullptr, padX + cardInnerPad + lblW + editW + ctx->Scale(8), curY, btnActionW, ctx->Scale(24), SWP_NOZORDER);

    curY += ctx->Scale(30);

    // Path row
    hDwp = DeferWindowPos(hDwp, ctx->hLblPath, nullptr, padX + cardInnerPad, curY + ctx->Scale(3), lblW, ctx->Scale(20), SWP_NOZORDER);
    hDwp = DeferWindowPos(hDwp, ctx->hEditPath, nullptr, padX + cardInnerPad + lblW, curY, editW, ctx->Scale(24), SWP_NOZORDER);
    hDwp = DeferWindowPos(hDwp, ctx->hBtnOpenFolder, nullptr, padX + cardInnerPad + lblW + editW + ctx->Scale(8), curY, btnActionW, ctx->Scale(24), SWP_NOZORDER);

    curY += ctx->Scale(34);

    // Action buttons row
    int actBtnW = ctx->Scale(130);
    int actBtnH = ctx->Scale(30);
    int actSpacing = ctx->Scale(10);
    hDwp = DeferWindowPos(hDwp, ctx->hBtnOptions, nullptr, padX + cardInnerPad, curY, actBtnW, actBtnH, SWP_NOZORDER);
    hDwp = DeferWindowPos(hDwp, ctx->hBtnToggle, nullptr, padX + cardInnerPad + (actBtnW + actSpacing), curY, actBtnW, actBtnH, SWP_NOZORDER);
    hDwp = DeferWindowPos(hDwp, ctx->hBtnReload, nullptr, padX + cardInnerPad + (actBtnW + actSpacing) * 2, curY, actBtnW, actBtnH, SWP_NOZORDER);
    hDwp = DeferWindowPos(hDwp, ctx->hBtnRemove, nullptr, padX + cardInnerPad + (actBtnW + actSpacing) * 3, curY, actBtnW, actBtnH, SWP_NOZORDER);

    // 4. Bottom bar
    int chkW = ctx->Scale(380);
    int closeBtnW = ctx->Scale(95);
    hDwp = DeferWindowPos(hDwp, ctx->hChkPreserve, nullptr, padX, bottomY + ctx->Scale(4), chkW, ctx->Scale(24), SWP_NOZORDER);

    int countX = padX + chkW + ctx->Scale(10);
    int countW = clientW - padX - closeBtnW - ctx->Scale(12) - countX;
    if (countW > ctx->Scale(40)) {
        hDwp = DeferWindowPos(hDwp, ctx->hStaticCount, nullptr, countX, bottomY + ctx->Scale(6), countW, ctx->Scale(20), SWP_NOZORDER);
    }
    hDwp = DeferWindowPos(hDwp, ctx->hBtnClose, nullptr, clientW - padX - closeBtnW, bottomY, closeBtnW, ctx->Scale(32), SWP_NOZORDER);

    EndDeferWindowPos(hDwp);
    InvalidateRect(ctx->hDlg, &ctx->cardRect, TRUE);
}

void UpdateDialogControls(ExtDlgContext* ctx) {
    if (!ctx || !ctx->hList) return;

    ListView_DeleteAllItems(ctx->hList);

    auto exts = ExtensionManager::Instance().GetExtensions();
    int selectRow = -1;
    size_t enabledCount = 0;

    for (size_t i = 0; i < exts.size(); ++i) {
        const auto& ext = exts[i];
        if (ext.isEnabled) enabledCount++;

        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = static_cast<int>(i);
        item.iSubItem = 0;
        item.pszText = const_cast<wchar_t*>(ext.name.c_str());
        item.lParam = static_cast<LPARAM>(i);
        ListView_InsertItem(ctx->hList, &item);

        std::wstring statusStr = ext.isEnabled ? L"🟢 已启用" : L"⚪ 已停用";
        ListView_SetItemText(ctx->hList, static_cast<int>(i), 1, const_cast<wchar_t*>(statusStr.c_str()));
        ListView_SetItemText(ctx->hList, static_cast<int>(i), 2, const_cast<wchar_t*>(ext.version.c_str()));
        ListView_SetItemText(ctx->hList, static_cast<int>(i), 3, const_cast<wchar_t*>(ext.id.c_str()));
        ListView_SetItemText(ctx->hList, static_cast<int>(i), 4, const_cast<wchar_t*>(ext.folderPath.c_str()));

        if (!ctx->selectedId.empty() && ext.id == ctx->selectedId) {
            selectRow = static_cast<int>(i);
        }
    }

    std::wstring countStr = L"共 " + std::to_wstring(exts.size()) + L" 个扩展程序 (" +
                            std::to_wstring(enabledCount) + L" 个运行中)";
    SetWindowTextW(ctx->hStaticCount, countStr.c_str());

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
        SetWindowTextW(ctx->hHeaderTitle, cur->name.c_str());
        std::wstring badge = cur->isEnabled ? (L"🟢 运行中 · v" + cur->version) : (L"⚪ 已停用 · v" + cur->version);
        SetWindowTextW(ctx->hHeaderBadge, badge.c_str());

        std::wstring desc = cur->description.empty() ? L"（该扩展程序未提供功能描述信息）" : cur->description;
        SetWindowTextW(ctx->hStaticDesc, desc.c_str());

        SetWindowTextW(ctx->hEditId, cur->id.c_str());
        SetWindowTextW(ctx->hEditPath, cur->folderPath.c_str());

        SetWindowTextW(ctx->hBtnToggle, cur->isEnabled ? L"⏸ 停用扩展" : L"▶ 启用扩展");

        EnableWindow(ctx->hBtnToggle, TRUE);
        EnableWindow(ctx->hBtnReload, TRUE);
        EnableWindow(ctx->hBtnRemove, TRUE);
        EnableWindow(ctx->hBtnCopyId, TRUE);
        EnableWindow(ctx->hBtnOpenFolder, !cur->folderPath.empty());
        EnableWindow(ctx->hBtnOptions, cur->isEnabled && (!cur->optionsPage.empty() || !cur->folderPath.empty()));
    } else {
        SetWindowTextW(ctx->hHeaderTitle, L"未选择任何扩展程序");
        SetWindowTextW(ctx->hHeaderBadge, L"");
        SetWindowTextW(ctx->hStaticDesc, L"请在上方列表中单击选择扩展程序，以查看详细配置和操作控制。");
        SetWindowTextW(ctx->hEditId, L"");
        SetWindowTextW(ctx->hEditPath, L"");
        SetWindowTextW(ctx->hBtnToggle, L"启用 / 停用");

        EnableWindow(ctx->hBtnToggle, FALSE);
        EnableWindow(ctx->hBtnReload, FALSE);
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
        DWORD cornerPref = 2; // DWMWCP_ROUND
        DwmSetWindowAttribute(hWnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &cornerPref, sizeof(cornerPref));

        CreateContextFonts(ctx);

        ctx->hBrushBg = CreateSolidBrush(RGB(24, 24, 24));
        ctx->hBrushCard = CreateSolidBrush(RGB(34, 34, 34));
        ctx->hBrushEdit = CreateSolidBrush(RGB(42, 42, 42));
        ctx->hPenBorder = CreatePen(PS_SOLID, 1, RGB(56, 56, 56));

        // 1. Top action buttons & status
        ctx->hBtnLoad = CreateWindowExW(0, L"BUTTON", L"📂 加载未打包扩展...",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 0, 0, hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EXTDLG_BTN_LOAD)), nullptr, nullptr);
        SendMessageW(ctx->hBtnLoad, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontBold), TRUE);

        ctx->hBtnRefresh = CreateWindowExW(0, L"BUTTON", L"🔄 刷新列表",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 0, 0, hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EXTDLG_BTN_REFRESH)), nullptr, nullptr);
        SendMessageW(ctx->hBtnRefresh, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontNormal), TRUE);

        ctx->hStaticEnv = CreateWindowExW(0, L"STATIC", L"🛡️ WebView2 Chrome 扩展运行环境: [已就绪 (支持 MV2 / MV3)]",
            WS_CHILD | WS_VISIBLE | SS_RIGHT,
            0, 0, 0, 0, hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EXTDLG_STATIC_ENV)), nullptr, nullptr);
        SendMessageW(ctx->hStaticEnv, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontSub), TRUE);

        // 2. Main ListView
        ctx->hList = CreateWindowExW(0, WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | WS_BORDER | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
            0, 0, 0, 0, hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EXTDLG_LIST)), nullptr, nullptr);
        SendMessageW(ctx->hList, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontNormal), TRUE);

        // Apply dark mode theme to listview
        SetWindowTheme(ctx->hList, L"DarkMode_Explorer", nullptr);
        ListView_SetBkColor(ctx->hList, RGB(26, 26, 26));
        ListView_SetTextBkColor(ctx->hList, RGB(26, 26, 26));
        ListView_SetTextColor(ctx->hList, RGB(240, 240, 240));

        ListView_SetExtendedListViewStyle(ctx->hList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);

        LVCOLUMNW lvc{};
        lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;

        lvc.iSubItem = 0;
        lvc.pszText = const_cast<wchar_t*>(L"扩展程序名称");
        lvc.cx = ctx->Scale(240);
        ListView_InsertColumn(ctx->hList, 0, &lvc);

        lvc.iSubItem = 1;
        lvc.pszText = const_cast<wchar_t*>(L"状态");
        lvc.cx = ctx->Scale(90);
        ListView_InsertColumn(ctx->hList, 1, &lvc);

        lvc.iSubItem = 2;
        lvc.pszText = const_cast<wchar_t*>(L"版本");
        lvc.cx = ctx->Scale(75);
        ListView_InsertColumn(ctx->hList, 2, &lvc);

        lvc.iSubItem = 3;
        lvc.pszText = const_cast<wchar_t*>(L"扩展标识 ID");
        lvc.cx = ctx->Scale(160);
        ListView_InsertColumn(ctx->hList, 3, &lvc);

        lvc.iSubItem = 4;
        lvc.pszText = const_cast<wchar_t*>(L"本地安装目录路径");
        lvc.cx = ctx->Scale(200);
        ListView_InsertColumn(ctx->hList, 4, &lvc);

        // 3. Card Deck Elements
        ctx->hHeaderTitle = CreateWindowExW(0, L"STATIC", L"请在上方列表选择扩展程序",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            0, 0, 0, 0, hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EXTDLG_HEADER_TITLE)), nullptr, nullptr);
        SendMessageW(ctx->hHeaderTitle, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontTitle), TRUE);

        ctx->hHeaderBadge = CreateWindowExW(0, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_RIGHT,
            0, 0, 0, 0, hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EXTDLG_HEADER_BADGE)), nullptr, nullptr);
        SendMessageW(ctx->hHeaderBadge, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontBold), TRUE);

        ctx->hStaticDesc = CreateWindowExW(0, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            0, 0, 0, 0, hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EXTDLG_STATIC_DESC)), nullptr, nullptr);
        SendMessageW(ctx->hStaticDesc, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontNormal), TRUE);

        // ID row
        ctx->hLblId = CreateWindowExW(0, L"STATIC", L"扩展 ID:", WS_CHILD | WS_VISIBLE,
            0, 0, 0, 0, hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EXTDLG_LBL_ID)), nullptr, nullptr);
        SendMessageW(ctx->hLblId, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontNormal), TRUE);

        ctx->hEditId = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_READONLY | ES_AUTOHSCROLL,
            0, 0, 0, 0, hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EXTDLG_EDIT_ID)), nullptr, nullptr);
        SendMessageW(ctx->hEditId, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontMono), TRUE);

        ctx->hBtnCopyId = CreateWindowExW(0, L"BUTTON", L"📋 复制 ID",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 0, 0, hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EXTDLG_BTN_COPY_ID)), nullptr, nullptr);
        SendMessageW(ctx->hBtnCopyId, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontNormal), TRUE);

        // Path row
        ctx->hLblPath = CreateWindowExW(0, L"STATIC", L"解压目录:", WS_CHILD | WS_VISIBLE,
            0, 0, 0, 0, hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EXTDLG_LBL_PATH)), nullptr, nullptr);
        SendMessageW(ctx->hLblPath, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontNormal), TRUE);

        ctx->hEditPath = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_READONLY | ES_AUTOHSCROLL,
            0, 0, 0, 0, hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EXTDLG_EDIT_PATH)), nullptr, nullptr);
        SendMessageW(ctx->hEditPath, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontMono), TRUE);

        ctx->hBtnOpenFolder = CreateWindowExW(0, L"BUTTON", L"📁 打开目录",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 0, 0, hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EXTDLG_BTN_OPEN_DIR)), nullptr, nullptr);
        SendMessageW(ctx->hBtnOpenFolder, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontNormal), TRUE);

        // Deck Action buttons
        ctx->hBtnOptions = CreateWindowExW(0, L"BUTTON", L"🌐 打开扩展界面",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 0, 0, hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EXTDLG_BTN_OPTIONS)), nullptr, nullptr);
        SendMessageW(ctx->hBtnOptions, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontBold), TRUE);

        ctx->hBtnToggle = CreateWindowExW(0, L"BUTTON", L"⏸ 停用扩展",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 0, 0, hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EXTDLG_BTN_TOGGLE)), nullptr, nullptr);
        SendMessageW(ctx->hBtnToggle, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontNormal), TRUE);

        ctx->hBtnReload = CreateWindowExW(0, L"BUTTON", L"🔄 重新载入",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 0, 0, hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EXTDLG_BTN_RELOAD)), nullptr, nullptr);
        SendMessageW(ctx->hBtnReload, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontNormal), TRUE);

        ctx->hBtnRemove = CreateWindowExW(0, L"BUTTON", L"🗑️ 移除此扩展",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 0, 0, hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EXTDLG_BTN_REMOVE)), nullptr, nullptr);
        SendMessageW(ctx->hBtnRemove, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontNormal), TRUE);

        // 4. Bottom bar
        ctx->hChkPreserve = CreateWindowExW(0, L"BUTTON", L"退出时保留扩展程序配置与解压数据 (推荐)",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            0, 0, 0, 0, hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EXTDLG_CHK_PRESERVE)), nullptr, nullptr);
        SendMessageW(ctx->hChkPreserve, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontNormal), TRUE);
        SendMessageW(ctx->hChkPreserve, BM_SETCHECK, Config::Instance().GetSettings().preserveExtensionData ? BST_CHECKED : BST_UNCHECKED, 0);

        ctx->hStaticCount = CreateWindowExW(0, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_RIGHT,
            0, 0, 0, 0, hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EXTDLG_STATIC_COUNT)), nullptr, nullptr);
        SendMessageW(ctx->hStaticCount, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontSub), TRUE);

        ctx->hBtnClose = CreateWindowExW(0, L"BUTTON", L"关闭",
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            0, 0, 0, 0, hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EXTDLG_BTN_CLOSE)), nullptr, nullptr);
        SendMessageW(ctx->hBtnClose, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontBold), TRUE);

        RECT cr;
        GetClientRect(hWnd, &cr);
        UpdateLayout(ctx, cr.right, cr.bottom);
        UpdateDialogControls(ctx);
        return 0;
    }

    case WM_SIZE: {
        int w = LOWORD(lParam);
        int h = HIWORD(lParam);
        UpdateLayout(ctx, w, h);
        return 0;
    }

    case WM_GETMINMAXINFO: {
        auto* pmmi = reinterpret_cast<MINMAXINFO*>(lParam);
        UINT dpi = ctx ? ctx->dpi : 96;
        pmmi->ptMinTrackSize.x = MulDiv(760, dpi, 96);
        pmmi->ptMinTrackSize.y = MulDiv(580, dpi, 96);
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);

        RECT cr;
        GetClientRect(hWnd, &cr);
        FillRect(hdc, &cr, ctx ? ctx->hBrushBg : reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));

        if (ctx && ctx->cardRect.right > ctx->cardRect.left && ctx->cardRect.bottom > ctx->cardRect.top) {
            HGDIOBJ oldBrush = SelectObject(hdc, ctx->hBrushCard);
            HGDIOBJ oldPen = SelectObject(hdc, ctx->hPenBorder);
            int radius = ctx->Scale(10);
            RoundRect(hdc, ctx->cardRect.left, ctx->cardRect.top, ctx->cardRect.right, ctx->cardRect.bottom, radius, radius);
            SelectObject(hdc, oldPen);
            SelectObject(hdc, oldBrush);
        }

        EndPaint(hWnd, &ps);
        return 0;
    }

    case WM_CTLCOLORDLG:
        return reinterpret_cast<INT_PTR>(ctx ? ctx->hBrushBg : GetStockObject(BLACK_BRUSH));

    case WM_CTLCOLORSTATIC: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        HWND hCtrl = reinterpret_cast<HWND>(lParam);
        SetBkMode(hdc, TRANSPARENT);

        bool inDeck = false;
        if (ctx) {
            if (hCtrl == ctx->hHeaderTitle || hCtrl == ctx->hHeaderBadge ||
                hCtrl == ctx->hStaticDesc || hCtrl == ctx->hLblId || hCtrl == ctx->hLblPath) {
                inDeck = true;
            }
        }

        if (ctx && hCtrl == ctx->hHeaderTitle) {
            SetTextColor(hdc, RGB(255, 255, 255));
        } else if (ctx && hCtrl == ctx->hHeaderBadge) {
            SetTextColor(hdc, RGB(50, 205, 120));
        } else if (ctx && (hCtrl == ctx->hLblId || hCtrl == ctx->hLblPath || hCtrl == ctx->hStaticCount)) {
            SetTextColor(hdc, RGB(170, 170, 170));
        } else if (ctx && hCtrl == ctx->hStaticEnv) {
            SetTextColor(hdc, RGB(140, 185, 255));
        } else {
            SetTextColor(hdc, RGB(235, 235, 235));
        }

        return reinterpret_cast<INT_PTR>(inDeck ? (ctx ? ctx->hBrushCard : GetStockObject(BLACK_BRUSH)) : (ctx ? ctx->hBrushBg : GetStockObject(BLACK_BRUSH)));
    }

    case WM_CTLCOLORBTN: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(235, 235, 235));
        return reinterpret_cast<INT_PTR>(ctx ? ctx->hBrushBg : GetStockObject(BLACK_BRUSH));
    }

    case WM_CTLCOLOREDIT: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetBkMode(hdc, OPAQUE);
        SetBkColor(hdc, RGB(42, 42, 42));
        SetTextColor(hdc, RGB(235, 235, 235));
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
                        SetWindowTextW(ctx->hHeaderTitle, cur->name.c_str());
                        std::wstring badge = cur->isEnabled ? (L"🟢 运行中 · v" + cur->version) : (L"⚪ 已停用 · v" + cur->version);
                        SetWindowTextW(ctx->hHeaderBadge, badge.c_str());

                        std::wstring desc = cur->description.empty() ? L"（该扩展程序未提供功能描述信息）" : cur->description;
                        SetWindowTextW(ctx->hStaticDesc, desc.c_str());

                        SetWindowTextW(ctx->hEditId, cur->id.c_str());
                        SetWindowTextW(ctx->hEditPath, cur->folderPath.c_str());

                        SetWindowTextW(ctx->hBtnToggle, cur->isEnabled ? L"⏸ 停用扩展" : L"▶ 启用扩展");

                        EnableWindow(ctx->hBtnToggle, TRUE);
                        EnableWindow(ctx->hBtnReload, TRUE);
                        EnableWindow(ctx->hBtnRemove, TRUE);
                        EnableWindow(ctx->hBtnCopyId, TRUE);
                        EnableWindow(ctx->hBtnOpenFolder, !cur->folderPath.empty());
                        EnableWindow(ctx->hBtnOptions, cur->isEnabled && (!cur->optionsPage.empty() || !cur->folderPath.empty()));
                    }
                }
            } else if (pnmhdr->code == NM_DBLCLK) {
                if (!ctx->selectedId.empty()) {
                    const ExtensionInfo* cur = ExtensionManager::Instance().FindExtension(ctx->selectedId);
                    if (cur && cur->isEnabled) {
                        SendMessageW(hWnd, WM_COMMAND, MAKEWPARAM(IDC_EXTDLG_BTN_OPTIONS, 0), 0);
                    } else {
                        SendMessageW(hWnd, WM_COMMAND, MAKEWPARAM(IDC_EXTDLG_BTN_TOGGLE, 0), 0);
                    }
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

        case IDC_EXTDLG_BTN_RELOAD: {
            if (ctx->selectedId.empty()) break;
            ExtensionManager::Instance().ReloadExtension(ctx->selectedId, [ctx](bool ok) {
                UpdateDialogControls(ctx);
                if (ok) {
                    MessageBoxW(ctx->hDlg, L"扩展程序已成功重新载入！", L"扩展程序", MB_OK | MB_ICONINFORMATION);
                } else {
                    MessageBoxW(ctx->hDlg, L"重新载入扩展程序失败。", L"错误", MB_OK | MB_ICONERROR);
                }
            });
            break;
        }

        case IDC_EXTDLG_BTN_REMOVE: {
            if (ctx->selectedId.empty()) break;
            const ExtensionInfo* cur = ExtensionManager::Instance().FindExtension(ctx->selectedId);
            std::wstring name = cur ? cur->name : ctx->selectedId;
            std::wstring prompt = L"确定要从浏览器中移除扩展程序【" + name + L"】吗？\n移除后该扩展将立即停止运行。";
            int ret = MessageBoxW(hWnd, prompt.c_str(), L"确认移除扩展程序", MB_YESNO | MB_ICONQUESTION);
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
                MessageBoxW(hWnd, L"扩展 ID 已复制到系统剪贴板！", L"复制成功", MB_OK | MB_ICONINFORMATION);
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

    case WM_DPICHANGED: {
        auto* lprc = reinterpret_cast<RECT*>(lParam);
        SetWindowPos(hWnd, nullptr, lprc->left, lprc->top, lprc->right - lprc->left, lprc->bottom - lprc->top, SWP_NOZORDER | SWP_NOACTIVATE);
        if (ctx) {
            ctx->dpi = HIWORD(wParam);
            CreateContextFonts(ctx);
            // Apply new fonts
            SendMessageW(ctx->hBtnLoad, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontBold), TRUE);
            SendMessageW(ctx->hBtnRefresh, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontNormal), TRUE);
            SendMessageW(ctx->hStaticEnv, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontSub), TRUE);
            SendMessageW(ctx->hList, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontNormal), TRUE);
            SendMessageW(ctx->hHeaderTitle, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontTitle), TRUE);
            SendMessageW(ctx->hHeaderBadge, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontBold), TRUE);
            SendMessageW(ctx->hStaticDesc, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontNormal), TRUE);
            SendMessageW(ctx->hLblId, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontNormal), TRUE);
            SendMessageW(ctx->hEditId, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontMono), TRUE);
            SendMessageW(ctx->hBtnCopyId, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontNormal), TRUE);
            SendMessageW(ctx->hLblPath, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontNormal), TRUE);
            SendMessageW(ctx->hEditPath, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontMono), TRUE);
            SendMessageW(ctx->hBtnOpenFolder, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontNormal), TRUE);
            SendMessageW(ctx->hBtnOptions, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontBold), TRUE);
            SendMessageW(ctx->hBtnToggle, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontNormal), TRUE);
            SendMessageW(ctx->hBtnReload, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontNormal), TRUE);
            SendMessageW(ctx->hBtnRemove, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontNormal), TRUE);
            SendMessageW(ctx->hChkPreserve, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontNormal), TRUE);
            SendMessageW(ctx->hStaticCount, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontSub), TRUE);
            SendMessageW(ctx->hBtnClose, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontBold), TRUE);
            RECT client;
            GetClientRect(hWnd, &client);
            UpdateLayout(ctx, client.right, client.bottom);
        }
        return 0;
    }

    case WM_CLOSE: {
        DestroyWindow(hWnd);
        return 0;
    }

    case WM_DESTROY: {
        if (ctx) {
            if (ctx->hFontNormal) DeleteObject(ctx->hFontNormal);
            if (ctx->hFontBold) DeleteObject(ctx->hFontBold);
            if (ctx->hFontTitle) DeleteObject(ctx->hFontTitle);
            if (ctx->hFontMono) DeleteObject(ctx->hFontMono);
            if (ctx->hFontSub) DeleteObject(ctx->hFontSub);
            if (ctx->hBrushBg) DeleteObject(ctx->hBrushBg);
            if (ctx->hBrushCard) DeleteObject(ctx->hBrushCard);
            if (ctx->hBrushEdit) DeleteObject(ctx->hBrushEdit);
            if (ctx->hPenBorder) DeleteObject(ctx->hPenBorder);
        }
        return 0;
    }

    default:
        break;
    }

    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

std::wstring NormalizeExtensionPath(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::path fPath = std::filesystem::weakly_canonical(path, ec);
    if (ec) {
        fPath = path;
    }
    std::wstring p = fPath.wstring();
    // Strip Windows extended length prefix \\?\ or \\?\UNC\ if present
    if (p.rfind(L"\\\\?\\UNC\\", 0) == 0) {
        p = L"\\\\" + p.substr(8);
    } else if (p.rfind(L"\\\\?\\", 0) == 0) {
        p = p.substr(4);
    }
    // Normalize forward slashes to backslashes
    for (auto& ch : p) {
        if (ch == L'/') ch = L'\\';
    }
    // Remove trailing slash/backslash if not root drive
    while (p.length() > 3 && (p.back() == L'\\' || p.back() == L'/')) {
        p.pop_back();
    }
    return p;
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
                std::vector<ExtensionConfigItem> toRestore;
                for (const auto& cfg : savedConfigs) {
                    if (!cfg.folderPath.empty()) {
                        std::wstring clean = NormalizeExtensionPath(cfg.folderPath);
                        if (std::filesystem::exists(clean)) {
                            if (loadedIds.find(cfg.id) == loadedIds.end()) {
                                auto copyCfg = cfg;
                                copyCfg.folderPath = clean;
                                toRestore.push_back(copyCfg);
                            }
                        }
                    }
                }

                if (toRestore.empty()) {
                    RefreshExtensions();
                    return S_OK;
                }

                auto pendingCount = std::make_shared<std::atomic<int>>(static_cast<int>(toRestore.size()));
                for (const auto& cfg : toRestore) {
                    m_profile7->AddBrowserExtension(
                        cfg.folderPath.c_str(),
                        Callback<ICoreWebView2ProfileAddBrowserExtensionCompletedHandler>(
                            [this, cfg, pendingCount](HRESULT addHr, ICoreWebView2BrowserExtension* ext) -> HRESULT {
                                if (SUCCEEDED(addHr) && ext && !cfg.enabled) {
                                    ext->Enable(FALSE, nullptr);
                                }
                                if (--(*pendingCount) <= 0) {
                                    RefreshExtensions();
                                }
                                return S_OK;
                            }
                        ).Get()
                    );
                }

                return S_OK;
            }
        ).Get()
    );
}

bool ExtensionManager::ParseManifest(const std::filesystem::path& folderPath, ExtensionInfo& outInfo, std::string& outError) {
    std::error_code ec;
    std::filesystem::path targetDir = folderPath;
    if (!std::filesystem::exists(targetDir, ec) || !std::filesystem::is_directory(targetDir, ec)) {
        outError = "指定的路径不是有效目录。";
        return false;
    }

    std::filesystem::path manifestPath = targetDir / "manifest.json";
    if (!std::filesystem::exists(manifestPath, ec) || !std::filesystem::is_regular_file(manifestPath, ec)) {
        // Automatically probe 1-level deep subdirectories for manifest.json
        // Common cases: dist/manifest.json, build/manifest.json, src/manifest.json, or an extracted root folder
        bool foundSub = false;
        const std::vector<std::string> probeNames = { "dist", "build", "src", "public", "app" };
        for (const auto& probe : probeNames) {
            std::filesystem::path p = targetDir / probe / "manifest.json";
            if (std::filesystem::exists(p, ec) && std::filesystem::is_regular_file(p, ec)) {
                targetDir = targetDir / probe;
                manifestPath = p;
                foundSub = true;
                break;
            }
        }

        if (!foundSub) {
            for (const auto& entry : std::filesystem::directory_iterator(targetDir, ec)) {
                if (entry.is_directory(ec)) {
                    std::filesystem::path p = entry.path() / "manifest.json";
                    if (std::filesystem::exists(p, ec) && std::filesystem::is_regular_file(p, ec)) {
                        targetDir = entry.path();
                        manifestPath = p;
                        foundSub = true;
                        break;
                    }
                }
            }
        }

        if (!foundSub) {
            outError = "目录及其直接子目录中未找到 manifest.json 扩展清单文件。\n请确保选择的是已解压的扩展程序根目录。";
            return false;
        }
    }

    std::ifstream file(manifestPath, std::ios::binary);
    if (!file.is_open()) {
        outError = "无法打开 manifest.json 文件进行读取。";
        return false;
    }

    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    // Strip UTF-8 BOM if present
    if (content.size() >= 3 &&
        static_cast<unsigned char>(content[0]) == 0xEF &&
        static_cast<unsigned char>(content[1]) == 0xBB &&
        static_cast<unsigned char>(content[2]) == 0xBF) {
        content = content.substr(3);
    }

#if __has_include(<nlohmann/json.hpp>)
    try {
        json manifest = json::parse(content, nullptr, true, true /* ignore comments */);

        if (!manifest.contains("manifest_version")) {
            outError = "manifest.json 缺少必需的 'manifest_version' 字段。";
            return false;
        }

        std::string rawName = "";
        if (manifest.contains("name") && manifest["name"].is_string()) {
            rawName = manifest["name"].get<std::string>();
        }
        if (rawName.empty()) {
            rawName = targetDir.filename().string();
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
                static const std::vector<std::string> localeDirs = { "zh_CN", "zh", "zh_TW", "en", "en_US", "en_GB" };
                for (const auto& loc : localeDirs) {
                    std::filesystem::path locFile = targetDir / "_locales" / loc / "messages.json";
                    if (std::filesystem::exists(locFile, ec)) {
                        try {
                            std::ifstream lf(locFile, std::ios::binary);
                            if (lf.is_open()) {
                                std::string locContent((std::istreambuf_iterator<char>(lf)), std::istreambuf_iterator<char>());
                                if (locContent.size() >= 3 &&
                                    static_cast<unsigned char>(locContent[0]) == 0xEF &&
                                    static_cast<unsigned char>(locContent[1]) == 0xBB &&
                                    static_cast<unsigned char>(locContent[2]) == 0xBF) {
                                    locContent = locContent.substr(3);
                                }
                                json locJson = json::parse(locContent, nullptr, true, true);
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
        if (optionsPage.empty() && manifest.contains("action") && manifest["action"].is_object()) {
            if (manifest["action"].contains("default_popup") && manifest["action"]["default_popup"].is_string()) {
                optionsPage = manifest["action"]["default_popup"].get<std::string>();
            }
        }
        if (optionsPage.empty() && manifest.contains("browser_action") && manifest["browser_action"].is_object()) {
            if (manifest["browser_action"].contains("default_popup") && manifest["browser_action"]["default_popup"].is_string()) {
                optionsPage = manifest["browser_action"]["default_popup"].get<std::string>();
            }
        }

        outInfo.name = StringUtils::Utf8ToWide(rawName);
        outInfo.version = StringUtils::Utf8ToWide(rawVersion);
        outInfo.description = StringUtils::Utf8ToWide(rawDesc);
        outInfo.folderPath = NormalizeExtensionPath(targetDir);
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
    outInfo.name = targetDir.filename().wstring();
    outInfo.version = L"1.0.0";
    outInfo.description = L"";
    outInfo.folderPath = NormalizeExtensionPath(targetDir);
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
            callback(false, L"当前环境不支持 WebView2 扩展接口 (需要 Microsoft Edge Evergreen Runtime >= 118)。");
        }
        return;
    }

    std::filesystem::path inputPath(folderPath);
    std::wstring normalizedInput = NormalizeExtensionPath(inputPath);

    // Security check: prohibit drive root (e.g. C:\) or Windows system directory
    std::wstring lowerP = normalizedInput;
    std::transform(lowerP.begin(), lowerP.end(), lowerP.begin(), ::towlower);
    if (lowerP.find(L":\\windows") != std::wstring::npos ||
        (lowerP.length() <= 3 && lowerP.find(L":\\") != std::wstring::npos)) {
        if (callback) {
            callback(false, L"出于安全考虑，禁止将 Windows 系统核心目录或驱动器根目录直接作为扩展加载！");
        }
        return;
    }

    ExtensionInfo parsedInfo;
    std::string parseErr;
    if (!ParseManifest(normalizedInput, parsedInfo, parseErr)) {
        if (callback) {
            callback(false, L"扩展清单验证失败: " + StringUtils::Utf8ToWide(parseErr));
        }
        return;
    }

    std::wstring loadDir = parsedInfo.folderPath;
    m_profile7->AddBrowserExtension(
        loadDir.c_str(),
        Callback<ICoreWebView2ProfileAddBrowserExtensionCompletedHandler>(
            [this, parsedInfo, loadDir, callback](HRESULT errorCode, ICoreWebView2BrowserExtension* extension) -> HRESULT {
                if (FAILED(errorCode) || !extension) {
                    wchar_t hexCode[32]{};
                    swprintf_s(hexCode, L"0x%08X", static_cast<unsigned int>(errorCode));
                    std::wstring errMsg = L"WebView2 加载扩展程序失败 (错误代码: " + std::wstring(hexCode) + L")。\n";
                    if (errorCode == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) {
                        errMsg += L"找不到 manifest.json 或清单文件格式无效。";
                    } else if (errorCode == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED)) {
                        errMsg += L"当前 WebView2 环境未启用扩展支持，请重启浏览器重试。";
                    } else if (errorCode == E_ACCESSDENIED) {
                        errMsg += L"无法加载带有下划线 '_' 前缀保留目录的扩展程序。";
                    } else {
                        errMsg += L"请检查扩展是否兼容（WebView2 仅支持已解压的未打包扩展文件夹）。";
                    }
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
                info.folderPath = loadDir;
                info.comExtension = extension;

                ExtensionConfigItem cfgItem;
                cfgItem.id = StringUtils::WideToUtf8(info.id);
                cfgItem.name = StringUtils::WideToUtf8(info.name);
                cfgItem.folderPath = info.folderPath;
                cfgItem.enabled = info.isEnabled;
                Config::Instance().AddOrUpdateExtensionConfig(cfgItem);

                // Refresh internal cache and synchronize list
                RefreshExtensions([callback, info](bool) {
                    std::wstring successMsg = L"扩展程序【" + info.name + L"】加载成功！\n\nID: " + info.id +
                        L"\n版本: " + info.version +
                        L"\n路径: " + info.folderPath;
                    if (callback) {
                        callback(true, successMsg);
                    }
                });

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

void ExtensionManager::ReloadExtension(const std::wstring& id, std::function<void(bool)> callback) {
    const ExtensionInfo* info = FindExtension(id);
    if (!info) {
        if (callback) callback(false);
        return;
    }
    bool wasEnabled = info->isEnabled;
    if (wasEnabled) {
        SetExtensionEnabled(id, false, [this, id, callback](bool) {
            SetExtensionEnabled(id, true, [this, callback](bool ok) {
                RefreshExtensions([callback, ok](bool) {
                    if (callback) callback(ok);
                });
            });
        });
    } else {
        SetExtensionEnabled(id, true, [this, callback](bool ok) {
            RefreshExtensions([callback, ok](bool) {
                if (callback) callback(ok);
            });
        });
    }
}

void ExtensionManager::OpenExtensionOptions(const std::wstring& id, ICoreWebView2* webView) {
    ICoreWebView2* targetWebView = webView ? webView : m_webView.get();
    if (!targetWebView) return;

    std::wstring optionsPage = L"";
    std::wstring folderPath = L"";
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& item : m_extensions) {
            if (item.id == id) {
                optionsPage = item.optionsPage;
                folderPath = item.folderPath;
                break;
            }
        }
    }

    if (folderPath.empty()) {
        auto saved = Config::Instance().GetInstalledExtensions();
        std::string idUtf8 = StringUtils::WideToUtf8(id);
        for (const auto& cfg : saved) {
            if (cfg.id == idUtf8) {
                folderPath = cfg.folderPath;
                break;
            }
        }
    }

    if (optionsPage.empty()) {
        optionsPage = L"options.html";
    }

    // Try file:/// URI if local file exists on disk, otherwise chrome-extension://
    std::filesystem::path optFile = std::filesystem::path(folderPath) / optionsPage;
    std::error_code ec;
    if (std::filesystem::exists(optFile, ec)) {
        std::wstring fileUri = L"file:///" + optFile.wstring();
        for (auto& c : fileUri) {
            if (c == L'\\') c = L'/';
        }
        targetWebView->Navigate(fileUri.c_str());
    } else {
        std::wstring url = L"chrome-extension://" + id + L"/" + optionsPage;
        targetWebView->Navigate(url.c_str());
    }
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

    if (folderPath.empty()) {
        auto saved = Config::Instance().GetInstalledExtensions();
        std::string idUtf8 = StringUtils::WideToUtf8(id);
        for (const auto& cfg : saved) {
            if (cfg.id == idUtf8) {
                folderPath = cfg.folderPath;
                break;
            }
        }
    }

    if (!folderPath.empty()) {
        std::wstring clean = NormalizeExtensionPath(folderPath);
        ShellExecuteW(nullptr, L"open", clean.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
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

    UINT dpi = 96;
    if (hWndParent && IsWindow(hWndParent)) {
        dpi = GetDpiForWindow(hWndParent);
    }
    if (dpi == 0) dpi = 96;

    int dlgW = MulDiv(820, dpi, 96);
    int dlgH = MulDiv(620, dpi, 96);

    RECT parentRect{ 0, 0, 1024, 768 };
    if (hWndParent && IsWindow(hWndParent)) {
        GetWindowRect(hWndParent, &parentRect);
    }

    int posX = parentRect.left + ((parentRect.right - parentRect.left) - dlgW) / 2;
    int posY = parentRect.top + ((parentRect.bottom - parentRect.top) - dlgH) / 2;
    if (posX < 0) posX = 50;
    if (posY < 0) posY = 50;

    ExtDlgContext ctx;
    ctx.dpi = dpi;

    HWND hDlg = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        kDlgClassName,
        L"🧩 扩展程序管理中心 (Chrome Extensions)",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MAXIMIZEBOX | WS_MINIMIZEBOX | WS_VISIBLE,
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
