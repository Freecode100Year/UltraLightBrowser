#include "WebViewManager.hpp"
#include "MainWindow.hpp"
#include "Config.hpp"
#include "ElementBlocker.hpp"
#include "NativeRequestFilter.hpp"
#include "PowerManager.hpp"
#include "StringUtils.hpp"
#include <iostream>
#include <fstream>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>

#pragma comment(lib, "propsys.lib")

#if __has_include(<nlohmann/json.hpp>)
#include <nlohmann/json.hpp>
using json = nlohmann::json;
#endif

using namespace Microsoft::WRL;

namespace UltraLight {

namespace {

void ShowWebView2InitError(HWND hWnd, const wchar_t* stage, HRESULT hr) {
    wchar_t hexCode[32]{};
    swprintf_s(hexCode, L"0x%08X", static_cast<unsigned int>(hr));

    std::wstring msg = L"【UltraLightBrowser】WebView2 ";
    msg += stage;
    msg += L"失败！\n\n错误代码: ";
    msg += hexCode;
    msg += L"\n\n【常见原因与解决方案】\n";

    if (hr == HRESULT_FROM_WIN32(ERROR_INVALID_STATE) || hr == static_cast<HRESULT>(0x8007139F)) {
        msg += L"⚠️ 检测到状态冲突 (0x8007139F)：\n"
               L"后台存在旧版本残留的 msedgewebview2.exe 进程！\n"
               L"因为版本升级后前后启动参数或用户数据目录状态不一致，WebView2 内核拒绝接入。\n\n"
               L"👉 解决方案：\n"
               L"1. 请按 Ctrl + Shift + Esc 打开任务管理器；\n"
               L"2. 在“详细信息”或“进程”列表中，结束所有 UltraLightBrowser.exe 和 msedgewebview2.exe 进程；\n"
               L"3. 或直接重启电脑后重新打开浏览器；\n"
               L"4. 亦可将 UserData 目录下的 EBWebView\\Local State 改名或删除后重试。";
    } else if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) || hr == static_cast<HRESULT>(0x80070002)) {
        msg += L"⚠️ 找不到 WebView2 运行时：\n"
               L"系统中未检测到 Microsoft Edge WebView2 Evergreen 运行时。\n"
               L"请前往微软官网下载并安装 WebView2 Runtime 后重试。";
    } else {
        msg += L"1. 后台可能残留旧版的 msedgewebview2.exe 进程，请在任务管理器中结束所有残留进程后重试；\n"
               L"2. 请确认系统中已安装 Microsoft Edge WebView2 Evergreen 运行时；\n"
               L"3. 检查 UserData 目录的读写权限，或删除 UserData 缓存后重新启动。";
    }

    MessageBoxW(hWnd, msg.c_str(), L"WebView2 内核启动失败", MB_OK | MB_ICONERROR);
}

} // namespace

WebViewManager::WebViewManager() {}

HRESULT WebViewManager::Initialize(HWND hWndParent, ReadyCallback onReady) {
    m_hWndParent = hWndParent;
    m_onReady = onReady;
    return TryInitEnvironment(0);
}

void WebViewManager::SanitizeLocalState(const std::filesystem::path& userDataDir) {
    std::filesystem::path localStatePath = userDataDir / "EBWebView" / "Local State";
    std::error_code ec;
    if (!std::filesystem::exists(localStatePath, ec)) return;

    bool isValid = false;
    try {
        auto sz = std::filesystem::file_size(localStatePath, ec);
        if (!ec && sz > 200) {
            std::ifstream inFile(localStatePath);
            if (inFile.is_open()) {
                std::string content((std::istreambuf_iterator<char>(inFile)), std::istreambuf_iterator<char>());
                if (content.find("\"os_crypt\"") != std::string::npos) {
#if __has_include(<nlohmann/json.hpp>)
                    if (json::accept(content)) {
                        isValid = true;
                    }
#else
                    isValid = true;
#endif
                }
            }
        }
    } catch (...) {
        isValid = false;
    }

    if (!isValid) {
        // Strip damaged or stub Local State (e.g. bare {"dns_over_https":...} lacking os_crypt)
        // allowing Chromium to cleanly regenerate a pristine, fully-featured Local State.
        std::filesystem::remove(localStatePath, ec);
    }
}

