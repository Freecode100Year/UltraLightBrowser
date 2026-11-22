// Built-in pages and panels: the sidebar and tab overview WebViews, and the
// JSON bridge used by https://ulb.internal/ pages (start page, history,
// bookmarks, settings, privacy report, reader).

#include "MainWindow.hpp"
#include "AppShell.hpp"
#include "Config.hpp"
#include "InternalPages.hpp"
#include "ElementBlocker.hpp"
#include "NativeRequestFilter.hpp"
#include "StringUtils.hpp"
#include "WarpManager.hpp"
#include "UserScripts.hpp"
#include "Extensions.hpp"
#include "RecommendedExtensions.hpp"

#include <shobjidl.h>

#include <commdlg.h>
#include <fstream>
#include <filesystem>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <nlohmann/json.hpp>

#include <mutex>
#include <sstream>
#include <thread>

#pragma comment(lib, "comdlg32.lib")

using json = nlohmann::json;
using namespace Microsoft::WRL;

#ifndef ULB_VERSION
#define ULB_VERSION "dev"
#endif

namespace UltraLight {

namespace {

std::string U8(const std::wstring& s) { return StringUtils::WideToUtf8(s); }
std::wstring W(const std::string& s) { return StringUtils::Utf8ToWide(s); }

std::string Arg(const json& args, const char* key) {
    if (!args.is_object()) return {};
    const auto it = args.find(key);
    return it != args.end() && it->is_string() ? it->get<std::string>() : std::string();
}

// Milliseconds since the epoch at local midnight `daysAgo` days back.
std::int64_t LocalMidnightMs(int daysAgo) {
    SYSTEMTIME local{};
    GetLocalTime(&local);
    local.wHour = local.wMinute = local.wSecond = local.wMilliseconds = 0;
    SYSTEMTIME utc{};
    if (!TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc)) return 0;
    FILETIME ft{};
    SystemTimeToFileTime(&utc, &ft);
    ULARGE_INTEGER v{};
    v.LowPart = ft.dwLowDateTime;
    v.HighPart = ft.dwHighDateTime;
    const std::int64_t ms = static_cast<std::int64_t>((v.QuadPart - 116444736000000000ULL) / 10000ULL);
    return ms - static_cast<std::int64_t>(daysAgo) * 86400000LL;
}

json BookmarkJson(const Bookmark& b) {
    return {{"id", b.id}, {"title", b.title}, {"url", b.url}, {"folder", b.folder}};
}

bool IsWebOrAbout(const std::string& url) {
    return Library::IsWebUrl(url) || url.rfind("about:", 0) == 0 || url.rfind("file:", 0) == 0;
}

// Line store edits come from the UI thread and from worker threads (tests,
// subscriptions); each load-change-save runs under this lock.
std::mutex g_linesMutex;

json LinesJson() {
    std::lock_guard<std::mutex> lock(g_linesMutex);
    const LineStore store = WarpManager::LoadStore();
    json lines = json::array();
    for (const auto& l : store.Lines()) {
        lines.push_back({{"id", l.id}, {"name", l.name}, {"proto", LineStore::ProtocolOf(l.link)},
                         {"server", LineStore::ServerOf(l.link)}, {"sub", l.sub}});
    }
    json subs = json::array();
    for (const auto& sub : store.Subs()) {
        subs.push_back({{"id", sub.id}, {"name", sub.name}, {"updated", sub.updated}, {"count", sub.count}});
    }
    return {{"lines", lines}, {"subs", subs}, {"selected", store.Selected()}, {"autoMax", LineStore::kAutoMax}};
}

std::vector<std::string> SplitLines(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        size_t start = line.find_first_not_of(" \t");
        if (start != std::string::npos) out.push_back(line.substr(start));
    }
    return out;
}

// Results of "-probe": index -> milliseconds (>= 0), or the reason it failed.
std::map<size_t, std::string> ParseProbe(const std::string& output) {
    std::map<size_t, std::string> results;
    for (const auto& line : SplitLines(output)) {
        if (line.rfind("PROBE ", 0) != 0) continue;
        const size_t sp = line.find(' ', 6);
        if (sp == std::string::npos) continue;
        results[std::strtoul(line.c_str() + 6, nullptr, 10)] = line.substr(sp + 1);
    }
    return results;
}

// Downloads a subscription through the helper; throws with the reason on failure.
std::vector<std::string> FetchSubscription(const std::string& url, int& skipped) {
    std::wstring args = L"-fetch";
    if (const int port = WarpManager::Instance().Port(); port > 0) args += L" -socks 127.0.0.1:" + std::to_wstring(port);
    const std::string out = WarpManager::RunTool(args, {{L"ULB_FETCH", W(url)}}, 70000);
    std::vector<std::string> links;
    skipped = 0;
    std::string error;
    bool done = false;
    for (const auto& line : SplitLines(out)) {
        if (line.rfind("LINK ", 0) == 0) links.push_back(line.substr(5));
        else if (line.rfind("FETCHED ", 0) == 0) { done = true; skipped = std::atoi(line.c_str() + line.rfind(' ') + 1); }
        else if (line.rfind("ERROR ", 0) == 0) error = line.substr(6);
    }
    if (!done) throw std::runtime_error(error.empty() ? "订阅下载失败" : "订阅下载失败：" + error);
    return links;
}

// A folder chosen in the system dialog, or empty.
std::filesystem::path PickFolder(HWND owner) {
    wil::com_ptr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))) || !dlg) return {};
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR);
    dlg->SetTitle(L"选择解压后的扩展文件夹（里面有 manifest.json）");
    if (FAILED(dlg->Show(owner))) return {};
    wil::com_ptr<IShellItem> item;
    wil::unique_cotaskmem_string path;
    if (FAILED(dlg->GetResult(&item)) || !item || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) || !path) return {};
    return path.get();
}

