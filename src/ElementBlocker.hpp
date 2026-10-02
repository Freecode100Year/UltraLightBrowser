#pragma once

#include <windows.h>
#include <wrl.h>
#include <wil/com.h>
#include <WebView2.h>
#include <string>
#include <unordered_map>

namespace UltraLight {

class ElementBlocker {
public:
    static ElementBlocker& Instance();

    // Sets up pre-render CSS injection for zero flicker (one registration per tab)
    void Initialize(ICoreWebView2* webView);
    void Unregister(ICoreWebView2* webView);

    // Synchronize and update pre-render injection script
    void UpdateRulesScript(ICoreWebView2* webView);
    void UpdateAllRulesScripts();

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
};

} // namespace UltraLight
