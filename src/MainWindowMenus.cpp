// The single "⋯" browser menu (all options), the share menu and the tab context
// menu, plus the commands that only menus reach.

#include "MainWindow.hpp"
#include "AppShell.hpp"
#include "Config.hpp"
#include "DnsManager.hpp"
#include "WarpManager.hpp"
#include "ElementBlocker.hpp"
#include "InternalPages.hpp"
#include "NativeRequestFilter.hpp"
#include "StringUtils.hpp"

#include <shellapi.h>
#include <cmath>

#ifndef ULB_VERSION
#define ULB_VERSION "dev"
#endif
#define ULB_VERSION_STRING L"" ULB_VERSION

namespace UltraLight {

namespace {

const int kZoomPresets[] = {50, 67, 75, 80, 90, 100, 110, 125, 150, 175, 200, 250, 300};

std::wstring Percent(double zoom) {
    return std::to_wstring(static_cast<int>(std::lround(zoom * 100))) + L"%";
}

} // namespace

std::string MainWindow::ActiveHost() const {
    const Tab* t = ActiveTab();
    if (!t) return {};
    const std::wstring url = DisplayUrl(*t);
    return url.empty() ? std::string() : Library::HostOf(StringUtils::WideToUtf8(url));
}

void MainWindow::AppendItem(HMENU menu, UINT id, const std::wstring& text, Icon icon, UINT flags) {
    AppendMenuW(menu, MF_STRING | flags, id, text.c_str());
    MENUITEMINFOW mii{sizeof(mii)};
    mii.fMask = MIIM_BITMAP;
    mii.hbmpItem = Icons::MenuBitmap(icon, GetSystemMetrics(SM_CXSMICON));
    SetMenuItemInfoW(menu, id, FALSE, &mii);
}

void MainWindow::AppendSubmenu(HMENU menu, HMENU sub, const std::wstring& text, Icon icon, UINT flags) {
    AppendMenuW(menu, MF_POPUP | flags, reinterpret_cast<UINT_PTR>(sub), text.c_str());
    MENUITEMINFOW mii{sizeof(mii)};
    mii.fMask = MIIM_BITMAP;
    mii.hbmpItem = Icons::MenuBitmap(icon, GetSystemMetrics(SM_CXSMICON));
    SetMenuItemInfoW(menu, GetMenuItemCount(menu) - 1, TRUE, &mii);
}

void MainWindow::TrackMenu(HMENU menu, const RECT& anchor, bool alignRight) {
    POINT pt{alignRight ? anchor.right : anchor.left, anchor.bottom + S(4)};
    ClientToScreen(m_hWnd, &pt);
    TPMPARAMS params{sizeof(params)};
    RECT exclude = anchor;
    MapWindowPoints(m_hWnd, nullptr, reinterpret_cast<POINT*>(&exclude), 2);
    params.rcExclude = exclude;
    const UINT cmd = TrackPopupMenuEx(menu, (alignRight ? TPM_RIGHTALIGN : TPM_LEFTALIGN) | TPM_TOPALIGN | TPM_RETURNCMD | TPM_VERTICAL,
                                      pt.x, pt.y, m_hWnd, &params);
    DestroyMenu(menu);
    m_pressed = {};
    InvalidateToolbar();
    if (cmd) OnCommand(static_cast<WORD>(cmd));
}

HMENU MainWindow::BuildZoomMenu() {
    HMENU m = CreatePopupMenu();
    const Tab* t = ActiveTab();
    const double zoom = t && t->view ? t->view->GetZoomFactor() : 1.0;
    AppendMenuW(m, MF_STRING, IDM_ZOOM_RESET, L"实际大小\tCtrl+0");
    AppendMenuW(m, MF_STRING, IDM_ZOOM_IN, L"放大\tCtrl+加号");
    AppendMenuW(m, MF_STRING, IDM_ZOOM_OUT, L"缩小\tCtrl+减号");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    for (size_t i = 0; i < std::size(kZoomPresets); ++i) {
        const bool on = std::abs(kZoomPresets[i] - zoom * 100) < 1.5;
        AppendMenuW(m, MF_STRING | (on ? MF_CHECKED : 0), IDM_ZOOM_SET_BASE + static_cast<UINT>(i), (std::to_wstring(kZoomPresets[i]) + L"%").c_str());
    }
    return m;
}

HMENU MainWindow::BuildSiteMenu() {
    HMENU m = CreatePopupMenu();
    const std::string host = ActiveHost();
    if (host.empty() || m_private) {
        AppendMenuW(m, MF_STRING | MF_GRAYED, 0, m_private ? L"无痕浏览窗口不保存网站设置" : L"此页面没有网站设置");
        return m;
    }
    const SiteSettings s = AppShell::Instance().Lib().Site(host);
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, StringUtils::Utf8ToWide(host).c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (s.autoReader ? MF_CHECKED : 0), IDM_SITE_AUTO_READER, L"可用时使用阅读器");
    AppendMenuW(m, MF_STRING | (s.adblock ? MF_CHECKED : 0), IDM_SITE_ADBLOCK, L"启用内容拦截器");
    AppendMenuW(m, MF_STRING | (s.zoom > 0 ? 0 : MF_GRAYED), IDM_SITE_ZOOM_RESET,
                (L"页面缩放\t" + (s.zoom > 0 ? Percent(s.zoom) : std::wstring(L"100%"))).c_str());
    auto perm = [&](const wchar_t* label, const std::string& value, UINT ask, UINT allow, UINT deny) {
        HMENU sub = CreatePopupMenu();
        AppendMenuW(sub, MF_STRING | (value == "ask" ? MF_CHECKED : 0), ask, L"询问");
        AppendMenuW(sub, MF_STRING | (value == "deny" ? MF_CHECKED : 0), deny, L"拒绝");
        AppendMenuW(sub, MF_STRING | (value == "allow" ? MF_CHECKED : 0), allow, L"允许");
        const std::wstring text = std::wstring(label) + L"\t" + (value == "allow" ? L"允许" : value == "deny" ? L"拒绝" : L"询问");
        AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(sub), text.c_str());
    };
    perm(L"摄像头", s.camera, IDM_SITE_CAMERA_ASK, IDM_SITE_CAMERA_ALLOW, IDM_SITE_CAMERA_DENY);
    perm(L"麦克风", s.microphone, IDM_SITE_MIC_ASK, IDM_SITE_MIC_ALLOW, IDM_SITE_MIC_DENY);
    perm(L"位置", s.location, IDM_SITE_LOC_ASK, IDM_SITE_LOC_ALLOW, IDM_SITE_LOC_DENY);
    HMENU popups = CreatePopupMenu();
    AppendMenuW(popups, MF_STRING | (s.popups != "allow" ? MF_CHECKED : 0), IDM_SITE_POPUP_BLOCK, L"拦截并通知");
    AppendMenuW(popups, MF_STRING | (s.popups == "allow" ? MF_CHECKED : 0), IDM_SITE_POPUP_ALLOW, L"允许");
    AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(popups), (std::wstring(L"弹出式窗口\t") + (s.popups == "allow" ? L"允许" : L"拦截并通知")).c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (s.IsDefault() ? MF_GRAYED : 0), IDM_SITE_RESET, L"移除此网站的设置");
    return m;
}

