#pragma once

#include <windows.h>
#include <wrl.h>
#include <wil/com.h>
#include <WebView2.h>
#include <string>
#include <vector>
#include <unordered_set>
#include <atomic>
#include <mutex>
#include <functional>
#include <unordered_map>

namespace UltraLight {

class NativeRequestFilter {
public:
    static NativeRequestFilter& Instance();

    // Attach native network request interceptor to a tab's WebView2
    void Initialize(ICoreWebView2* webView, ICoreWebView2Environment* environment);
    void Unregister(ICoreWebView2* webView);

    // Site exceptions are owned by the browser library.
    using SiteAllowsAdsCallback = std::function<bool(const std::wstring& topHost)>;
    void SetSiteAllowsAdsCallback(SiteAllowsAdsCallback cb) { m_siteAllowsAds = std::move(cb); }

    // Test whether an outgoing request URI should be intercepted and blocked
    bool ShouldBlock(const std::wstring& uri);

    // Enable / disable native blocking
    void SetEnabled(bool enabled);
    bool IsEnabled() const { return m_enabled; }

    // Intercepted request counter
    uint64_t GetBlockedCount() const { return m_blockedCount.load(); }
    void ResetBlockedCount() { m_blockedCount.store(0); }

    // Main-frame navigation tracking to prevent false-positive blocking of top-level navigations
    void SetMainFrameNavigation(ICoreWebView2* webView, const std::wstring& uri);
    void ClearMainFrameNavigation(ICoreWebView2* webView);
    bool IsMainFrameNavigation(ICoreWebView2* webView, const std::wstring& uri) const;
    std::wstring GetCurrentMainHost(ICoreWebView2* webView) const;

    // Event handler for WebResourceRequested
    HRESULT HandleWebResourceRequested(ICoreWebView2* sender, ICoreWebView2WebResourceRequestedEventArgs* args);

    // Robust hostname extractor (handles user:pass@, IPv6 [...], port, and trailing dot stripping)
    static std::wstring ExtractHost(const std::wstring& uri);

    // Domain comparison utilities for third-party scoping
    static std::wstring GetBaseDomain(const std::wstring& host);
    static bool IsThirdParty(const std::wstring& reqHost, const std::wstring& topHost);

    const std::unordered_set<std::wstring>& GetBlockedDomainSet() const { return m_blockedDomainSet; }

private:
    NativeRequestFilter();
    ~NativeRequestFilter() = default;

    NativeRequestFilter(const NativeRequestFilter&) = delete;
    NativeRequestFilter& operator=(const NativeRequestFilter&) = delete;

    bool m_enabled = true;
    std::atomic<uint64_t> m_blockedCount{0};
    ICoreWebView2Environment* m_environment = nullptr;
    struct NavState {
        std::wstring pendingUri;
        std::wstring mainHost;
    };
    mutable std::mutex m_navMutex;
    std::unordered_map<ICoreWebView2*, NavState> m_nav;
    SiteAllowsAdsCallback m_siteAllowsAds;

    std::unordered_set<std::wstring> m_blockedDomainSet;
    std::vector<std::wstring> m_blockedKeywords;
};

} // namespace UltraLight
