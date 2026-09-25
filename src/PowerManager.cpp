#include "PowerManager.hpp"
#include <tlhelp32.h>
#include <psapi.h>
#include <iostream>

namespace UltraLight {

PowerManager& PowerManager::Instance() {
    static PowerManager s_instance;
    return s_instance;
}

void PowerManager::EnableEcoQoS(HANDLE hProcess) {
    // Windows 11 EcoQoS (Power Throttling): pin non-critical threads to E-Cores (efficiency cores)
    PROCESS_POWER_THROTTLING_STATE throttling{};
    throttling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    throttling.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    throttling.StateMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED; // Turn on EcoQoS

    SetProcessInformation(
        hProcess,
        ProcessPowerThrottling,
        &throttling,
        sizeof(throttling)
    );
}

void PowerManager::DisableEcoQoS(HANDLE hProcess) {
    // Disable Windows 11 EcoQoS (Power Throttling) to restore standard scheduling on P-Cores
    PROCESS_POWER_THROTTLING_STATE throttling{};
    throttling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    throttling.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    throttling.StateMask = 0; // Turn off throttling (disable EcoQoS)

    SetProcessInformation(
        hProcess,
        ProcessPowerThrottling,
        &throttling,
        sizeof(throttling)
    );
}

void PowerManager::SetHighPerformanceMode(bool enable) {
    if (m_isHighPerformance == enable) return;
    m_isHighPerformance = enable;

    SetPriorityClass(
        GetCurrentProcess(),
        enable ? ABOVE_NORMAL_PRIORITY_CLASS : NORMAL_PRIORITY_CLASS
    );
}

void PowerManager::SetProcessTreeEcoQoS(DWORD parentPid, bool enableEcoQoS) {
    if (parentPid == 0) return;

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(PROCESSENTRY32W);

    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ParentProcessID == parentPid) {
                HANDLE hChild = OpenProcess(
                    PROCESS_SET_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION,
                    FALSE,
                    entry.th32ProcessID
                );
                if (hChild) {
                    if (enableEcoQoS) {
                        EnableEcoQoS(hChild);
                    } else {
                        DisableEcoQoS(hChild);
                    }
                    CloseHandle(hChild);
                }
                // Recursively traverse child processes
                SetProcessTreeEcoQoS(entry.th32ProcessID, enableEcoQoS);
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
}

void PowerManager::HandleWindowMinimize(ICoreWebView2Controller* controller, ICoreWebView2* webView, bool isPlayingAudio) {
    if (!controller || !webView) return;

    UINT32 browserPid = 0;
    webView->get_BrowserProcessId(&browserPid);

    if (isPlayingAudio) {
        // Backstage audio playback:
        // NEVER call TrySuspend() as it halts the renderer process and kills the audio stream!
        // Cull DirectComposition / GPU frame rasterization via put_IsVisible(FALSE).
        controller->put_IsVisible(FALSE);
        m_isAudioPlaybackBackgrounded = true;

        // Keep audio rendering threads un-throttled at standard priority to prevent crackling / buffer underrun
        DisableEcoQoS(GetCurrentProcess());
        if (browserPid != 0) {
            HANDLE hBrowser = OpenProcess(PROCESS_SET_INFORMATION, FALSE, browserPid);
            if (hBrowser) {
                DisableEcoQoS(hBrowser);
                CloseHandle(hBrowser);
            }
        }
    } else {
        // Inactive non-audio background:
        if (!m_isSuspended) {
            wil::com_ptr<ICoreWebView2_3> webView3;
            if (SUCCEEDED(webView->QueryInterface(IID_PPV_ARGS(&webView3))) && webView3) {
                m_isSuspended = true;
                webView3->TrySuspend(
                    Microsoft::WRL::Callback<ICoreWebView2TrySuspendCompletedHandler>(
                        [this](HRESULT hr, BOOL isSuccessful) -> HRESULT {
                            if (SUCCEEDED(hr) && isSuccessful) {
                                this->TrimWorkingSet();
                            }
                            return S_OK;
                        }
                    ).Get()
                );
            } else {
                TrimWorkingSet();
            }

            // Enforce Windows 11 EcoQoS on child renderer processes (scheduled on E-Cores)
            if (browserPid != 0) {
                SetProcessTreeEcoQoS(browserPid, true);
            }
            EnableEcoQoS(GetCurrentProcess());
        }
    }
}

void PowerManager::HandleWindowRestore(ICoreWebView2Controller* controller, ICoreWebView2* webView) {
    if (!controller || !webView) return;

    UINT32 browserPid = 0;
    webView->get_BrowserProcessId(&browserPid);

    // 1. Restore visibility (DirectComposition rasterization resume)
    controller->put_IsVisible(TRUE);
    m_isAudioPlaybackBackgrounded = false;

    // 2. Resume renderer if suspended
    if (m_isSuspended) {
        wil::com_ptr<ICoreWebView2_3> webView3;
        if (SUCCEEDED(webView->QueryInterface(IID_PPV_ARGS(&webView3))) && webView3) {
            webView3->Resume();
        }
        m_isSuspended = false;
    }

    // 3. Remove EcoQoS and return to normal/high performance scheduling on P-Cores
    DisableEcoQoS(GetCurrentProcess());
    if (browserPid != 0) {
        SetProcessTreeEcoQoS(browserPid, false);
    }
}

void PowerManager::HandleInactivitySuspend(ICoreWebView2Controller* controller, ICoreWebView2* webView, bool isPlayingAudio) {
    HandleWindowMinimize(controller, webView, isPlayingAudio);
}

void PowerManager::HandleActivityResume(ICoreWebView2Controller* controller, ICoreWebView2* webView) {
    HandleWindowRestore(controller, webView);
}

void PowerManager::TrimWorkingSet() {
    // Release physical memory back to system down to minimal working set
    SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));
}

} // namespace UltraLight