HMENU MainWindow::BuildBlockerMenu() {
    HMENU m = CreatePopupMenu();
    const bool enabled = NativeRequestFilter::Instance().IsEnabled();
    AppendMenuW(m, MF_STRING | (enabled ? MF_CHECKED : 0), IDM_BLOCKER_TOGGLE_NATIVE, L"启用内容拦截器（所有网站）");
    const std::string host = ActiveHost();
    if (!host.empty() && !m_private) {
        const bool siteOn = AppShell::Instance().Lib().Site(host).adblock;
        AppendMenuW(m, MF_STRING | (siteOn ? MF_CHECKED : 0) | (enabled ? 0 : MF_GRAYED), IDM_SITE_ADBLOCK,
                    (L"在“" + StringUtils::Utf8ToWide(host) + L"”上启用").c_str());
    }
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, (L"已阻止 " + std::to_wstring(NativeRequestFilter::Instance().GetBlockedCount()) + L" 个跟踪器和广告请求").c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_BLOCKER_PICKER, L"隐藏干扰项目…\tCtrl+Shift+H");
    AppendMenuW(m, MF_STRING | (host.empty() ? MF_GRAYED : 0), IDM_BLOCKER_CLEAR_RULES, L"显示隐藏的项目");
    return m;
}

HMENU MainWindow::BuildSoundMenu() {
    const auto& settings = Config::Instance().GetSettings();
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | (settings.systemAudioPassthrough ? MF_CHECKED : 0), IDM_AUDIO_NATIVE, L"原声输出（配合 Windows 空间音效，切换会刷新）");
    AppendMenuW(m, MF_STRING | (!settings.systemAudioPassthrough ? MF_CHECKED : 0), IDM_AUDIO_ENHANCED, L"浏览器音频增强（切换会刷新）");
    HMENU dsp = CreatePopupMenu();
    const bool surround = settings.enableSurroundSound;
    AppendMenuW(dsp, MF_STRING | (surround ? MF_CHECKED : 0), IDM_SURROUND_TOGGLE, L"2 声道虚拟环绕");
    AppendMenuW(dsp, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(dsp, MF_STRING | (surround && settings.surroundSoundMode == "dialogue" ? MF_CHECKED : 0), IDM_AUDIO_DIALOGUE, L"对白");
    AppendMenuW(dsp, MF_STRING | (surround && settings.surroundSoundMode == "light" ? MF_CHECKED : 0), IDM_SURROUND_MODE_LIGHT, L"轻柔");
    AppendMenuW(dsp, MF_STRING | (surround && settings.surroundSoundMode == "standard" ? MF_CHECKED : 0), IDM_SURROUND_MODE_STANDARD, L"标准");
    AppendMenuW(dsp, MF_STRING | (surround && settings.surroundSoundMode == "cinema" ? MF_CHECKED : 0), IDM_SURROUND_MODE_CINEMA, L"影院");
    AppendMenuW(dsp, MF_SEPARATOR, 0, nullptr);
    HMENU dev = CreatePopupMenu();
    AppendMenuW(dev, MF_STRING | (settings.audioDeviceMode == "auto" ? MF_CHECKED : 0), IDM_SURROUND_DEV_AUTO, L"自动检测");
    AppendMenuW(dev, MF_STRING | (settings.audioDeviceMode == "headphones" ? MF_CHECKED : 0), IDM_SURROUND_DEV_HEADPHONES, L"耳机");
    AppendMenuW(dev, MF_STRING | (settings.audioDeviceMode == "speakers" ? MF_CHECKED : 0), IDM_SURROUND_DEV_SPEAKERS, L"音箱");
    AppendMenuW(dsp, MF_POPUP, reinterpret_cast<UINT_PTR>(dev), L"输出设备");
    AppendMenuW(dsp, MF_STRING | (settings.enableVocalBoost ? MF_CHECKED : 0), IDM_SURROUND_VOCAL_BOOST, L"人声增强");
    AppendMenuW(dsp, MF_STRING | (settings.enableDeEsser ? MF_CHECKED : 0), IDM_AUDIO_DEESSER, L"齿音抑制");
    AppendMenuW(dsp, MF_STRING | (settings.enableNightMode ? MF_CHECKED : 0), IDM_AUDIO_NIGHT, L"夜间模式（减小音量起伏）");
    AppendMenuW(dsp, MF_STRING | (settings.enableMonoDownmix ? MF_CHECKED : 0), IDM_SURROUND_MONO_DOWNMIX, L"单声道合并");
    HMENU vol = CreatePopupMenu();
    const struct { UINT id; double v; const wchar_t* t; } levels[] = {
        {IDM_SURROUND_BOOST_100, 1.0, L"100%"}, {IDM_SURROUND_BOOST_150, 1.5, L"150%"},
        {IDM_SURROUND_BOOST_200, 2.0, L"200%"}, {IDM_SURROUND_BOOST_300, 3.0, L"300%（可能失真）"}};
    for (const auto& l : levels) {
        AppendMenuW(vol, MF_STRING | (std::abs(settings.audioVolumeBoost - l.v) < 0.1 ? MF_CHECKED : 0), l.id, l.t);
    }
    AppendMenuW(dsp, MF_POPUP, reinterpret_cast<UINT_PTR>(vol), L"音量放大");
    AppendMenuW(m, MF_POPUP | (settings.systemAudioPassthrough ? MF_GRAYED : 0), reinterpret_cast<UINT_PTR>(dsp), L"增强选项");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_AUDIO_DIAGNOSTICS, L"播放诊断");
    AppendMenuW(m, MF_STRING, IDM_AUDIO_WINDOWS_SETTINGS, L"Windows 声音设置…");
    return m;
}

