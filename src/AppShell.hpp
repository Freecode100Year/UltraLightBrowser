#pragma once

#include <windows.h>
#include <objbase.h>
#include <WebView2.h>
#include <wil/com.h>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "Library.hpp"

namespace UltraLight {

class MainWindow;
class WebViewManager;

// Process-wide browser state: windows, the shared WebView2 environment, the
// user library and shutdown/cleanup ordering.
class AppShell {
public:
    static AppShell& Instance();

    int Run(HINSTANCE hInstance, int nCmdShow);

    MainWindow* OpenWindow(bool isPrivate, const std::vector<std::wstring>& urls = {});
    void OnWindowDestroyed(MainWindow* window);
    size_t WindowCount() const;
    size_t VisibleWindowCount() const;
    bool IsLastWindow(const MainWindow* window) const;
    void PrepareExit(MainWindow* lastWindow);  // called before the last window closes its tabs

    // Shared environment; callbacks run once it exists (immediately if it already does).
    void WhenEnvironmentReady(HWND errorOwner, std::function<void(ICoreWebView2Environment*)> callback);

    Library& Lib() { return *m_library; }
    void LibraryChanged();                     // refresh built-in pages in every window
    void SaveLibrarySoon();     // serialize now, write on a background thread
    void SaveLibraryNow();      // synchronous (exit)

    // Closed tabs stay alive until late completions have drained.
    void Retire(std::unique_ptr<WebViewManager> view);

    // Reader articles, keyed for reader.html#<key>
    std::string StoreArticle(const std::string& articleJson);
    std::string Article(const std::string& key) const;

    static std::int64_t NowMs();
    static bool AnotherInstanceRunning();  // another UltraLightBrowser process has a window open

private:
    AppShell() = default;
    void CollectGarbage();
    void FinalCleanup();

    HINSTANCE m_hInstance = nullptr;
    int m_nCmdShow = SW_SHOWDEFAULT;
    std::vector<std::unique_ptr<MainWindow>> m_windows;
    std::vector<std::unique_ptr<MainWindow>> m_deadWindows;
    std::vector<std::pair<ULONGLONG, std::unique_ptr<WebViewManager>>> m_graveyard;
    std::unique_ptr<Library> m_library;
    wil::com_ptr<ICoreWebView2Environment> m_environment;
    bool m_environmentRequested = false;
    bool m_environmentFailed = false;
    std::vector<std::function<void(ICoreWebView2Environment*)>> m_envWaiters;
    std::map<std::string, std::string> m_articles;
    std::vector<std::string> m_articleOrder;
    std::uint64_t m_articleCounter = 0;
    UINT32 m_browserPid = 0;
    ULONG_PTR m_gdiplusToken = 0;
    bool m_exiting = false;
    std::mutex m_saveMutex;
    std::atomic<bool> m_saveFailed{false};  // a background write failed; the next save retries
};

} // namespace UltraLight
