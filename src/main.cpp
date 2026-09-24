#include <windows.h>
#include <commctrl.h>
#include <shellscalingapi.h>
#include <memory>
#include "MainWindow.hpp"
#include "PowerManager.hpp"
#include "Config.hpp"

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
    icex.dwICC = ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES;
    InitCommonControlsEx(&icex);

    // 4. Initialize Configuration
    UltraLight::Config::Instance();

    // 5. Disable EcoQoS on Host Process for maximum responsiveness
    UltraLight::PowerManager::Instance().DisableEcoQoS(GetCurrentProcess());

    // 6. Create Native Windows 11 Main Window
    auto mainWindow = std::make_unique<UltraLight::MainWindow>();
    if (!mainWindow->Create(hInstance, nCmdShow)) {
        CoUninitialize();
        return 1;
    }

    HWND hWnd = mainWindow->GetHwnd();

    // 7. Register Global Accelerators (Ctrl+Shift+H for Element Picker, Ctrl+R / F5 for Reload, Ctrl+L for address)
    ACCEL accels[] = {
        { FCONTROL | FSHIFT | FVIRTKEY, 'H', 1005 }, // IDC_BTN_BLOCKER
        { FCONTROL | FVIRTKEY, 'R', 1003 },          // IDC_BTN_RELOAD
        { FVIRTKEY, VK_F5, 1003 }                    // IDC_BTN_RELOAD
    };
    HACCEL hAccel = CreateAcceleratorTableW(accels, static_cast<int>(std::size(accels)));

    // 8. Standard Win32 Message Pump
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (hAccel && TranslateAcceleratorW(hWnd, hAccel, &msg)) {
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (hAccel) {
        DestroyAcceleratorTable(hAccel);
    }

    // 9. Shutdown & COM Cleanup
    mainWindow.reset();
    CoUninitialize();

    return static_cast<int>(msg.wParam);
}