HMENU MainWindow::BuildDnsMenu() {
    const auto& settings = Config::Instance().GetSettings();
    const auto& providers = DnsManager::Instance().GetProviders();
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | (settings.enablePublicDns ? MF_CHECKED : 0), IDM_DNS_TOGGLE_ENABLE, L"使用加密 DNS（DoH）");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    for (size_t i = 0; i < providers.size() && i < 50; ++i) {
        const bool on = settings.enablePublicDns && settings.selectedDnsProvider == providers[i].id;
        AppendMenuW(m, MF_STRING | (on ? MF_CHECKED : 0), IDM_DNS_SELECT_BASE + static_cast<UINT>(i), providers[i].name.c_str());
    }
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_DNS_OPEN_SETTINGS, L"高级设置…");
    return m;
}

HMENU MainWindow::BuildWarpMenu() {
    const auto& settings = Config::Instance().GetSettings();
    HMENU m = CreatePopupMenu();
    auto& warp = WarpManager::Instance();
    const std::wstring state = L"状态：" + warp.StatusText();
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, state.c_str());
    const std::wstring endpoint = warp.EndpointText();
    if (!endpoint.empty()) AppendMenuW(m, MF_STRING | MF_GRAYED, 0, (L"入口：" + endpoint).c_str());
    const bool running = warp.GetState() == WarpManager::State::Up || warp.GetState() == WarpManager::State::Down;
    AppendMenuW(m, MF_STRING | (running && !warp.Scanning() ? 0 : MF_GRAYED), IDM_WARP_RESCAN,
                warp.Scanning() ? L"正在优选 IP…" : L"重新优选 IP");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (settings.warpEnabled ? MF_CHECKED : 0), IDM_WARP_TOGGLE, L"使用 Cloudflare WARP");
    AppendMenuW(m, MF_STRING | (settings.warpFailClosed ? MF_CHECKED : 0) | (settings.warpEnabled ? 0 : MF_GRAYED),
                IDM_WARP_FAIL_CLOSED, L"WARP 断开时阻止联网");
    return m;
}

