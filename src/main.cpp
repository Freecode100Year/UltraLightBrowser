#include <windows.h>
#include <commctrl.h>
#include <shellscalingapi.h>
#include "AppShell.hpp"
#include "Config.hpp"
#include "PowerManager.hpp"
#include "WarpManager.hpp"
#include "WebViewManager.hpp"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shcore.lib")

int WINAPI wWinMain(
    _In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE /*hPrevInstance*/,
    _In_ PWSTR /*pCmdLine*/,
    _In_ int nCmdShow
) {
    // 1. Enable Per-Monitor V2 High-DPI Awareness
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // 2. Initialize COM for STA threading required by WebView2
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hr)) {
        return 1;
    }

    // 3. Initialize Common Controls v6
    INITCOMMONCONTROLSEX icex{};
    icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icex.dwICC = ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES | ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icex);

    // 4. Initialize Configuration, remove anything a crash or forced kill left behind
    // (unless another copy is running on the same profile), then start WARP
    UltraLight::Config::Instance();
    if (!UltraLight::AppShell::AnotherInstanceRunning()) UltraLight::WebViewManager::PurgeAllCacheAndTempFiles();
    UltraLight::WarpManager::Instance().Start();  // before the WebView2 environment exists

    // 5. Disable EcoQoS on Host Process for maximum responsiveness
    UltraLight::PowerManager::Instance().DisableEcoQoS(GetCurrentProcess());

    // 6. Windows, tabs and the message loop
    const int result = UltraLight::AppShell::Instance().Run(hInstance, nCmdShow);
    UltraLight::WarpManager::Instance().Stop();

    CoUninitialize();
    return result;
}