json ExtensionsJson() {
    json list = json::array();
    for (const auto& e : Extensions::Instance().List()) {
        list.push_back({{"dir", e.dir}, {"name", e.name}, {"version", e.version}, {"description", e.description},
                        {"enabled", e.enabled}, {"loaded", !e.id.empty()}, {"error", e.error},
                        {"popup", !e.popup.empty()}, {"options", !e.options.empty()}, {"proxy", e.proxy}, {"source", e.source}});
    }
    return list;
}

} // namespace

void MainWindow::ReplyLater(ICoreWebView2* source, const std::string& idJson, std::function<std::string()> work) {
    wil::com_ptr<ICoreWebView2> target(source);
    const HWND hwnd = m_hWnd;
    std::thread([hwnd, target, idJson, work = std::move(work)]() {
        std::string reply;
        try {
            reply = "{\"id\":" + idJson + ",\"result\":" + work() + "}";
        } catch (const std::exception& e) {
            reply = "{\"id\":" + idJson + ",\"error\":" + json(e.what()).dump(-1, ' ', false, json::error_handler_t::replace) + "}";
        }
        auto* r = new PageReply{target, W(reply)};
        if (!IsWindow(hwnd) || !PostMessageW(hwnd, MainWindow::WM_APP_PAGE_REPLY, 0, reinterpret_cast<LPARAM>(r))) delete r;
    }).detach();
}

std::string MainWindow::TabsJson(bool withThumbs) const {
    json arr = json::array();
    for (const auto& t : m_tabs) {
        const std::wstring url = DisplayUrl(*t);
        json tab = {{"id", t->id}, {"title", U8(t->title)}, {"url", U8(url)}, {"active", t->id == m_activeId},
                    {"audio", t->audio}, {"muted", t->muted}, {"suspended", t->suspended}};
        tab["favicon"] = t->faviconDataUrl.empty() ? json(nullptr) : json(t->faviconDataUrl);
        if (withThumbs) tab["thumb"] = t->thumbDataUrl.empty() ? json(nullptr) : json(t->thumbDataUrl);
        arr.push_back(std::move(tab));
    }
    return arr.dump();
}

void MainWindow::PostEvent(ICoreWebView2* target, const char* event, const std::string& dataJson) {
    if (!target) return;
    const std::string msg = std::string("{\"event\":") + json(event).dump() + ",\"data\":" + (dataJson.empty() ? "null" : dataJson) + "}";
    target->PostWebMessageAsJson(W(msg).c_str());
}

void MainWindow::BroadcastToInternalPages(const char* event, const std::string& dataJson) {
    for (const auto& t : m_tabs) {
        if (t->view && t->view->GetWebView() && InternalPages::IsInternal(t->url)) {
            PostEvent(t->view->GetWebView(), event, dataJson);
        }
    }
    if (m_spare && m_spare->view && m_spare->view->GetWebView()) PostEvent(m_spare->view->GetWebView(), event, dataJson);
    if (m_sidebar.ready) PostEvent(m_sidebar.webView.get(), event, dataJson);
}

void MainWindow::ApplyUserScripts() {
    const auto& sources = AppShell::Instance().UserScriptSources();
    for (const auto& t : m_tabs) {
        if (t->view) t->view->ApplyUserScripts(sources);
    }
}

void MainWindow::OnLibraryChanged() {
    BroadcastToInternalPages("library", "null");
    InvalidateToolbar();
}

void MainWindow::OpenInternalPage(const std::wstring& page, bool newTab) {
    Tab* tab = ActiveTab();
    const bool reuse = tab && (InternalPages::PageName(tab->url) == L"start.html" || tab->url == L"about:blank" || tab->url.empty());
    if (!newTab || reuse) {
        if (tab && tab->view) tab->view->Navigate(InternalPages::Url(page));
        else NewTab(InternalPages::Url(page), true);
    } else {
        NewTab(InternalPages::Url(page), true, m_activeId);
    }
}

// ------------------------------------------------------------------ panels

