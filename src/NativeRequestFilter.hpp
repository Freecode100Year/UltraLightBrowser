#pragma once

#include <windows.h>
#include <wrl.h>
#include <wil/com.h>
#include <WebView2.h>
#include <string>
#include <vector>
#include <atomic>

namespace UltraLight {

class NativeRequestFilter {
public:
    static NativeRequestFilter& Instance();

    // Attach native network request interceptor to WebView2
    void Initialize(ICoreWebView2* webView, ICoreWebView2Environment* environment);

    // Test whether an outgoing request URI should be intercepted and blocked
    bool ShouldBlock(const std::wstring& uri);

    // Enable / disable native blocking
    void SetEnabled(bool enabled);
    bool IsEnabled() const { return m_enabled; }

    // Intercepted request counter
    uint64_t GetBlockedCount() const { return m_blockedCount.load(); }
    void ResetBlockedCount() { m_blockedCount.store(0); }

    // Event handler for WebResourceRequested
    HRESULT HandleWebResourceRequested(ICoreWebView2* sender, ICoreWebView2WebResourceRequestedEventArgs* args);

private:
    NativeRequestFilter();
    ~NativeRequestFilter() = default;

    NativeRequestFilter(const NativeRequestFilter&) = delete;
    NativeRequestFilter& operator=(const NativeRequestFilter&) = delete;

    std::wstring ExtractHost(const std::wstring& uri);

    bool m_enabled = true;
    std::atomic<uint64_t> m_blockedCount{0};
    ICoreWebView2Environment* m_environment = nullptr;
    EventRegistrationToken m_resourceRequestedToken{};

    std::vector<std::wstring> m_blockedDomains;
    std::vector<std::wstring> m_blockedKeywords;
};

} // namespace UltraLight
