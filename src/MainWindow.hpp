#pragma once

#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <memory>
#include <string>
#include <vector>
#include "Commands.hpp"
#include "Icons.hpp"
#include "Library.hpp"
#include "WebViewManager.hpp"

namespace UltraLight {

struct Tab {
    int id = 0;
    std::unique_ptr<WebViewManager> view;
    std::wstring title;
    std::wstring url;
    bool loading = false;
    bool audio = false;
    bool muted = false;
    bool suspended = false;
    bool suspendPending = false;
    bool canGoBack = false;
    bool canGoForward = false;
    bool readerAvailable = false;
    std::wstring readerSource;          // article URL while the reader page is shown
    std::unique_ptr<Gdiplus::Bitmap> favicon;
    std::string faviconDataUrl;
    std::string thumbDataUrl;
    ULONGLONG lastActive = 0;
    ULONGLONG loadStarted = 0;
    std::wstring zoomHost;              // host whose saved zoom was last applied
    bool applyingZoom = false;
};

// Thumbnail produced on a worker thread and posted back with WM_APP_THUMBNAIL.
struct ThumbnailMessage {
    int tabId = 0;
    std::string dataUrl;
    std::function<void()> done;
};

// Lightweight WebView2 host for built-in panels (sidebar, tab overview).
struct PanelView {
    wil::com_ptr<ICoreWebView2Controller> controller;
    wil::com_ptr<ICoreWebView2> webView;
    bool creating = false;
    bool ready = false;
};

class MainWindow {
public:
    explicit MainWindow(bool isPrivate);
    ~MainWindow();
    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    bool Create(HINSTANCE hInstance, int nCmdShow, const std::vector<std::wstring>& initialUrls);
    HWND GetHwnd() const { return m_hWnd; }
    bool IsPrivate() const { return m_private; }
    bool IsDestroyed() const { return m_destroyed; }
    void SetFullScreen(bool enable);
    void ToggleFullScreen();
    bool IsFullScreen() const { return m_isFullScreen; }

