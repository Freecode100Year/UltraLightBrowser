#include "WebViewManager.hpp"
#include "Commands.hpp"
#include "InternalPages.hpp"
#include "Config.hpp"
#include "VideoDiagnostics.hpp"
#include "UserAgent.hpp"
#include "MacStealth.hpp"
#include "SelfTestPage.hpp"
#include "ElementBlocker.hpp"
#include "NativeRequestFilter.hpp"
#include "PowerManager.hpp"
#include "StringUtils.hpp"
#include <cwchar>
#include <iostream>
#include <fstream>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <mmdeviceapi.h>

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

WebViewManager::~WebViewManager() {
    // Probe timers carry `this` as their id; never let one fire on a dead object.
    if (m_hWndParent) {
        KillTimer(m_hWndParent, reinterpret_cast<UINT_PTR>(this));
        KillTimer(m_hWndParent, reinterpret_cast<UINT_PTR>(this) + 1);
    }
    Close();
}

std::wstring WebViewManager::s_capturedUaMetadata;

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

void WebViewManager::CreateEnvironment(HWND errorOwner, EnvironmentCallback done, int attempt) {
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
        L"--enable-features=DnsOverHttps,HighEfficiencyModeAvailable,PageDiscarding,Freezer,BatterySaverModeAvailable "
        L"--enable-hardware-overlays=\"single-fullscreen,single-on-top,underlay\" "
        L"--enable-native-gpu-memory-buffers "
        L"--media-cache-size=134217728 "
        L"--disk-cache-size=209715200 "
        L"--disable-features=Translate,OptimizationHints,MediaRouter "
        L"--disable-sync "
        L"--disable-domain-reliability "
        L"--disable-breakpad "
        L"--disable-speech-api "
        L"--no-first-run";

    // NOTE: --host-resolver-rules was completely removed to prevent command-line parsing conflicts
    // and argument mismatch 0x8007139F errors with running processes. NativeRequestFilter handles all blocking.

    if (!Config::Instance().GetSettings().hardwareAcceleration) {
        performanceArgs += L" --disable-gpu";
    }
    options->put_AdditionalBrowserArguments(performanceArgs.c_str());

    const std::wstring userDataDirStr = userDataDir.wstring();
    const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr,
        userDataDirStr.c_str(),
        options.Get(),
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [errorOwner, done, attempt](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(result) || !env) {
                    if ((result == HRESULT_FROM_WIN32(ERROR_INVALID_STATE) || result == static_cast<HRESULT>(0x8007139F)) && attempt < 2) {
                        Sleep(500);
                        CreateEnvironment(errorOwner, done, attempt + 1);
                        return S_OK;
                    }
                    ShowWebView2InitError(errorOwner, L"环境初始化", FAILED(result) ? result : E_FAIL);
                    done(FAILED(result) ? result : E_FAIL, nullptr);
                    return S_OK;
                }
                done(S_OK, env);
                return S_OK;
            }).Get());

    if (FAILED(hr)) {
        if ((hr == HRESULT_FROM_WIN32(ERROR_INVALID_STATE) || hr == static_cast<HRESULT>(0x8007139F)) && attempt < 2) {
            Sleep(500);
            CreateEnvironment(errorOwner, done, attempt + 1);
            return;
        }
        ShowWebView2InitError(errorOwner, L"环境创建调用", hr);
        done(hr, nullptr);
    }
}

HRESULT WebViewManager::Initialize(HWND hWndParent, ICoreWebView2Environment* environment, bool inPrivate,
                                   bool visible, ReadyCallback onReady) {
    m_hWndParent = hWndParent;
    m_environment = environment;
    m_inPrivate = inPrivate;
    m_startVisible = visible;
    m_onReady = std::move(onReady);
    if (!m_environment) return E_INVALIDARG;
    return CreateController(0);
}

HRESULT WebViewManager::CreateController(int attempt) {
    auto handler = Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
        [this, attempt](HRESULT res, ICoreWebView2Controller* controller) -> HRESULT {
            if (m_closed) {
                if (controller) controller->Close();
                return S_OK;
            }
            if (FAILED(res) || !controller) {
                if ((res == HRESULT_FROM_WIN32(ERROR_INVALID_STATE) || res == static_cast<HRESULT>(0x8007139F)) && attempt < 2) {
                    Sleep(500);
                    CreateController(attempt + 1);
                    return S_OK;
                }
                ShowWebView2InitError(m_hWndParent, L"控制器创建", FAILED(res) ? res : E_FAIL);
                return S_OK;
            }
            OnControllerCreated(controller);
            return S_OK;
        });

    if (m_inPrivate) {
        wil::com_ptr<ICoreWebView2Environment10> env10;
        wil::com_ptr<ICoreWebView2ControllerOptions> options;
        if (SUCCEEDED(m_environment->QueryInterface(IID_PPV_ARGS(&env10))) && env10 &&
            SUCCEEDED(env10->CreateCoreWebView2ControllerOptions(&options)) && options) {
            options->put_IsInPrivateModeEnabled(TRUE);
            return env10->CreateCoreWebView2ControllerWithOptions(m_hWndParent, options.get(), handler.Get());
        }
        // Without InPrivate support a private window would silently persist data.
        MessageBoxW(m_hWndParent, L"当前 WebView2 Runtime 不支持无痕模式，请更新 Microsoft Edge WebView2 Runtime。",
                    L"无痕浏览", MB_OK | MB_ICONWARNING);
        return E_NOTIMPL;
    }
    return m_environment->CreateCoreWebView2Controller(m_hWndParent, handler.Get());
}

