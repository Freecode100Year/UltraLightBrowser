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
    icex.dwICC = ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES | ICC_LISTVIEW_CLASSES;
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

    // 7. Register Global Accelerators (Ctrl+Shift+H, Ctrl+R, F5, Ctrl+L, Alt+Left, Alt+Right, F11, Zoom)
    ACCEL accels[] = {
        { FCONTROL | FSHIFT | FVIRTKEY, 'H', UltraLight::IDC_BTN_BLOCKER },
        { FCONTROL | FVIRTKEY, 'R', UltraLight::IDC_BTN_RELOAD },
        { FVIRTKEY, VK_F5, UltraLight::IDC_BTN_RELOAD },
        { FCONTROL | FVIRTKEY, 'L', UltraLight::IDM_FOCUS_ADDRESS_BAR },
        { FALT | FVIRTKEY, VK_LEFT, UltraLight::IDC_BTN_BACK },
        { FALT | FVIRTKEY, VK_RIGHT, UltraLight::IDC_BTN_FORWARD },
        { FVIRTKEY, VK_F11, UltraLight::IDM_TOGGLE_FULLSCREEN },
        // Zoom shortcuts: Ctrl + Plus / Minus / 0
        { FCONTROL | FVIRTKEY, VK_OEM_PLUS, UltraLight::IDM_ZOOM_IN },
        { FCONTROL | FVIRTKEY, VK_ADD, UltraLight::IDM_ZOOM_IN },
        { FCONTROL | FSHIFT | FVIRTKEY, VK_OEM_PLUS, UltraLight::IDM_ZOOM_IN },
        { FCONTROL | FVIRTKEY, VK_OEM_MINUS, UltraLight::IDM_ZOOM_OUT },
        { FCONTROL | FVIRTKEY, VK_SUBTRACT, UltraLight::IDM_ZOOM_OUT },
        { FCONTROL | FVIRTKEY, '0', UltraLight::IDM_ZOOM_RESET },
        { FCONTROL | FVIRTKEY, VK_NUMPAD0, UltraLight::IDM_ZOOM_RESET }
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
