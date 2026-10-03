#include "AppShell.hpp"
#include "Commands.hpp"
#include "Config.hpp"
#include "DnsManager.hpp"
#include "Icons.hpp"
#include "InternalPages.hpp"
#include "MainWindow.hpp"
#include "NativeRequestFilter.hpp"
#include "PowerManager.hpp"
#include "StringUtils.hpp"
#include "WebViewManager.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <set>
#include <shobjidl.h>
#include <cstdio>
#include <thread>
#include "GdiPlus.hpp"

#pragma comment(lib, "gdiplus.lib")

namespace UltraLight {

namespace {

// Dark context menus (uxtheme ordinals; present since Windows 10 1903).
void EnableDarkMenus() {
    HMODULE uxtheme = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!uxtheme) return;
    using SetPreferredAppModeFn = int(WINAPI*)(int);
    using FlushMenuThemesFn = void(WINAPI*)();
    auto setMode = reinterpret_cast<SetPreferredAppModeFn>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(135)));
    auto flush = reinterpret_cast<FlushMenuThemesFn>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(136)));
    if (setMode) setMode(2);  // ForceDark
    if (flush) flush();
}

bool IsBrowserWindow(HWND hwnd) {
    wchar_t cls[64]{};
    GetClassNameW(hwnd, cls, 64);
    return wcscmp(cls, L"UltraLightBrowserMainWindow") == 0;
}

std::string FaviconFileStem(const std::string& host) {
    std::string safe;
    for (char c : host) {
        safe += ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-') ? c : '_';
    }
    return safe;
}

// Site icons are cached for the start page and bookmarks; icons of sites that are
// not bookmarked, in the reading list or in a tab group would reveal visited sites.
void PruneFavicons(const Library& lib) {
    std::set<std::string> keep;
    for (const auto& b : lib.Bookmarks()) keep.insert(FaviconFileStem(Library::HostOf(b.url)));
    for (const auto& r : lib.ReadingList()) keep.insert(FaviconFileStem(Library::HostOf(r.url)));
    for (const auto& g : lib.Groups()) {
        for (const auto& t : g.tabs) keep.insert(FaviconFileStem(Library::HostOf(t.url)));
    }
    std::error_code ec;
    std::vector<std::filesystem::path> doomed;
    for (const auto& entry : std::filesystem::directory_iterator(InternalPages::FaviconCacheDir(), ec)) {
        if (!keep.count(entry.path().stem().string())) doomed.push_back(entry.path());
    }
    for (const auto& path : doomed) std::filesystem::remove_all(path, ec);
}

// Windows keeps resolved host names in its DNS client cache (ipconfig /displaydns).
void FlushSystemDnsCache() {
    HMODULE dnsapi = LoadLibraryExW(L"dnsapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dnsapi) return;
    using FlushFn = BOOL(WINAPI*)();
    if (auto flush = reinterpret_cast<FlushFn>(GetProcAddress(dnsapi, "DnsFlushResolverCache"))) flush();
    FreeLibrary(dnsapi);
}

// Taskbar jump list entries ("最近") added by file dialogs.
void ClearJumpList() {
    wil::com_ptr<IApplicationDestinations> destinations;
    if (SUCCEEDED(CoCreateInstance(CLSID_ApplicationDestinations, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&destinations)))) {
        destinations->RemoveAllDestinations();
    }
}

} // namespace

AppShell& AppShell::Instance() {
    static AppShell s_instance;
    return s_instance;
}

std::int64_t AppShell::NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

bool AppShell::AnotherInstanceRunning() {
    struct Search { DWORD self; bool found; } search{GetCurrentProcessId(), false};
    EnumWindows([](HWND hwnd, LPARAM param) -> BOOL {
        auto* s = reinterpret_cast<Search*>(param);
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid != s->self && IsBrowserWindow(hwnd)) {
            s->found = true;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&search));
    return search.found;
}


int AppShell::Run(HINSTANCE hInstance, int nCmdShow) {
    m_hInstance = hInstance;
    m_nCmdShow = nCmdShow;

    Gdiplus::GdiplusStartupInput gdiplusInput;
    Gdiplus::GdiplusStartup(&m_gdiplusToken, &gdiplusInput, nullptr);
    EnableDarkMenus();

    m_library = std::make_unique<Library>(Config::Instance().GetAppDataPath());
    m_library->Load();
    InternalPages::Extract();
    // After a crash or a forced kill the exit cleanup never ran; finish it now.
    if (!AnotherInstanceRunning()) {
        PruneFavicons(*m_library);
        FlushSystemDnsCache();
    }

    // Per-site ad-block exceptions ("此网站的设置").
    NativeRequestFilter::Instance().SetSiteAllowsAdsCallback([this](const std::wstring& topHost) {
        return !m_library->Site(StringUtils::WideToUtf8(topHost)).adblock;
    });

    const auto& settings = Config::Instance().GetSettings();
    std::vector<std::wstring> urls;
    if (settings.startupPage == "home" && !settings.startUrl.empty()) urls.push_back(settings.startUrl);
    if (!OpenWindow(false, urls)) return 1;

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN) {
            HWND root = GetAncestor(msg.hwnd, GA_ROOT);
            if (root && IsBrowserWindow(root)) {
                const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
                const UINT key = static_cast<UINT>(msg.wParam);
                // Let edit controls keep their own editing keys.
                const bool editKey = ctrl && !shift && !alt && (key == 'A' || key == 'C' || key == 'V' || key == 'X' || key == 'Z' || key == 'Y');
                if (!editKey) {
                    if (const WORD command = MapShortcut(key, ctrl, shift, alt)) {
                        SendMessageW(root, WM_COMMAND, MAKEWPARAM(command, 1), 0);
                        continue;
                    }
                }
            }
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        CollectGarbage();
    }

    Icons::ReleaseMenuBitmaps();
    if (m_gdiplusToken) Gdiplus::GdiplusShutdown(m_gdiplusToken);
    return static_cast<int>(msg.wParam);
}

