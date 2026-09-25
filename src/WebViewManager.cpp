#include "WebViewManager.hpp"
#include "MainWindow.hpp"
#include "Config.hpp"
#include "ElementBlocker.hpp"
#include "NativeRequestFilter.hpp"
#include "PowerManager.hpp"
#include "StringUtils.hpp"
#include <iostream>
#include <algorithm>
#include <cmath>
#include <filesystem>

using namespace Microsoft::WRL;

namespace UltraLight {

WebViewManager::WebViewManager() {}

HRESULT WebViewManager::Initialize(HWND hWndParent, ReadyCallback onReady) {
    m_hWndParent = hWndParent;
    m_onReady = onReady;

    std::wstring userDataDir = Config::Instance().GetUserDataDirectory().wstring();

    auto options = Make<CoreWebView2EnvironmentOptions>();

    // Inject pure hardware GPU pipeline, aggressive discard, 120Hz VSync and low-latency network flags
    std::wstring performanceArgs =
        L"--enable-gpu-rasterization "
        L"--enable-zero-copy "
        L"--enable-accelerated-video-decode "
        L"--enable-features=NvidiaVsr,IntelVsr,Prerender2,DnsOverHttps,HighEfficiencyModeAvailable,PageDiscarding,Freezer,BatterySaverModeAvailable "
        L"--fake-vsync-rate=120 "
        L"--max-gum-fps=120 "
        L"--disable-software-rasterizer "
        L"--disable-gpu-watchdog "
        L"--enable-hardware-overlays=\"single-fullscreen,single-on-top,underlay\" "
        L"--enable-native-gpu-memory-buffers "
        L"--intensive-wake-up-throttling "
        L"--enable-quic "
        L"--enable-async-dns "
        L"--media-cache-size=134217728 "
        L"--disk-cache-size=209715200 "
        L"--disable-features=AudioServiceOutOfProcess,Translate,OptimizationHints,MediaRouter "
        L"--disable-background-networking "
        L"--disable-sync "
        L"--disable-domain-reliability "
        L"--disable-breakpad "
        L"--no-pings "
        L"--disable-speech-api "
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

                            // Aggressive memory compression: LOW target level forces V8 Major GC and trims image decode cache
                            ApplyMemoryUsageTargetLow();

                            // Use raw physical pixels for WebView2 bounds so it matches Win32 GetClientRect exactly
                            wil::com_ptr<ICoreWebView2Controller3> controller3;
                            if (SUCCEEDED(m_controller->QueryInterface(IID_PPV_ARGS(&controller3))) && controller3) {
                                controller3->put_BoundsMode(COREWEBVIEW2_BOUNDS_MODE_USE_RAW_PIXELS);
                            }

                            // Intercept keyboard accelerators (Zoom, Fullscreen, Address bar)
                            m_controller->add_AcceleratorKeyPressed(
                                Callback<ICoreWebView2AcceleratorKeyPressedEventHandler>(
                                    [this](ICoreWebView2Controller* /*sender*/, ICoreWebView2AcceleratorKeyPressedEventArgs* args) -> HRESULT {
                                        COREWEBVIEW2_KEY_EVENT_KIND kind;
                                        if (SUCCEEDED(args->get_KeyEventKind(&kind))) {
                                            if (kind == COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN || kind == COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN) {
                                                UINT key = 0;
                                                if (SUCCEEDED(args->get_VirtualKey(&key))) {
                                                    if (m_userActivityCb) {
                                                        m_userActivityCb();
                                                    }

                                                    bool isCtrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                                                    bool isShift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                                                    bool isAlt = (GetKeyState(VK_MENU) & 0x8000) != 0;

                                                    if (isCtrl && !isAlt) {
                                                        // Zoom In: Ctrl + Plus, Ctrl + Add, Ctrl + '='
                                                        if (key == VK_OEM_PLUS || key == VK_ADD || key == 0xBB) {
                                                            PostMessageW(m_hWndParent, WM_COMMAND, MAKEWPARAM(IDM_ZOOM_IN, 0), 0);
                                                            args->put_Handled(TRUE);
                                                            return S_OK;
                                                        }
                                                        // Zoom Out: Ctrl + Minus, Ctrl + Subtract
                                                        if (!isShift && (key == VK_OEM_MINUS || key == VK_SUBTRACT || key == 0xBD)) {
                                                            PostMessageW(m_hWndParent, WM_COMMAND, MAKEWPARAM(IDM_ZOOM_OUT, 0), 0);
                                                            args->put_Handled(TRUE);
                                                            return S_OK;
                                                        }
                                                        // Zoom Reset: Ctrl + 0
                                                        if (!isShift && (key == '0' || key == VK_NUMPAD0)) {
                                                            PostMessageW(m_hWndParent, WM_COMMAND, MAKEWPARAM(IDM_ZOOM_RESET, 0), 0);
                                                            args->put_Handled(TRUE);
                                                            return S_OK;
                                                        }
                                                        // Focus Address Bar: Ctrl + L
                                                        if (!isShift && (key == 'L' || key == 'l')) {
                                                            PostMessageW(m_hWndParent, WM_COMMAND, MAKEWPARAM(IDM_FOCUS_ADDRESS_BAR, 0), 0);
                                                            args->put_Handled(TRUE);
                                                            return S_OK;
                                                        }
                                                        // Element Blocker: Ctrl + Shift + H
                                                        if (isShift && (key == 'H' || key == 'h')) {
                                                            PostMessageW(m_hWndParent, WM_COMMAND, MAKEWPARAM(IDC_BTN_BLOCKER, 0), 0);
                                                            args->put_Handled(TRUE);
                                                            return S_OK;
                                                        }
                                                        // Immersive Mode: Ctrl + Shift + U
                                                        if (isShift && (key == 'U' || key == 'u')) {
                                                            PostMessageW(m_hWndParent, WM_COMMAND, MAKEWPARAM(IDM_TOGGLE_IMMERSIVE, 0), 0);
                                                            args->put_Handled(TRUE);
                                                            return S_OK;
                                                        }
                                                    }

                                                    // Immersive Mode: F9
                                                    if (key == VK_F9) {
                                                        PostMessageW(m_hWndParent, WM_COMMAND, MAKEWPARAM(IDM_TOGGLE_IMMERSIVE, 0), 0);
                                                        args->put_Handled(TRUE);
                                                        return S_OK;
                                                    }

                                                    if (key == VK_F11) {
                                                        PostMessageW(m_hWndParent, WM_COMMAND, MAKEWPARAM(IDM_TOGGLE_FULLSCREEN, 0), 0);
                                                        args->put_Handled(TRUE);
                                                        return S_OK;
                                                    }
                                                    if (key == VK_ESCAPE) {
                                                        PostMessageW(m_hWndParent, WM_COMMAND, MAKEWPARAM(IDM_EXIT_FULLSCREEN, 0), 0);
                                                    }
                                                }
                                            }
                                        }
                                        return S_OK;
                                    }
                                ).Get(),
                                nullptr
                            );

                            // Setup bounds
                            RECT bounds;
                            GetClientRect(m_hWndParent, &bounds);
                            m_controller->put_Bounds(bounds);
                            m_controller->put_IsVisible(TRUE);

                            // Register internal and feature handlers
                            RegisterEventHandlers();

                            // Initialize modules
                            ElementBlocker::Instance().Initialize(m_webView.get());
                            NativeRequestFilter::Instance().Initialize(m_webView.get(), m_environment.get());

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
        Callback<ICoreWebView2SourceChangedEventHandler>(
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
                wil::unique_cotaskmem_string sourceUri;
                std::wstring src = L"";
                if (SUCCEEDED(args->get_Source(&sourceUri)) && sourceUri.get()) {
                    src = sourceUri.get();
                }

                if (SUCCEEDED(args->TryGetWebMessageAsString(&messageRaw)) && messageRaw.get()) {
                    ElementBlocker::Instance().HandleWebMessage(messageRaw.get(), src);
                } else if (SUCCEEDED(args->get_WebMessageAsJson(&messageRaw)) && messageRaw.get()) {
                    ElementBlocker::Instance().HandleWebMessage(messageRaw.get(), src);
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

    // Zoom factor changed (e.g. via Ctrl+MouseWheel or navigation)
    if (m_controller) {
        m_controller->add_ZoomFactorChanged(
            Callback<ICoreWebView2ZoomFactorChangedEventHandler>(
                [this](ICoreWebView2Controller* sender, IUnknown* /*args*/) -> HRESULT {
                    double zoom = 1.0;
                    if (SUCCEEDED(sender->get_ZoomFactor(&zoom)) && m_zoomFactorChangedCb) {
                        m_zoomFactorChangedCb(zoom);
                    }
                    return S_OK;
                }
            ).Get(),
            nullptr
        );
    }

    // Audio Playing State Changed (Media/Audio playback awareness)
    wil::com_ptr<ICoreWebView2_8> webView8;
    if (SUCCEEDED(m_webView->QueryInterface(IID_PPV_ARGS(&webView8))) && webView8) {
        webView8->add_IsDocumentPlayingAudioChanged(
            Callback<ICoreWebView2IsDocumentPlayingAudioChangedEventHandler>(
                [this](ICoreWebView2* sender, IUnknown* /*args*/) -> HRESULT {
                    wil::com_ptr<ICoreWebView2_8> s8;
                    if (SUCCEEDED(sender->QueryInterface(IID_PPV_ARGS(&s8))) && s8) {
                        BOOL isPlaying = FALSE;
                        if (SUCCEEDED(s8->get_IsDocumentPlayingAudio(&isPlaying))) {
                            m_isPlayingAudio = (isPlaying != FALSE);
                            if (m_audioPlayingCb) {
                                m_audioPlayingCb(m_isPlayingAudio);
                            }
                        }
                    }
                    return S_OK;
                }
            ).Get(),
            &m_audioPlayingToken
        );
    }
}

void WebViewManager::ApplyMemoryUsageTargetLow() {
    if (!m_webView) return;
    wil::com_ptr<ICoreWebView2_19> webView19;
    if (SUCCEEDED(m_webView->QueryInterface(IID_PPV_ARGS(&webView19))) && webView19) {
        webView19->put_MemoryUsageTargetLevel(COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_LOW);
    }
}

void WebViewManager::SetVisible(bool isVisible) {
    if (m_controller) {
        m_controller->put_IsVisible(isVisible ? TRUE : FALSE);
    }
}

bool WebViewManager::IsVisible() const {
    if (!m_controller) return false;
    BOOL visible = FALSE;
    m_controller->get_IsVisible(&visible);
    return visible != FALSE;
}

void WebViewManager::Resize(const RECT& bounds) {
    if (m_controller) {
        m_controller->put_Bounds(bounds);
    }
}

void WebViewManager::NotifyParentWindowPositionChanged() {
    if (m_controller) {
        m_controller->NotifyParentWindowPositionChanged();
    }
}

void WebViewManager::Navigate(const std::wstring& url) {
    if (!m_webView) return;

    std::wstring target = url;
    while (!target.empty() && iswspace(target.front())) target.erase(target.begin());
    while (!target.empty() && iswspace(target.back())) target.pop_back();
    if (target.empty()) return;

    if (target.find(L"://") != std::wstring::npos ||
        target.rfind(L"about:", 0) == 0 ||
        target.rfind(L"data:", 0) == 0 ||
        target.rfind(L"javascript:", 0) == 0) {
        // Direct URL with recognized scheme
    } else if (target.rfind(L"localhost", 0) == 0 || target.rfind(L"127.0.0.1", 0) == 0) {
        target = L"http://" + target;
    } else if (target.find(L'.') != std::wstring::npos && target.find(L' ') == std::wstring::npos) {
        target = L"https://" + target;
    } else {
        target = L"https://www.google.com/search?q=" + StringUtils::UrlEncode(target);
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

static const double kZoomLevels[] = {
    0.25, 0.33, 0.50, 0.67, 0.75, 0.80, 0.90, 1.00,
    1.10, 1.25, 1.50, 1.75, 2.00, 2.50, 3.00, 4.00, 5.00
};

void WebViewManager::ZoomIn() {
    double current = GetZoomFactor();
    for (double level : kZoomLevels) {
        if (level > current + 0.005) {
            SetZoomFactor(level);
            return;
        }
    }
    SetZoomFactor(5.0);
}

void WebViewManager::ZoomOut() {
    double current = GetZoomFactor();
    for (int i = static_cast<int>(std::size(kZoomLevels)) - 1; i >= 0; --i) {
        if (kZoomLevels[i] < current - 0.005) {
            SetZoomFactor(kZoomLevels[i]);
            return;
        }
    }
    SetZoomFactor(0.25);
}

void WebViewManager::ZoomReset() {
    SetZoomFactor(1.0);
}

double WebViewManager::GetZoomFactor() const {
    if (!m_controller) return 1.0;
    double factor = 1.0;
    if (SUCCEEDED(m_controller->get_ZoomFactor(&factor))) {
        return factor;
    }
    return 1.0;
}

void WebViewManager::SetZoomFactor(double factor) {
    if (!m_controller) return;
    factor = std::clamp(factor, 0.25, 5.0);
    if (SUCCEEDED(m_controller->put_ZoomFactor(factor))) {
        if (m_zoomFactorChangedCb) {
            m_zoomFactorChangedCb(factor);
        }
    }
}

namespace {

void SpawnDeferredDirectoryPurge(const std::filesystem::path& dirPath) {
    std::wstring pathStr = dirPath.wstring();
    if (pathStr.empty()) return;

    std::wstring cmd = L"cmd.exe /c timeout /t 1 /nobreak >nul & if exist \"" + pathStr + L"\" rmdir /s /q \"" + pathStr + L"\"";
    
    STARTUPINFOW si{ sizeof(STARTUPINFOW) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};

    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');

    if (CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

} // namespace

void WebViewManager::PurgeAllCacheAndTempFiles() {
    std::filesystem::path userDataDir = Config::Instance().GetUserDataDirectory();
    std::error_code ec;

    if (std::filesystem::exists(userDataDir, ec)) {
        for (int retry = 0; retry < 3; ++retry) {
            std::filesystem::remove_all(userDataDir, ec);
            if (!std::filesystem::exists(userDataDir, ec)) {
                break;
            }
            Sleep(40);
        }

        if (std::filesystem::exists(userDataDir, ec)) {
            SpawnDeferredDirectoryPurge(userDataDir);
        }
    }

    std::filesystem::path appDataDir = Config::Instance().GetAppDataPath();
    std::filesystem::path ebWebViewDir = appDataDir / "EBWebView";
    if (std::filesystem::exists(ebWebViewDir, ec)) {
        std::filesystem::remove_all(ebWebViewDir, ec);
        if (std::filesystem::exists(ebWebViewDir, ec)) {
            SpawnDeferredDirectoryPurge(ebWebViewDir);
        }
    }
}

void WebViewManager::ShutdownAndPurgeData() {
    UINT32 browserProcessId = 0;
    if (m_webView) {
        m_webView->get_BrowserProcessId(&browserProcessId);

        // 1. In-process Profile Data Wipe via ClearBrowsingDataAll
        wil::com_ptr<ICoreWebView2_13> webView13;
        if (SUCCEEDED(m_webView->QueryInterface(IID_PPV_ARGS(&webView13))) && webView13) {
            wil::com_ptr<ICoreWebView2Profile> profile;
            if (SUCCEEDED(webView13->get_Profile(&profile)) && profile) {
                wil::com_ptr<ICoreWebView2Profile2> profile2;
                if (SUCCEEDED(profile->QueryInterface(IID_PPV_ARGS(&profile2))) && profile2) {
                    HANDLE hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                    profile2->ClearBrowsingDataAll(
                        Callback<ICoreWebView2ClearBrowsingDataCompletedHandler>(
                            [hEvent](HRESULT) -> HRESULT {
                                SetEvent(hEvent);
                                return S_OK;
                            }
                        ).Get()
                    );

                    DWORD start = GetTickCount();
                    while (WaitForSingleObject(hEvent, 10) != WAIT_OBJECT_0 && (GetTickCount() - start) < 500) {
                        MSG msg;
                        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                            TranslateMessage(&msg);
                            DispatchMessageW(&msg);
                        }
                    }
                    CloseHandle(hEvent);
                }
            }
        }
    }

    // 2. Close controller and release all COM pointers
    if (m_controller) {
        m_controller->Close();
        m_controller.reset();
    }
    m_webView.reset();
    m_environment.reset();

    // 3. Ensure browser subprocesses have terminated
    if (browserProcessId != 0) {
        HANDLE hProc = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, browserProcessId);
        if (hProc) {
            if (WaitForSingleObject(hProc, 1000) == WAIT_TIMEOUT) {
                TerminateProcess(hProc, 0);
                WaitForSingleObject(hProc, 300);
            }
            CloseHandle(hProc);
        }
    }

    // 4. Forcibly wipe all cache and temporary files on disk
    PurgeAllCacheAndTempFiles();
}

} // namespace UltraLight