void WebViewManager::OnControllerCreated(ICoreWebView2Controller* controller) {
    m_controller = controller;
    m_controller->get_CoreWebView2(&m_webView);
    if (!m_webView) return;

    // Built-in pages (start page, history, reader...) live on https://ulb.internal/.
    InternalPages::MapToWebView(m_webView.get());

    const auto uaResult = ApplyUserAgentProfile(Config::Instance().GetSettings().userAgentProfile);
    if (FAILED(uaResult) && Config::Instance().GetSettings().userAgentProfile != "default") {
        MessageBoxW(m_hWndParent, L"无法应用已保存的 UA，当前使用默认浏览器标识。", L"浏览器标识", MB_OK | MB_ICONWARNING);
    }

    // Foreground starts at NORMAL. PowerManager lowers the
    // memory budget only for silent background content.
    wil::com_ptr<ICoreWebView2_19> webView19;
    if (SUCCEEDED(m_webView->QueryInterface(IID_PPV_ARGS(&webView19))) && webView19) {
        webView19->put_MemoryUsageTargetLevel(COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_NORMAL);
    }

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

    // Browser shortcuts must work while the page has focus.
    m_controller->add_AcceleratorKeyPressed(
        Callback<ICoreWebView2AcceleratorKeyPressedEventHandler>(
            [this](ICoreWebView2Controller* /*sender*/, ICoreWebView2AcceleratorKeyPressedEventArgs* args) -> HRESULT {
                COREWEBVIEW2_KEY_EVENT_KIND kind;
                if (FAILED(args->get_KeyEventKind(&kind)) ||
                    (kind != COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN && kind != COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN)) {
                    return S_OK;
                }
                UINT key = 0;
                if (FAILED(args->get_VirtualKey(&key))) return S_OK;
                if (m_userActivityCb) m_userActivityCb();
                const bool isCtrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                const bool isShift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                const bool isAlt = (GetKeyState(VK_MENU) & 0x8000) != 0;
                if (const WORD command = MapShortcut(key, isCtrl, isShift, isAlt)) {
                    PostMessageW(m_hWndParent, WM_COMMAND, MAKEWPARAM(command, 0), 0);
                    args->put_Handled(TRUE);
                    return S_OK;
                }
                if (key == VK_ESCAPE) {
                    // Leave Escape to the page too (closing its own dialogs, exiting video fullscreen).
                    PostMessageW(m_hWndParent, WM_COMMAND, MAKEWPARAM(IDM_EXIT_FULLSCREEN, 0), 0);
                }
                return S_OK;
            }).Get(),
        nullptr);

    wil::com_ptr<ICoreWebView2Settings> settings;
    if (SUCCEEDED(m_webView->get_Settings(&settings)) && settings) {
        settings->put_IsStatusBarEnabled(TRUE);
        settings->put_AreDefaultScriptDialogsEnabled(TRUE);
    }

    // Default download flyout anchored under the toolbar's right edge.
    wil::com_ptr<ICoreWebView2_9> webView9;
    if (SUCCEEDED(m_webView->QueryInterface(IID_PPV_ARGS(&webView9))) && webView9) {
        webView9->put_DefaultDownloadDialogCornerAlignment(COREWEBVIEW2_DEFAULT_DOWNLOAD_DIALOG_CORNER_ALIGNMENT_TOP_RIGHT);
        POINT margin{12, 8};
        webView9->put_DefaultDownloadDialogMargin(margin);
    }

    // Setup bounds
    RECT bounds{};
    if (m_hasBounds) bounds = m_bounds;
    else GetClientRect(m_hWndParent, &bounds);
    m_controller->put_Bounds(bounds);
    m_controller->put_IsVisible(m_startVisible ? TRUE : FALSE);

    // Register internal and feature handlers
    RegisterEventHandlers();

    // Initialize modules
    ElementBlocker::Instance().Initialize(m_webView.get());
    NativeRequestFilter::Instance().Initialize(m_webView.get(), m_environment.get(), m_inPrivate);
    InjectSurroundSoundScript();

    // Apply QoS optimizations
    PowerManager::Instance().DisableEcoQoS();
    PowerManager::Instance().OptimizeProcessTree(GetCurrentProcessId());

    m_ready = true;
    if (!m_spoofStarting && !m_pendingNavigation.empty()) {
        const std::wstring url = std::move(m_pendingNavigation);
        m_pendingNavigation.clear();
        m_webView->Navigate(url.c_str());
    }
    if (m_onReady) {
        auto ready = std::move(m_onReady);
        m_onReady = nullptr;
        ready();
    }
}

void WebViewManager::Close() {
    m_closed = true;
    // Drop every callback first: completions that arrive after closing must not
    // reach a window that may already be gone.
    m_onReady = nullptr;
    m_titleChangedCb = nullptr;
    m_sourceChangedCb = nullptr;
    m_fullScreenCb = nullptr;
    m_zoomFactorChangedCb = nullptr;
    m_userActivityCb = nullptr;
    m_audioPlayingCb = nullptr;
    m_navStateCb = nullptr;
    m_navStartingCb = nullptr;
    m_navCompletedCb = nullptr;
    m_historyCb = nullptr;
    m_faviconCb = nullptr;
    m_newWindowCb = nullptr;
    m_permissionCb = nullptr;
    m_internalMessageCb = nullptr;
    m_contextActionCb = nullptr;
    m_contentLoadedCb = nullptr;
    if (m_webView) {
        ElementBlocker::Instance().Unregister(m_webView.get());
        NativeRequestFilter::Instance().Unregister(m_webView.get());
    }
    if (m_probeController) {
        m_probeController->Close();
        m_probeController = nullptr;
    }
    m_probeDone = nullptr;
    if (m_controller) {
        m_controller->Close();
        m_controller.reset();
    }
    m_webView.reset();
    m_environment.reset();
    m_ready = false;
}

UINT32 WebViewManager::BrowserProcessId() const {
    UINT32 pid = 0;
    if (m_webView) m_webView->get_BrowserProcessId(&pid);
    return pid;
}

