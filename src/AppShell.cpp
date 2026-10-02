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
#include <cstdio>
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

} // namespace

AppShell& AppShell::Instance() {
    static AppShell s_instance;
    return s_instance;
}

std::int64_t AppShell::NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string AppShell::DayKey(int daysAgo) {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    if (daysAgo != 0) {
        FILETIME ft{};
        SystemTimeToFileTime(&st, &ft);
        ULARGE_INTEGER v{};
        v.LowPart = ft.dwLowDateTime;
        v.HighPart = ft.dwHighDateTime;
        v.QuadPart -= static_cast<ULONGLONG>(daysAgo) * 24ULL * 3600ULL * 10000000ULL;
        ft.dwLowDateTime = v.LowPart;
        ft.dwHighDateTime = v.HighPart;
        FileTimeToSystemTime(&ft, &st);
    }
    char buf[16];
    snprintf(buf, sizeof(buf), "%04u-%02u-%02u", st.wYear, st.wMonth, st.wDay);
    return buf;
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

    // Privacy report and per-site ad-block exceptions.
    NativeRequestFilter::Instance().SetBlockedCallback([this](const std::wstring& requestHost, const std::wstring& topHost) {
        const std::string tracker = StringUtils::WideToUtf8(NativeRequestFilter::GetBaseDomain(requestHost));
        m_library->RecordBlocked(tracker, StringUtils::WideToUtf8(topHost), DayKey());
    });
    NativeRequestFilter::Instance().SetSiteAllowsAdsCallback([this](const std::wstring& topHost) {
        return !m_library->Site(StringUtils::WideToUtf8(topHost)).adblock;
    });

    const auto& settings = Config::Instance().GetSettings();
    bool opened = false;
    if (settings.startupPage == "restore") {
        for (const auto& tabs : m_library->Session()) {
            std::vector<std::wstring> urls;
            for (const auto& t : tabs) urls.push_back(StringUtils::Utf8ToWide(t.url));
            if (OpenWindow(false, urls)) opened = true;
        }
    }
    if (!opened) {
        std::vector<std::wstring> urls;
        if (settings.startupPage == "home" && !settings.startUrl.empty()) urls.push_back(settings.startUrl);
        if (!OpenWindow(false, urls)) return 1;
    }

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
    const auto& settings = Config::Instance().GetSettings();
    std::vector<std::vector<SavedTab>> session;
    if (settings.startupPage == "restore" && lastWindow && !lastWindow->IsPrivate()) {
        session.push_back(lastWindow->SavedTabs());
    }
    m_library->SetSession(session);
    if (settings.clearHistoryOnExit) m_library->ClearHistory(0);
    m_library->Save();

    // Same privacy guarantee as before: cookies, cache and site data never outlive the session.
    if (lastWindow) {
        m_browserPid = lastWindow->BrowserProcessId();
        WebViewManager::ClearProfileData(lastWindow->ActiveWebView());
    }
}

void AppShell::FinalCleanup() {
    if (!m_exiting) {
        m_exiting = true;
        m_library->Save();
    }
    for (auto& [when, view] : m_graveyard) view->Close();
    m_graveyard.clear();
    m_environment.reset();

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
    // Library writes are cheap; windows also flush on a timer and at exit.
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
