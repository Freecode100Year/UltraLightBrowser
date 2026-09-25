#pragma once

#include <windows.h>
#include <wrl.h>
#include <wil/com.h>
#include <WebView2.h>
#include <WebView2EnvironmentOptions.h>
#include <string>
#include <functional>

namespace UltraLight {

class WebViewManager {
public:
    using ReadyCallback = std::function<void()>;
    using TitleChangedCallback = std::function<void(const std::wstring&)>;
    using SourceChangedCallback = std::function<void(const std::wstring&)>;
    using FullScreenCallback = std::function<void(bool)>;
    using ZoomFactorChangedCallback = std::function<void(double)>;
    using UserActivityCallback = std::function<void()>;

    WebViewManager();
    ~WebViewManager() = default;

    // Initialize environment and controller bound to hWnd
    HRESULT Initialize(HWND hWndParent, ReadyCallback onReady = nullptr);

    // Synchronize bounds on WM_SIZE
    void Resize(const RECT& bounds);
    void NotifyParentWindowPositionChanged();

    // Navigation controls
    void Navigate(const std::wstring& url);
    void GoBack();
    void GoForward();
    void Reload();
    void Stop();

    // Zoom controls
    void ZoomIn();
    void ZoomOut();
    void ZoomReset();
    double GetZoomFactor() const;
    void SetZoomFactor(double zoom);

    // Cache and temporary files purge
    void ShutdownAndPurgeData();
    static void PurgeAllCacheAndTempFiles();

    // Callbacks
    void SetTitleChangedCallback(TitleChangedCallback cb) { m_titleChangedCb = cb; }
    void SetSourceChangedCallback(SourceChangedCallback cb) { m_sourceChangedCb = cb; }
    void SetFullScreenCallback(FullScreenCallback cb) { m_fullScreenCb = cb; }
    void SetZoomFactorChangedCallback(ZoomFactorChangedCallback cb) { m_zoomFactorChangedCb = cb; }
    void SetUserActivityCallback(UserActivityCallback cb) { m_userActivityCb = cb; }

    // Direct interface access
    ICoreWebView2Environment* GetEnvironment() const { return m_environment.get(); }
    ICoreWebView2Controller* GetController() const { return m_controller.get(); }
    ICoreWebView2* GetWebView() const { return m_webView.get(); }

private:
    void RegisterEventHandlers();

    HWND m_hWndParent = nullptr;
    wil::com_ptr<ICoreWebView2Environment> m_environment;
    wil::com_ptr<ICoreWebView2Controller> m_controller;
    wil::com_ptr<ICoreWebView2> m_webView;

    ReadyCallback m_onReady;
    TitleChangedCallback m_titleChangedCb;
    SourceChangedCallback m_sourceChangedCb;
    FullScreenCallback m_fullScreenCb;
    ZoomFactorChangedCallback m_zoomFactorChangedCb;
    UserActivityCallback m_userActivityCb;
};

} // namespace UltraLight
