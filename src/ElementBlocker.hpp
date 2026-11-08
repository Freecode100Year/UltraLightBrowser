#pragma once

#include <windows.h>
#include <wrl.h>
#include <wil/com.h>
#include <WebView2.h>
#include <string>
#include <functional>
#include <unordered_map>
#include <vector>

namespace UltraLight {

class ElementBlocker {
public:
    static ElementBlocker& Instance();

    // Sets up pre-render CSS injection for zero flicker (one registration per tab)
    void Initialize(ICoreWebView2* webView);
    void Unregister(ICoreWebView2* webView);

    // Synchronize and update pre-render injection script. When reloadWhenReady is
    // one of the tabs, it reloads once its new script is registered, so the reload
    // already sees the change (registration completes asynchronously).
    void UpdateRulesScript(ICoreWebView2* webView, bool reloadWhenReady = false);
    void UpdateAllRulesScripts(ICoreWebView2* reloadWhenReady = nullptr);

    // Top-level hosts where the content blocker is switched off ("此网站的设置").
    using DisabledHostsProvider = std::function<std::vector<std::wstring>()>;
    void SetDisabledHostsProvider(DisabledHostsProvider provider) { m_disabledHosts = std::move(provider); }

    // Updates injection rules based on domain navigation
    void OnNavigationStarting(ICoreWebView2* webView, const std::wstring& uri);

    // Toggle interactive element picker mode (Ctrl + Shift + H)
    void TogglePickerMode(ICoreWebView2* webView);

    // Handles messages from Web (WebMessageReceived) with verified source origin
    bool HandleWebMessage(ICoreWebView2* sender, const std::wstring& messageJson, const std::wstring& sourceUri = L"");

    bool IsPickerActive() const { return m_pickerActive; }
    const std::wstring& GetCurrentHost() const { return m_currentHost; }

private:
    ElementBlocker() = default;
    ~ElementBlocker() = default;

    ElementBlocker(const ElementBlocker&) = delete;
    ElementBlocker& operator=(const ElementBlocker&) = delete;

    std::string ExtractHostFromUri(const std::wstring& uri);

    bool m_pickerActive = false;
    std::wstring m_currentHost;
    std::unordered_map<ICoreWebView2*, std::wstring> m_injectedScriptIds;
    ICoreWebView2* m_pickerWebView = nullptr;
    DisabledHostsProvider m_disabledHosts;
};

} // namespace UltraLight