HMENU MainWindow::BuildIdentityMenu() {
    HMENU m = CreatePopupMenu();
    const bool mac = Config::Instance().GetSettings().userAgentProfile == "macos-edge";
    AppendMenuW(m, MF_STRING | (!mac ? MF_CHECKED : 0), IDM_UA_DEFAULT, L"默认（Microsoft Edge — Windows）");
    AppendMenuW(m, MF_STRING | (mac ? MF_CHECKED : 0), IDM_UA_MACOS_EDGE, L"Microsoft Edge — macOS");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_UA_SELFTEST, L"检查当前用户代理…");
    return m;
}

HMENU MainWindow::BuildPowerMenu() {
    const int minutes = Config::Instance().GetSettings().tabSuspendMinutes;
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"后台标签页自动挂起");
    const struct { UINT id; int v; const wchar_t* t; } opts[] = {
        {IDM_SUSPEND_NEVER, 0, L"从不"}, {IDM_SUSPEND_5, 5, L"5 分钟后"}, {IDM_SUSPEND_10, 10, L"10 分钟后"},
        {IDM_SUSPEND_30, 30, L"30 分钟后"}, {IDM_SUSPEND_60, 60, L"1 小时后"}};
    for (const auto& o : opts) AppendMenuW(m, MF_STRING | (minutes == o.v ? MF_CHECKED : 0), o.id, o.t);
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (m_tabs.size() > 1 ? 0 : MF_GRAYED), IDM_SUSPEND_OTHERS, L"立即挂起其他标签页");
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"最小化时自动挂起（播放声音时除外）");
    return m;
}

void MainWindow::ShowMainMenu() {
    const auto& settings = Config::Instance().GetSettings();
    const Tab* tab = ActiveTab();
    const bool webPage = tab && !DisplayUrl(*tab).empty();
    HMENU m = CreatePopupMenu();
    AppendItem(m, IDM_NEW_TAB, L"新建标签页\tCtrl+T", Icon::Plus);
    AppendItem(m, IDM_NEW_WINDOW, L"新建窗口\tCtrl+N", Icon::Window);
    AppendItem(m, IDM_NEW_PRIVATE_WINDOW, L"新建无痕浏览窗口\tCtrl+Shift+N", Icon::Private);
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendItem(m, IDM_BOOKMARKS, L"编辑书签\tCtrl+Shift+B", Icon::Bookmark);
    AppendItem(m, IDM_HISTORY, L"显示所有历史记录\tCtrl+H", Icon::Clock);
    AppendItem(m, IDM_DOWNLOADS, L"显示下载项\tCtrl+J", Icon::Download);
    AppendItem(m, IDM_READING_LIST, L"显示阅读列表", Icon::ReadingList);
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendItem(m, IDM_SIDEBAR, m_sidebarVisible ? L"隐藏边栏\tCtrl+Shift+L" : L"显示边栏\tCtrl+Shift+L", Icon::Sidebar);
    AppendItem(m, IDM_OVERVIEW, L"显示标签页概览\tCtrl+Shift+\\", Icon::Grid);
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendItem(m, IDM_READER, tab && !tab->readerSource.empty() ? L"隐藏阅读器\tCtrl+Shift+R" : L"显示阅读器\tCtrl+Shift+R", Icon::Reader,
               webPage ? 0 : MF_GRAYED);
    AppendItem(m, IDM_FIND, L"查找…\tCtrl+F", Icon::Search);
    const double zoom = tab && tab->view ? tab->view->GetZoomFactor() : 1.0;
    AppendSubmenu(m, BuildZoomMenu(), L"缩放\t" + Percent(zoom), Icon::Zoom);
    AppendItem(m, IDM_PRINT, L"打印…\tCtrl+P", Icon::Print, webPage ? 0 : MF_GRAYED);
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendSubmenu(m, BuildSiteMenu(), L"此网站的设置", Icon::Gear);
    AppendSubmenu(m, BuildBlockerMenu(), L"内容拦截器", Icon::Block);
    AppendSubmenu(m, BuildSoundMenu(), std::wstring(L"声音\t") + (settings.systemAudioPassthrough ? L"原声" : L"增强"), Icon::Sound);
    std::wstring dnsLabel = L"系统";
    if (settings.enablePublicDns) {
        const auto* p = DnsManager::Instance().GetActiveProvider();
        dnsLabel = settings.selectedDnsProvider == "custom" ? L"自定义" : (p ? p->name : L"已开启");
        if (dnsLabel.size() > 14) dnsLabel = dnsLabel.substr(0, 14) + L"…";
    }
    AppendSubmenu(m, BuildDnsMenu(), L"DNS\t" + dnsLabel, Icon::Globe);
    const auto warpState = WarpManager::Instance().GetState();
    const wchar_t* warpLabel = warpState == WarpManager::State::Up ? L"已连接"
        : warpState == WarpManager::State::Off ? L"关闭" : L"未连接";
    AppendSubmenu(m, BuildWarpMenu(), std::wstring(L"Cloudflare WARP\t") + warpLabel, Icon::Shield);
    AppendSubmenu(m, BuildIdentityMenu(), std::wstring(L"用户代理\t") + (settings.userAgentProfile == "macos-edge" ? L"macOS" : L"默认"), Icon::Monitor);
    const int minutes = settings.tabSuspendMinutes;
    AppendSubmenu(m, BuildPowerMenu(), std::wstring(L"节能\t") + (minutes > 0 ? L"后台标签自动挂起" : L"关闭"), Icon::Bolt);
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendItem(m, IDM_TOGGLE_FULLSCREEN, m_isFullScreen ? L"退出全屏幕\tF11" : L"进入全屏幕\tF11", Icon::Fullscreen);
    AppendItem(m, IDM_SETTINGS, L"设置…\tCtrl+,", Icon::Gear);
    AppendItem(m, IDM_ABOUT, L"关于 UltraLightBrowser", Icon::Info);
    TrackMenu(m, m_rcMenuBtn, true);
}