HRESULT WebViewManager::TryInitEnvironment(int attempt) {
    std::filesystem::path userDataDir;
    if (attempt < 2) {
        userDataDir = Config::Instance().GetUserDataDirectory();
    } else {
        // Fallback directory to isolate from any stubborn stale Edge processes
        userDataDir = Config::Instance().GetAppDataPath() / L"UserData_Safe";
    }

    // Proactively self-heal: remove damaged or stub Local State before WebView2 initializes
    SanitizeLocalState(userDataDir);

    auto options = Make<CoreWebView2EnvironmentOptions>();

    // Inject pure hardware GPU pipeline, aggressive discard, and low-latency network flags
    std::wstring performanceArgs =
        L"--enable-gpu-rasterization "
        L"--enable-zero-copy "
        L"--enable-accelerated-video-decode "
        L"--enable-features=NvidiaVsr,IntelVsr,Prerender2,DnsOverHttps,HighEfficiencyModeAvailable,PageDiscarding,Freezer,BatterySaverModeAvailable "
        L"--enable-hardware-overlays=\"single-fullscreen,single-on-top,underlay\" "
        L"--enable-native-gpu-memory-buffers "
        L"--media-cache-size=134217728 "
        L"--disk-cache-size=209715200 "
        L"--disable-features=AudioServiceOutOfProcess,Translate,OptimizationHints,MediaRouter "
        L"--disable-sync "
        L"--disable-domain-reliability "
        L"--disable-breakpad "
        L"--disable-speech-api "
        L"--no-first-run";

    // NOTE: --host-resolver-rules was completely removed to prevent command-line parsing conflicts
    // and argument mismatch 0x8007139F errors with running processes. NativeRequestFilter handles all blocking.

    options->put_AdditionalBrowserArguments(performanceArgs.c_str());

    std::wstring userDataDirStr = userDataDir.wstring();
    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr,
        userDataDirStr.c_str(),
        options.Get(),
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this, attempt](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(result) || !env) {
                    if ((result == HRESULT_FROM_WIN32(ERROR_INVALID_STATE) || result == static_cast<HRESULT>(0x8007139F)) && attempt < 2) {
                        Sleep(500);
                        return TryInitEnvironment(attempt + 1);
                    }
                    ShowWebView2InitError(m_hWndParent, L"环境初始化", FAILED(result) ? result : E_FAIL);
                    return result;
                }
                m_environment = env;

                return m_environment->CreateCoreWebView2Controller(
                    m_hWndParent,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [this, attempt](HRESULT res, ICoreWebView2Controller* controller) -> HRESULT {
                            if (FAILED(res) || !controller) {
                                if ((res == HRESULT_FROM_WIN32(ERROR_INVALID_STATE) || res == static_cast<HRESULT>(0x8007139F)) && attempt < 2) {
                                    Sleep(500);
                                    return TryInitEnvironment(attempt + 1);
                                }
                                ShowWebView2InitError(m_hWndParent, L"控制器创建", FAILED(res) ? res : E_FAIL);
                                return res;
                            }
                            m_controller = controller;
                            m_controller->get_CoreWebView2(&m_webView);

                            // Aggressive memory compression: LOW target level forces V8 Major GC and trims image decode cache
                            ApplyMemoryUsageTargetLow();

                            // Enable native strict tracking prevention (engine-level tracker blocking)
                            wil::com_ptr<ICoreWebView2_13> webView13;
                            if (SUCCEEDED(m_webView->QueryInterface(IID_PPV_ARGS(&webView13))) && webView13) {
                                wil::com_ptr<ICoreWebView2Profile> profile;
                                if (SUCCEEDED(webView13->get_Profile(&profile)) && profile) {
                                    wil::com_ptr<ICoreWebView2Profile3> profile3;
                                    if (SUCCEEDED(profile->QueryInterface(IID_PPV_ARGS(&profile3))) && profile3) {
                                        profile3->put_PreferredTrackingPreventionLevel(COREWEBVIEW2_TRACKING_PREVENTION_LEVEL_STRICT);
                                    }
                                }
                            }

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
                            InjectSurroundSoundScript();

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

    if (FAILED(hr)) {
        if ((hr == HRESULT_FROM_WIN32(ERROR_INVALID_STATE) || hr == static_cast<HRESULT>(0x8007139F)) && attempt < 2) {
            Sleep(500);
            return TryInitEnvironment(attempt + 1);
        }
        ShowWebView2InitError(m_hWndParent, L"环境创建调用", hr);
    }

    return hr;
}

void WebViewManager::RegisterEventHandlers() {
    if (!m_webView) return;

    // Process Failed (browser process crash or exit)
    wil::com_ptr<ICoreWebView2_2> webView2_2;
    if (SUCCEEDED(m_webView->QueryInterface(IID_PPV_ARGS(&webView2_2))) && webView2_2) {
        webView2_2->add_ProcessFailed(
            Callback<ICoreWebView2ProcessFailedEventHandler>(
                [this](ICoreWebView2* sender, ICoreWebView2ProcessFailedEventArgs* args) -> HRESULT {
                    COREWEBVIEW2_PROCESS_FAILED_KIND kind;
                    if (SUCCEEDED(args->get_ProcessFailedKind(&kind))) {
                        if (kind == COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED) {
                            MessageBoxW(m_hWndParent,
                                L"WebView2 核心主进程异常退出！\n可能由于后台进程冲突或显卡驱动崩溃引起。\n请在任务管理器中结束所有残留的 msedgewebview2.exe 进程后重新打开。",
                                L"核心进程异常退出", MB_OK | MB_ICONERROR);
                        } else if (kind == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED ||
                                   kind == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_UNRESPONSIVE) {
                            sender->Reload();
                        }
                    }
                    return S_OK;
                }
            ).Get(),
            nullptr
        );
    }

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

    // Navigation Starting (Element Blocker rule injection and loading state)
    m_webView->add_NavigationStarting(
        Callback<ICoreWebView2NavigationStartingEventHandler>(
            [this](ICoreWebView2* sender, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                wil::unique_cotaskmem_string uri;
                if (SUCCEEDED(args->get_Uri(&uri)) && uri.get()) {
                    NativeRequestFilter::Instance().SetMainFrameNavigation(uri.get());
                    ElementBlocker::Instance().OnNavigationStarting(sender, uri.get());
                }
                if (m_navStateCb) {
                    m_navStateCb(true);
                }
                return S_OK;
            }
        ).Get(),
        nullptr
    );

    // Navigation Completed (loading state finished)
    m_webView->add_NavigationCompleted(
        Callback<ICoreWebView2NavigationCompletedEventHandler>(
            [this](ICoreWebView2* /*sender*/, ICoreWebView2NavigationCompletedEventArgs* /*args*/) -> HRESULT {
                NativeRequestFilter::Instance().ClearMainFrameNavigation();
                if (m_navStateCb) {
                    m_navStateCb(false);
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

void WebViewManager::PurgeAllCacheAndTempFiles() {
    std::filesystem::path userDataDir = Config::Instance().GetUserDataDirectory();
    std::error_code ec;

    if (std::filesystem::exists(userDataDir, ec)) {
        std::filesystem::path ebWebViewDir = userDataDir / "EBWebView";
        std::filesystem::path localStatePath = ebWebViewDir / "Local State";

        bool hasValidLocalState = false;
        std::string localStateContent;
        if (std::filesystem::exists(localStatePath, ec)) {
            try {
                auto sz = std::filesystem::file_size(localStatePath, ec);
                if (!ec && sz > 200) {
                    std::ifstream inFile(localStatePath);
                    if (inFile.is_open()) {
                        std::string content((std::istreambuf_iterator<char>(inFile)), std::istreambuf_iterator<char>());
                        if (content.find("\"os_crypt\"") != std::string::npos) {
#if __has_include(<nlohmann/json.hpp>)
                            if (json::accept(content)) {
                                hasValidLocalState = true;
                                localStateContent = std::move(content);
                            }
#else
                            hasValidLocalState = true;
                            localStateContent = std::move(content);
#endif
                        }
                    }
                }
            } catch (...) {
                hasValidLocalState = false;
            }
        }

        for (int retry = 0; retry < 5; ++retry) {
            std::filesystem::remove_all(userDataDir, ec);
            if (!std::filesystem::exists(userDataDir, ec)) {
                break;
            }
            Sleep(50);
        }

        // Restore healthy Local State so encryption keys and DoH settings survive cleanup
        if (hasValidLocalState && !localStateContent.empty()) {
            try {
                std::filesystem::create_directories(ebWebViewDir, ec);
                std::ofstream outFile(localStatePath);
                if (outFile.is_open()) {
                    outFile << localStateContent;
                }
            } catch (...) {}
        }
    }

    std::filesystem::path appDataDir = Config::Instance().GetAppDataPath();
    std::filesystem::path ebWebViewDir = appDataDir / "EBWebView";
    if (std::filesystem::exists(ebWebViewDir, ec)) {
        for (int retry = 0; retry < 5; ++retry) {
            std::filesystem::remove_all(ebWebViewDir, ec);
            if (!std::filesystem::exists(ebWebViewDir, ec)) {
                break;
            }
            Sleep(50);
        }
    }

    std::filesystem::path safeUserDataDir = appDataDir / "UserData_Safe";
    if (std::filesystem::exists(safeUserDataDir, ec)) {
        SanitizeLocalState(safeUserDataDir);
    }
}

void WebViewManager::ShutdownAndPurgeData() {
    UINT32 browserProcessId = 0;
    if (m_webView) {
        m_webView->get_BrowserProcessId(&browserProcessId);

        // 1. In-process Profile Data Wipe via ClearBrowsingData
        wil::com_ptr<ICoreWebView2_13> webView13;
        if (SUCCEEDED(m_webView->QueryInterface(IID_PPV_ARGS(&webView13))) && webView13) {
            wil::com_ptr<ICoreWebView2Profile> profile;
            if (SUCCEEDED(webView13->get_Profile(&profile)) && profile) {
                wil::com_ptr<ICoreWebView2Profile2> profile2;
                if (SUCCEEDED(profile->QueryInterface(IID_PPV_ARGS(&profile2))) && profile2) {
                    HANDLE hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                    auto clearCb = Callback<ICoreWebView2ClearBrowsingDataCompletedHandler>(
                        [hEvent](HRESULT) -> HRESULT {
                            SetEvent(hEvent);
                            return S_OK;
                        }
                    );

                    profile2->ClearBrowsingDataAll(clearCb.Get());

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

AudioEndpointType WebViewManager::GetDetectedAudioEndpoint() const {
    AudioEndpointType result = AudioEndpointType::Speakers;
    wil::com_ptr<IMMDeviceEnumerator> pEnum;
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&pEnum)
    );
    if (FAILED(hr) || !pEnum) return result;

    wil::com_ptr<IMMDevice> pDevice;
    hr = pEnum->GetDefaultAudioEndpoint(eRender, eMultimedia, &pDevice);
    if (FAILED(hr) || !pDevice) return result;

    wil::com_ptr<IPropertyStore> pProps;
    hr = pDevice->OpenPropertyStore(STGM_READ, &pProps);
    if (FAILED(hr) || !pProps) return result;

    PROPVARIANT var;
    PropVariantInit(&var);
    hr = pProps->GetValue(PKEY_AudioEndpoint_FormFactor, &var);
    if (SUCCEEDED(hr) && var.vt == VT_UI4) {
        // 3: Headphones, 5: Headset
        if (var.ulVal == 3 || var.ulVal == 5) {
            result = AudioEndpointType::Headphones;
        } else {
            result = AudioEndpointType::Speakers;
        }
    } else {
        PropVariantClear(&var);
        hr = pProps->GetValue(PKEY_Device_FriendlyName, &var);
        if (SUCCEEDED(hr) && var.vt == VT_LPWSTR && var.pwszVal) {
            std::wstring name = var.pwszVal;
            std::wstring lower;
            for (wchar_t c : name) lower += towlower(c);
            if (lower.find(L"headphone") != std::wstring::npos ||
                lower.find(L"headset") != std::wstring::npos ||
                lower.find(L"earphone") != std::wstring::npos ||
                lower.find(L"airpods") != std::wstring::npos ||
                lower.find(L"buds") != std::wstring::npos ||
                lower.find(L"耳机") != std::wstring::npos) {
                result = AudioEndpointType::Headphones;
            }
        }
    }
    PropVariantClear(&var);
    return result;
}

void WebViewManager::InjectSurroundSoundScript() {
    if (!m_webView) return;

    const auto& settings = Config::Instance().GetSettings();
    std::string initMode = settings.surroundSoundMode.empty() ? "standard" : settings.surroundSoundMode;
    std::string initEnabled = settings.enableSurroundSound ? "true" : "false";
    std::string effectiveDevice = settings.audioDeviceMode;
    if (effectiveDevice == "auto") {
        AudioEndpointType detected = GetDetectedAudioEndpoint();
        effectiveDevice = (detected == AudioEndpointType::Headphones) ? "headphones" : "speakers";
    }
    std::string initVocalBoost = settings.enableVocalBoost ? "true" : "false";
    std::string initVolumeBoost = std::to_string(settings.audioVolumeBoost);
    std::string initMonoDownmix = settings.enableMonoDownmix ? "true" : "false";

    std::string jsCode = R"raw(
(function() {
    if (window.__UltraLightSurroundInstalled) return;
    window.__UltraLightSurroundInstalled = true;

    let cfg = {
        enabled: )raw" + initEnabled + R"raw(,
        mode: ")raw" + initMode + R"raw(",
        device: ")raw" + effectiveDevice + R"raw(",
        vocalBoost: )raw" + initVocalBoost + R"raw(,
        volumeBoost: )raw" + initVolumeBoost + R"raw(,
        monoDownmix: )raw" + initMonoDownmix + R"raw(
    };

    const PRESETS = {
        off: { width: 1.0, crossfeed: 0.0, reverbAmount: 0.0 },
        light: { width: 1.15, crossfeed: 0.12, reverbAmount: 0.06 },
        standard: { width: 1.35, crossfeed: 0.20, reverbAmount: 0.10 },
        cinema: { width: 1.55, crossfeed: 0.25, reverbAmount: 0.16 }
    };

    const attachedElements = new WeakMap();
    const pageClaimedElements = new WeakSet();
    const pendingPlayElements = new WeakSet();
    let audioCtx = null;
    let cachedReverbBuffer = null;
    let idleTimer = null;

    // 1. Give precedence to web-page native Web Audio
    try {
        const AC = window.AudioContext || window.webkitAudioContext;
        if (AC && AC.prototype && AC.prototype.createMediaElementSource) {
            const origCreate = AC.prototype.createMediaElementSource;
            AC.prototype.createMediaElementSource = function(mediaEl) {
                if (mediaEl) {
                    pageClaimedElements.add(mediaEl);
                    const existing = attachedElements.get(mediaEl);
                    if (existing && existing.release) {
                        try { existing.release(); } catch (e) {}
                        attachedElements.delete(mediaEl);
                    }
                }
                return origCreate.apply(this, arguments);
            };
        }
    } catch (e) {}

    // 2. Interactive Low-Latency AudioContext (avoids lip-sync drift)
    function getAudioContext() {
        if (!audioCtx) {
            const AudioContextClass = window.AudioContext || window.webkitAudioContext;
            if (!AudioContextClass) return null;
            try {
                audioCtx = new AudioContextClass({ latencyHint: "interactive" });
            } catch (e) {
                try { audioCtx = new AudioContextClass(); } catch (e2) { return null; }
            }
        }
        return audioCtx;
    }

    // 3. Synthesized Small-Room Impulse Response (Reverb)
    function getSmallRoomBuffer(ctx) {
        if (cachedReverbBuffer && cachedReverbBuffer.sampleRate === ctx.sampleRate) {
            return cachedReverbBuffer;
        }
        const rate = ctx.sampleRate;
        const duration = 0.035; // 35ms studio acoustic space
        const numSamples = Math.floor(rate * duration);
        const buf = ctx.createBuffer(2, numSamples, rate);
        const left = buf.getChannelData(0);
        const right = buf.getChannelData(1);
        const decay = 0.009;
        for (let i = 0; i < numSamples; ++i) {
            const env = Math.exp(-(i / rate) / decay);
            const damp = 1.0 - (i / numSamples) * 0.45;
            left[i] = (Math.random() * 2 - 1) * env * damp * 0.5;
            right[i] = (Math.random() * 2 - 1) * env * damp * 0.5;
        }
        cachedReverbBuffer = buf;
        return buf;
    }

    // 4. Safe URL & CORS Check
    function isSafeUrl(urlStr, el) {
        if (!urlStr || typeof urlStr !== "string") return false;
        if (urlStr.startsWith("blob:") || urlStr.startsWith("data:")) return true;
        try {
            const u = new URL(urlStr, window.location.href);
            if (u.origin === window.location.origin) return true;
        } catch (e) {
            return false;
        }
        if (el && el.crossOrigin && (el.crossOrigin === "anonymous" || el.crossOrigin === "use-credentials")) {
            return true;
        }
        return false;
    }

    function canProcessElement(el) {
        if (!el || attachedElements.has(el) || pageClaimedElements.has(el)) return false;
        // Strict guard: Skip DRM encrypted media (Netflix, Spotify, Apple TV+, etc.)
        if (el.mediaKeys) return false;

        const src = el.currentSrc || el.src;
        if (src) {
            return isSafeUrl(src, el);
        }

        const sources = el.querySelectorAll ? el.querySelectorAll("source") : [];
        if (sources.length > 0) {
            let hasAnySafe = false;
            for (let i = 0; i < sources.length; ++i) {
                const sUrl = sources[i].src;
                if (sUrl) {
                    if (isSafeUrl(sUrl, el)) {
                        hasAnySafe = true;
                    } else {
                        return false;
                    }
                }
            }
            return hasAnySafe;
        }

        return false;
    }

    // 5. Idle Auto-Suspend / Wakeup
    function wakeAudioContext() {
        if (idleTimer) {
            clearTimeout(idleTimer);
            idleTimer = null;
        }
        const ctx = getAudioContext();
        if (ctx && ctx.state === "suspended") {
            ctx.resume().catch(() => {});
        }
    }

    function scheduleIdleSuspend() {
        let anyPlaying = false;
        try {
            document.querySelectorAll("video, audio").forEach(m => {
                if (!m.paused && !m.ended && m.readyState > 2) anyPlaying = true;
            });
        } catch (e) {}

        if (!anyPlaying) {
            if (!idleTimer) {
                idleTimer = setTimeout(() => {
                    idleTimer = null;
                    const ctx = getAudioContext();
                    if (ctx && ctx.state === "running") {
                        ctx.suspend().catch(() => {});
                    }
                }, 1000);
            }
        }
    }

    function trySetupSurround(el) {
        if (!canProcessElement(el)) return;
        const ctx = getAudioContext();
        if (!ctx) return;

        wakeAudioContext();

        if (ctx.state !== "running") {
            pendingPlayElements.add(el);
            ctx.resume().then(() => {
                if (ctx.state === "running" && pendingPlayElements.has(el) && !el.paused) {
                    pendingPlayElements.delete(el);
                    setupSurroundForElement(el);
                }
            }).catch(() => {});
            return;
        }

        setupSurroundForElement(el);
    }

    // 6. Build the Full DSP Graph for Media Element
    function setupSurroundForElement(el) {
        if (!canProcessElement(el)) return;
        const ctx = getAudioContext();
        if (!ctx || ctx.state !== "running") return;

        let sourceNode = null;
        try {
            sourceNode = ctx.createMediaElementSource(el);
        } catch (e) {
            return;
        }

        // --- DSP GRAPH ---
        // A. Input Preamp Boost Gain (100% ~ 300%)
        const preampGain = ctx.createGain();
        preampGain.gain.setValueAtTime(cfg.volumeBoost || 1.0, ctx.currentTime);
        sourceNode.connect(preampGain);

        // B. Channel Splitter
        const splitter = ctx.createChannelSplitter(2);
        preampGain.connect(splitter);

        // C. Mid Bus = 0.5 * L + 0.5 * R
        const midL = ctx.createGain(); midL.gain.value = 0.5;
        const midR = ctx.createGain(); midR.gain.value = 0.5;
        const midBus = ctx.createGain();
        splitter.connect(midL, 0); midL.connect(midBus);
        splitter.connect(midR, 1); midR.connect(midBus);

        // Vocal Boost: Peaking Filter on Mid Bus (3000Hz, Q=1.2, +4.5dB)
        const vocalFilter = ctx.createBiquadFilter();
        vocalFilter.type = "peaking";
        vocalFilter.frequency.value = 3000;
        vocalFilter.Q.value = 1.2;
        vocalFilter.gain.value = cfg.vocalBoost ? 4.5 : 0.0;
        midBus.connect(vocalFilter);

        // D. Side Bus = 0.5 * L - 0.5 * R
        const sideL = ctx.createGain(); sideL.gain.value = 0.5;
        const sideR = ctx.createGain(); sideR.gain.value = -0.5;
        const sideBus = ctx.createGain();
        splitter.connect(sideL, 0); sideL.connect(sideBus);
        splitter.connect(sideR, 1); sideR.connect(sideBus);

        // Mono Bass Protection: 150Hz Highpass Filter on Side
        // Low frequencies (<150Hz) stay 100% in Mid, never phase-widened or attenuated!
        const sideHighpass = ctx.createBiquadFilter();
        sideHighpass.type = "highpass";
        sideHighpass.frequency.value = 150;
        sideHighpass.Q.value = 0.707;
        sideBus.connect(sideHighpass);

        const sideWidthGain = ctx.createGain();
        sideHighpass.connect(sideWidthGain);

        const sideToLeft = ctx.createGain(); sideToLeft.gain.value = 1.0;
        const sideToRight = ctx.createGain(); sideToRight.gain.value = -1.0;
        sideWidthGain.connect(sideToLeft);
        sideWidthGain.connect(sideToRight);

        // Reconstructed widened signals
        const postWidenerL = ctx.createGain();
        const postWidenerR = ctx.createGain();
        vocalFilter.connect(postWidenerL);
        sideToLeft.connect(postWidenerL);
        vocalFilter.connect(postWidenerR);
        sideToRight.connect(postWidenerR);

        // E. Crossfeed Network (~0.3ms ITD delay + 2.5kHz head shadow lowpass)
        const delayLR = ctx.createDelay(0.01); delayLR.delayTime.value = 0.0003;
        const filterLR = ctx.createBiquadFilter(); filterLR.type = "lowpass"; filterLR.frequency.value = 2500;
        const crossGainLR = ctx.createGain();
        postWidenerL.connect(delayLR); delayLR.connect(filterLR); filterLR.connect(crossGainLR);

        const delayRL = ctx.createDelay(0.01); delayRL.delayTime.value = 0.0003;
        const filterRL = ctx.createBiquadFilter(); filterRL.type = "lowpass"; filterRL.frequency.value = 2500;
        const crossGainRL = ctx.createGain();
        postWidenerR.connect(delayRL); delayRL.connect(filterRL); filterRL.connect(crossGainRL);

        const crossMergedL = ctx.createGain();
        const crossMergedR = ctx.createGain();
        postWidenerL.connect(crossMergedL);
        crossGainRL.connect(crossMergedL);
        postWidenerR.connect(crossMergedR);
        crossGainLR.connect(crossMergedR);

        // F. Virtual 3D HRTF Speakers (Headphones)
        const pannerL = ctx.createPanner();
        pannerL.panningModel = "HRTF";
        pannerL.distanceModel = "inverse";
        pannerL.positionX.setValueAtTime(-0.5, ctx.currentTime);
        pannerL.positionY.setValueAtTime(0.0, ctx.currentTime);
        pannerL.positionZ.setValueAtTime(-0.866, ctx.currentTime);

        const pannerR = ctx.createPanner();
        pannerR.panningModel = "HRTF";
        pannerR.distanceModel = "inverse";
        pannerR.positionX.setValueAtTime(0.5, ctx.currentTime);
        pannerR.positionY.setValueAtTime(0.0, ctx.currentTime);
        pannerR.positionZ.setValueAtTime(-0.866, ctx.currentTime);

        crossMergedL.connect(pannerL);
        crossMergedR.connect(pannerR);

        // Cinema Mode Rear Surround Panners (±110 deg)
        const rearDelay = ctx.createDelay(0.05); rearDelay.delayTime.value = 0.015;
        const rearGain = ctx.createGain(); rearGain.gain.value = 0.0;
        sideBus.connect(rearDelay);
        rearDelay.connect(rearGain);

        const pannerRearL = ctx.createPanner();
        pannerRearL.panningModel = "HRTF";
        pannerRearL.positionX.setValueAtTime(-0.94, ctx.currentTime);
        pannerRearL.positionY.setValueAtTime(0.0, ctx.currentTime);
        pannerRearL.positionZ.setValueAtTime(0.34, ctx.currentTime);

        const pannerRearR = ctx.createPanner();
        pannerRearR.panningModel = "HRTF";
        pannerRearR.positionX.setValueAtTime(0.94, ctx.currentTime);
        pannerRearR.positionY.setValueAtTime(0.0, ctx.currentTime);
        pannerRearR.positionZ.setValueAtTime(0.34, ctx.currentTime);

        rearGain.connect(pannerRearL);
        rearGain.connect(pannerRearR);

        // G. Speaker Direct Path (Bypasses HRTF when in Speaker mode)
        const speakerBusL = ctx.createGain();
        const speakerBusR = ctx.createGain();
        postWidenerL.connect(speakerBusL);
        postWidenerR.connect(speakerBusR);

        // H. Wet Mix Bus
        const wetMerger = ctx.createGain();
        pannerL.connect(wetMerger);
        pannerR.connect(wetMerger);
        pannerRearL.connect(wetMerger);
        pannerRearR.connect(wetMerger);
        speakerBusL.connect(wetMerger);
        speakerBusR.connect(wetMerger);

        // I. Convolver Reverb Node (Small Room Acoustic Space)
        const convolver = ctx.createConvolver();
        try { convolver.buffer = getSmallRoomBuffer(ctx); } catch (e) {}
        const reverbGain = ctx.createGain();
        postWidenerL.connect(convolver);
        postWidenerR.connect(convolver);
        convolver.connect(reverbGain);
        reverbGain.connect(wetMerger);

        // J. Output Limiter & Dry/Wet Mixer
        const dryGain = ctx.createGain();
        const wetGain = ctx.createGain();
        preampGain.connect(dryGain);

        const limiter = ctx.createDynamicsCompressor();
        // True Brickwall Peak Limiter settings (no pumping):
        limiter.threshold.setValueAtTime(-1.0, ctx.currentTime);
        limiter.knee.setValueAtTime(0.0, ctx.currentTime);
        limiter.ratio.setValueAtTime(20.0, ctx.currentTime);
        limiter.attack.setValueAtTime(0.001, ctx.currentTime);
        limiter.release.setValueAtTime(0.05, ctx.currentTime);

        dryGain.connect(limiter);
        wetMerger.connect(wetGain);
        wetGain.connect(limiter);

        limiter.connect(ctx.destination);

        const controller = {
            update(newCfg) {
                const now = ctx.currentTime;
                cfg = Object.assign(cfg, newCfg);

                preampGain.gain.setValueAtTime(cfg.volumeBoost || 1.0, now);
                vocalFilter.gain.setValueAtTime(cfg.vocalBoost ? 4.5 : 0.0, now);

                if (cfg.monoDownmix) {
                    sideWidthGain.gain.setValueAtTime(0.0, now);
                }

                if (!cfg.enabled || cfg.mode === "off") {
                    dryGain.gain.setValueAtTime(1.0, now);
                    wetGain.gain.setValueAtTime(0.0, now);
                    return;
                }

                dryGain.gain.setValueAtTime(0.0, now);
                wetGain.gain.setValueAtTime(1.0, now);

                const preset = PRESETS[cfg.mode] || PRESETS.standard;
                const isHp = (cfg.device !== "speakers");

                if (!cfg.monoDownmix) {
                    sideWidthGain.gain.setValueAtTime(preset.width, now);
                }

                if (isHp) {
                    crossGainLR.gain.setValueAtTime(preset.crossfeed, now);
                    crossGainRL.gain.setValueAtTime(preset.crossfeed, now);
                    speakerBusL.gain.setValueAtTime(0.0, now);
                    speakerBusR.gain.setValueAtTime(0.0, now);
                    pannerL.positionX.setValueAtTime(-0.5, now);
                    pannerR.positionX.setValueAtTime(0.5, now);

                    if (cfg.mode === "cinema") {
                        rearGain.gain.setValueAtTime(0.35, now);
                    } else {
                        rearGain.gain.setValueAtTime(0.0, now);
                    }
                } else {
                    crossGainLR.gain.setValueAtTime(0.0, now);
                    crossGainRL.gain.setValueAtTime(0.0, now);
                    rearGain.gain.setValueAtTime(0.0, now);
                    speakerBusL.gain.setValueAtTime(1.0, now);
                    speakerBusR.gain.setValueAtTime(1.0, now);
                }

                reverbGain.gain.setValueAtTime(preset.reverbAmount, now);
            },
            release() {
                try {
                    dryGain.disconnect();
                    wetGain.disconnect();
                    limiter.disconnect();
                    sourceNode.disconnect();
                } catch (e) {}
            }
        };

        controller.update(cfg);
        attachedElements.set(el, controller);
    }

    function registerElementEvents(el) {
        if (el.__ultraLightEventsAttached) return;
        el.__ultraLightEventsAttached = true;

        el.addEventListener("play", () => {
            wakeAudioContext();
            trySetupSurround(el);
        }, { passive: true });

        el.addEventListener("playing", () => {
            wakeAudioContext();
            trySetupSurround(el);
        }, { passive: true });

        el.addEventListener("pause", scheduleIdleSuspend, { passive: true });
        el.addEventListener("ended", scheduleIdleSuspend, { passive: true });
        el.addEventListener("emptied", scheduleIdleSuspend, { passive: true });
    }

    function scanMedia() {
        try {
            document.querySelectorAll("video, audio").forEach(el => {
                registerElementEvents(el);
                if (cfg.enabled && !el.paused) {
                    trySetupSurround(el);
                }
            });
        } catch (e) {}
    }

    // 7. Debounced DOM Mutation Observer
    let scanTimer = null;
    function scheduleScan() {
        if (scanTimer) return;
        scanTimer = setTimeout(() => {
            scanTimer = null;
            scanMedia();
        }, 200);
    }

    const observer = new MutationObserver(mutations => {
        let shouldScan = false;
        for (let i = 0; i < mutations.length; ++i) {
            const added = mutations[i].addedNodes;
            for (let j = 0; j < added.length; ++j) {
                const node = added[j];
                if (node.nodeType === 1) {
                    if (node.tagName === "VIDEO" || node.tagName === "AUDIO" || (node.querySelector && node.querySelector("video, audio"))) {
                        shouldScan = true;
                        break;
                    }
                }
            }
            if (shouldScan) break;
        }
        if (shouldScan) scheduleScan();
    });

    function startObserving() {
        const root = document.documentElement || document.body;
        if (root) {
            try { observer.observe(root, { childList: true, subtree: true }); } catch (e) {}
            scanMedia();
        } else {
            document.addEventListener("DOMContentLoaded", () => {
                const r = document.documentElement || document.body;
                if (r) {
                    try { observer.observe(r, { childList: true, subtree: true }); } catch (e) {}
                }
                scanMedia();
            }, { once: true });
        }
    }
    startObserving();

    // Interaction wakeup
    function onUserGesture() {
        wakeAudioContext();
        scanMedia();
    }
    ["click", "pointerdown", "keydown"].forEach(evt => {
        window.addEventListener(evt, onUserGesture, { passive: true });
    });

    // Global controller API
    window.__UltraLightSurround = {
        updateConfig(newCfg) {
            cfg = Object.assign(cfg, newCfg);
            try {
                document.querySelectorAll("video, audio").forEach(el => {
                    const c = attachedElements.get(el);
                    if (c) {
                        c.update(cfg);
                    } else if (cfg.enabled && !el.paused && canProcessElement(el)) {
                        trySetupSurround(el);
                    }
                });
            } catch (e) {}
        }
    };

    // Listen for WebMessages from host application
    if (window.chrome && window.chrome.webview) {
        window.chrome.webview.addEventListener("message", ev => {
            try {
                const data = typeof ev.data === "string" ? JSON.parse(ev.data) : ev.data;
                if (data && data.type === "updateAudioEnhancer") {
                    window.__UltraLightSurround.updateConfig(data);
                }
            } catch (e) {}
        });
    }
})();
)raw";

    std::wstring wideJs = StringUtils::Utf8ToWide(jsCode);
    m_webView->AddScriptToExecuteOnDocumentCreated(wideJs.c_str(), nullptr);
    m_webView->ExecuteScript(wideJs.c_str(), nullptr);
}

void WebViewManager::UpdateAudioEnhancer() {
    if (!m_webView) return;

    const auto& settings = Config::Instance().GetSettings();
    std::string effectiveDevice = settings.audioDeviceMode;
    if (effectiveDevice == "auto") {
        AudioEndpointType detected = GetDetectedAudioEndpoint();
        effectiveDevice = (detected == AudioEndpointType::Headphones) ? "headphones" : "speakers";
    }

#if __has_include(<nlohmann/json.hpp>)
    json msg = {
        {"type", "updateAudioEnhancer"},
        {"enabled", settings.enableSurroundSound},
        {"mode", settings.surroundSoundMode.empty() ? "standard" : settings.surroundSoundMode},
        {"device", effectiveDevice},
        {"vocalBoost", settings.enableVocalBoost},
        {"volumeBoost", settings.audioVolumeBoost},
        {"monoDownmix", settings.enableMonoDownmix}
    };
    std::string jsonNarrow = msg.dump();
#else
    std::string jsonNarrow = "{\"type\":\"updateAudioEnhancer\",\"enabled\":" +
        std::string(settings.enableSurroundSound ? "true" : "false") +
        ",\"mode\":\"" + settings.surroundSoundMode + "\",\"device\":\"" + effectiveDevice +
        "\",\"vocalBoost\":" + (settings.enableVocalBoost ? "true" : "false") +
        ",\"volumeBoost\":" + std::to_string(settings.audioVolumeBoost) +
        ",\"monoDownmix\":" + (settings.enableMonoDownmix ? "true" : "false") + "}";
#endif

    std::wstring wideJson = StringUtils::Utf8ToWide(jsonNarrow);
    m_webView->PostWebMessageAsJson(wideJson.c_str());

    std::wstring script = L"if (window.__UltraLightSurround && window.__UltraLightSurround.updateConfig) { "
                          L"window.__UltraLightSurround.updateConfig(" + wideJson + L"); }";
    m_webView->ExecuteScript(script.c_str(), nullptr);
}

} // namespace UltraLight
