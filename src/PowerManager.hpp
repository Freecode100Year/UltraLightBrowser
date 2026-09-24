#pragma once

#include <windows.h>
#include <wrl.h>
#include <wil/com.h>
#include <WebView2.h>
#include <vector>

namespace UltraLight {

class PowerManager {
public:
    static PowerManager& Instance();

    // Disable Windows 11 EcoQoS (Power Throttling) for optimal render throughput
    void DisableEcoQoS(HANDLE hProcess = GetCurrentProcess());

    // Elevate priority during heavy tasks / full screen video / active user interaction
    void SetHighPerformanceMode(bool enable);

    // Track child WebView2 renderer processes and optimize their QoS
    void OptimizeProcessTree(DWORD parentPid);

    // Memory trimming & suspension
    void HandleWindowMinimize(ICoreWebView2* webView);
    void HandleWindowRestore(ICoreWebView2* webView);

    // Release working set
    void TrimWorkingSet();

private:
    PowerManager() = default;
    ~PowerManager() = default;

    PowerManager(const PowerManager&) = delete;
    PowerManager& operator=(const PowerManager&) = delete;

    bool m_isHighPerformance = false;
    bool m_isSuspended = false;
};

} // namespace UltraLight