void MainWindow::ShowShareMenu() {
    const Tab* tab = ActiveTab();
    if (!tab || DisplayUrl(*tab).empty()) return;
    const std::string url = StringUtils::WideToUtf8(DisplayUrl(*tab));
    const bool bookmarked = AppShell::Instance().Lib().FindBookmarkByUrl(url) != nullptr;
    HMENU m = CreatePopupMenu();
    AppendItem(m, IDM_SHARE_COPY_URL, L"拷贝链接", Icon::Copy);
    AppendItem(m, IDM_ADD_BOOKMARK, bookmarked ? L"添加书签…\tCtrl+D" : L"添加书签…\tCtrl+D", Icon::Bookmark);
    AppendItem(m, IDM_ADD_FAVORITE, L"添加到个人收藏", Icon::Star);
    AppendItem(m, IDM_ADD_READING, L"添加到阅读列表\tCtrl+Shift+D", Icon::ReadingList);
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendItem(m, IDM_SHARE_OPEN_DEFAULT, L"在默认浏览器中打开页面", Icon::Open);
    TrackMenu(m, m_rcShare, false);
}

void MainWindow::ShowTabMenu(int tabId, POINT screenPt) {
    const Tab* tab = FindTab(tabId);
    if (!tab) return;
    HMENU m = CreatePopupMenu();
    AppendItem(m, IDM_TAB_RELOAD, L"重新载入", Icon::Reload);
    AppendItem(m, IDM_TAB_DUPLICATE, L"复制标签页", Icon::Copy);
    AppendItem(m, IDM_TAB_MUTE, tab->muted ? L"取消标签页静音" : L"将标签页静音", tab->muted ? Icon::Speaker : Icon::SpeakerMute);
    AppendItem(m, IDM_TAB_BOOKMARK, L"为此标签页添加书签…", Icon::Bookmark, DisplayUrl(*tab).empty() ? MF_GRAYED : 0);
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendItem(m, IDM_CLOSE_TAB, L"关闭标签页", Icon::Close);
    AppendMenuW(m, MF_STRING | (m_tabs.size() > 1 ? 0 : MF_GRAYED), IDM_TAB_CLOSE_OTHERS, L"关闭其他标签页");
    AppendMenuW(m, MF_STRING | (IndexOf(tabId) + 1 < static_cast<int>(m_tabs.size()) ? 0 : MF_GRAYED), IDM_TAB_CLOSE_RIGHT, L"关闭右侧的标签页");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendItem(m, IDM_REOPEN_TAB, L"重新打开上次关闭的标签页\tCtrl+Shift+T", Icon::Back, m_closedUrls.empty() ? MF_GRAYED : 0);
    const UINT cmd = TrackPopupMenu(m, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RETURNCMD, screenPt.x, screenPt.y, 0, m_hWnd, nullptr);
    DestroyMenu(m);
    switch (cmd) {
    case IDM_TAB_RELOAD: if (Tab* t = FindTab(tabId); t && t->view) t->view->Reload(); break;
    case IDM_TAB_DUPLICATE: DuplicateTab(tabId); break;
    case IDM_TAB_MUTE: if (Tab* t = FindTab(tabId)) SetTabMuted(*t, !t->muted); break;
    case IDM_TAB_BOOKMARK:
        if (Tab* t = FindTab(tabId)) {
            AppShell::Instance().Lib().AddBookmark(StringUtils::WideToUtf8(t->title), StringUtils::WideToUtf8(DisplayUrl(*t)), kBookmarksFolder, AppShell::NowMs());
            AppShell::Instance().LibraryChanged();
            ShowToast(L"已添加书签");
        }
        break;
    case IDM_CLOSE_TAB: CloseTab(tabId); break;
    case IDM_TAB_CLOSE_OTHERS: CloseOtherTabs(tabId, false); break;
    case IDM_TAB_CLOSE_RIGHT: CloseOtherTabs(tabId, true); break;
    case IDM_REOPEN_TAB: ReopenClosedTab(); break;
    default: break;
    }
}

