#include "WebViewManager.hpp"
#include "Config.hpp"
#include "ElementBlocker.hpp"
#include "ExtensionManager.hpp"
#include "PowerManager.hpp"
#include <iostream>

using namespace Microsoft::WRL;

namespace UltraLight {

WebViewManager::WebViewManager() {}

HRESULT WebViewManager::Initialize(HWND hWndParent, ReadyCallback onReady) {
    m_hWndParent = hWndParent;
    m_onReady = onReady;

    std::wstring userDataDir = Config::Instance().GetUserDataDirectory().wstring();

    auto options = Make<CoreWebView2EnvironmentOptions>();

    // Inject the full performance, hardware acceleration, and low-latency network flags
    std::wstring performanceArgs =
        L"--enable-gpu-rasterization "
        L"--enable-zero-copy "
        L"--enable-accelerated-video-decode "
        L"--ignore-gpu-blocklist "
        L"--enable-hardware-overlays=single-fullscreen,single-on-top "
        L"--disable-direct-composition-video-overlays=false "
        L"--enable-features=NvidiaVsr,IntelVsr,Prerender2 "
        L"--enable-quic "
        L"--quic-version=h3 "
        L"--enable-bbr "
        L"--enable-async-dns "
        L"--enable-tcp-fast-open "
        L"--max-connections-per-host=12 "
        L"--media-cache-size=134217728 "
        L"--disk-cache-size=209715200 "
        L"--disable-features=Translate,OptimizationHints,MediaRouter "
        L"--disable-background-networking "
        L"--no-first-run";

    options->put_AdditionalBrowserArguments(performanceArgs.c_str());

    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr,
        userDataDir.c_str(),
        options.Get(),
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(result) || !env) return result;
                m_environment = env;

                return m_environment->CreateCoreWebView2Controller(
                    m_hWndParent,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [this](HRESULT res, ICoreWebView2Controller* controller) -> HRESULT {
                            if (FAILED(res) || !controller) return res;
                            m_controller = controller;
                            m_controller->get_CoreWebView2(&m_webView);

                            // Retrieve profile for extensions
                            wil::com_ptr<ICoreWebView2_13> webView13;
                            if (SUCCEEDED(m_webView->QueryInterface(IID_PPV_ARGS(&webView13))) && webView13) {
                                webView13->get_Profile(&m_profile);
                            }

                            // Setup bounds
                            RECT bounds;
                            GetClientRect(m_hWndParent, &bounds);
                            m_controller->put_Bounds(bounds);
                            m_controller->put_IsVisible(TRUE);

                            // Register internal and feature handlers
                            RegisterEventHandlers();

                            // Initialize modules
                            ElementBlocker::Instance().Initialize(m_webView.get());
                            if (m_profile) {
                                ExtensionManager::Instance().Initialize(m_profile.get());
                            }

                            // Apply QoS optimizations
                            PowerManager::Instance().DisableEcoQoS();
                            PowerManager::Instance().OptimizeProcessTree(GetCurrentProcessId());

                            if (m_onReady) {
                                m_onReady();
                            }

                            return S_OK;
                        }
                    ).Get()
                );
            }
        ).Get()
    );

    return hr;
}

void WebViewManager::RegisterEventHandlers() {
    if (!m_webView) return;

    // Document Title Changed
    m_webView->add_DocumentTitleChanged(
        Callback<ICoreWebView2DocumentTitleChangedEventHandler>(
            [this](ICoreWebView2* sender, IUnknown* /*args*/) -> HRESULT {
                wil::unique_cotaskmem_string title;
                if (SUCCEEDED(sender->get_DocumentTitle(&title)) && m_titleChangedCb) {
                    m_titleChangedCb(title.get());
                }
                return S_OK;
            }
        ).Get(),
        nullptr
    );

    // Source (URL) Changed
    m_webView->add_SourceChanged(
        Callback<ICoreWebView2SourceChangedEventArgsHandler>(
            [this](ICoreWebView2* sender, ICoreWebView2SourceChangedEventArgs* /*args*/) -> HRESULT {
                wil::unique_cotaskmem_string uri;
                if (SUCCEEDED(sender->get_Source(&uri)) && m_sourceChangedCb) {
                    m_sourceChangedCb(uri.get());
                }
                return S_OK;
            }
        ).Get(),
        nullptr
    );

    // Navigation Starting (Element Blocker rule injection)
    m_webView->add_NavigationStarting(
        Callback<ICoreWebView2NavigationStartingEventHandler>(
            [](ICoreWebView2* sender, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                wil::unique_cotaskmem_string uri;
                if (SUCCEEDED(args->get_Uri(&uri))) {
                    ElementBlocker::Instance().OnNavigationStarting(sender, uri.get());
                }
                return S_OK;
            }
        ).Get(),
        nullptr
    );

    // Web Message Received (DOM element picker callback)
    m_webView->add_WebMessageReceived(
        Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [](ICoreWebView2* /*sender*/, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                wil::unique_cotaskmem_string messageRaw;
                if (SUCCEEDED(args->get_WebMessageAsJson(&messageRaw))) {
                    ElementBlocker::Instance().HandleWebMessage(messageRaw.get());
                }
                return S_OK;
            }
        ).Get(),
        nullptr
    );

    // Full screen element changed (PowerManager priority boost)
    m_webView->add_ContainsFullScreenElementChanged(
        Callback<ICoreWebView2ContainsFullScreenElementChangedEventHandler>(
            [this](ICoreWebView2* sender, IUnknown* /*args*/) -> HRESULT {
                BOOL isFullScreen = FALSE;
                sender->get_ContainsFullScreenElement(&isFullScreen);
                PowerManager::Instance().SetHighPerformanceMode(isFullScreen != FALSE);
                if (m_fullScreenCb) {
                    m_fullScreenCb(isFullScreen != FALSE);
                }
                return S_OK;
            }
        ).Get(),
        nullptr
    );
}

void WebViewManager::Resize(const RECT& bounds) {
    if (m_controller) {
        m_controller->put_Bounds(bounds);
    }
}

void WebViewManager::Navigate(const std::wstring& url) {
    if (!m_webView) return;

    std::wstring target = url;
    if (target.find(L"://") == std::wstring::npos) {
        if (target.find(L'.') != std::wstring::npos && target.find(L' ') == std::wstring::npos) {
            target = L"https://" + target;
        } else {
            target = L"https://www.google.com/search?q=" + target;
        }
    }
    m_webView->Navigate(target.c_str());
}

void WebViewManager::GoBack() {
    if (m_webView) m_webView->GoBack();
}

void WebViewManager::GoForward() {
    if (m_webView) m_webView->GoForward();
}

void WebViewManager::Reload() {
    if (m_webView) m_webView->Reload();
}

void WebViewManager::Stop() {
    if (m_webView) m_webView->Stop();
}

} // namespace UltraLight