void MainWindow::EnsurePanel(PanelView& panel, const std::wstring& page, std::function<void()> onReady) {
    if (panel.ready) {
        if (onReady) onReady();
        return;
    }
    if (panel.creating) return;
    panel.creating = true;
    PanelView* p = &panel;
    AppShell::Instance().WhenEnvironmentReady(m_hWnd, [this, p, page, onReady](ICoreWebView2Environment* env) {
        if (m_closing) return;
        env->CreateCoreWebView2Controller(m_hWnd, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [this, p, page, onReady](HRESULT hr, ICoreWebView2Controller* controller) -> HRESULT {
                p->creating = false;
                if (FAILED(hr) || !controller || m_closing || m_destroyed) {
                    if (controller) controller->Close();
                    return S_OK;
                }
                p->controller = controller;
                controller->get_CoreWebView2(&p->webView);
                if (!p->webView) return S_OK;
                controller->put_IsVisible(FALSE);
                wil::com_ptr<ICoreWebView2Controller2> c2;
                if (SUCCEEDED(controller->QueryInterface(IID_PPV_ARGS(&c2))) && c2) {
                    COREWEBVIEW2_COLOR bg{255, 0x24, 0x26, 0x2c};
                    c2->put_DefaultBackgroundColor(bg);
                }
                wil::com_ptr<ICoreWebView2Controller3> c3;
                if (SUCCEEDED(controller->QueryInterface(IID_PPV_ARGS(&c3))) && c3) {
                    c3->put_BoundsMode(COREWEBVIEW2_BOUNDS_MODE_USE_RAW_PIXELS);
                }
                wil::com_ptr<ICoreWebView2Settings> settings;
                if (SUCCEEDED(p->webView->get_Settings(&settings)) && settings) {
                    settings->put_AreDefaultContextMenusEnabled(FALSE);
                    settings->put_IsZoomControlEnabled(FALSE);
                    settings->put_IsStatusBarEnabled(FALSE);
                }
                InternalPages::MapToWebView(p->webView.get());
                p->webView->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                    [this, p](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                        wil::unique_cotaskmem_string source, msg;
                        if (FAILED(args->get_Source(&source)) || !source.get() || !InternalPages::IsInternal(source.get())) return S_OK;
                        if (SUCCEEDED(args->get_WebMessageAsJson(&msg)) && msg.get()) HandlePageMessage(p->webView.get(), msg.get());
                        return S_OK;
                    }).Get(), nullptr);
                // Panels only ever show built-in pages; anything else opens in a tab.
                p->webView->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>(
                    [this](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                        wil::unique_cotaskmem_string uri;
                        if (SUCCEEDED(args->get_Uri(&uri)) && uri.get() && !InternalPages::IsInternal(uri.get())) {
                            args->put_Cancel(TRUE);
                            const std::wstring target = uri.get();
                            NavigateActive(target);
                        }
                        return S_OK;
                    }).Get(), nullptr);
                p->webView->add_NewWindowRequested(Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                    [this](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                        wil::unique_cotaskmem_string uri;
                        args->put_Handled(TRUE);
                        if (SUCCEEDED(args->get_Uri(&uri)) && uri.get()) NewTab(uri.get(), true);
                        return S_OK;
                    }).Get(), nullptr);
                controller->add_AcceleratorKeyPressed(Callback<ICoreWebView2AcceleratorKeyPressedEventHandler>(
                    [this](ICoreWebView2Controller*, ICoreWebView2AcceleratorKeyPressedEventArgs* args) -> HRESULT {
                        COREWEBVIEW2_KEY_EVENT_KIND kind;
                        UINT key = 0;
                        if (FAILED(args->get_KeyEventKind(&kind)) || FAILED(args->get_VirtualKey(&key))) return S_OK;
                        if (kind != COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN && kind != COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN) return S_OK;
                        const WORD cmd = MapShortcut(key, (GetKeyState(VK_CONTROL) & 0x8000) != 0,
                                                     (GetKeyState(VK_SHIFT) & 0x8000) != 0, (GetKeyState(VK_MENU) & 0x8000) != 0);
                        if (cmd) {
                            PostMessageW(m_hWnd, WM_COMMAND, MAKEWPARAM(cmd, 0), 0);
                            args->put_Handled(TRUE);
                        }
                        return S_OK;
                    }).Get(), nullptr);
                p->webView->Navigate(InternalPages::Url(page).c_str());
                p->ready = true;
                LayoutPanels();
                if (onReady) onReady();
                return S_OK;
            }).Get());
    });
}

void MainWindow::LayoutPanels() {
    RECT client{};
    GetClientRect(m_hWnd, &client);
    const bool chrome = !m_isFullScreen;
    if (m_sidebar.controller) {
        const bool show = chrome && m_sidebarVisible && !m_overviewVisible;
        if (show) {
            RECT rc{0, m_topbarHeight, S(260), client.bottom};
            m_sidebar.controller->put_Bounds(rc);
        }
        m_sidebar.controller->put_IsVisible(show ? TRUE : FALSE);
    }
    if (m_overview.controller) {
        const bool show = chrome && m_overviewVisible;
        if (show) {
            RECT rc{0, m_topbarHeight, client.right, client.bottom};
            m_overview.controller->put_Bounds(rc);
        }
        m_overview.controller->put_IsVisible(show ? TRUE : FALSE);
    }
    if (Tab* active = ActiveTab(); active && active->view && active->view->IsReady()) {
        active->view->SetVisible(!m_overviewVisible || !chrome);
    }
}

void MainWindow::ToggleSidebar(const wchar_t* segment) {
    const bool show = segment ? true : !m_sidebarVisible;
    m_sidebarVisible = show;
    Config::Instance().GetSettings().sidebarVisible = show;
    Config::Instance().Save();
    if (show) {
        const std::string seg = segment ? U8(segment) : std::string();
        EnsurePanel(m_sidebar, L"sidebar.html", [this, seg]() {
            if (!seg.empty()) PostEvent(m_sidebar.webView.get(), "segment", json(seg).dump());
            PostEvent(m_sidebar.webView.get(), "tabs", TabsJson(false));
        });
    }
    UpdateLayout();
}

void MainWindow::ToggleOverview() {
    if (m_overviewVisible) {
        HideOverview();
        return;
    }
    if (m_isAddressFocused) EndAddressEdit(false);
    CaptureThumbnail(m_activeId, [this]() {
        if (m_closing) return;
        m_overviewVisible = true;
        EnsurePanel(m_overview, L"overview.html", [this]() {
            PostEvent(m_overview.webView.get(), "focus", "null");
            PostEvent(m_overview.webView.get(), "tabs", TabsJson(true));
            if (m_overview.controller) m_overview.controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        });
        LayoutPanels();
        InvalidateToolbar();
    });
}

void MainWindow::HideOverview(int activateId) {
    if (!m_overviewVisible) return;
    m_overviewVisible = false;
    LayoutPanels();
    InvalidateToolbar();
    if (activateId) ActivateTab(activateId);
    else if (Tab* t = ActiveTab(); t && t->view && t->view->GetController()) {
        t->view->GetController()->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
    }
}

// ------------------------------------------------------------------ bridge

