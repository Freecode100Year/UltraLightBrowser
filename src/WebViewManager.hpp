#pragma once

#include <windows.h>
#include <wrl.h>
#include <wil/com.h>
#include <WebView2.h>
#include <WebView2EnvironmentOptions.h>
#include <string>
#include <functional>
#include <filesystem>
#include <cstdint>
#include <vector>

namespace UltraLight {

enum class AudioEndpointType {
    Headphones,
    Speakers,
    Unknown
};

// One browser tab: a WebView2 controller plus everything injected into it
// (identity override, audio enhancer, blockers). All tabs share one environment.
class WebViewManager {
public:
    using ReadyCallback = std::function<void()>;
    using EnvironmentCallback = std::function<void(HRESULT, ICoreWebView2Environment*)>;
    using TitleChangedCallback = std::function<void(const std::wstring&)>;
    using SourceChangedCallback = std::function<void(const std::wstring&)>;
    using FullScreenCallback = std::function<void(bool)>;
    using ZoomFactorChangedCallback = std::function<void(double)>;
    using UserActivityCallback = std::function<void()>;
    using NavigationStateCallback = std::function<void(bool isLoading)>;
    using NavigationStartingCallback = std::function<bool(const std::wstring& uri, bool userInitiated, bool historyNavigation)>;
    using NavigationCompletedCallback = std::function<void(bool success, const std::wstring& uri)>;
    using HistoryCallback = std::function<void(bool canGoBack, bool canGoForward)>;
    using FaviconCallback = std::function<void(const std::wstring& pageUri, const std::vector<std::uint8_t>& png)>;
    using NewWindowCallback = std::function<void(ICoreWebView2NewWindowRequestedEventArgs*)>;
    using PermissionCallback = std::function<COREWEBVIEW2_PERMISSION_STATE(COREWEBVIEW2_PERMISSION_KIND, const std::wstring& uri)>;
    using InternalMessageCallback = std::function<void(const std::wstring& json)>;
    using ContextActionCallback = std::function<void(const std::wstring& action, const std::wstring& link, const std::wstring& text)>;
    using ContentLoadedCallback = std::function<void(bool domReady)>;

    WebViewManager();
    ~WebViewManager();
    WebViewManager(const WebViewManager&) = delete;
    WebViewManager& operator=(const WebViewManager&) = delete;

    // Shared browser environment (one per process); retried and self-healing.
    static void CreateEnvironment(HWND errorOwner, EnvironmentCallback done, int attempt = 0);

    // Create this tab's controller inside `environment`.
    HRESULT Initialize(HWND hWndParent, ICoreWebView2Environment* environment, bool inPrivate,
                       bool visible, ReadyCallback onReady = nullptr);
    bool IsReady() const { return m_ready; }
    bool IsPrivate() const { return m_inPrivate; }
    void Close();
    UINT32 BrowserProcessId() const;
    static void ClearProfileData(ICoreWebView2* webView);

    // Synchronize bounds on WM_SIZE
    void Resize(const RECT& bounds);
    void NotifyParentWindowPositionChanged();

    // Navigation controls
    static std::wstring ResolveInput(const std::wstring& input);
    static std::wstring SearchUrl(const std::wstring& query);
    void Navigate(const std::wstring& url);
    bool ConsumeInternalGrant();
    void GoBack();
    void GoForward();
    void Reload();
    HRESULT ApplyUserAgentProfile(const std::string& profile, bool reloadPage = false);
    void OpenIdentitySelfTest();
    void Stop();

    // Zoom controls
    void ZoomIn();
    void ZoomOut();
    void ZoomReset();
    double GetZoomFactor() const;
    void SetZoomFactor(double zoom);

    // Cache and temporary files purge
    static void PurgeAllCacheAndTempFiles();

    // Callbacks
    void SetTitleChangedCallback(TitleChangedCallback cb) { m_titleChangedCb = std::move(cb); }
    void SetSourceChangedCallback(SourceChangedCallback cb) { m_sourceChangedCb = std::move(cb); }
    void SetFullScreenCallback(FullScreenCallback cb) { m_fullScreenCb = std::move(cb); }
    void SetZoomFactorChangedCallback(ZoomFactorChangedCallback cb) { m_zoomFactorChangedCb = std::move(cb); }
    void SetUserActivityCallback(UserActivityCallback cb) { m_userActivityCb = std::move(cb); }
    void SetAudioPlayingCallback(std::function<void(bool)> cb) { m_audioPlayingCb = std::move(cb); }
    void SetNavigationStateCallback(NavigationStateCallback cb) { m_navStateCb = std::move(cb); }
    void SetNavigationStartingCallback(NavigationStartingCallback cb) { m_navStartingCb = std::move(cb); }
    void SetNavigationCompletedCallback(NavigationCompletedCallback cb) { m_navCompletedCb = std::move(cb); }
    void SetHistoryCallback(HistoryCallback cb) { m_historyCb = std::move(cb); }
    void SetFaviconCallback(FaviconCallback cb) { m_faviconCb = std::move(cb); }
    void SetNewWindowCallback(NewWindowCallback cb) { m_newWindowCb = std::move(cb); }
    void SetPermissionCallback(PermissionCallback cb) { m_permissionCb = std::move(cb); }
    void SetInternalMessageCallback(InternalMessageCallback cb) { m_internalMessageCb = std::move(cb); }
    void SetContextActionCallback(ContextActionCallback cb) { m_contextActionCb = std::move(cb); }
    void SetContentLoadedCallback(ContentLoadedCallback cb) { m_contentLoadedCb = std::move(cb); }