void WebViewManager::ClearProfileData(ICoreWebView2* webView) {
    if (!webView) return;
    wil::com_ptr<ICoreWebView2_13> webView13;
    if (FAILED(webView->QueryInterface(IID_PPV_ARGS(&webView13))) || !webView13) return;
    wil::com_ptr<ICoreWebView2Profile> profile;
    if (FAILED(webView13->get_Profile(&profile)) || !profile) return;
    wil::com_ptr<ICoreWebView2Profile2> profile2;
    if (FAILED(profile->QueryInterface(IID_PPV_ARGS(&profile2))) || !profile2) return;
    HANDLE hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!hEvent) return;
    auto clearCb = Callback<ICoreWebView2ClearBrowsingDataCompletedHandler>(
        [hEvent](HRESULT) -> HRESULT {
            SetEvent(hEvent);
            return S_OK;
        });
    if (SUCCEEDED(profile2->ClearBrowsingDataAll(clearCb.Get()))) {
        const ULONGLONG start = GetTickCount64();
        while (WaitForSingleObject(hEvent, 10) != WAIT_OBJECT_0 && (GetTickCount64() - start) < 500) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
    }
    CloseHandle(hEvent);
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
                            static bool s_reported = false;
                            if (!s_reported && !m_closed) {
                                s_reported = true;
                                MessageBoxW(m_hWndParent,
                                    L"WebView2 核心主进程异常退出！\n可能由于后台进程冲突或显卡驱动崩溃引起。\n请在任务管理器中结束所有残留的 msedgewebview2.exe 进程后重新打开。",
                                    L"核心进程异常退出", MB_OK | MB_ICONERROR);
                            }
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
                    m_titleChangedCb(title.get() ? title.get() : L"");
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
                    m_sourceChangedCb(uri.get() ? uri.get() : L"");
                }
                return S_OK;
            }
        ).Get(),
        nullptr
    );

    m_webView->add_HistoryChanged(
        Callback<ICoreWebView2HistoryChangedEventHandler>(
            [this](ICoreWebView2* sender, IUnknown*) -> HRESULT {
                BOOL back = FALSE, forward = FALSE;
                sender->get_CanGoBack(&back);
                sender->get_CanGoForward(&forward);
                if (m_historyCb) m_historyCb(back != FALSE, forward != FALSE);
                return S_OK;
            }).Get(),
        nullptr);

    // Navigation Starting (Element Blocker rule injection and loading state)
    m_webView->add_NavigationStarting(
        Callback<ICoreWebView2NavigationStartingEventHandler>(
            [this](ICoreWebView2* sender, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                wil::unique_cotaskmem_string uri;
                if (FAILED(args->get_Uri(&uri)) || !uri.get()) return S_OK;
                if (m_navStartingCb) {
                    BOOL userInitiated = FALSE;
                    args->get_IsUserInitiated(&userInitiated);
                    bool historyNavigation = false;
                    wil::com_ptr<ICoreWebView2NavigationStartingEventArgs3> args3;
                    if (SUCCEEDED(args->QueryInterface(IID_PPV_ARGS(&args3))) && args3) {
                        COREWEBVIEW2_NAVIGATION_KIND navKind{};
                        if (SUCCEEDED(args3->get_NavigationKind(&navKind))) {
                            historyNavigation = navKind == COREWEBVIEW2_NAVIGATION_KIND_BACK_OR_FORWARD ||
                                                navKind == COREWEBVIEW2_NAVIGATION_KIND_RELOAD;
                        }
                    }
                    if (!m_navStartingCb(uri.get(), userInitiated != FALSE, historyNavigation)) {
                        args->put_Cancel(TRUE);
                        return S_OK;
                    }
                }
                NativeRequestFilter::Instance().SetMainFrameNavigation(sender, uri.get());
                ElementBlocker::Instance().OnNavigationStarting(sender, uri.get());
                if (m_navStateCb) {
                    m_navStateCb(true);
                }
                return S_OK;
            }
        ).Get(),
        nullptr
    );

    // Built-in pages must never be framed by web content.
    m_webView->add_FrameNavigationStarting(
        Callback<ICoreWebView2NavigationStartingEventHandler>(
            [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                wil::unique_cotaskmem_string uri;
                if (SUCCEEDED(args->get_Uri(&uri)) && uri.get() && InternalPages::IsInternal(uri.get())) {
                    args->put_Cancel(TRUE);
                }
                return S_OK;
            }).Get(),
        nullptr);

    // Navigation Completed (loading state finished)
    m_webView->add_NavigationCompleted(
        Callback<ICoreWebView2NavigationCompletedEventHandler>(
            [this](ICoreWebView2* sender, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                NativeRequestFilter::Instance().ClearMainFrameNavigation(sender);
                BOOL success = FALSE;
                args->get_IsSuccess(&success);
                wil::unique_cotaskmem_string uri;
                sender->get_Source(&uri);
                if (m_navStateCb) {
                    m_navStateCb(false);
                }
                if (m_navCompletedCb) {
                    m_navCompletedCb(success != FALSE, uri.get() ? uri.get() : L"");
                }
                return S_OK;
            }
        ).Get(),
        nullptr
    );

    m_webView->add_ContentLoading(
        Callback<ICoreWebView2ContentLoadingEventHandler>(
            [this](ICoreWebView2*, ICoreWebView2ContentLoadingEventArgs*) -> HRESULT {
                if (m_contentLoadedCb) m_contentLoadedCb(false);
                return S_OK;
            }).Get(),
        nullptr);

    wil::com_ptr<ICoreWebView2_2> webViewDom;
    if (SUCCEEDED(m_webView->QueryInterface(IID_PPV_ARGS(&webViewDom))) && webViewDom) {
        webViewDom->add_DOMContentLoaded(
            Callback<ICoreWebView2DOMContentLoadedEventHandler>(
                [this](ICoreWebView2*, ICoreWebView2DOMContentLoadedEventArgs*) -> HRESULT {
                    if (m_contentLoadedCb) m_contentLoadedCb(true);
                    return S_OK;
                }).Get(),
            nullptr);
    }

    // Web messages: built-in pages talk to the browser; web pages only to the element picker.
    m_webView->add_WebMessageReceived(
        Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [this](ICoreWebView2* sender, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                wil::unique_cotaskmem_string sourceUri;
                std::wstring src;
                if (SUCCEEDED(args->get_Source(&sourceUri)) && sourceUri.get()) {
                    src = sourceUri.get();
                }
                wil::unique_cotaskmem_string messageRaw;
                if (InternalPages::IsInternal(src)) {
                    if (m_internalMessageCb && SUCCEEDED(args->get_WebMessageAsJson(&messageRaw)) && messageRaw.get()) {
                        m_internalMessageCb(messageRaw.get());
                    }
                    return S_OK;
                }
                if (SUCCEEDED(args->TryGetWebMessageAsString(&messageRaw)) && messageRaw.get()) {
                    ElementBlocker::Instance().HandleWebMessage(sender, messageRaw.get(), src);
                } else if (SUCCEEDED(args->get_WebMessageAsJson(&messageRaw)) && messageRaw.get()) {
                    ElementBlocker::Instance().HandleWebMessage(sender, messageRaw.get(), src);
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

    // Script-opened windows and "open link in new window" become tabs.
    m_webView->add_NewWindowRequested(
        Callback<ICoreWebView2NewWindowRequestedEventHandler>(
            [this](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                if (m_newWindowCb) m_newWindowCb(args);
                return S_OK;
            }).Get(),
        nullptr);

    m_webView->add_PermissionRequested(
        Callback<ICoreWebView2PermissionRequestedEventHandler>(
            [this](ICoreWebView2*, ICoreWebView2PermissionRequestedEventArgs* args) -> HRESULT {
                if (!m_permissionCb) return S_OK;
                COREWEBVIEW2_PERMISSION_KIND kind{};
                wil::unique_cotaskmem_string uri;
                if (FAILED(args->get_PermissionKind(&kind)) || FAILED(args->get_Uri(&uri)) || !uri.get()) return S_OK;
                const auto state = m_permissionCb(kind, uri.get());
                if (state != COREWEBVIEW2_PERMISSION_STATE_DEFAULT) args->put_State(state);
                return S_OK;
            }).Get(),
        nullptr);

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

    // Site icons for the tab bar, sidebar and start page.
    wil::com_ptr<ICoreWebView2_15> webView15;
    if (SUCCEEDED(m_webView->QueryInterface(IID_PPV_ARGS(&webView15))) && webView15) {
        webView15->add_FaviconChanged(
            Callback<ICoreWebView2FaviconChangedEventHandler>(
                [this](ICoreWebView2* sender, IUnknown*) -> HRESULT {
                    wil::com_ptr<ICoreWebView2_15> s15;
                    if (FAILED(sender->QueryInterface(IID_PPV_ARGS(&s15))) || !s15) return S_OK;
                    wil::unique_cotaskmem_string pageUri;
                    sender->get_Source(&pageUri);
                    const std::wstring page = pageUri.get() ? pageUri.get() : L"";
                    s15->GetFavicon(COREWEBVIEW2_FAVICON_IMAGE_FORMAT_PNG,
                        Callback<ICoreWebView2GetFaviconCompletedHandler>(
                            [this, page](HRESULT hr, IStream* stream) -> HRESULT {
                                std::vector<std::uint8_t> png;
                                if (SUCCEEDED(hr) && stream) {
                                    std::uint8_t buf[8192];
                                    ULONG read = 0;
                                    while (SUCCEEDED(stream->Read(buf, sizeof(buf), &read)) && read > 0) {
                                        png.insert(png.end(), buf, buf + read);
                                        if (png.size() > 512 * 1024) { png.clear(); break; }
                                    }
                                }
                                if (m_faviconCb) m_faviconCb(page, png);
                                return S_OK;
                            }).Get());
                    return S_OK;
                }).Get(),
            nullptr);
    }

    // Link context menu: open in a tab / private window, add to the reading list.
    wil::com_ptr<ICoreWebView2_11> webView11;
    wil::com_ptr<ICoreWebView2Environment9> env9;
    if (SUCCEEDED(m_webView->QueryInterface(IID_PPV_ARGS(&webView11))) && webView11 &&
        SUCCEEDED(m_environment->QueryInterface(IID_PPV_ARGS(&env9))) && env9) {
        webView11->add_ContextMenuRequested(
            Callback<ICoreWebView2ContextMenuRequestedEventHandler>(
                [this, env9](ICoreWebView2*, ICoreWebView2ContextMenuRequestedEventArgs* args) -> HRESULT {
                    wil::com_ptr<ICoreWebView2ContextMenuTarget> target;
                    if (FAILED(args->get_ContextMenuTarget(&target)) || !target) return S_OK;
                    BOOL hasLink = FALSE;
                    target->get_HasLinkUri(&hasLink);
                    if (!hasLink) return S_OK;
                    wil::unique_cotaskmem_string link, text;
                    target->get_LinkUri(&link);
                    target->get_LinkText(&text);
                    if (!link.get()) return S_OK;
                    const std::wstring linkUri = link.get();
                    const std::wstring linkText = text.get() ? text.get() : L"";
                    wil::com_ptr<ICoreWebView2ContextMenuItemCollection> items;
                    if (FAILED(args->get_MenuItems(&items)) || !items) return S_OK;

                    UINT32 count = 0, insertAt = 0;
                    items->get_Count(&count);
                    for (UINT32 i = 0; i < count; ++i) {
                        wil::com_ptr<ICoreWebView2ContextMenuItem> item;
                        wil::unique_cotaskmem_string name;
                        if (SUCCEEDED(items->GetValueAtIndex(i, &item)) && item && SUCCEEDED(item->get_Name(&name)) &&
                            name.get() && std::wstring(name.get()) == L"openLinkInNewWindow") {
                            items->RemoveValueAtIndex(i);
                            insertAt = i;
                            break;
                        }
                    }
                    const struct { const wchar_t* label; const wchar_t* action; } entries[] = {
                        {L"在新标签页中打开链接", L"tab"},
                        {L"在后台标签页中打开链接", L"background"},
                        {L"在无痕窗口中打开链接", L"private"},
                        {L"添加链接到阅读列表", L"reading"},
                    };
                    for (const auto& e : entries) {
                        if (m_inPrivate && std::wstring(e.action) == L"private") continue;
                        wil::com_ptr<ICoreWebView2ContextMenuItem> item;
                        if (FAILED(env9->CreateContextMenuItem(e.label, nullptr, COREWEBVIEW2_CONTEXT_MENU_ITEM_KIND_COMMAND, &item)) || !item) continue;
                        const std::wstring action = e.action;
                        item->add_CustomItemSelected(
                            Callback<ICoreWebView2CustomItemSelectedEventHandler>(
                                [this, action, linkUri, linkText](ICoreWebView2ContextMenuItem*, IUnknown*) -> HRESULT {
                                    if (m_contextActionCb) m_contextActionCb(action, linkUri, linkText);
                                    return S_OK;
                                }).Get(),
                            nullptr);
                        items->InsertValueAtIndex(insertAt++, item.get());
                    }
                    return S_OK;
                }).Get(),
            nullptr);
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
    m_bounds = bounds;
    m_hasBounds = true;
    if (m_controller) {
        m_controller->put_Bounds(bounds);
    }
}

void WebViewManager::NotifyParentWindowPositionChanged() {
    if (m_controller) {
        m_controller->NotifyParentWindowPositionChanged();
    }
}

HRESULT WebViewManager::ApplyUserAgentProfile(const std::string& profile, bool reloadPage) {
    if (!m_webView) return E_PENDING;
    if (m_defaultUserAgent.empty()) {
        wil::com_ptr<ICoreWebView2Settings> settings;
        HRESULT hr = m_webView->get_Settings(&settings);
        if (FAILED(hr)) return hr;
        wil::com_ptr<ICoreWebView2Settings2> settings2;
        hr = settings->QueryInterface(IID_PPV_ARGS(&settings2));
        if (FAILED(hr)) return hr;
        LPWSTR original = nullptr;
        hr = settings2->get_UserAgent(&original);
        if (SUCCEEDED(hr) && original) m_defaultUserAgent = original;
        CoTaskMemFree(original);
        if (FAILED(hr)) return hr;
        if (m_defaultUserAgent.empty()) return E_FAIL;
    }
#if __has_include(<nlohmann/json.hpp>)
    if (profile == "macos-edge") {
        if (m_macSpoofActive || m_spoofStarting) {
            if (reloadPage && m_macSpoofActive) Reload();
            return S_OK;
        }
        if (BuildUserAgent(m_defaultUserAgent, profile).empty()) return E_INVALIDARG;
        // Hold the first navigation until the override is in place so no
        // request ever leaves with the Windows identity.
        if (!s_capturedUaMetadata.empty()) {
            // Metadata already probed for this runtime: apply before the first navigation.
            EnableMacSpoof(StringUtils::WideToUtf8(s_capturedUaMetadata));
            if (reloadPage) Reload();
            return S_OK;
        }
        m_spoofStarting = true;
        CaptureUaMetadata([this, reloadPage](const std::string& captured) {
            if (!captured.empty()) s_capturedUaMetadata = StringUtils::Utf8ToWide(captured);
            EnableMacSpoof(captured);
            m_spoofStarting = false;
            if (!m_pendingNavigation.empty()) {
                const std::wstring url = std::move(m_pendingNavigation);
                m_pendingNavigation.clear();
                if (m_webView) m_webView->Navigate(url.c_str());
            } else if (reloadPage) {
                Reload();
            }
        });
        return S_OK;
    }
    if (m_macSpoofActive) {
        DisableMacSpoof();
        if (reloadPage) Reload();
    }
    return S_OK;
#else
    return profile == "macos-edge" ? E_NOTIMPL : S_OK;
#endif
}

#if __has_include(<nlohmann/json.hpp>)
void WebViewManager::CallCdp(const wchar_t* method, const std::string& params, const wchar_t* sessionId) {
    if (!m_webView) return;
    const std::wstring wparams = StringUtils::Utf8ToWide(params);
    auto ignore = Callback<ICoreWebView2CallDevToolsProtocolMethodCompletedHandler>(
        [](HRESULT, LPCWSTR) -> HRESULT { return S_OK; });
    if (sessionId) {
        wil::com_ptr<ICoreWebView2_11> webView11;
        if (SUCCEEDED(m_webView->QueryInterface(IID_PPV_ARGS(&webView11))) && webView11) {
            webView11->CallDevToolsProtocolMethodForSession(sessionId, method, wparams.c_str(), ignore.Get());
        }
        return;
    }
    m_webView->CallDevToolsProtocolMethod(method, wparams.c_str(), ignore.Get());
}

void CALLBACK WebViewManager::ProbeTimeoutProc(HWND hWnd, UINT, UINT_PTR id, DWORD) {
    KillTimer(hWnd, id);
    if (auto* self = reinterpret_cast<WebViewManager*>(id)) {
        self->m_probeTimer = 0;
        self->FinishProbe({});
    }
}

void WebViewManager::FinishProbe(const std::string& result) {
    if (!m_probeDone || m_probeFinishing) return;
    m_probeFinishing = true;
    m_probeResult = result;
    if (m_probeTimer) { KillTimer(m_hWndParent, m_probeTimer); m_probeTimer = 0; }
    // Never close the probe WebView (or start the override) from inside one of
    // its own callbacks: WebView2 still touches the controller after we return.
    SetTimer(m_hWndParent, reinterpret_cast<UINT_PTR>(this) + 1, 0, &WebViewManager::ProbeFinishProc);
}

void CALLBACK WebViewManager::ProbeFinishProc(HWND hWnd, UINT, UINT_PTR id, DWORD) {
    KillTimer(hWnd, id);
    auto* self = reinterpret_cast<WebViewManager*>(id - 1);
    auto done = std::move(self->m_probeDone);
    self->m_probeDone = nullptr;
    self->m_probeFinishing = false;
    auto controller = std::move(self->m_probeController);
    self->m_probeController = nullptr;
    if (controller) controller->Close();
    if (done) done(self->m_probeResult);
}

// Reads the runtime's real brand list / full versions from a hidden WebView on a
// local secure origin, so the macOS metadata matches this exact Edge build.
void WebViewManager::CaptureUaMetadata(std::function<void(const std::string&)> done) {
    m_probeDone = std::move(done);
    m_probeTimer = SetTimer(m_hWndParent, reinterpret_cast<UINT_PTR>(this), 4000, &WebViewManager::ProbeTimeoutProc);

    std::error_code ec;
    const auto folder = Config::Instance().GetAppDataPath() / "probe";
    std::filesystem::create_directories(folder, ec);
    { std::ofstream(folder / "index.html") << "<!doctype html><title>probe</title>"; }

    const HRESULT hr = m_environment->CreateCoreWebView2Controller(m_hWndParent,
        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [this, folder](HRESULT res, ICoreWebView2Controller* controller) -> HRESULT {
                if (!m_probeDone) { if (controller) controller->Close(); return S_OK; }
                if (FAILED(res) || !controller) { FinishProbe({}); return S_OK; }
                m_probeController = controller;
                controller->put_IsVisible(FALSE);
                wil::com_ptr<ICoreWebView2> probe;
                wil::com_ptr<ICoreWebView2_3> probe3;
                if (FAILED(controller->get_CoreWebView2(&probe)) || !probe ||
                    FAILED(probe->QueryInterface(IID_PPV_ARGS(&probe3))) || !probe3) {
                    FinishProbe({});
                    return S_OK;
                }
                probe3->SetVirtualHostNameToFolderMapping(L"ulb-probe.example", folder.wstring().c_str(),
                    COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY);
                probe->add_NavigationCompleted(
                    Callback<ICoreWebView2NavigationCompletedEventHandler>(
                        [this](ICoreWebView2* sender, ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT {
                            const json params = {
                                {"expression", "(async()=>{const d=navigator.userAgentData;const h=await d.getHighEntropyValues("
                                               "['fullVersionList','uaFullVersion','formFactors']);return JSON.stringify("
                                               "{brands:d.brands,fullVersionList:h.fullVersionList,uaFullVersion:h.uaFullVersion,"
                                               "formFactors:h.formFactors||null});})()"},
                                {"awaitPromise", true}, {"returnByValue", true}};
                            sender->CallDevToolsProtocolMethod(L"Runtime.evaluate",
                                StringUtils::Utf8ToWide(params.dump()).c_str(),
                                Callback<ICoreWebView2CallDevToolsProtocolMethodCompletedHandler>(
                                    [this](HRESULT hr, LPCWSTR resultJson) -> HRESULT {
                                        std::string value;
                                        if (SUCCEEDED(hr) && resultJson) {
                                            try {
                                                const auto r = json::parse(StringUtils::WideToUtf8(resultJson));
                                                const auto res = r.find("result");
                                                if (res != r.end() && res->contains("value") && (*res)["value"].is_string())
                                                    value = (*res)["value"].get<std::string>();
                                            } catch (...) {}
                                        }
                                        FinishProbe(value);
                                        return S_OK;
                                    }).Get());
                            return S_OK;
                        }).Get(), nullptr);
                probe->Navigate(L"https://ulb-probe.example/index.html");
                return S_OK;
            }).Get());
    if (FAILED(hr)) FinishProbe({});
}

void WebViewManager::EnableMacSpoof(const std::string& capturedJson) {
    if (!m_webView) return;
    json meta = {
        {"platform", "macOS"}, {"platformVersion", Config::Instance().GetSettings().macPlatformVersion}, {"architecture", "arm"},
        {"model", ""}, {"mobile", false}, {"bitness", "64"}, {"wow64", false},
        {"formFactors", json::array({"Desktop"})}};
    bool captured = false;
    try {
        const auto c = json::parse(capturedJson);
        const auto isArray = [&c](const char* key) { return c.contains(key) && c[key].is_array(); };
        if (isArray("brands") && !c["brands"].empty() && isArray("fullVersionList")) {
            // Real Edge does not list the "Microsoft Edge WebView2" brand.
            const auto withoutWebView2 = [](const json& list) {
                json out = json::array();
                for (const auto& b : list) {
                    if (!(b.is_object() && b.contains("brand") && b["brand"] == "Microsoft Edge WebView2")) out.push_back(b);
                }
                return out;
            };
            meta["brands"] = withoutWebView2(c["brands"]);
            meta["fullVersionList"] = withoutWebView2(c["fullVersionList"]);
            if (c.contains("uaFullVersion") && c["uaFullVersion"].is_string()) meta["fullVersion"] = c["uaFullVersion"];
            if (isArray("formFactors")) meta["formFactors"] = c["formFactors"];
            captured = true;
        }
    } catch (...) {}
    if (!captured) {
        // Fallback: rebuild Chromium's GREASE list from the UA string.
        const std::string chrome = UserAgentToken(m_defaultUserAgent, L"Chrome");
        std::string edge = UserAgentToken(m_defaultUserAgent, L"Edg");
        if (edge.empty()) edge = chrome;
        const int major = std::atoi(chrome.c_str());
        const std::string majorStr = std::to_string(major);
        json brands = json::array(), full = json::array();
        for (const auto& [b, v] : GreasedBrandList(major, "Microsoft Edge", majorStr, majorStr)) {
            brands.push_back({{"brand", b}, {"version", v}});
            const bool grease = b != "Microsoft Edge" && b != "Chromium";
            full.push_back({{"brand", b}, {"version", grease ? v + ".0.0.0" : edge}});
        }
        meta["brands"] = brands;
        meta["fullVersionList"] = full;
        meta["fullVersion"] = edge;
    }
    const json ov = {
        {"userAgent", StringUtils::WideToUtf8(BuildUserAgent(m_defaultUserAgent, "macos-edge"))},
        {"platform", "MacIntel"},
        {"userAgentMetadata", meta}};
    m_macOverrideParams = ov.dump();
    m_stealthSource = kMacStealthScript;
    const std::wstring quotedUa = StringUtils::Utf8ToWide(ov["userAgent"].dump());
    const auto at = m_stealthSource.find(kMacUaPlaceholder);
    if (at != std::wstring::npos) m_stealthSource.replace(at, wcslen(kMacUaPlaceholder), quotedUa);

    CallCdp(L"Emulation.setUserAgentOverride", m_macOverrideParams);
    // macOS uses overlay scrollbars: no layout width, unlike Windows' ~17px.
    CallCdp(L"Emulation.setScrollbarsHidden", R"({"hidden":true})");

    m_webView->AddScriptToExecuteOnDocumentCreated(m_stealthSource.c_str(),
        Callback<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler>(
            [this](HRESULT hr, LPCWSTR id) -> HRESULT {
                if (SUCCEEDED(hr) && id) {
                    if (m_macSpoofActive) m_stealthScriptId = id;
                    else if (m_webView) m_webView->RemoveScriptToExecuteOnDocumentCreated(id);
                }
                return S_OK;
            }).Get());

    // Out-of-process iframes and workers get their own DevTools targets; pause
    // each at start, apply the same identity, then let it run.
    wil::com_ptr<ICoreWebView2_11> webView11;
    const bool canUseSessions = SUCCEEDED(m_webView->QueryInterface(IID_PPV_ARGS(&webView11))) && webView11;
    if (canUseSessions && !m_targetEventsHooked) {
        wil::com_ptr<ICoreWebView2DevToolsProtocolEventReceiver> receiver;
        if (SUCCEEDED(m_webView->GetDevToolsProtocolEventReceiver(L"Target.attachedToTarget", &receiver)) && receiver) {
            receiver->add_DevToolsProtocolEventReceived(
                Callback<ICoreWebView2DevToolsProtocolEventReceivedEventHandler>(
                    [this](ICoreWebView2*, ICoreWebView2DevToolsProtocolEventReceivedEventArgs* args) -> HRESULT {
                        wil::unique_cotaskmem_string params;
                        if (SUCCEEDED(args->get_ParameterObjectAsJson(&params)) && params.get()) {
                            OnTargetAttached(params.get());
                        }
                        return S_OK;
                    }).Get(), nullptr);
            m_targetEventsHooked = true;
        }
    }
    if (m_targetEventsHooked) {
        CallCdp(L"Target.setAutoAttach", R"({"autoAttach":true,"waitForDebuggerOnStart":true,"flatten":true})");
    }
    m_macSpoofActive = true;
}

void WebViewManager::OnTargetAttached(const std::wstring& paramsJson) {
    std::wstring sessionId;
    std::string type;
    try {
        const auto p = json::parse(StringUtils::WideToUtf8(paramsJson));
        if (p.contains("sessionId") && p["sessionId"].is_string())
            sessionId = StringUtils::Utf8ToWide(p["sessionId"].get<std::string>());
        if (p.contains("targetInfo") && p["targetInfo"].is_object())
            type = p["targetInfo"].value("type", std::string());
    } catch (...) {}
    if (sessionId.empty()) return;
    const wchar_t* sid = sessionId.c_str();
    if (m_macSpoofActive) {
        if (type == "iframe") {
            CallCdp(L"Emulation.setUserAgentOverride", m_macOverrideParams, sid);
            CallCdp(L"Emulation.setScrollbarsHidden", R"({"hidden":true})", sid);
            CallCdp(L"Target.setAutoAttach", R"({"autoAttach":true,"waitForDebuggerOnStart":true,"flatten":true})", sid);
        } else if (type == "worker" || type == "service_worker" || type == "shared_worker") {
            CallCdp(L"Network.setUserAgentOverride", m_macOverrideParams, sid);
            const json eval = {{"expression", StringUtils::WideToUtf8(m_stealthSource)}};
            CallCdp(L"Runtime.evaluate", eval.dump(), sid);
        }
    }
    // Commands on a session run in order; always resume so nothing stays paused.
    CallCdp(L"Runtime.runIfWaitingForDebugger", "{}", sid);
}

void WebViewManager::DisableMacSpoof() {
    m_macSpoofActive = false;
    if (!m_webView) return;
    if (m_targetEventsHooked) {
        CallCdp(L"Target.setAutoAttach", R"({"autoAttach":false,"waitForDebuggerOnStart":false,"flatten":true})");
    }
    CallCdp(L"Emulation.setUserAgentOverride", R"({"userAgent":""})");
    CallCdp(L"Emulation.setScrollbarsHidden", R"({"hidden":false})");
    if (!m_stealthScriptId.empty()) {
        m_webView->RemoveScriptToExecuteOnDocumentCreated(m_stealthScriptId.c_str());
        m_stealthScriptId.clear();
    }
}
#endif

std::wstring WebViewManager::SearchUrl(const std::wstring& query) {
    const std::string& engine = Config::Instance().GetSettings().searchEngine;
    const std::wstring q = StringUtils::UrlEncode(query);
    if (engine == "bing") return L"https://www.bing.com/search?q=" + q;
    if (engine == "duckduckgo") return L"https://duckduckgo.com/?q=" + q;
    if (engine == "startpage") return L"https://www.startpage.com/do/search?q=" + q;
    if (engine == "baidu") return L"https://www.baidu.com/s?wd=" + q;
    return L"https://www.google.com/search?q=" + q;
}

std::wstring WebViewManager::ResolveInput(const std::wstring& input) {
    std::wstring target = input;
    while (!target.empty() && iswspace(target.front())) target.erase(target.begin());
    while (!target.empty() && iswspace(target.back())) target.pop_back();
    if (target.empty()) return target;

    if (target.find(L"://") != std::wstring::npos ||
        target.rfind(L"about:", 0) == 0 ||
        target.rfind(L"data:", 0) == 0 ||
        target.rfind(L"javascript:", 0) == 0) {
        // Direct URL with recognized scheme
    } else if (target.size() > 2 && target[1] == L':' && (target[2] == L'\\' || target[2] == L'/')) {
        target = L"file:///" + target;  // dropped or typed local path
    } else if (target.rfind(L"localhost", 0) == 0 || target.rfind(L"127.0.0.1", 0) == 0) {
        target = L"http://" + target;
    } else if (target.find(L'.') != std::wstring::npos && target.find(L' ') == std::wstring::npos) {
        target = L"https://" + target;
    } else {
        target = SearchUrl(target);
    }
    return target;
}

void WebViewManager::Navigate(const std::wstring& url) {
    const std::wstring target = ResolveInput(url);
    if (target.empty()) return;
    if (InternalPages::IsInternal(target)) m_internalGrant = true;
    if (!m_webView || m_spoofStarting) {
        m_pendingNavigation = target;
        return;
    }
    m_webView->Navigate(target.c_str());
}

bool WebViewManager::ConsumeInternalGrant() {
    const bool granted = m_internalGrant;
    m_internalGrant = false;
    return granted;
}

void WebViewManager::OpenIdentitySelfTest() {
    if (!m_webView) return;
    wil::com_ptr<ICoreWebView2_3> webView3;
    if (FAILED(m_webView->QueryInterface(IID_PPV_ARGS(&webView3))) || !webView3) return;
    std::error_code ec;
    const auto folder = Config::Instance().GetAppDataPath() / "selftest";
    std::filesystem::create_directories(folder, ec);
    { std::ofstream(folder / "index.html", std::ios::binary) << kSelfTestHtml; }
    { std::ofstream(folder / "sw.js", std::ios::binary) << kSelfTestServiceWorker; }
    webView3->SetVirtualHostNameToFolderMapping(L"ulb-selftest.example", folder.wstring().c_str(),
        COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY);
    Navigate(L"https://ulb-selftest.example/index.html");
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

// Static PROPERTYKEY definitions avoiding external GUID library linkage issues across Windows SDKs
// PKEY_AudioEndpoint_FormFactor: {1DA5D803-D492-4EDD-8C23-ED48FFEE79DF}, 0
static const PROPERTYKEY kPKeyAudioEndpointFormFactor = {
    { 0x1da5d803, 0xd492, 0x4edd, { 0x8c, 0x23, 0xed, 0x48, 0xff, 0xee, 0x79, 0xdf } },
    0
};

// PKEY_Device_FriendlyName: {A45C254E-DF1C-4EFD-8020-67D146A850E0}, 14
static const PROPERTYKEY kPKeyDeviceFriendlyName = {
    { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } },
    14
};

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
    hr = pProps->GetValue(kPKeyAudioEndpointFormFactor, &var);
    if (SUCCEEDED(hr) && var.vt == VT_UI4) {
        // 3: Headphones, 5: Headset
        if (var.ulVal == 3 || var.ulVal == 5) {
            result = AudioEndpointType::Headphones;
        } else {
            result = AudioEndpointType::Speakers;
        }
    } else {
        PropVariantClear(&var);
        hr = pProps->GetValue(kPKeyDeviceFriendlyName, &var);
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

void WebViewManager::InjectSurroundSoundScript(bool reloadPage) {
    if (!m_webView) return;

    const auto& settings = Config::Instance().GetSettings();
    std::string initMode = settings.surroundSoundMode.empty() ? "standard" : settings.surroundSoundMode;
    std::string initEnabled = settings.enableSurroundSound ? "true" : "false";
    std::string effectiveDevice = settings.audioDeviceMode;
    if (effectiveDevice == "auto") {
        AudioEndpointType detected = GetDetectedAudioEndpoint();
        effectiveDevice = (detected == AudioEndpointType::Headphones) ? "headphones" : "speakers";
    }
    std::string initDeEsser = settings.enableDeEsser ? "true" : "false";
    std::string initNightMode = settings.enableNightMode ? "true" : "false";
    std::string initVocalBoost = settings.enableVocalBoost ? "true" : "false";
    std::string initVolumeBoost = std::to_string(settings.audioVolumeBoost);
    std::string initMonoDownmix = settings.enableMonoDownmix ? "true" : "false";
    std::string initNativeOutput = settings.systemAudioPassthrough ? "true" : "false";

    std::string jsCode;
    jsCode.reserve(16384);

    jsCode += R"raw(
(function() {
    if (window.__UltraLightSurroundInstalled) return;
    window.__UltraLightSurroundInstalled = true;

    let cfg = {
        nativeOutput: )raw";
    jsCode += initNativeOutput;
    jsCode += R"raw(,
        enabled: )raw";
    jsCode += initEnabled;
    jsCode += R"raw(,
        mode: ")raw";
    jsCode += initMode;
    jsCode += R"raw(",
        device: ")raw";
    jsCode += effectiveDevice;
    jsCode += R"raw(",
        deEsser: )raw";
    jsCode += initDeEsser;
    jsCode += R"raw(,
        nightMode: )raw";
    jsCode += initNightMode;
    jsCode += R"raw(,
        vocalBoost: )raw";
    jsCode += initVocalBoost;
    jsCode += R"raw(,
        volumeBoost: )raw";
    jsCode += initVolumeBoost;
    jsCode += R"raw(,
        monoDownmix: )raw";
    jsCode += initMonoDownmix;
    jsCode += R"raw(
    };

    const PRESETS = {
        dialogue: { width: 1.0, crossfeed: 0.0, reverbAmount: 0.0 },
        off: { width: 1.0, crossfeed: 0.0, reverbAmount: 0.0 },
        light: { width: 1.15, crossfeed: 0.12, reverbAmount: 0.06 },
        standard: { width: 1.35, crossfeed: 0.20, reverbAmount: 0.10 },
        cinema: { width: 1.55, crossfeed: 0.25, reverbAmount: 0.16 }
    };

    // Adapted from XQL-MUSIC: lowpass crossfeed with centre-image compensation.
    const CROSSFEED_PARAMS = {
        light: { cutoff: 800, level: 0.25, compFreq: 520, compDb: -2.35 },
        standard: { cutoff: 700, level: 0.40, compFreq: 470, compDb: -3.65 },
        cinema: { cutoff: 620, level: 0.55, compFreq: 430, compDb: -4.80 }
    };
    const DEESS_CROSSOVER_HZ = 5500;
    // Web Audio low/highpass Q is in dB; peaking EQ Q remains linear.
    const BUTTERWORTH_Q_DB = -3.01029995664;
    const EQ_HEADROOM_DB = 0.5;
    const attachedElements = new WeakMap();
    const pageClaimedElements = new WeakSet();
    const pendingPlayElements = new WeakSet();
    let audioCtx = null;
    let cachedReverbBuffer = null;
    let idleTimer = null;

    // 1. Give precedence to web-page native Web Audio
    try {
        const AC = window.AudioContext || window.webkitAudioContext;
        if (!cfg.nativeOutput && AC && AC.prototype && AC.prototype.createMediaElementSource) {
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
        if (cfg.nativeOutput) return null;
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
)raw";

    jsCode += R"raw(
    // 3. Synthesized Small-Room Impulse Response (Reverb)
    function getSmallRoomBuffer(ctx) {
        if (cachedReverbBuffer && cachedReverbBuffer.sampleRate === ctx.sampleRate) {
            return cachedReverbBuffer;
        }
        const rate = ctx.sampleRate;
        const duration = 0.035;
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
)raw";

    jsCode += R"raw(
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

    function needsProcessing() {
        return !cfg.nativeOutput && (cfg.enabled || cfg.vocalBoost || cfg.monoDownmix || cfg.deEsser || cfg.nightMode || cfg.volumeBoost > 1.0);
    }

    function trySetupSurround(el) {
        if (!needsProcessing() || !canProcessElement(el)) return;
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
)raw";

    jsCode += R"raw(
    // 6. Build only the DSP nodes required by the selected preset.
    function setupSurroundForElement(el) {
        if (!needsProcessing() || !canProcessElement(el)) return;
        const ctx = getAudioContext();
        if (!ctx || ctx.state !== "running") return;
        let sourceNode;
        try { sourceNode = ctx.createMediaElementSource(el); } catch (e) { return; }

        let graphNodes = [];
        let preamp = null;
        let vocal = null;
        let topology = "";
        let rebuildTimer = null;
        const output = ctx.createGain();
        output.connect(ctx.destination);
        function smooth(param, value) {
            param.cancelScheduledValues(ctx.currentTime);
            param.setTargetAtTime(value, ctx.currentTime, 0.015);
        }
        function make(method, ...args) {
            const node = ctx[method](...args);
            graphNodes.push(node);
            return node;
        }
        function gain(value) {
            const node = make("createGain"); node.gain.value = value; return node;
        }
        function clearGraph() {
            sourceNode.disconnect();
            for (const node of graphNodes) { try { node.disconnect(); } catch (e) {} }
            graphNodes = []; preamp = null; vocal = null;
        }
        function voiceDb() {
            return (cfg.vocalBoost || (cfg.enabled && cfg.mode === "dialogue")) ? 4.5 : 0;
        }
        function preampValue() {
            const requested = Math.max(1, Math.min(3, Number(cfg.volumeBoost) || 1));
            const trimDb = voiceDb() > 0 ? voiceDb() + EQ_HEADROOM_DB : 0;
            return requested * Math.pow(10, -trimDb / 20);
        }
        function build() {
            clearGraph();
            // This unity path retains the source channels; no EQ or compression.
            if (!needsProcessing()) {
                sourceNode.connect(output);
                return;
            }
            preamp = gain(preampValue());
            preamp.channelCount = 2;
            preamp.channelCountMode = "explicit";
            preamp.channelInterpretation = "speakers";
            sourceNode.connect(preamp);
            vocal = make("createBiquadFilter");
            vocal.type = "peaking"; vocal.frequency.value = 3000;
            vocal.Q.value = 1.2; vocal.gain.value = voiceDb();
            preamp.connect(vocal);
            let toneOutput = vocal;
            if (cfg.deEsser) {
                const merge = gain(1);
                function crossover(type) {
                    const a = make("createBiquadFilter"), b = make("createBiquadFilter");
                    a.type = b.type = type;
                    a.frequency.value = b.frequency.value = DEESS_CROSSOVER_HZ;
                    a.Q.value = b.Q.value = BUTTERWORTH_Q_DB;
                    vocal.connect(a); a.connect(b); return b;
                }
                // Match DynamicsCompressorNode's specified 6ms lookahead delay.
                const lowDelay = make("createDelay", 0.02);
                lowDelay.delayTime.value = 0.006;
                crossover("lowpass").connect(lowDelay); lowDelay.connect(merge);
                const high = crossover("highpass");
                const deEsser = make("createDynamicsCompressor");
                deEsser.threshold.value = -32; deEsser.knee.value = 6;
                deEsser.ratio.value = 4; deEsser.attack.value = 0.002;
                deEsser.release.value = 0.06;
                high.connect(deEsser); deEsser.connect(merge); toneOutput = merge;
            }
            const splitter = make("createChannelSplitter", 2);
            toneOutput.connect(splitter);
            const mid = gain(1), side = gain(1);
            const midL = gain(0.5), midR = gain(0.5);
            const sideL = gain(0.5), sideR = gain(-0.5);
            splitter.connect(midL, 0); splitter.connect(midR, 1);
            midL.connect(mid); midR.connect(mid);
            splitter.connect(sideL, 0); splitter.connect(sideR, 1);
            sideL.connect(side); sideR.connect(side);
            const preset = cfg.enabled ? (PRESETS[cfg.mode] || PRESETS.standard) : PRESETS.off;
            const left = gain(1), right = gain(1);
            mid.connect(left); mid.connect(right);
            if (!cfg.monoDownmix) {
                let sideInput = side;
                if (cfg.enabled && cfg.mode !== "dialogue") {
                    const highpass = make("createBiquadFilter");
                    highpass.type = "highpass"; highpass.frequency.value = 150;
                    highpass.Q.value = BUTTERWORTH_Q_DB; side.connect(highpass); sideInput = highpass;
                }
                const widthL = gain(preset.width), widthR = gain(-preset.width);
                sideInput.connect(widthL); sideInput.connect(widthR);
                widthL.connect(left); widthR.connect(right);
            }
            let program;
            if (cfg.enabled && cfg.device !== "speakers" && cfg.mode !== "dialogue") {
                const crossLeft = gain(1), crossRight = gain(1);
                left.connect(crossLeft); right.connect(crossRight);
                const cf = CROSSFEED_PARAMS[cfg.mode] || CROSSFEED_PARAMS.standard;
                function crossfeed(from, to) {
                    const filter = make("createBiquadFilter");
                    filter.type = "lowpass"; filter.frequency.value = cf.cutoff;
                    filter.Q.value = BUTTERWORTH_Q_DB;
                    const mix = gain(cf.level);
                    from.connect(filter); filter.connect(mix); mix.connect(to);
                }
                crossfeed(left, crossRight); crossfeed(right, crossLeft);
                function compensate(from) {
                    const shelf = make("createBiquadFilter");
                    shelf.type = "lowshelf"; shelf.frequency.value = cf.compFreq;
                    shelf.gain.value = cf.compDb;
                    from.connect(shelf); return shelf;
                }
                function panner(from, x, z) {
                    const p = make("createPanner"); p.panningModel = "HRTF";
                    p.positionX.value = x; p.positionY.value = 0; p.positionZ.value = z;
                    from.connect(p); return p;
                }
                program = gain(1);
                panner(compensate(crossLeft), -0.5, -0.866).connect(program);
                panner(compensate(crossRight), 0.5, -0.866).connect(program);
                if (cfg.mode === "cinema" && !cfg.monoDownmix) {
                    const delay = make("createDelay", 0.05); delay.delayTime.value = 0.015;
                    const rear = gain(0.20); side.connect(delay); delay.connect(rear);
                    panner(rear, -0.94, 0.34).connect(program);
                    panner(rear, 0.94, 0.34).connect(program);
                }
            } else {
                // A gain bus would sum L/R to mono; a merger preserves stereo.
                program = make("createChannelMerger", 2);
                left.connect(program, 0, 0); right.connect(program, 0, 1);
            }
            let finalMix = program;
            if (cfg.enabled && cfg.mode === "cinema") {
                const mix = gain(1), convolver = make("createConvolver");
                convolver.buffer = getSmallRoomBuffer(ctx);
                const wet = gain(preset.reverbAmount);
                program.connect(mix); program.connect(convolver);
)raw";

    jsCode += R"raw(                convolver.connect(wet); wet.connect(mix); finalMix = mix;
            }
            if (cfg.nightMode) {
                const night = make("createDynamicsCompressor");
                night.threshold.value = -24; night.knee.value = 18;
                night.ratio.value = 3; night.attack.value = 0.015;
                night.release.value = 0.24;
                finalMix.connect(night); finalMix = night;
            }
            const compressor = make("createDynamicsCompressor");
            // Soft protection, not a certified true-peak/brickwall limiter.
            const dialogue = cfg.enabled && cfg.mode === "dialogue";
            compressor.threshold.value = dialogue ? -18 : -3;
            compressor.knee.value = dialogue ? 6 : 3;
            compressor.ratio.value = dialogue ? 3 : 12; compressor.attack.value = 0.003;
            compressor.release.value = 0.10;
            finalMix.connect(compressor); compressor.connect(output);
        }
        function key() {
            return JSON.stringify([needsProcessing(), cfg.enabled, cfg.mode, cfg.device, cfg.monoDownmix, cfg.deEsser, cfg.nightMode]);
        }
        const controller = {
            update() {
                const next = key();
                if (next !== topology) {
                    topology = next;
                    if (rebuildTimer) clearTimeout(rebuildTimer);
                    smooth(output.gain, 0);
                    rebuildTimer = setTimeout(() => {
                        rebuildTimer = null;
                        try { build(); } catch (e) { clearGraph(); sourceNode.connect(output); }
                        smooth(output.gain, 1);
                    }, 40);
                } else {
                    if (preamp) smooth(preamp.gain, preampValue());
                    if (vocal) smooth(vocal.gain, voiceDb());
                }
            },
            release() {
                if (rebuildTimer) clearTimeout(rebuildTimer);
                clearGraph(); output.disconnect();
            }
        };
        topology = key();
        try { build(); } catch (e) { clearGraph(); sourceNode.connect(output); }
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
                if (needsProcessing() && !el.paused) {
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
)raw";

    jsCode += R"raw(
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
        getStatus() {
            const media = [];
            document.querySelectorAll("video, audio").forEach(el => {
                const quality = el.getVideoPlaybackQuality ? el.getVideoPlaybackQuality() : null;
                media.push({ kind: el.tagName || "media", paused: el.paused,
                    processed: attachedElements.has(el),
                    width: el.videoWidth || 0, height: el.videoHeight || 0,
                    totalFrames: quality ? quality.totalVideoFrames : null,
                    droppedFrames: quality ? quality.droppedVideoFrames : null });
            });
            return { output: cfg.nativeOutput ? "native" : "enhanced",
                preset: cfg.mode, audioContext: audioCtx ? audioCtx.state : "not-created", media };
        },
        updateConfig(newCfg) {
            cfg = Object.assign(cfg, newCfg);
            try {
                document.querySelectorAll("video, audio").forEach(el => {
                    const c = attachedElements.get(el);
                    if (c) {
                        c.update(cfg);
                    } else if (needsProcessing() && !el.paused && canProcessElement(el)) {
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
    const auto generation = ++m_audioScriptGeneration;
    m_audioReloadPending = m_audioReloadPending || reloadPage;
    if (!m_audioScriptId.empty()) {
        m_webView->RemoveScriptToExecuteOnDocumentCreated(m_audioScriptId.c_str());
        m_audioScriptId.clear();
    }
    ++m_pendingAudioScriptRegistrations;
    const HRESULT registrationResult = m_webView->AddScriptToExecuteOnDocumentCreated(wideJs.c_str(),
        Callback<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler>(
            [this, generation](HRESULT result, LPCWSTR id) -> HRESULT {
                --m_pendingAudioScriptRegistrations;
                if (generation != m_audioScriptGeneration) {
                    if (SUCCEEDED(result) && id && m_webView) {
                        m_webView->RemoveScriptToExecuteOnDocumentCreated(id);
                    }
                } else if (SUCCEEDED(result) && id) {
                    m_audioScriptId = id;
                }
                // Wait for all older callbacks to remove their stale scripts.
                // A subsequent volume/preset update must not lose a mode-switch reload.
                if (m_pendingAudioScriptRegistrations == 0) {
                    const bool shouldReload = m_audioReloadPending && !m_audioScriptId.empty();
                    m_audioReloadPending = false;
                    if (shouldReload && m_webView) m_webView->Reload();
                }
                return S_OK;
            }).Get());
    if (FAILED(registrationResult)) {
        --m_pendingAudioScriptRegistrations;
        if (m_pendingAudioScriptRegistrations == 0) m_audioReloadPending = false;
    }
    m_webView->ExecuteScript(wideJs.c_str(), nullptr);
}

void WebViewManager::ShowMediaDiagnostics() {
    if (!m_webView) return;
    const HWND owner = m_hWndParent;
    const wchar_t* script = kVideoDiagnosticsScript;
    m_webView->ExecuteScript(script,
        Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
            [owner](HRESULT result, LPCWSTR payload) -> HRESULT {
                if (FAILED(result) || !payload || !IsWindow(owner)) return S_OK;
#if __has_include(<nlohmann/json.hpp>)
                try {
                    const auto value = json::parse(StringUtils::WideToUtf8(payload));
                    if (value.is_string()) {
                        const auto text = StringUtils::Utf8ToWide(value.get<std::string>());
                        MessageBoxW(owner, text.c_str(), L"播放诊断", MB_OK | MB_ICONINFORMATION);
                    }
                } catch (...) {}
#endif
                return S_OK;
            }).Get());
}

void WebViewManager::UpdateAudioEnhancer(bool reloadPage) {
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
        {"nativeOutput", settings.systemAudioPassthrough},
        {"enabled", settings.enableSurroundSound},
        {"mode", settings.surroundSoundMode.empty() ? "standard" : settings.surroundSoundMode},
        {"device", effectiveDevice},
        {"deEsser", settings.enableDeEsser},
        {"nightMode", settings.enableNightMode},
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
    // Refresh the document-created script so subsequent navigations keep settings.
    InjectSurroundSoundScript(reloadPage);
}

} // namespace UltraLight