void MainWindow::UpdateSite(const std::function<void(SiteSettings&)>& change) {
    const std::string host = ActiveHost();
    if (host.empty() || m_private) return;
    auto& lib = AppShell::Instance().Lib();
    SiteSettings s = lib.Site(host);
    change(s);
    lib.SetSite(host, s);
    AppShell::Instance().SaveLibrarySoon();
}

void MainWindow::AddBookmark(const char* folder) {
    const Tab* t = ActiveTab();
    if (!t || DisplayUrl(*t).empty()) return;
    std::wstring title = t->title;
    if (!t->readerSource.empty() && title.empty()) title = t->readerSource;
    AppShell::Instance().Lib().AddBookmark(StringUtils::WideToUtf8(title), StringUtils::WideToUtf8(DisplayUrl(*t)), folder, AppShell::NowMs());
    AppShell::Instance().LibraryChanged();
    ShowToast(std::string(folder) == kFavoritesFolder ? L"已添加到个人收藏" : L"已添加书签");
}

void MainWindow::AddToReadingList() {
    const Tab* t = ActiveTab();
    if (!t || DisplayUrl(*t).empty()) return;
    AppShell::Instance().Lib().AddReading(StringUtils::WideToUtf8(t->title), StringUtils::WideToUtf8(DisplayUrl(*t)), AppShell::NowMs());
    AppShell::Instance().LibraryChanged();
    ShowToast(L"已添加到阅读列表");
}

void MainWindow::ShowAboutDialog() {
    TASKDIALOGCONFIG dialog{};
    dialog.cbSize = sizeof(dialog);
    dialog.hwndParent = m_hWnd;
    dialog.dwFlags = TDF_ENABLE_HYPERLINKS | TDF_ALLOW_DIALOG_CANCELLATION;
    dialog.dwCommonButtons = TDCBF_CLOSE_BUTTON;
    dialog.pszWindowTitle = L"关于 UltraLightBrowser";
    dialog.pszMainInstruction = L"UltraLightBrowser " ULB_VERSION_STRING;
    dialog.pszContent = L"Windows 原生浏览器 · Microsoft Edge WebView2\n\n项目源码与问题反馈：\n<a href=\"https://github.com/Freecode100Year/UltraLightBrowser\">https://github.com/Freecode100Year/UltraLightBrowser</a>";
    dialog.pfCallback = [](HWND owner, UINT notification, WPARAM, LPARAM link, LONG_PTR) -> HRESULT {
        if (notification == TDN_HYPERLINK_CLICKED && link &&
            std::wstring(reinterpret_cast<LPCWSTR>(link)) == L"https://github.com/Freecode100Year/UltraLightBrowser") {
            ShellExecuteW(owner, L"open", reinterpret_cast<LPCWSTR>(link), nullptr, nullptr, SW_SHOWNORMAL);
        }
        return S_OK;
    };
    if (FAILED(TaskDialogIndirect(&dialog, nullptr, nullptr, nullptr))) {
        MessageBoxW(m_hWnd, L"UltraLightBrowser " ULB_VERSION_STRING L"\n项目：https://github.com/Freecode100Year/UltraLightBrowser", L"关于", MB_OK);
    }
}