MainWindow* AppShell::OpenWindow(bool isPrivate, const std::vector<std::wstring>& urls) {
    auto window = std::make_unique<MainWindow>(isPrivate);
    if (!window->Create(m_hInstance, m_windows.empty() ? m_nCmdShow : SW_SHOWNORMAL, urls)) return nullptr;
    MainWindow* raw = window.get();
    m_windows.push_back(std::move(window));
    return raw;
}

void AppShell::OnWindowDestroyed(MainWindow* window) {
    auto it = std::find_if(m_windows.begin(), m_windows.end(), [window](const auto& w) { return w.get() == window; });
    if (it == m_windows.end()) return;
    m_deadWindows.push_back(std::move(*it));
    m_windows.erase(it);
    if (m_windows.empty()) {
        FinalCleanup();
        PostQuitMessage(0);
    }
}

size_t AppShell::WindowCount() const { return m_windows.size(); }

size_t AppShell::VisibleWindowCount() const {
    size_t n = 0;
    for (const auto& w : m_windows) {
        if (w->GetHwnd() && IsWindowVisible(w->GetHwnd()) && !IsIconic(w->GetHwnd())) ++n;
    }
    return n;
}

bool AppShell::IsLastWindow(const MainWindow* window) const {
    return m_windows.size() == 1 && m_windows.front().get() == window;
}

void AppShell::PrepareExit(MainWindow* lastWindow) {
    m_exiting = true;
    SaveLibraryNow();

    // Nothing from the session outlives it: history and site settings were never
    // written, and cookies, cache and site data are cleared here and deleted from
    // disk in FinalCleanup.
    if (lastWindow) {
        m_browserPid = lastWindow->BrowserProcessId();
        WebViewManager::ClearProfileData(lastWindow->ActiveWebView());
    }
}

void AppShell::FinalCleanup() {
    if (!m_exiting) {
        m_exiting = true;
        SaveLibraryNow();
    }
    for (auto& [when, view] : m_graveyard) view->Close();
    m_graveyard.clear();
    m_environment.reset();

    // A second copy of the browser shares the profile; the last one to exit cleans up.
    if (AnotherInstanceRunning()) return;
    if (m_browserPid != 0) {
        HANDLE hProc = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, m_browserPid);
        if (hProc) {
            if (WaitForSingleObject(hProc, 1500) == WAIT_TIMEOUT) {
                TerminateProcess(hProc, 0);
                WaitForSingleObject(hProc, 300);
            }
            CloseHandle(hProc);
        }
    }
    WebViewManager::PurgeAllCacheAndTempFiles();
    PruneFavicons(*m_library);
    FlushSystemDnsCache();
    ClearJumpList();
}

void AppShell::WhenEnvironmentReady(HWND errorOwner, std::function<void(ICoreWebView2Environment*)> callback) {
    if (m_environment) {
        callback(m_environment.get());
        return;
    }
    if (m_environmentFailed) return;
    m_envWaiters.push_back(std::move(callback));
    if (m_environmentRequested) return;
    m_environmentRequested = true;
    WebViewManager::CreateEnvironment(errorOwner, [this](HRESULT hr, ICoreWebView2Environment* env) {
        if (FAILED(hr) || !env) {
            m_environmentFailed = true;
            m_envWaiters.clear();
            return;
        }
        m_environment = env;
        DnsManager::Instance().ApplySettings();
        auto waiters = std::move(m_envWaiters);
        m_envWaiters.clear();
        for (auto& w : waiters) w(m_environment.get());
    });
}

void AppShell::LibraryChanged() {
    SaveLibrarySoon();
    for (const auto& w : m_windows) w->OnLibraryChanged();
}

void AppShell::SaveLibrarySoon() {
    auto snapshot = m_library->TakeSnapshot();
    if (snapshot.empty()) return;
    // File I/O stays off the UI thread; writes are serialized by the mutex.
    std::thread([this, snapshot = std::move(snapshot)]() {
        std::lock_guard<std::mutex> lock(m_saveMutex);
        for (const auto& [path, data] : snapshot) Library::WriteFileAtomic(path, data);
    }).detach();
}

void AppShell::SaveLibraryNow() {
    std::lock_guard<std::mutex> lock(m_saveMutex);  // waits for background writes
    m_library->Save();
}

void AppShell::Retire(std::unique_ptr<WebViewManager> view) {
    if (!view) return;
    view->Close();
    m_graveyard.emplace_back(GetTickCount64(), std::move(view));
}

void AppShell::CollectGarbage() {
    m_deadWindows.clear();
    const ULONGLONG now = GetTickCount64();
    m_graveyard.erase(std::remove_if(m_graveyard.begin(), m_graveyard.end(),
        [now](const auto& entry) { return now - entry.first > 30000; }), m_graveyard.end());
}

std::string AppShell::StoreArticle(const std::string& articleJson) {
    const std::string key = "a" + std::to_string(++m_articleCounter) + "-" + std::to_string(GetTickCount64() % 100000);
    m_articles[key] = articleJson;
    m_articleOrder.push_back(key);
    while (m_articleOrder.size() > 30) {
        m_articles.erase(m_articleOrder.front());
        m_articleOrder.erase(m_articleOrder.begin());
    }
    return key;
}

std::string AppShell::Article(const std::string& key) const {
    const auto it = m_articles.find(key);
    return it != m_articles.end() ? it->second : std::string();
}

} // namespace UltraLight