    // App-level hooks
    ICoreWebView2* ActiveWebView() const;
    UINT32 BrowserProcessId() const;
    void OnLibraryChanged();
    void OpenInNewTab(const std::wstring& url, bool activate);
    void FocusWindow();

private:
    static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK AddressBarSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData);
    static LRESULT CALLBACK FindEditSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData);
    static LRESULT CALLBACK SuggestWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK ToastWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);
    void OnCommand(WORD id);
    void OnClose();

    // ---- Tabs (MainWindowTabs.cpp)
    Tab* NewTab(const std::wstring& url, bool activate, int insertAfterId = 0,
                std::function<void(Tab&)> onReady = nullptr);
    void CreateTabView(int tabId, std::function<void(Tab&)> onReady);
    void WireTab(Tab& tab);
    Tab* FindTab(int id);
    const Tab* FindTab(int id) const;
    int IndexOf(int id) const;
    Tab* ActiveTab();
    const Tab* ActiveTab() const;
    void ActivateTab(int id);
    void CloseTab(int id);
    void CloseOtherTabs(int keepId, bool onlyRight);
    void MoveTab(int id, int newIndex);
    void DuplicateTab(int id);
    void ReopenClosedTab();
    void SuspendBackgroundTabs(bool force);
    void ResumeTab(Tab& tab);
    void SetTabMuted(Tab& tab, bool muted);
    void OnTabUrlChanged(Tab& tab);
    void OnTabNavigationCompleted(Tab& tab, bool success, const std::wstring& uri);
    void OnTabFavicon(Tab& tab, const std::wstring& pageUri, const std::vector<std::uint8_t>& png);
    void HandleNewWindowRequest(int openerId, ICoreWebView2NewWindowRequestedEventArgs* args);
    COREWEBVIEW2_PERMISSION_STATE PermissionFor(COREWEBVIEW2_PERMISSION_KIND kind, const std::wstring& uri);
    void CaptureThumbnail(int tabId, std::function<void()> done);
    void ApplySiteZoom(Tab& tab);
    void ToggleReader();
    void CheckReaderAvailability(int tabId);
    void NavigateActive(const std::wstring& input);
    std::wstring NewTabUrl() const;
    void UpdateWindowTitle();
    void TabsChanged();             // repaint + notify panels (debounced)
    void FlushPanels();
    void PrepareSpareTab();
    Tab* TakeSpareTab(int insertAfterId);
    void OnThumbnailReady(int tabId, std::string dataUrl);

    // ---- Toolbar (MainWindowPaint.cpp)
    enum class Hit { None, Traffic, Sidebar, Back, Forward, Share, NewTab, Overview, Menu,
                     Tab, TabClose, TabAudio, Reader, Reload, Address, Overflow,
                     FindPrev, FindNext, FindDone };
    struct HitResult { Hit kind = Hit::None; int tabId = 0; };
    struct TabSlot { int id = 0; RECT rc{}; RECT close{}; RECT audio{}; };
    void LayoutToolbar(int width);
    void UpdateLayout();
    void PaintToolbar(HDC hdc, int width);
    void PaintFindBar(HDC hdc);
    HitResult HitTest(POINT pt) const;
    void OnToolbarMouseMove(POINT pt);
    void OnToolbarButtonDown(POINT pt);
    void OnToolbarButtonUp(POINT pt);
    void OnToolbarMiddleUp(POINT pt);
    void OnToolbarRightUp(POINT pt);
    void InvalidateToolbar();
    int S(int v) const { return MulDiv(v, static_cast<int>(m_dpi), 96); }
    RECT ContentRect() const;       // area for the active tab (excludes sidebar and find bar)
    void UpdateDpiScaling(UINT dpi);
    void ApplyModernTheme();

    // ---- Address bar
    void BeginAddressEdit(const std::wstring& text, bool selectAll);
    void EndAddressEdit(bool focusPage);
    void CommitAddress();
    void UpdateSuggestions();
    void HideSuggestions();
    void MoveSuggestion(int delta);
    std::wstring DisplayUrl(const Tab& tab) const;

    // ---- Find bar
    void ShowFindBar();
    void HideFindBar();
    void RunFind(const wchar_t* action);

    // ---- Panels (MainWindowPages.cpp)
    void ToggleSidebar(const wchar_t* segment = nullptr);
    void ToggleOverview();
    void HideOverview(int activateId = 0);
    void EnsurePanel(PanelView& panel, const std::wstring& page, std::function<void()> onReady);
    void LayoutPanels();
    void HandlePageMessage(ICoreWebView2* source, const std::wstring& json);
    void PostEvent(ICoreWebView2* target, const char* event, const std::string& dataJson);
    void BroadcastToInternalPages(const char* event, const std::string& dataJson);
    std::string TabsJson(bool withThumbs) const;
    void OpenInternalPage(const std::wstring& page, bool newTab);

    // ---- Menus (MainWindowMenus.cpp)
    void ShowMainMenu();
    void ShowShareMenu();
    void ShowTabMenu(int tabId, POINT screenPt);
    void AppendItem(HMENU menu, UINT id, const std::wstring& text, Icon icon, UINT flags = 0);
    void AppendSubmenu(HMENU menu, HMENU sub, const std::wstring& text, Icon icon, UINT flags = 0);
    HMENU BuildZoomMenu();
    HMENU BuildSiteMenu();
    HMENU BuildBlockerMenu();
    HMENU BuildSoundMenu();
    HMENU BuildDnsMenu();
    HMENU BuildIdentityMenu();
    HMENU BuildPowerMenu();
    void TrackMenu(HMENU menu, const RECT& anchor, bool alignRight);
    bool HandleMenuCommand(WORD id);
    void UpdateSite(const std::function<void(SiteSettings&)>& change);
    std::string ActiveHost() const;
    void AddBookmark(const char* folder);
    void AddToReadingList();
    void ShowAboutDialog();
    void ShowToast(const std::wstring& text);

    HWND m_hWnd = nullptr;
    HINSTANCE m_hInstance = nullptr;
    UINT m_dpi = 96;
    bool m_private = false;
    bool m_destroyed = false;
    bool m_closing = false;
    std::vector<std::wstring> m_initialUrls;

    // Tabs
    std::vector<std::unique_ptr<Tab>> m_tabs;
    int m_activeId = 0;
    int m_nextTabId = 1;
    std::vector<std::wstring> m_closedUrls;
    std::unique_ptr<Tab> m_spare;       // pre-warmed new-tab page, shown instantly on Ctrl+T
    bool m_panelsDirty = false;

    // Toolbar geometry
    int m_topbarHeight = 52;
    RECT m_rcTrafficClose{}, m_rcTrafficMin{}, m_rcTrafficMax{}, m_rcTrafficGroup{};
    RECT m_rcSidebarBtn{}, m_rcBack{}, m_rcForward{}, m_rcShare{}, m_rcNewTab{}, m_rcOverviewBtn{}, m_rcMenuBtn{};
    RECT m_rcPrivateBadge{};
    RECT m_rcStrip{};
    std::vector<TabSlot> m_slots;
    RECT m_rcActive{}, m_rcReader{}, m_rcReload{}, m_rcAddressText{}, m_rcOverflow{};
    int m_overflowCount = 0;
    HitResult m_hover;
    HitResult m_pressed;
    int m_trafficHoveredBtn = 0;
    int m_trafficPressedBtn = 0;
    bool m_isTrafficGroupHovered = false;
    bool m_isWindowActive = true;
    bool m_trackingMouse = false;

    // Tab dragging
    int m_dragTabId = 0;
    int m_dragStartX = 0;
    int m_dragOffsetX = 0;
    bool m_dragging = false;

    // Fonts
    HFONT m_hUiFont = nullptr;
    HFONT m_hAddressFont = nullptr;
    HFONT m_hSmallFont = nullptr;
    HBRUSH m_hBrAddressBg = nullptr;

    // Address bar
    HWND m_hEditAddress = nullptr;
    bool m_isAddressFocused = false;
    HWND m_hSuggest = nullptr;
    std::vector<Suggestion> m_suggestions;
    std::wstring m_suggestQuery;
    int m_suggestSel = 0;
    int m_suggestHover = -1;
    bool m_suppressSuggest = false;

    // Find bar
    bool m_findVisible = false;
    HWND m_hFindEdit = nullptr;
    int m_findCount = 0;
    int m_findIndex = 0;
    std::wstring m_findQuery;
    RECT m_rcFindBar{}, m_rcFindPrev{}, m_rcFindNext{}, m_rcFindDone{}, m_rcFindStatus{};

    // Panels
    PanelView m_sidebar;
    PanelView m_overview;
    bool m_sidebarVisible = false;
    bool m_overviewVisible = false;
    std::wstring m_pendingSidebarSegment;

    // Toast
    HWND m_hToast = nullptr;
    std::wstring m_toastText;

    // Fullscreen
    bool m_isFullScreen = false;
    WINDOWPLACEMENT m_wpPrev{ sizeof(WINDOWPLACEMENT) };
    DWORD m_dwStylePrev = 0;

    // Power
    bool m_backgrounded = false;
    ULONGLONG m_lastInteractionTick = 0;

    static constexpr UINT_PTR IDT_INACTIVITY_CHECK = 5001;
    static constexpr UINT_PTR IDT_AUDIO_STOP_GRACE = 5002;
    static constexpr UINT_PTR IDT_TAB_SUSPEND = 5003;
    static constexpr UINT_PTR IDT_PROGRESS = 5004;
    static constexpr UINT_PTR IDT_FIND_DEBOUNCE = 5005;
    static constexpr UINT_PTR IDT_TOAST = 5006;
    static constexpr UINT_PTR IDT_THUMB = 5007;
    static constexpr UINT_PTR IDT_LIBRARY_SAVE = 5008;
    static constexpr UINT_PTR IDT_PANELS = 5009;
    static constexpr UINT_PTR IDT_SPARE = 5010;
    static constexpr UINT WM_APP_THUMBNAIL = WM_APP + 1;

    // Toolbar paint cache
    HBITMAP m_paintBitmap = nullptr;
    SIZE m_paintSize{};
    std::unique_ptr<Gdiplus::Font> m_gpUiFont, m_gpAddressFont, m_gpSmallFont;
    void EnsurePaintFonts(HDC hdc);
};

} // namespace UltraLight
