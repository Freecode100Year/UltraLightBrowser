// Tab lifecycle: creation (sharing one WebView2 environment), switching,
// closing, background suspension, favicons, thumbnails, reader mode and the
// per-site settings that apply while a page loads.

#include "MainWindow.hpp"
#include "AppShell.hpp"
#include "Config.hpp"
#include "InternalPages.hpp"
#include "PowerManager.hpp"
#include "StringUtils.hpp"

#include <shlwapi.h>
#include <wincrypt.h>
#include <cmath>
#include <fstream>
#include <set>
#include <thread>
#include <nlohmann/json.hpp>

#pragma comment(lib, "crypt32.lib")

using json = nlohmann::json;
using namespace Microsoft::WRL;

namespace UltraLight {

namespace {

std::string Base64(const std::vector<std::uint8_t>& data) {
    if (data.empty()) return {};
    DWORD len = 0;
    CryptBinaryToStringA(data.data(), static_cast<DWORD>(data.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &len);
    std::string out(len, '\0');
    if (!CryptBinaryToStringA(data.data(), static_cast<DWORD>(data.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, out.data(), &len)) return {};
    out.resize(len);
    while (!out.empty() && out.back() == '\0') out.pop_back();
    return out;
}

std::vector<std::uint8_t> ReadStream(IStream* stream) {
    std::vector<std::uint8_t> bytes;
    if (!stream) return bytes;
    LARGE_INTEGER zero{};
    stream->Seek(zero, STREAM_SEEK_SET, nullptr);
    std::uint8_t buf[16384];
    ULONG read = 0;
    while (SUCCEEDED(stream->Read(buf, sizeof(buf), &read)) && read > 0) {
        bytes.insert(bytes.end(), buf, buf + read);
    }
    return bytes;
}

bool EncoderClsid(const wchar_t* mime, CLSID* clsid) {
    UINT num = 0, size = 0;
    if (Gdiplus::GetImageEncodersSize(&num, &size) != Gdiplus::Ok || size == 0) return false;
    std::vector<std::uint8_t> buf(size);
    auto* codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buf.data());
    if (Gdiplus::GetImageEncoders(num, size, codecs) != Gdiplus::Ok) return false;
    for (UINT i = 0; i < num; ++i) {
        if (wcscmp(codecs[i].MimeType, mime) == 0) {
            *clsid = codecs[i].Clsid;
            return true;
        }
    }
    return false;
}

std::string HostUtf8(const std::wstring& url) {
    return Library::HostOf(StringUtils::WideToUtf8(url));
}

std::filesystem::path FaviconPath(const std::string& host) {
    std::string safe;
    for (char c : host) {
        safe += ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-') ? c : '_';
    }
    return InternalPages::FaviconCacheDir() / (StringUtils::Utf8ToWide(safe) + L".png");
}

} // namespace

Tab* MainWindow::FindTab(int id) {
    for (auto& t : m_tabs) if (t->id == id) return t.get();
    if (m_spare && m_spare->id == id) return m_spare.get();
    return nullptr;
}

const Tab* MainWindow::FindTab(int id) const {
    for (const auto& t : m_tabs) if (t->id == id) return t.get();
    if (m_spare && m_spare->id == id) return m_spare.get();
    return nullptr;
}

void MainWindow::PrepareSpareTab() {
    if (m_spare || m_closing || m_destroyed) return;
    auto tab = std::make_unique<Tab>();
    tab->id = m_nextTabId++;
    tab->url = NewTabUrl();
    tab->title = InternalPages::PageName(tab->url) == L"start.html" || IsDefaultHomeUrl(tab->url) ? L"起始页" : L"";
    const int id = tab->id;
    const std::wstring url = tab->url;
    m_spare = std::move(tab);
    CreateTabView(id, [url](Tab& t) {
        t.view->Navigate(url);
        t.view->ApplyMemoryUsageTargetLow();
    });
}

Tab* MainWindow::TakeSpareTab(int insertAfterId) {
    if (!m_spare || !m_spare->view || !m_spare->view->IsReady() || m_spare->url != NewTabUrl()) return nullptr;
    std::unique_ptr<Tab> tab = std::move(m_spare);
    Tab* raw = tab.get();
    raw->lastActive = GetTickCount64();
    int insertAt = static_cast<int>(m_tabs.size());
    if (insertAfterId) {
        const int idx = IndexOf(insertAfterId);
        if (idx >= 0) insertAt = idx + 1;
    }
    m_tabs.insert(m_tabs.begin() + insertAt, std::move(tab));
    // Refresh the pre-rendered start page (favorites, reading list) and warm the next one.
    if (raw->view->GetWebView() && InternalPages::IsInternal(raw->url)) PostEvent(raw->view->GetWebView(), "library", "null");
    SetTimer(m_hWnd, IDT_SPARE, 1500, nullptr);
    return raw;
}

int MainWindow::IndexOf(int id) const {
    for (size_t i = 0; i < m_tabs.size(); ++i) if (m_tabs[i]->id == id) return static_cast<int>(i);
    return -1;
}

Tab* MainWindow::ActiveTab() { return FindTab(m_activeId); }
const Tab* MainWindow::ActiveTab() const { return FindTab(m_activeId); }

std::wstring MainWindow::NewTabUrl() const {
    const auto& s = Config::Instance().GetSettings();
    if (s.newTabPage == "blank") return L"about:blank";
    if (s.newTabPage == "home" && !s.startUrl.empty()) return s.startUrl;
    return InternalPages::Url(L"start.html");
}

Tab* MainWindow::NewTab(const std::wstring& url, bool activate, int insertAfterId, std::function<void(Tab&)> onReady) {
    if (!onReady && activate && url == NewTabUrl()) {
        if (Tab* spare = TakeSpareTab(insertAfterId)) {
            ActivateTab(spare->id);
            return spare;
        }
    }
    auto tab = std::make_unique<Tab>();
    tab->id = m_nextTabId++;
    tab->url = url;
    tab->lastActive = GetTickCount64();
    if ((InternalPages::IsInternal(url) && InternalPages::PageName(url) == L"start.html") || IsDefaultHomeUrl(url)) tab->title = L"起始页";
    Tab* raw = tab.get();
    int insertAt = static_cast<int>(m_tabs.size());
    if (insertAfterId) {
        const int idx = IndexOf(insertAfterId);
        if (idx >= 0) insertAt = idx + 1;
        // Keep consecutive children of the same opener in order.
        while (insertAt < static_cast<int>(m_tabs.size()) && m_tabs[static_cast<size_t>(insertAt)]->lastActive == 0) ++insertAt;
    }
    m_tabs.insert(m_tabs.begin() + insertAt, std::move(tab));
    const int id = raw->id;
    const std::wstring initialUrl = url;
    CreateTabView(id, [this, initialUrl, onReady](Tab& t) {
        if (onReady) onReady(t);
        else if (!initialUrl.empty()) t.view->Navigate(initialUrl);
    });
    if (activate) ActivateTab(id);
    else {
        raw->lastActive = 0;  // never shown yet
        UpdateLayout();
        TabsChanged();
    }
    return raw;
}

void MainWindow::CreateTabView(int tabId, std::function<void(Tab&)> onReady) {
    Tab* tab = FindTab(tabId);
    if (!tab) return;
    tab->view = std::make_unique<WebViewManager>();
    WireTab(*tab);
    const HWND hwnd = m_hWnd;
    AppShell::Instance().WhenEnvironmentReady(hwnd, [this, tabId, onReady](ICoreWebView2Environment* env) {
        Tab* t = FindTab(tabId);
        if (!t || !t->view || m_closing) return;
        t->view->Resize(ContentRect());
        if (!m_spare) SetTimer(m_hWnd, IDT_SPARE, 2500, nullptr);
        t->view->Initialize(m_hWnd, env, m_private, tabId == m_activeId, [this, tabId, onReady]() {
            Tab* ready = FindTab(tabId);
            if (!ready) return;
            if (tabId != m_activeId) {
                ready->view->SetVisible(false);
                ready->view->ApplyMemoryUsageTargetLow();
            } else {
                // The tab may have become active while its controller was being created.
                ready->view->Resize(ContentRect());
                ready->view->SetVisible(!m_overviewVisible);
                if (!m_isAddressFocused && ready->view->GetController()) {
                    ready->view->GetController()->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
                }
            }
            if (onReady) onReady(*ready);
        });
    });
}

void MainWindow::WireTab(Tab& tab) {
    const int id = tab.id;
    WebViewManager& v = *tab.view;

    v.SetTitleChangedCallback([this, id](const std::wstring& title) {
        Tab* t = FindTab(id);
        if (!t) return;
        t->title = title;
        if (InternalPages::PageName(t->url) == L"start.html" || IsDefaultHomeUrl(t->url)) t->title = L"起始页";
        if (!m_private && Config::Instance().GetSettings().saveHistory && !InternalPages::IsInternal(t->url)) {
            AppShell::Instance().Lib().UpdateTitle(StringUtils::WideToUtf8(t->url), StringUtils::WideToUtf8(title));
        }
        if (id == m_activeId) UpdateWindowTitle();
        TabsChanged();
    });
    v.SetSourceChangedCallback([this, id](const std::wstring& uri) {
        Tab* t = FindTab(id);
        if (!t) return;
        const std::wstring previousHost = StringUtils::Utf8ToWide(HostUtf8(t->url));
        t->url = uri;
        t->prefetchUrl.clear();
        if (InternalPages::PageName(uri) != L"reader.html") t->readerSource.clear();
        if (StringUtils::Utf8ToWide(HostUtf8(uri)) != previousHost) {
            t->favicon.reset();
            t->faviconDataUrl.clear();
        }
        t->readerAvailable = false;
        OnTabUrlChanged(*t);
    });
    v.SetNavigationStateCallback([this, id](bool loading) {
        Tab* t = FindTab(id);
        if (!t) return;
        t->loading = loading;
        if (loading) {
            t->loadStarted = GetTickCount64();
            if (id == m_activeId) SetTimer(m_hWnd, IDT_PROGRESS, 100, nullptr);
        }
        InvalidateToolbar();
    });
    v.SetNavigationStartingCallback([this, id](const std::wstring& uri, bool /*userInitiated*/, bool historyNavigation) {
        Tab* t = FindTab(id);
        if (!t || !t->view) return true;
        // Web pages may not navigate into the built-in pages (they can reach the browser bridge).
        if (InternalPages::IsInternal(uri) && !historyNavigation) {
            const bool granted = t->view->ConsumeInternalGrant();
            if (!granted && !InternalPages::IsInternal(t->url)) return false;
        }
        if (id == m_activeId && m_findVisible) {
            m_findQuery.clear();
            m_findCount = m_findIndex = 0;
            if (m_hFindEdit) SetWindowTextW(m_hFindEdit, L"");
            InvalidateRect(m_hWnd, &m_rcFindBar, FALSE);
        }
        return true;
    });
    v.SetNavigationCompletedCallback([this, id](bool success, const std::wstring& uri) {
        if (Tab* t = FindTab(id)) OnTabNavigationCompleted(*t, success, uri);
    });
    v.SetHistoryCallback([this, id](bool back, bool forward) {
        Tab* t = FindTab(id);
        if (!t) return;
        t->canGoBack = back;
        t->canGoForward = forward;
        if (id == m_activeId) InvalidateToolbar();
    });
    v.SetFaviconCallback([this, id](const std::wstring& pageUri, const std::vector<std::uint8_t>& png) {
        if (Tab* t = FindTab(id)) OnTabFavicon(*t, pageUri, png);
    });
    v.SetFullScreenCallback([this](bool fs) { SetFullScreen(fs); });
    v.SetZoomFactorChangedCallback([this, id](double zoom) {
        Tab* t = FindTab(id);
        if (!t || t->applyingZoom) return;
        // Safari remembers page zoom per website.
        const std::string host = HostUtf8(t->url);
        if (!host.empty() && !InternalPages::IsInternal(t->url) && !m_private) {
            auto& lib = AppShell::Instance().Lib();
            SiteSettings s = lib.Site(host);
            s.zoom = std::fabs(zoom - 1.0) < 0.001 ? 0.0 : zoom;
            lib.SetSite(host, s);
        }
    });
    v.SetUserActivityCallback([this]() {
        m_lastInteractionTick = GetTickCount64();
        Tab* active = ActiveTab();
        if (m_backgrounded && active && active->view && active->view->GetWebView()) {
            m_backgrounded = false;
            PowerManager::Instance().HandleActivityResume(active->view->GetController(), active->view->GetWebView());
        }
    });
    v.SetAudioPlayingCallback([this, id](bool playing) {
        Tab* t = FindTab(id);
        if (!t) return;
        t->audio = playing;
        if (playing && t->suspended) t->suspended = false;
        UpdateLayout();
        TabsChanged();
        if (id != m_activeId || !m_backgrounded || !t->view->GetWebView()) return;
        KillTimer(m_hWnd, IDT_AUDIO_STOP_GRACE);
        if (playing) {
            PowerManager::Instance().HandleWindowMinimize(t->view->GetController(), t->view->GetWebView(), true);
        } else {
            // Playlists pause briefly between tracks; suspending at once would stop autoplay.
            SetTimer(m_hWnd, IDT_AUDIO_STOP_GRACE, 60000, nullptr);
        }
    });
    v.SetNewWindowCallback([this, id](ICoreWebView2NewWindowRequestedEventArgs* args) {
        HandleNewWindowRequest(id, args);
    });
    v.SetPermissionCallback([this](COREWEBVIEW2_PERMISSION_KIND kind, const std::wstring& uri) {
        return PermissionFor(kind, uri);
    });
    v.SetInternalMessageCallback([this, id](const std::wstring& msg) {
        Tab* t = FindTab(id);
        if (t && t->view) HandlePageMessage(t->view->GetWebView(), msg);
    });
    v.SetContextActionCallback([this, id](const std::wstring& action, const std::wstring& link, const std::wstring& text) {
        if (action == L"tab") NewTab(link, true, id);
        else if (action == L"background") NewTab(link, false, id);
        else if (action == L"private") AppShell::Instance().OpenWindow(true, {link});
        else if (action == L"reading") {
            AppShell::Instance().Lib().AddReading(StringUtils::WideToUtf8(text), StringUtils::WideToUtf8(link), AppShell::NowMs());
            AppShell::Instance().LibraryChanged();
            ShowToast(L"已添加到阅读列表");
        }
    });
}

void MainWindow::ActivateTab(int id) {
    Tab* next = FindTab(id);
    if (!next) return;
    const int previous = m_activeId;
    if (previous == id && next->view && next->view->IsReady()) {
        UpdateLayout();
        return;
    }
    if (m_isAddressFocused) EndAddressEdit(false);
    if (m_overviewVisible) HideOverview();

    if (Tab* prev = FindTab(previous); prev && prev != next && prev->view) {
        prev->lastActive = GetTickCount64();
        prev->view->SetVisible(false);
        prev->view->ApplyMemoryUsageTargetLow();
    }
    m_activeId = id;
    next->lastActive = GetTickCount64();
    if (next->view) {
        ResumeTab(*next);
        next->view->Resize(ContentRect());
        next->view->SetVisible(true);
        if (auto* wv = next->view->GetWebView()) {
            wil::com_ptr<ICoreWebView2_19> wv19;
            if (SUCCEEDED(wv->QueryInterface(IID_PPV_ARGS(&wv19))) && wv19) {
                wv19->put_MemoryUsageTargetLevel(COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_NORMAL);
            }
        }
        if (auto* c = next->view->GetController()) c->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
    }
    if (m_findVisible) HideFindBar();
    if (next->loading) SetTimer(m_hWnd, IDT_PROGRESS, 100, nullptr);
    UpdateLayout();
    UpdateWindowTitle();
    TabsChanged();
    // A freshly shown page gets its reader check and overview thumbnail once idle.
    next->thumbPending = true;
    ScheduleIdleWork();
}

void MainWindow::CloseTab(int id) {
    const int idx = IndexOf(id);
    if (idx < 0) return;
    if (m_tabs.size() == 1) {
        PostMessageW(m_hWnd, WM_CLOSE, 0, 0);
        return;
    }
    Tab& tab = *m_tabs[static_cast<size_t>(idx)];
    const std::wstring url = !tab.readerSource.empty() ? tab.readerSource : tab.url;
    if (!url.empty() && !InternalPages::IsInternal(url) && url != L"about:blank") {
        m_closedUrls.push_back(url);
        if (m_closedUrls.size() > 25) m_closedUrls.erase(m_closedUrls.begin());
    }
    std::unique_ptr<WebViewManager> view = std::move(tab.view);
    const bool wasActive = id == m_activeId;
    m_tabs.erase(m_tabs.begin() + idx);
    if (view) AppShell::Instance().Retire(std::move(view));

    if (wasActive) {
        // Safari selects the tab to the right, else the one to the left.
        const size_t nextIdx = static_cast<size_t>(idx) < m_tabs.size() ? static_cast<size_t>(idx) : m_tabs.size() - 1;
        m_activeId = 0;
        ActivateTab(m_tabs[nextIdx]->id);
    } else {
        UpdateLayout();
        TabsChanged();
    }
}

void MainWindow::CloseOtherTabs(int keepId, bool onlyRight) {
    const int keepIdx = IndexOf(keepId);
    if (keepIdx < 0) return;
    std::vector<int> ids;
    for (size_t i = 0; i < m_tabs.size(); ++i) {
        if (m_tabs[i]->id == keepId) continue;
        if (onlyRight && static_cast<int>(i) < keepIdx) continue;
        ids.push_back(m_tabs[i]->id);
    }
    if (m_activeId != keepId) ActivateTab(keepId);
    for (int id : ids) CloseTab(id);
}

void MainWindow::MoveTab(int id, int newIndex) {
    const int from = IndexOf(id);
    if (from < 0 || newIndex < 0 || newIndex >= static_cast<int>(m_tabs.size()) || from == newIndex) return;
    auto tab = std::move(m_tabs[static_cast<size_t>(from)]);
    m_tabs.erase(m_tabs.begin() + from);
    m_tabs.insert(m_tabs.begin() + newIndex, std::move(tab));
    RECT client{};
    GetClientRect(m_hWnd, &client);
    LayoutToolbar(client.right);
}

void MainWindow::DuplicateTab(int id) {
    const Tab* t = FindTab(id);
    if (!t) return;
    NewTab(!t->readerSource.empty() ? t->readerSource : t->url, true, id);
}

void MainWindow::ReopenClosedTab() {
    if (m_closedUrls.empty()) return;
    const std::wstring url = m_closedUrls.back();
    m_closedUrls.pop_back();
    NewTab(url, true);
}

void MainWindow::ResumeTab(Tab& tab) {
    if (!tab.view || !tab.view->GetWebView()) return;
    if (tab.suspended || tab.suspendPending) {
        wil::com_ptr<ICoreWebView2_3> wv3;
        if (SUCCEEDED(tab.view->GetWebView()->QueryInterface(IID_PPV_ARGS(&wv3))) && wv3) wv3->Resume();
    }
    tab.suspended = false;
    tab.suspendPending = false;
}

void MainWindow::SuspendBackgroundTabs(bool force) {
    const int minutes = Config::Instance().GetSettings().tabSuspendMinutes;
    if (!force && minutes <= 0) return;
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG idleMs = static_cast<ULONGLONG>(minutes) * 60000ULL;
    for (auto& tp : m_tabs) {
        Tab& t = *tp;
        if (t.id == m_activeId || t.audio || t.suspended || t.suspendPending || !t.view || !t.view->GetWebView()) continue;
        if (!force && t.lastActive != 0 && now - t.lastActive < idleMs) continue;
        if (!force && t.lastActive == 0 && now - t.loadStarted < idleMs) continue;
        wil::com_ptr<ICoreWebView2_3> wv3;
        if (FAILED(t.view->GetWebView()->QueryInterface(IID_PPV_ARGS(&wv3))) || !wv3) continue;
        t.suspendPending = true;
        const int id = t.id;
        const HRESULT hr = wv3->TrySuspend(Callback<ICoreWebView2TrySuspendCompletedHandler>(
            [this, id](HRESULT result, BOOL ok) -> HRESULT {
                Tab* tab = FindTab(id);
                if (!tab) return S_OK;
                const bool wasPending = tab->suspendPending;
                tab->suspendPending = false;
                if (!wasPending) return S_OK;  // resumed meanwhile
                tab->suspended = SUCCEEDED(result) && ok;
                if (tab->suspended) TabsChanged();
                return S_OK;
            }).Get());
        if (FAILED(hr)) t.suspendPending = false;
    }
}

void MainWindow::SetTabMuted(Tab& tab, bool muted) {
    if (!tab.view || !tab.view->GetWebView()) return;
    wil::com_ptr<ICoreWebView2_8> wv8;
    if (SUCCEEDED(tab.view->GetWebView()->QueryInterface(IID_PPV_ARGS(&wv8))) && wv8) {
        wv8->put_IsMuted(muted ? TRUE : FALSE);
        tab.muted = muted;
        UpdateLayout();
        TabsChanged();
    }
}

void MainWindow::OnTabUrlChanged(Tab& tab) {
    if (tab.id == m_activeId) {
        if (m_isAddressFocused && GetFocus() != m_hEditAddress) EndAddressEdit(false);
        UpdateWindowTitle();
    }
    InvalidateToolbar();
    TabsChanged();
}

void MainWindow::OnTabNavigationCompleted(Tab& tab, bool success, const std::wstring& uri) {
    if (tab.id == m_activeId) KillTimer(m_hWnd, IDT_PROGRESS);
    InvalidateToolbar();
    if (!success || uri.empty()) return;
    const bool internal = InternalPages::IsInternal(uri);
    if (!internal) {
        const std::string url = StringUtils::WideToUtf8(uri);
        auto& lib = AppShell::Instance().Lib();
        if (!m_private && Config::Instance().GetSettings().saveHistory && Library::IsWebUrl(url) && !IsDefaultHomeUrl(uri)) {
            lib.RecordVisit(url, StringUtils::WideToUtf8(tab.title), AppShell::NowMs());
        }
        ApplySiteZoom(tab);
        tab.readerPending = Library::IsWebUrl(url);
    } else {
        tab.zoomHost.clear();
    }
    tab.thumbPending = true;
    if (tab.id == m_activeId) ScheduleIdleWork();
}

// The browser's own follow-up work (reader check, overview thumbnail, the pre-rendered
// new tab) waits until the visible page has finished loading, so it never competes
// with the page for CPU, GPU or bandwidth while the first screen is being drawn.
void MainWindow::ScheduleIdleWork() {
    SetTimer(m_hWnd, IDT_IDLE_WORK, 700, nullptr);
}

void MainWindow::RunIdleWork() {
    KillTimer(m_hWnd, IDT_IDLE_WORK);
    Tab* tab = ActiveTab();
    if (!tab || !tab->view || !tab->view->GetWebView()) return;
    if (tab->loading) {
        ScheduleIdleWork();
        return;
    }
    if (tab->readerPending) {
        tab->readerPending = false;
        CheckReaderAvailability(tab->id);
    }
    if (tab->thumbPending && !IsIconic(m_hWnd) && !m_overviewVisible) {
        tab->thumbPending = false;
        CaptureThumbnail(tab->id, nullptr);
    }
}

void MainWindow::ApplySiteZoom(Tab& tab) {
    if (!tab.view || !tab.view->GetController()) return;
    const std::string host = HostUtf8(tab.url);
    const std::wstring whost = StringUtils::Utf8ToWide(host);
    if (whost == tab.zoomHost) return;
    tab.zoomHost = whost;
    const double zoom = m_private ? 0.0 : AppShell::Instance().Lib().Site(host).zoom;
    tab.applyingZoom = true;
    tab.view->SetZoomFactor(zoom > 0 ? zoom : 1.0);
    tab.applyingZoom = false;
}

void MainWindow::OnTabFavicon(Tab& tab, const std::wstring& pageUri, const std::vector<std::uint8_t>& png) {
    tab.favicon.reset();
    tab.faviconDataUrl.clear();
    if (!png.empty()) {
        wil::com_ptr<IStream> stream;
        stream.attach(SHCreateMemStream(png.data(), static_cast<UINT>(png.size())));
        if (stream) {
            std::unique_ptr<Gdiplus::Bitmap> bmp(Gdiplus::Bitmap::FromStream(stream.get()));
            if (bmp && bmp->GetLastStatus() == Gdiplus::Ok && bmp->GetWidth() > 0) tab.favicon = std::move(bmp);
        }
        tab.faviconDataUrl = "data:image/png;base64," + Base64(png);
        // Cache by host for the start page, history and bookmarks (normal windows only).
        const std::string host = HostUtf8(pageUri);
        if (!m_private && !host.empty() && !InternalPages::IsInternal(pageUri)) {
            std::ofstream out(FaviconPath(host), std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
        }
    }
    InvalidateToolbar();
    TabsChanged();
}

void MainWindow::HandleNewWindowRequest(int openerId, ICoreWebView2NewWindowRequestedEventArgs* args) {
    Tab* opener = FindTab(openerId);
    if (!opener || !args) return;
    BOOL userInitiated = FALSE;
    args->get_IsUserInitiated(&userInitiated);
    wil::unique_cotaskmem_string uri;
    args->get_Uri(&uri);
    const std::wstring target = uri.get() ? uri.get() : L"";
    const std::string openerHost = HostUtf8(opener->url);
    if (!userInitiated && AppShell::Instance().Lib().Site(openerHost).popups != "allow") {
        args->put_Handled(TRUE);
        ShowToast(L"已拦截弹出式窗口");
        return;
    }
    // Ctrl/middle click opens in the background, like Safari's Cmd-click.
    const bool background = (GetKeyState(VK_CONTROL) & 0x8000) != 0 || (GetKeyState(VK_MBUTTON) & 0x8000) != 0;
    wil::com_ptr<ICoreWebView2Deferral> deferral;
    if (FAILED(args->GetDeferral(&deferral)) || !deferral) {
        args->put_Handled(TRUE);
        NewTab(target, !background, openerId);
        return;
    }
    wil::com_ptr<ICoreWebView2NewWindowRequestedEventArgs> keep(args);
    NewTab(L"", !background, openerId, [keep, deferral, target](Tab& t) {
        // Hand the new WebView to the opener so window.opener and postMessage keep working.
        if (t.view && t.view->GetWebView()) {
            keep->put_NewWindow(t.view->GetWebView());
            t.url = target;
        }
        keep->put_Handled(TRUE);
        deferral->Complete();
    });
}

COREWEBVIEW2_PERMISSION_STATE MainWindow::PermissionFor(COREWEBVIEW2_PERMISSION_KIND kind, const std::wstring& uri) {
    const SiteSettings site = AppShell::Instance().Lib().Site(HostUtf8(uri));
    const std::string* policy = nullptr;
    switch (kind) {
    case COREWEBVIEW2_PERMISSION_KIND_CAMERA: policy = &site.camera; break;
    case COREWEBVIEW2_PERMISSION_KIND_MICROPHONE: policy = &site.microphone; break;
    case COREWEBVIEW2_PERMISSION_KIND_GEOLOCATION: policy = &site.location; break;
    default: return COREWEBVIEW2_PERMISSION_STATE_DEFAULT;
    }
    if (*policy == "allow") return COREWEBVIEW2_PERMISSION_STATE_ALLOW;
    if (*policy == "deny") return COREWEBVIEW2_PERMISSION_STATE_DENY;
    return COREWEBVIEW2_PERMISSION_STATE_DEFAULT;
}

namespace {

// Runs on a worker thread: decode the JPEG preview, scale it down, re-encode.
std::string MakeThumbnail(const std::vector<std::uint8_t>& jpeg) {
    wil::com_ptr<IStream> in;
    in.attach(SHCreateMemStream(jpeg.data(), static_cast<UINT>(jpeg.size())));
    if (!in) return {};
    std::unique_ptr<Gdiplus::Bitmap> full(Gdiplus::Bitmap::FromStream(in.get()));
    if (!full || full->GetLastStatus() != Gdiplus::Ok || full->GetWidth() == 0) return {};
    const UINT w = 400;
    const UINT h = (std::max)(1u, (std::min)(260u, full->GetHeight() * w / full->GetWidth()));
    Gdiplus::Bitmap scaled(static_cast<INT>(w), static_cast<INT>(h), PixelFormat24bppRGB);
    {
        Gdiplus::Graphics g(&scaled);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBilinear);
        const UINT srcH = (std::min)(full->GetHeight(), full->GetWidth() * h / w);
        g.DrawImage(full.get(), Gdiplus::Rect(0, 0, static_cast<INT>(w), static_cast<INT>(h)),
                    0, 0, static_cast<INT>(full->GetWidth()), static_cast<INT>(srcH), Gdiplus::UnitPixel);
    }
    CLSID clsid{};
    wil::com_ptr<IStream> out;
    out.attach(SHCreateMemStream(nullptr, 0));
    if (!out || !EncoderClsid(L"image/jpeg", &clsid) || scaled.Save(out.get(), &clsid, nullptr) != Gdiplus::Ok) return {};
    return "data:image/jpeg;base64," + Base64(ReadStream(out.get()));
}

struct ThumbnailResult {
    int tabId;
    std::string dataUrl;
    std::function<void()> done;
};

} // namespace

void MainWindow::CaptureThumbnail(int tabId, std::function<void()> done) {
    Tab* tab = FindTab(tabId);
    if (!tab || !tab->view || !tab->view->GetWebView() || tab->id != m_activeId) {
        if (done) done();
        return;
    }
    wil::com_ptr<IStream> stream;
    stream.attach(SHCreateMemStream(nullptr, 0));
    if (!stream) {
        if (done) done();
        return;
    }
    const HWND hwnd = m_hWnd;
    const HRESULT hr = tab->view->GetWebView()->CapturePreview(COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_JPEG, stream.get(),
        Callback<ICoreWebView2CapturePreviewCompletedHandler>([hwnd, tabId, stream, done](HRESULT result) -> HRESULT {
            std::vector<std::uint8_t> jpeg;
            if (SUCCEEDED(result)) jpeg = ReadStream(stream.get());
            // Decoding and re-encoding a full-window image takes tens of milliseconds:
            // keep it off the UI thread and hand the result back with a message.
            std::thread([hwnd, tabId, jpeg = std::move(jpeg), done]() {
                auto* r = new ThumbnailResult{tabId, jpeg.empty() ? std::string() : MakeThumbnail(jpeg), done};
                if (!IsWindow(hwnd) || !PostMessageW(hwnd, MainWindow::WM_APP_THUMBNAIL, 0, reinterpret_cast<LPARAM>(r))) delete r;
            }).detach();
            return S_OK;
        }).Get());
    if (FAILED(hr) && done) done();
}

void MainWindow::OnThumbnailReady(int tabId, std::string dataUrl) {
    if (Tab* t = FindTab(tabId); t && !dataUrl.empty()) t->thumbDataUrl = std::move(dataUrl);
}

void MainWindow::CheckReaderAvailability(int tabId) {
    Tab* tab = FindTab(tabId);
    if (!tab || !tab->view || !tab->view->GetWebView()) return;
    tab->view->GetWebView()->ExecuteScript(InternalPages::ReaderableScript().c_str(),
        Callback<ICoreWebView2ExecuteScriptCompletedHandler>([this, tabId](HRESULT hr, LPCWSTR result) -> HRESULT {
            Tab* t = FindTab(tabId);
            if (!t) return S_OK;
            t->readerAvailable = SUCCEEDED(hr) && result && std::wstring(result) == L"true";
            if (tabId == m_activeId) InvalidateToolbar();
            if (t->readerAvailable && tabId == m_activeId && t->readerSource.empty() && !m_private &&
                AppShell::Instance().Lib().Site(HostUtf8(t->url)).autoReader) {
                SetTimer(m_hWnd, 6000 + static_cast<UINT_PTR>(tabId), 100, nullptr);  // auto reader
            }
            return S_OK;
        }).Get());
}

void MainWindow::ToggleReader() {
    Tab* tab = ActiveTab();
    if (!tab || !tab->view || !tab->view->GetWebView()) return;
    if (!tab->readerSource.empty()) {
        // Leave the reader: the article page is the previous history entry.
        if (tab->canGoBack) tab->view->GoBack();
        else tab->view->Navigate(tab->readerSource);
        return;
    }
    if (InternalPages::IsInternal(tab->url) || !Library::IsWebUrl(StringUtils::WideToUtf8(tab->url))) {
        ShowToast(L"此页面没有阅读器视图");
        return;
    }
    const int tabId = tab->id;
    const std::wstring source = tab->url;
    tab->view->GetWebView()->ExecuteScript(InternalPages::ExtractScript().c_str(),
        Callback<ICoreWebView2ExecuteScriptCompletedHandler>([this, tabId, source](HRESULT hr, LPCWSTR result) -> HRESULT {
            Tab* t = FindTab(tabId);
            if (!t || !t->view) return S_OK;
            std::string article;
            if (SUCCEEDED(hr) && result) {
                try {
                    const auto v = json::parse(StringUtils::WideToUtf8(result));
                    if (v.is_string()) article = v.get<std::string>();
                } catch (...) {}
            }
            if (article.empty()) {
                ShowToast(L"此页面没有阅读器视图");
                return S_OK;
            }
            const std::string key = AppShell::Instance().StoreArticle(article);
            t->readerSource = source;
            t->view->Navigate(InternalPages::Url(L"reader.html#" + StringUtils::Utf8ToWide(key)));
            InvalidateToolbar();
            return S_OK;
        }).Get());
}

void MainWindow::NavigateActive(const std::wstring& input) {
    Tab* tab = ActiveTab();
    if (!tab) {
        NewTab(WebViewManager::ResolveInput(input), true);
        return;
    }
    if (!tab->view) return;
    KillTimer(m_hWnd, IDT_PREFETCH);
    const std::wstring target = WebViewManager::ResolveInput(input);
    if (!tab->prefetchUrl.empty() && target == tab->prefetchUrl && tab->view->GetWebView()) {
        // Only a navigation started by the page itself may use the page's prefetch.
        tab->prefetchUrl.clear();
        const std::wstring script = L"location.href = " + StringUtils::Utf8ToWide(json(StringUtils::WideToUtf8(target)).dump()) + L";";
        tab->view->GetWebView()->ExecuteScript(script.c_str(), nullptr);
        return;
    }
    tab->view->Navigate(input);
}

namespace {

// Only text that is clearly a web address is fetched ahead: a full URL, or a host
// name ending in a common top-level domain. Searches and half-typed words are not.
bool LooksLikeAddress(const std::wstring& text, const std::wstring& resolved) {
    if (resolved.rfind(L"https://", 0) != 0 && resolved.rfind(L"http://", 0) != 0) return false;
    if (text.find(L"://") != std::wstring::npos) return true;
    std::wstring host = StringUtils::Utf8ToWide(Library::HostOf(StringUtils::WideToUtf8(resolved)));
    const size_t dot = host.rfind(L'.');
    if (dot == std::wstring::npos || dot == 0) return false;
    static const std::set<std::wstring> kTlds = {
        L"com", L"org", L"net", L"edu", L"gov", L"io", L"co", L"info", L"app", L"dev", L"me", L"tv", L"ai",
        L"cn", L"hk", L"tw", L"jp", L"kr", L"sg", L"uk", L"de", L"fr", L"ca", L"au", L"us", L"eu", L"ru", L"in"};
    return kTlds.count(host.substr(dot + 1)) > 0;
}

} // namespace

// While an address is typed on the start page, the page itself prefetches it with
// speculation rules (no referrer, no cookies for other sites), so pressing Enter
// finds the document already downloaded.
void MainWindow::UpdateAddressPrefetch() {
    KillTimer(m_hWnd, IDT_PREFETCH);
    Tab* tab = ActiveTab();
    if (!tab || !tab->view || !tab->view->GetWebView() || tab->loading || !m_isAddressFocused) return;
    if (InternalPages::PageName(tab->url) != L"start.html") return;
    const int len = GetWindowTextLengthW(m_hEditAddress);
    std::wstring text(static_cast<size_t>(len) + 1, L'\0');
    GetWindowTextW(m_hEditAddress, text.data(), len + 1);
    text.resize(static_cast<size_t>(len));
    const std::wstring resolved = WebViewManager::ResolveInput(text);
    const std::wstring url = LooksLikeAddress(text, resolved) ? resolved : std::wstring();
    if (url == tab->prefetchUrl) return;
    tab->prefetchUrl = url;
    json rules = nullptr;
    if (!url.empty()) {
        rules = {{"prefetch", json::array({{{"source", "list"}, {"urls", json::array({StringUtils::WideToUtf8(url)})},
                                            {"referrer_policy", "no-referrer"}}})}};
    }
    const std::string script =
        "(() => { const old = document.getElementById('ulb-prefetch'); if (old) old.remove(); const rules = " + rules.dump() +
        "; if (!rules) return; const s = document.createElement('script'); s.type = 'speculationrules'; s.id = 'ulb-prefetch';"
        " s.textContent = JSON.stringify(rules); document.head.append(s); })()";
    tab->view->GetWebView()->ExecuteScript(StringUtils::Utf8ToWide(script).c_str(), nullptr);
}

void MainWindow::UpdateWindowTitle() {
    const Tab* tab = ActiveTab();
    std::wstring title = tab && !tab->title.empty() ? tab->title : L"新标签页";
    title += m_private ? L" — 无痕浏览 — UltraLightBrowser" : L" — UltraLightBrowser";
    SetWindowTextW(m_hWnd, title.c_str());
}

void MainWindow::TabsChanged() {
    InvalidateToolbar();
    if (!m_panelsDirty) {
        m_panelsDirty = true;
        SetTimer(m_hWnd, IDT_PANELS, 120, nullptr);
    }
}

void MainWindow::FlushPanels() {
    m_panelsDirty = false;
    if ((m_sidebarVisible && m_sidebar.ready) || (m_overviewVisible && m_overview.ready)) {
        const std::string tabs = TabsJson(m_overviewVisible);
        if (m_sidebarVisible && m_sidebar.webView) PostEvent(m_sidebar.webView.get(), "tabs", tabs);
        if (m_overviewVisible && m_overview.webView) PostEvent(m_overview.webView.get(), "tabs", tabs);
    }
}

} // namespace UltraLight
