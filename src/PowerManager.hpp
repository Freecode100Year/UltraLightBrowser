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

    // Windows 11 EcoQoS (Power Throttling) management
    void EnableEcoQoS(HANDLE hProcess);
    void DisableEcoQoS(HANDLE hProcess = GetCurrentProcess());

    // Elevate priority during heavy tasks / full screen video / active user interaction
    void SetHighPerformanceMode(bool enable);

    // Apply or remove EcoQoS on child processes (e.g. renderer, utility)
    void SetProcessTreeEcoQoS(DWORD parentPid, bool enableEcoQoS);
    void OptimizeProcessTree(DWORD parentPid) { SetProcessTreeEcoQoS(parentPid, false); }

    // Audio-aware lifecycle & memory management
    void HandleWindowMinimize(ICoreWebView2Controller* controller, ICoreWebView2* webView, bool isPlayingAudio);
    void HandleWindowRestore(ICoreWebView2Controller* controller, ICoreWebView2* webView);
    void HandleInactivitySuspend(ICoreWebView2Controller* controller, ICoreWebView2* webView, bool isPlayingAudio);
    void HandleActivityResume(ICoreWebView2Controller* controller, ICoreWebView2* webView);

    bool IsSuspended() const { return m_isSuspended; }
    bool IsAudioPlaybackBackgrounded() const { return m_isAudioPlaybackBackgrounded; }

    // Release working set
    void TrimWorkingSet();

private:
    PowerManager() = default;
    ~PowerManager() = default;

    PowerManager(const PowerManager&) = delete;
    PowerManager& operator=(const PowerManager&) = delete;

    bool m_isHighPerformance = false;
    bool m_isSuspended = false;
    bool m_isAudioPlaybackBackgrounded = false;
};

} // namespace UltraLight