void MainWindow::HandlePageMessage(ICoreWebView2* source, const std::wstring& rawJson) {
    json msg;
    try {
        msg = json::parse(U8(rawJson));
    } catch (...) {
        return;
    }
    if (!msg.is_object() || !msg.contains("id") || !msg.contains("cmd") || !msg["cmd"].is_string()) return;
    const json id = msg["id"];
    const std::string cmd = msg["cmd"].get<std::string>();
    const json args = msg.contains("args") ? msg["args"] : json::object();
    auto& lib = AppShell::Instance().Lib();
    auto& settings = Config::Instance().GetSettings();
    json result = nullptr;
    bool libraryChanged = false;

    // The tab that sent the message (null for panels).
    Tab* sender = nullptr;
    for (auto& t : m_tabs) {
        if (t->view && t->view->GetWebView() == source) sender = t.get();
    }

    try {
        if (cmd == "getStart") {
            json favorites = json::array();
            for (const auto& b : lib.BookmarksIn(kFavoritesFolder)) favorites.push_back(BookmarkJson(b));
            json reading = json::array();
            for (const auto& r : lib.ReadingList()) {
                if (r.read) continue;
                reading.push_back({{"id", r.id}, {"title", r.title}, {"url", r.url}});
                if (reading.size() >= 3) break;
            }
            result = {{"favorites", favorites}, {"reading", reading},
                      {"show", {{"favorites", settings.startShowFavorites}, {"reading", settings.startShowReading}}},
                      {"background", settings.startBackground}};
        } else if (cmd == "getHistory") {
            const std::string q = Arg(args, "q");
            const size_t limit = args.contains("limit") && args["limit"].is_number_unsigned() ? (std::min)(args["limit"].get<size_t>(), static_cast<size_t>(5000)) : 500;
            json items = json::array();
            for (const auto& h : lib.QueryHistory(q, limit)) {
                items.push_back({{"url", h.url}, {"title", h.title}, {"last", h.last}, {"visits", h.visits}});
            }
            result = {{"enabled", settings.saveHistory && !m_private}, {"items", items}};
        } else if (cmd == "deleteHistory") {
            result = lib.RemoveHistory(Arg(args, "url"));
        } else if (cmd == "clearHistory") {
            const std::string range = Arg(args, "range");
            std::int64_t since = 0;
            if (range == "hour") since = AppShell::NowMs() - 3600000;
            else if (range == "today") since = LocalMidnightMs(0);
            else if (range == "twodays") since = LocalMidnightMs(1);
            lib.ClearHistory(since);
            libraryChanged = true;
            result = true;
        } else if (cmd == "getBookmarks") {
            json bms = json::array();
            for (const auto& b : lib.Bookmarks()) bms.push_back(BookmarkJson(b));
            result = {{"folders", lib.Folders()}, {"bookmarks", bms}};
        } else if (cmd == "addBookmark") {
            const std::string url = Arg(args, "url");
            if (!IsWebOrAbout(url)) throw std::runtime_error("只能添加 http/https 网址");
            const std::string folder = Arg(args, "folder").empty() ? kBookmarksFolder : Arg(args, "folder");
            result = lib.AddBookmark(Arg(args, "title"), url, folder, AppShell::NowMs());
            libraryChanged = true;
        } else if (cmd == "updateBookmark") {
            const std::string url = Arg(args, "url");
            if (!url.empty() && !IsWebOrAbout(url)) throw std::runtime_error("只能使用 http/https 网址");
            result = lib.UpdateBookmark(Arg(args, "id"), Arg(args, "title"), url, Arg(args, "folder"));
            libraryChanged = true;
        } else if (cmd == "removeBookmark") {
            result = lib.RemoveBookmark(Arg(args, "id"));
            libraryChanged = true;
        } else if (cmd == "moveBookmark") {
            result = lib.MoveBookmark(Arg(args, "id"), Arg(args, "before"));
            libraryChanged = true;
        } else if (cmd == "addFolder") {
            result = lib.AddFolder(Arg(args, "name"));
            libraryChanged = true;
        } else if (cmd == "renameFolder") {
            result = lib.RenameFolder(Arg(args, "from"), Arg(args, "to"));
            libraryChanged = true;
        } else if (cmd == "removeFolder") {
            result = lib.RemoveFolder(Arg(args, "name"));
            libraryChanged = true;
        } else if (cmd == "importBookmarks") {
            wchar_t file[MAX_PATH]{};
            OPENFILENAMEW ofn{sizeof(ofn)};
            ofn.hwndOwner = m_hWnd;
            ofn.lpstrFilter = L"书签 HTML 文件 (*.html;*.htm)\0*.html;*.htm\0所有文件\0*.*\0";
            ofn.lpstrFile = file;
            ofn.nMaxFile = MAX_PATH;
            ofn.lpstrTitle = L"导入书签（Chrome / Edge / Firefox / Safari 导出的 HTML）";
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_DONTADDTORECENT;
            int count = -1;
            if (GetOpenFileNameW(&ofn)) {
                std::ifstream in(file, std::ios::binary);
                std::stringstream ss;
                ss << in.rdbuf();
                std::string html = ss.str();
                if (html.size() > 50 * 1024 * 1024) html.clear();
                count = lib.ImportNetscapeHtml(html, AppShell::NowMs());
                libraryChanged = true;
            }
            result = {{"count", count}};
        } else if (cmd == "exportBookmarks") {
            wchar_t file[MAX_PATH] = L"bookmarks.html";
            OPENFILENAMEW ofn{sizeof(ofn)};
            ofn.hwndOwner = m_hWnd;
            ofn.lpstrFilter = L"书签 HTML 文件 (*.html)\0*.html\0";
            ofn.lpstrFile = file;
            ofn.nMaxFile = MAX_PATH;
            ofn.lpstrDefExt = L"html";
            ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_DONTADDTORECENT;
            bool saved = false;
            if (GetSaveFileNameW(&ofn)) {
                std::ofstream out(file, std::ios::binary | std::ios::trunc);
                out << lib.ExportNetscapeHtml();
                saved = static_cast<bool>(out);
            }
            result = {{"saved", saved}};
        } else if (cmd == "removeReading") {
            result = lib.RemoveReading(Arg(args, "id"));
            libraryChanged = true;
        } else if (cmd == "markRead") {
            result = lib.SetRead(Arg(args, "id"), args.value("read", true));
            libraryChanged = true;
        } else if (cmd == "warpRescan") {
            result = WarpManager::Instance().Rescan();
        } else if (cmd == "getScripts") {
            UserScriptStore store(AppShell::UserScriptDir());
            store.Load();
            json list = json::array();
            for (const auto& sc : store.Scripts()) {
                std::vector<std::string> sites = sc.matches;
                sites.insert(sites.end(), sc.includes.begin(), sc.includes.end());
                list.push_back({{"id", sc.id}, {"name", sc.name}, {"version", sc.version}, {"description", sc.description},
                                {"sites", sites}, {"enabled", sc.enabled}, {"ignored", sc.ignored}, {"runAt", sc.runAt}});
            }
            result = list;
        } else if (cmd == "importScriptFile") {
            wchar_t file[MAX_PATH] = L"";
            OPENFILENAMEW ofn{sizeof(ofn)};
            ofn.hwndOwner = m_hWnd;
            ofn.lpstrFilter = L"用户脚本 (*.js)\0*.js\0所有文件 (*.*)\0*.*\0";
            ofn.lpstrFile = file;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_DONTADDTORECENT;
            if (!GetOpenFileNameW(&ofn)) {
                result = nullptr;
            } else {
                std::ifstream in(file, std::ios::binary);
                std::string code((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                if (code.size() > UserScriptStore::kMaxSize) throw std::runtime_error("脚本超过 2 MB");
                if (code.rfind("\xEF\xBB\xBF", 0) == 0) code.erase(0, 3);  // UTF-8 BOM
                std::string name = U8(std::filesystem::path(file).filename().wstring());
                UserScript meta;
                std::string error;
                if (!UserScriptStore::ParseMeta(code, meta, error)) {
                    // Plain JavaScript: the page asks which sites it is for, then installs it.
                    result = {{"needSites", true}, {"name", name}, {"code", code}};
                } else {
                    UserScriptStore store(AppShell::UserScriptDir());
                    store.Load();
                    store.Install(code, name, "");
                    if (!store.Save()) throw std::runtime_error("无法保存脚本");
                    AppShell::Instance().UserScriptsChanged();
                    result = {{"installed", meta.name.empty() ? name : meta.name}, {"ignored", meta.ignored}};
                }
            }
        } else if (cmd == "installScript") {
            UserScriptStore store(AppShell::UserScriptDir());
            store.Load();
            store.Install(Arg(args, "code"), Arg(args, "name"), Arg(args, "sites"));
            if (!store.Save()) throw std::runtime_error("无法保存脚本");
            AppShell::Instance().UserScriptsChanged();
            result = true;
        } else if (cmd == "setScriptEnabled" || cmd == "removeScript") {
            UserScriptStore store(AppShell::UserScriptDir());
            store.Load();
            const bool ok = cmd == "removeScript" ? store.Remove(Arg(args, "id")) : store.SetEnabled(Arg(args, "id"), args.value("enabled", true));
            if (ok && !store.Save()) throw std::runtime_error("无法保存脚本");
            if (ok) AppShell::Instance().UserScriptsChanged();
            result = ok;
        } else if (cmd == "getExtensions") {
            result = {{"list", ExtensionsJson()}, {"private", m_private}};
        } else if (cmd == "installExtensionFolder" || cmd == "installExtensionFile") {
            std::filesystem::path chosen;
            if (cmd == "installExtensionFolder") {
                chosen = PickFolder(m_hWnd);
            } else {
                wchar_t file[MAX_PATH] = L"";
                OPENFILENAMEW ofn{sizeof(ofn)};
                ofn.hwndOwner = m_hWnd;
                ofn.lpstrFilter = L"扩展包 (*.crx;*.zip)\0*.crx;*.zip\0所有文件 (*.*)\0*.*\0";
                ofn.lpstrFile = file;
                ofn.nMaxFile = MAX_PATH;
                ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_DONTADDTORECENT;
                if (GetOpenFileNameW(&ofn)) chosen = file;
            }
            if (chosen.empty()) {
                result = nullptr;
            } else {
                const std::string name = cmd == "installExtensionFolder" ? Extensions::Instance().InstallFolder(chosen)
                                                                         : Extensions::Instance().InstallPackage(chosen);
                result = {{"installed", name}};
            }
        } else if (cmd == "getRecommendedExtensions") {
            json list = json::array();
            for (const auto& r : RecommendedExtensions()) {
                list.push_back({{"key", r.key}, {"name", r.name}, {"summary", r.summary}, {"note", r.note},
                                {"homepage", r.homepage}, {"proxy", r.proxy}});
            }
            result = list;
        } else if (cmd == "downloadRecommendedExtension") {
            // Step 1, off the UI thread: the helper downloads and verifies the file.
            const RecommendedExtension* r = FindRecommendedExtension(Arg(args, "key"));
            if (!r) throw std::runtime_error("未知的推荐扩展");
            if (m_private) throw std::runtime_error("无痕窗口不运行扩展，请在普通窗口安装");
            // Through the browser's own line, never around it.
            const int port = WarpManager::Instance().Port();
            if (Config::Instance().GetSettings().warpEnabled && port <= 0) throw std::runtime_error("线路还没连上，连上后再安装");
            std::error_code ec;
            std::filesystem::create_directories(Extensions::Folder(), ec);
            const std::filesystem::path file = Extensions::Folder() / (std::string(".staging-download-") + r->key);
            std::vector<std::pair<std::wstring, std::wstring>> vars = {
                {L"ULB_DOWNLOAD", W(*r->storeId ? StoreDownloadUrl(r->storeId) : std::string(r->url))},
                {L"ULB_DOWNLOAD_TO", file.wstring()}};
            if (*r->storeId) vars.emplace_back(L"ULB_DOWNLOAD_CRXID", W(r->storeId));
            else vars.emplace_back(L"ULB_DOWNLOAD_SHA256", W(r->sha256));
            std::wstring toolArgs = L"-download";
            if (port > 0) toolArgs += L" -socks 127.0.0.1:" + std::to_wstring(port);
            ReplyLater(source, id.dump(), [toolArgs, vars]() {
                std::string error;
                bool done = false;
                for (const auto& line : SplitLines(WarpManager::RunTool(toolArgs, vars, 150000))) {
                    if (line.rfind("DOWNLOADED ", 0) == 0) done = true;
                    else if (line.rfind("ERROR download: ", 0) == 0) error = line.substr(16);
                }
                if (!done) throw std::runtime_error(error.empty() ? "下载失败" : "下载失败：" + error);
                return std::string("true");
            });
            return;
        } else if (cmd == "installRecommendedExtension") {
            // Step 2, on the UI thread (it adds the extension to the browser profile).
            const RecommendedExtension* r = FindRecommendedExtension(Arg(args, "key"));
            if (!r) throw std::runtime_error("未知的推荐扩展");
            const std::filesystem::path file = Extensions::Folder() / (std::string(".staging-download-") + r->key);
            std::error_code ec;
            if (!std::filesystem::exists(file, ec)) throw std::runtime_error("请先下载");
            std::string name;
            try {
                name = Extensions::Instance().InstallPackage(file, r->key);
            } catch (...) {
                std::filesystem::remove(file, ec);
                throw;
            }
            std::filesystem::remove(file, ec);
            result = {{"installed", name}};
        } else if (cmd == "setExtensionEnabled") {
            result = Extensions::Instance().SetEnabled(Arg(args, "dir"), args.value("enabled", true));
        } else if (cmd == "removeExtension") {
            result = Extensions::Instance().Remove(Arg(args, "dir"));
        } else if (cmd == "openExtensionPage") {
            const std::wstring url = Extensions::Instance().PageUrl(Arg(args, "dir"), Arg(args, "page") == "options");
            if (url.empty()) throw std::runtime_error("扩展还没有加载，或没有这个页面");
            if (m_private) throw std::runtime_error("无痕窗口不运行扩展，请在普通窗口打开");
            NewTab(url, true);
            result = true;
        } else if (cmd == "getLines") {
            result = LinesJson();
        } else if (cmd == "addLines") {
            // Checked by the helper itself, so the browser accepts exactly what it can run.
            const std::vector<std::string> links = SplitLines(Arg(args, "text"));
            if (links.empty()) throw std::runtime_error("没有找到链接");
            if (links.size() > 64) throw std::runtime_error("一次最多导入 64 条，订阅请用“添加订阅”");
            ReplyLater(source, id.dump(), [links]() {
                std::string text;
                for (const auto& l : links) text += l + "\n";
                const auto probe = ParseProbe(WarpManager::RunTool(L"-probe -parse-only", {{L"ULB_PROBE", W(text)}}, 15000));
                std::vector<std::string> good;
                json invalid = json::array();
                for (size_t i = 0; i < links.size(); ++i) {
                    const auto it = probe.find(i);
                    if (it != probe.end() && it->second == "ok") good.push_back(links[i]);
                    else invalid.push_back({{"name", LineStore::NameOf(links[i])},
                                            {"reason", it == probe.end() ? "无法检查" : it->second.substr(it->second.find(' ') + 1)}});
                }
                std::lock_guard<std::mutex> lock(g_linesMutex);
                LineStore store = WarpManager::LoadStore();
                const size_t added = store.Add(good);
                if (added && !WarpManager::SaveStore(store)) throw std::runtime_error("无法保存线路");
                return json{{"added", added}, {"duplicate", good.size() - added}, {"invalid", invalid}}.dump();
            });
            return;
        } else if (cmd == "removeLine" || cmd == "renameLine" || cmd == "selectLine" || cmd == "removeSub") {
            bool ok;
            {
                std::lock_guard<std::mutex> lock(g_linesMutex);
                LineStore store = WarpManager::LoadStore();
                const std::string target = Arg(args, "id");
                ok = cmd == "removeLine" ? store.Remove(target)
                   : cmd == "renameLine" ? store.Rename(target, Arg(args, "name"))
                   : cmd == "selectLine" ? store.Select(target)
                   : store.RemoveSub(target);
                if (ok && !WarpManager::SaveStore(store)) throw std::runtime_error("无法保存线路");
            }
            if (ok && cmd == "selectLine") {
                // Choosing a line also switches the network to "我的线路".
                settings.warpEnabled = true;
                settings.useLine = true;
                Config::Instance().Save();
            }
            result = ok;
        } else if (cmd == "probeLines") {
            std::vector<std::pair<std::string, std::string>> targets;  // id, link
            {
                std::lock_guard<std::mutex> lock(g_linesMutex);
                const LineStore store = WarpManager::LoadStore();
                if (args.contains("ids") && args["ids"].is_array()) {
                    for (const auto& v : args["ids"]) {
                        if (!v.is_string()) continue;
                        if (const LineEntry* l = store.Find(v.get<std::string>()); l && targets.size() < 64) targets.emplace_back(l->id, l->link);
                    }
                }
            }
            ReplyLater(source, id.dump(), [targets]() {
                std::string text;
                for (const auto& t : targets) text += t.second + "\n";
                const auto probe = ParseProbe(WarpManager::RunTool(L"-probe", {{L"ULB_PROBE", W(text)}}, 90000));
                json out = json::object();
                for (size_t i = 0; i < targets.size(); ++i) {
                    const auto it = probe.find(i);
                    if (it == probe.end()) out[targets[i].first] = {{"error", "超时"}};
                    else if (it->second.rfind("fail ", 0) == 0) out[targets[i].first] = {{"error", it->second.substr(5)}};
                    else out[targets[i].first] = {{"ms", std::atoi(it->second.c_str())}};
                }
                return out.dump(-1, ' ', false, json::error_handler_t::replace);
            });
            return;
        } else if (cmd == "addSub" || cmd == "updateSub") {
            std::string url = Arg(args, "url"), name = Arg(args, "name"), subId = Arg(args, "id");
            if (cmd == "updateSub") {
                std::lock_guard<std::mutex> lock(g_linesMutex);
                const LineStore store = WarpManager::LoadStore();
                for (const auto& sub : store.Subs()) if (sub.id == subId) url = sub.url;
                if (url.empty()) throw std::runtime_error("订阅不存在");
            } else if (url.rfind("https://", 0) != 0 && url.rfind("http://", 0) != 0) {
                throw std::runtime_error("订阅地址要以 https:// 开头");
            }
            ReplyLater(source, id.dump(), [url, name, subId]() {
                int skipped = 0;
                const std::vector<std::string> links = FetchSubscription(url, skipped);
                std::lock_guard<std::mutex> lock(g_linesMutex);
                LineStore store = WarpManager::LoadStore();
                const std::string target = subId.empty() ? store.AddSub(name, url) : subId;
                if (!store.ReplaceSub(target, links, AppShell::NowMs())) throw std::runtime_error("订阅不存在");
                if (!WarpManager::SaveStore(store)) throw std::runtime_error("无法保存线路");
                size_t count = 0;
                for (const auto& l : store.Lines()) count += l.sub == target;
                return json{{"count", count}, {"skipped", skipped}}.dump();
            });
            return;
        } else if (cmd == "getSettings") {
            result = {{"startupPage", settings.startupPage}, {"newTabPage", settings.newTabPage}, {"homeUrl", U8(settings.startUrl)},
                      {"searchEngine", settings.searchEngine}, {"tabSuspendMinutes", settings.tabSuspendMinutes},
                      {"startShowFavorites", settings.startShowFavorites}, {"startShowReading", settings.startShowReading},
                      {"startBackground", settings.startBackground}, {"saveHistory", settings.saveHistory},
                      {"enableAdBlock", NativeRequestFilter::Instance().IsEnabled()},
                      {"readerTheme", settings.readerTheme}, {"readerFont", settings.readerFont}, {"readerFontSize", settings.readerFontSize},
                      {"hardwareAcceleration", settings.hardwareAcceleration}, {"videoEnhancement", settings.videoEnhancement}, {"preloadLinks", settings.preloadLinks}, {"preloadAvailable", WebViewManager::PrefetchAllowed()}, {"warpFailClosed", settings.warpFailClosed},
                      {"networkMode", !settings.warpEnabled ? "direct" : settings.useLine ? "line" : "warp"},
                      {"lineMode", WarpManager::Instance().LineMode()},
                      {"warpStatus", U8(WarpManager::Instance().StatusText() + (WarpManager::Instance().EndpointText().empty() ? std::wstring() : L"，入口 " + WarpManager::Instance().EndpointText()))}, {"siteCount", lib.SiteCount()}, {"version", ULB_VERSION}};
        } else if (cmd == "setSetting") {
            const std::string key = Arg(args, "key");
            const json value = args.contains("value") ? args["value"] : json(nullptr);
            auto str = [&](std::initializer_list<const char*> allowed) -> std::string {
                if (!value.is_string()) throw std::runtime_error("invalid value");
                const auto v = value.get<std::string>();
                for (const char* a : allowed) if (v == a) return v;
                throw std::runtime_error("invalid value");
            };
            auto boolean = [&]() -> bool {
                if (!value.is_boolean()) throw std::runtime_error("invalid value");
                return value.get<bool>();
            };
            if (key == "startupPage") settings.startupPage = str({"start", "home"});
            else if (key == "newTabPage") settings.newTabPage = str({"start", "blank", "home"});
            else if (key == "searchEngine") settings.searchEngine = str({"google", "brave", "bing", "duckduckgo", "startpage", "baidu"});
            else if (key == "homeUrl") {
                if (!value.is_string()) throw std::runtime_error("invalid value");
                const std::wstring home = WebViewManager::ResolveInput(W(value.get<std::string>()));
                if (!Library::IsWebUrl(U8(home))) throw std::runtime_error("主页必须是 http/https 网址");
                settings.startUrl = home;
            } else if (key == "tabSuspendMinutes") {
                if (!value.is_number_integer()) throw std::runtime_error("invalid value");
                settings.tabSuspendMinutes = std::clamp(value.get<int>(), 0, 240);
            } else if (key == "startShowFavorites") settings.startShowFavorites = boolean();
            else if (key == "startShowReading") settings.startShowReading = boolean();
            else if (key == "startBackground") settings.startBackground = str({"aurora", "ocean", "sunset", "plain"});
            else if (key == "saveHistory") settings.saveHistory = boolean();
            else if (key == "enableAdBlock") NativeRequestFilter::Instance().SetEnabled(boolean());
            else if (key == "readerTheme") settings.readerTheme = str({"light", "sepia", "gray", "dark"});
            else if (key == "readerFont") settings.readerFont = str({"serif", "sans"});
            else if (key == "readerFontSize") {
                if (!value.is_number_integer()) throw std::runtime_error("invalid value");
                settings.readerFontSize = std::clamp(value.get<int>(), 12, 40);
            } else if (key == "hardwareAcceleration") settings.hardwareAcceleration = boolean();
            else if (key == "videoEnhancement") settings.videoEnhancement = boolean();
            else if (key == "preloadLinks") settings.preloadLinks = boolean();
            else if (key == "networkMode") {
                const std::string mode = str({"direct", "warp", "line"});
                settings.warpEnabled = mode != "direct";
                settings.useLine = mode == "line";
            }
            else if (key == "warpFailClosed") settings.warpFailClosed = boolean();
            else throw std::runtime_error("unknown setting");
            Config::Instance().Save();
            libraryChanged = key.rfind("start", 0) == 0;
            result = true;
        } else if (cmd == "clearSiteSettings") {
            lib.ClearSites();
            ElementBlocker::Instance().UpdateAllRulesScripts();
            libraryChanged = true;
            result = true;
        } else if (cmd == "getTabs") {
            result = json::parse(TabsJson(true));
        } else if (cmd == "activateTab") {
            const int tabId = args.value("id", 0);
            if (m_overviewVisible) HideOverview(tabId);
            else ActivateTab(tabId);
            result = true;
        } else if (cmd == "closeTab") {
            CloseTab(args.value("id", 0));
            result = true;
        } else if (cmd == "newTab") {
            HideOverview();
            OnCommand(IDM_NEW_TAB);
            result = true;
        } else if (cmd == "closeOverview") {
            HideOverview();
            result = true;
        } else if (cmd == "getSidebar") {
            json groups = json::array();
            for (const auto& g : lib.Groups()) groups.push_back({{"id", g.id}, {"name", g.name}, {"count", g.tabs.size()}});
            json bms = json::array();
            for (const auto& b : lib.Bookmarks()) bms.push_back(BookmarkJson(b));
            json reading = json::array();
            for (const auto& r : lib.ReadingList()) reading.push_back({{"id", r.id}, {"title", r.title}, {"url", r.url}, {"read", r.read}});
            result = {{"tabs", json::parse(TabsJson(false))}, {"groups", groups}, {"folders", lib.Folders()},
                      {"bookmarks", bms}, {"reading", reading}, {"private", m_private}};
        } else if (cmd == "saveGroup") {
            std::vector<SavedTab> tabs;
            for (const auto& t : m_tabs) {
                const std::wstring url = DisplayUrl(*t);
                if (!url.empty()) tabs.push_back({U8(t->title), U8(url)});
            }
            result = lib.SaveGroup(Arg(args, "name"), tabs, AppShell::NowMs());
            if (result.get<std::string>().empty()) throw std::runtime_error("没有可保存的网页标签");
            libraryChanged = true;
        } else if (cmd == "openGroup") {
            if (const TabGroup* g = lib.FindGroup(Arg(args, "id"))) {
                const std::vector<SavedTab> tabs = g->tabs;  // copy: opening tabs may touch the library
                bool first = true;
                for (const auto& t : tabs) {
                    NewTab(W(t.url), first);
                    first = false;
                }
            }
            result = true;
        } else if (cmd == "deleteGroup") {
            result = lib.RemoveGroup(Arg(args, "id"));
            libraryChanged = true;
        } else if (cmd == "open") {
            const std::string url = Arg(args, "url");
            if (!IsWebOrAbout(url) && !InternalPages::IsInternal(W(url))) throw std::runtime_error("unsupported url");
            if (m_overviewVisible) HideOverview();
            Tab* target = sender ? sender : ActiveTab();
            if (target && target->view) target->view->Navigate(W(url));
            else NewTab(W(url), true);
            result = true;
        } else if (cmd == "newPrivateWindow") {
            AppShell::Instance().OpenWindow(true);
            result = true;
        } else if (cmd == "getArticle") {
            const std::string article = AppShell::Instance().Article(Arg(args, "key"));
            if (article.empty()) {
                result = nullptr;
            } else {
                result = json::parse(article);
                result["prefs"] = {{"theme", settings.readerTheme}, {"font", settings.readerFont}, {"size", settings.readerFontSize}};
            }
        } else if (cmd == "setReaderPrefs") {
            const std::string theme = Arg(args, "theme"), font = Arg(args, "font");
            if (theme == "light" || theme == "sepia" || theme == "gray" || theme == "dark") settings.readerTheme = theme;
            if (font == "serif" || font == "sans") settings.readerFont = font;
            if (args.contains("size") && args["size"].is_number_integer()) settings.readerFontSize = std::clamp(args["size"].get<int>(), 12, 40);
            Config::Instance().Save();
            result = true;
        } else if (cmd == "exitReader") {
            if (sender && sender->id == m_activeId) ToggleReader();
            result = true;
        } else {
            throw std::runtime_error("unknown command");
        }
    } catch (const std::exception& e) {
        const json reply = {{"id", id}, {"error", e.what()}};
        source->PostWebMessageAsJson(W(reply.dump()).c_str());
        return;
    }

    const json reply = {{"id", id}, {"result", result}};
    source->PostWebMessageAsJson(W(reply.dump(-1, ' ', false, json::error_handler_t::replace)).c_str());
    if (libraryChanged) AppShell::Instance().LibraryChanged();
}

} // namespace UltraLight