    // Audio & Visibility status
    bool IsDocumentPlayingAudio() const { return m_isPlayingAudio; }
    void SetVisible(bool isVisible);
    bool IsVisible() const;
    void ApplyMemoryUsageTargetLow();

    // Audio Enhancement & Virtual Surround (v1.6.0)
    void UpdateAudioEnhancer(bool reloadPage = false);
    void ShowMediaDiagnostics();
    void InjectSurroundSoundScript(bool reloadPage = false);
    AudioEndpointType GetDetectedAudioEndpoint() const;

    // Direct interface access
    ICoreWebView2Environment* GetEnvironment() const { return m_environment.get(); }
    ICoreWebView2Controller* GetController() const { return m_controller.get(); }
    ICoreWebView2* GetWebView() const { return m_webView.get(); }

private:
    HRESULT CreateController(int attempt);
    void OnControllerCreated(ICoreWebView2Controller* controller);
    // macOS Edge identity (UA, Client Hints, navigator.platform, GPU strings, workers)
    void CaptureUaMetadata(std::function<void(const std::string&)> done);
    void EnableMacSpoof(const std::string& capturedJson);
    void DisableMacSpoof();
    void FinishProbe(const std::string& result);
    void OnTargetAttached(const std::wstring& paramsJson);
    void CallCdp(const wchar_t* method, const std::string& params, const wchar_t* sessionId = nullptr);
    static void CALLBACK ProbeTimeoutProc(HWND, UINT, UINT_PTR, DWORD);
    static void CALLBACK ProbeFinishProc(HWND, UINT, UINT_PTR, DWORD);
    static void SanitizeLocalState(const std::filesystem::path& userDataDir);
    void RegisterEventHandlers();

    static std::wstring s_capturedUaMetadata;

    HWND m_hWndParent = nullptr;
    wil::com_ptr<ICoreWebView2Environment> m_environment;
    wil::com_ptr<ICoreWebView2Controller> m_controller;
    wil::com_ptr<ICoreWebView2> m_webView;
    bool m_inPrivate = false;
    bool m_startVisible = true;
    bool m_ready = false;
    bool m_closed = false;
    bool m_internalGrant = false;
    RECT m_bounds{};
    bool m_hasBounds = false;

    std::uint64_t m_audioScriptGeneration = 0;
    unsigned m_pendingAudioScriptRegistrations = 0;
    bool m_audioReloadPending = false;
    std::wstring m_audioScriptId;
    std::wstring m_defaultUserAgent;
    bool m_macSpoofActive = false;
    bool m_spoofStarting = false;
    std::wstring m_pendingNavigation;
    std::string m_macOverrideParams;
    std::wstring m_stealthScriptId;
    std::wstring m_stealthSource;
    bool m_targetEventsHooked = false;
    wil::com_ptr<ICoreWebView2Controller> m_probeController;
    std::function<void(const std::string&)> m_probeDone;
    bool m_probeFinishing = false;
    std::string m_probeResult;
    UINT_PTR m_probeTimer = 0;
    bool m_isPlayingAudio = false;
    EventRegistrationToken m_audioPlayingToken{};

    ReadyCallback m_onReady;
    TitleChangedCallback m_titleChangedCb;
    SourceChangedCallback m_sourceChangedCb;
    FullScreenCallback m_fullScreenCb;
    ZoomFactorChangedCallback m_zoomFactorChangedCb;
    UserActivityCallback m_userActivityCb;
    std::function<void(bool)> m_audioPlayingCb;
    NavigationStateCallback m_navStateCb;
    NavigationStartingCallback m_navStartingCb;
    NavigationCompletedCallback m_navCompletedCb;
    HistoryCallback m_historyCb;
    FaviconCallback m_faviconCb;
    NewWindowCallback m_newWindowCb;
    PermissionCallback m_permissionCb;
    InternalMessageCallback m_internalMessageCb;
    ContextActionCallback m_contextActionCb;
    ContentLoadedCallback m_contentLoadedCb;
};

} // namespace UltraLight