bool MainWindow::HandleMenuCommand(WORD id) {
    auto& settings = Config::Instance().GetSettings();
    Tab* tab = ActiveTab();
    auto audioChanged = [&](bool reload) {
        Config::Instance().Save();
        for (auto& t : m_tabs) if (t->view) t->view->UpdateAudioEnhancer(reload && t->id == m_activeId);
    };
    switch (id) {
    case IDM_SITE_AUTO_READER: UpdateSite([](SiteSettings& s) { s.autoReader = !s.autoReader; }); return true;
    case IDM_SITE_ADBLOCK:
        UpdateSite([](SiteSettings& s) { s.adblock = !s.adblock; });
        if (tab && tab->view) tab->view->Reload();
        return true;
    case IDM_SITE_ZOOM_RESET:
        UpdateSite([](SiteSettings& s) { s.zoom = 0; });
        if (tab && tab->view) { tab->applyingZoom = true; tab->view->SetZoomFactor(1.0); tab->applyingZoom = false; }
        return true;
    case IDM_SITE_CAMERA_ASK: UpdateSite([](SiteSettings& s) { s.camera = "ask"; }); return true;
    case IDM_SITE_CAMERA_ALLOW: UpdateSite([](SiteSettings& s) { s.camera = "allow"; }); return true;
    case IDM_SITE_CAMERA_DENY: UpdateSite([](SiteSettings& s) { s.camera = "deny"; }); return true;
    case IDM_SITE_MIC_ASK: UpdateSite([](SiteSettings& s) { s.microphone = "ask"; }); return true;
    case IDM_SITE_MIC_ALLOW: UpdateSite([](SiteSettings& s) { s.microphone = "allow"; }); return true;
    case IDM_SITE_MIC_DENY: UpdateSite([](SiteSettings& s) { s.microphone = "deny"; }); return true;
    case IDM_SITE_LOC_ASK: UpdateSite([](SiteSettings& s) { s.location = "ask"; }); return true;
    case IDM_SITE_LOC_ALLOW: UpdateSite([](SiteSettings& s) { s.location = "allow"; }); return true;
    case IDM_SITE_LOC_DENY: UpdateSite([](SiteSettings& s) { s.location = "deny"; }); return true;
    case IDM_SITE_POPUP_BLOCK: UpdateSite([](SiteSettings& s) { s.popups = "block"; }); return true;
    case IDM_SITE_POPUP_ALLOW: UpdateSite([](SiteSettings& s) { s.popups = "allow"; }); return true;
    case IDM_SITE_RESET: {
        const std::string host = ActiveHost();
        if (!host.empty()) {
            AppShell::Instance().Lib().ClearSite(host);
            AppShell::Instance().SaveLibrarySoon();
            if (tab && tab->view) { tab->applyingZoom = true; tab->view->SetZoomFactor(1.0); tab->applyingZoom = false; }
        }
        return true;
    }
    case IDM_SUSPEND_NEVER: case IDM_SUSPEND_5: case IDM_SUSPEND_10: case IDM_SUSPEND_30: case IDM_SUSPEND_60: {
        const int values[] = {0, 5, 10, 30, 60};
        settings.tabSuspendMinutes = values[id - IDM_SUSPEND_NEVER];
        Config::Instance().Save();
        return true;
    }
    case IDM_SUSPEND_OTHERS:
        SuspendBackgroundTabs(true);
        ShowToast(L"已挂起其他标签页");
        return true;
    case IDM_BLOCKER_PICKER:
        if (tab && tab->view && tab->view->GetWebView()) ElementBlocker::Instance().TogglePickerMode(tab->view->GetWebView());
        return true;
    case IDM_BLOCKER_TOGGLE_NATIVE: {
        const bool next = !NativeRequestFilter::Instance().IsEnabled();
        NativeRequestFilter::Instance().SetEnabled(next);
        ShowToast(next ? L"已启用内容拦截器" : L"已停用内容拦截器");
        return true;
    }
    case IDM_BLOCKER_CLEAR_RULES: {
        const std::string host = ActiveHost();
        if (!host.empty()) {
            Config::Instance().ClearBlockRulesForHost(host);
            ElementBlocker::Instance().UpdateAllRulesScripts();
            if (tab && tab->view) tab->view->Reload();
            ShowToast(L"已显示此网站隐藏的项目");
        }
        return true;
    }
    case IDM_DNS_TOGGLE_ENABLE:
        settings.enablePublicDns = !settings.enablePublicDns;
        Config::Instance().Save();
        DnsManager::Instance().ApplySettings();
        ShowToast(settings.enablePublicDns ? L"已开启加密 DNS，重新启动后完全生效" : L"已关闭加密 DNS，重新启动后完全生效");
        return true;
    case IDM_WARP_TOGGLE:
        settings.warpEnabled = !settings.warpEnabled;
        Config::Instance().Save();
        ShowToast(settings.warpEnabled ? L"已开启 WARP，重新启动 UltraLightBrowser 后生效" : L"已关闭 WARP，重新启动 UltraLightBrowser 后生效");
        return true;
    case IDM_WARP_RESCAN:
        WarpManager::Instance().Rescan();
        ShowToast(L"正在优选 WARP 入口 IP，约需 5 秒");
        return true;
    case IDM_WARP_FAIL_CLOSED:
        settings.warpFailClosed = !settings.warpFailClosed;
        Config::Instance().Save();
        ShowToast(L"重新启动 UltraLightBrowser 后生效");
        return true;
    case IDM_DNS_OPEN_SETTINGS:
        DnsManager::Instance().ShowDnsDialog(m_hWnd);
        return true;
    case IDM_UA_DEFAULT:
    case IDM_UA_MACOS_EDGE: {
        const std::string profile = id == IDM_UA_MACOS_EDGE ? "macos-edge" : "default";
        if (settings.userAgentProfile == profile) return true;
        HRESULT hr = S_OK;
        for (auto& t : m_tabs) {
            if (!t->view || !t->view->IsReady()) continue;
            const HRESULT r = t->view->ApplyUserAgentProfile(profile, true);
            if (FAILED(r)) hr = r;
        }
        if (SUCCEEDED(hr)) {
            settings.userAgentProfile = profile;
            Config::Instance().Save();
        } else {
            MessageBoxW(m_hWnd, L"UA 切换失败，设置未保存。请等待页面初始化，或更新 WebView2 Runtime 后重试。", L"浏览器标识", MB_OK | MB_ICONWARNING);
        }
        return true;
    }
    case IDM_UA_SELFTEST:
        if (tab && tab->view) tab->view->OpenIdentitySelfTest();
        return true;
    case IDM_ABOUT: ShowAboutDialog(); return true;
    case IDM_AUDIO_NATIVE:
    case IDM_AUDIO_ENHANCED: {
        const bool native = id == IDM_AUDIO_NATIVE;
        if (settings.systemAudioPassthrough != native) {
            settings.systemAudioPassthrough = native;
            audioChanged(true);
        }
        return true;
    }
    case IDM_AUDIO_DEESSER: settings.enableDeEsser = !settings.enableDeEsser; audioChanged(false); return true;
    case IDM_AUDIO_NIGHT: settings.enableNightMode = !settings.enableNightMode; audioChanged(false); return true;
    case IDM_AUDIO_DIAGNOSTICS: if (tab && tab->view) tab->view->ShowMediaDiagnostics(); return true;
    case IDM_AUDIO_WINDOWS_SETTINGS: ShellExecuteW(m_hWnd, L"open", L"ms-settings:sound", nullptr, nullptr, SW_SHOWNORMAL); return true;
    case IDM_AUDIO_DIALOGUE: settings.enableSurroundSound = true; settings.surroundSoundMode = "dialogue"; audioChanged(false); return true;
    case IDM_SURROUND_TOGGLE: settings.enableSurroundSound = !settings.enableSurroundSound; audioChanged(false); return true;
    case IDM_SURROUND_MODE_LIGHT: settings.enableSurroundSound = true; settings.surroundSoundMode = "light"; audioChanged(false); return true;
    case IDM_SURROUND_MODE_STANDARD: settings.enableSurroundSound = true; settings.surroundSoundMode = "standard"; audioChanged(false); return true;
    case IDM_SURROUND_MODE_CINEMA: settings.enableSurroundSound = true; settings.surroundSoundMode = "cinema"; audioChanged(false); return true;
    case IDM_SURROUND_VOCAL_BOOST: settings.enableVocalBoost = !settings.enableVocalBoost; audioChanged(false); return true;
    case IDM_SURROUND_BOOST_100: settings.audioVolumeBoost = 1.0; audioChanged(false); return true;
    case IDM_SURROUND_BOOST_150: settings.audioVolumeBoost = 1.5; audioChanged(false); return true;
    case IDM_SURROUND_BOOST_200: settings.audioVolumeBoost = 2.0; audioChanged(false); return true;
    case IDM_SURROUND_BOOST_300: settings.audioVolumeBoost = 3.0; audioChanged(false); return true;
    case IDM_SURROUND_MONO_DOWNMIX: settings.enableMonoDownmix = !settings.enableMonoDownmix; audioChanged(false); return true;
    case IDM_SURROUND_DEV_AUTO: settings.audioDeviceMode = "auto"; audioChanged(false); return true;
    case IDM_SURROUND_DEV_HEADPHONES: settings.audioDeviceMode = "headphones"; audioChanged(false); return true;
    case IDM_SURROUND_DEV_SPEAKERS: settings.audioDeviceMode = "speakers"; audioChanged(false); return true;
    default: break;
    }
    if (id >= IDM_ZOOM_SET_BASE && id < IDM_ZOOM_SET_BASE + std::size(kZoomPresets)) {
        if (tab && tab->view) tab->view->SetZoomFactor(kZoomPresets[id - IDM_ZOOM_SET_BASE] / 100.0);
        return true;
    }
    if (id >= IDM_DNS_SELECT_BASE && id < IDM_DNS_SELECT_BASE + 50) {
        const auto& providers = DnsManager::Instance().GetProviders();
        const size_t idx = id - IDM_DNS_SELECT_BASE;
        if (idx < providers.size()) {
            settings.enablePublicDns = true;
            settings.selectedDnsProvider = providers[idx].id;
            Config::Instance().Save();
            DnsManager::Instance().ApplySettings();
            ShowToast(L"已切换到 " + providers[idx].name + L"，重新启动后完全生效");
        }
        return true;
    }
    return false;
}

} // namespace UltraLight
