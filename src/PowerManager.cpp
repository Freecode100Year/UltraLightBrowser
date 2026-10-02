#include "PowerManager.hpp"
#include <tlhelp32.h>
#include <psapi.h>
#include <unordered_map>
#include <unordered_set>

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

    // Capture the process tree once instead of taking a full system snapshot
    // recursively for every descendant. Iteration also avoids stack exhaustion.
    std::unordered_map<DWORD, std::vector<DWORD>> children;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(PROCESSENTRY32W);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            children[entry.th32ParentProcessID].push_back(entry.th32ProcessID);
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);

    std::vector<DWORD> pending{parentPid};
    std::unordered_set<DWORD> visited{parentPid};
    for (size_t i = 0; i < pending.size(); ++i) {
        const auto it = children.find(pending[i]);
        if (it == children.end()) continue;
        for (DWORD pid : it->second) {
            if (!visited.insert(pid).second) continue;
            pending.push_back(pid);
            HANDLE hChild = OpenProcess(PROCESS_SET_INFORMATION, FALSE, pid);
            if (hChild) {
                if (enableEcoQoS) EnableEcoQoS(hChild);
                else DisableEcoQoS(hChild);
                CloseHandle(hChild);
            }
        }
    }
}

void PowerManager::HandleWindowMinimize(ICoreWebView2Controller* controller, ICoreWebView2* webView, bool isPlayingAudio) {
    if (!controller || !webView) return;

    UINT32 browserPid = 0;
    webView->get_BrowserProcessId(&browserPid);

    if (isPlayingAudio) {
        // A document may start audio while a background suspend is pending.
        // Invalidate that request and undo all renderer throttling first.
        if (m_isSuspended || m_suspendPending) {
            HandleWindowRestore(controller, webView);
        }
        controller->put_IsVisible(FALSE);
        m_isBackgrounded = true;
        m_isAudioPlaybackBackgrounded = true;
        // A rejected suspension still lowered the memory target. Audio must
        // restore it even when no suspend operation remains pending.
        wil::com_ptr<ICoreWebView2_19> webView19;
        if (SUCCEEDED(webView->QueryInterface(IID_PPV_ARGS(&webView19))) && webView19) {
            webView19->put_MemoryUsageTargetLevel(COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_NORMAL);
        }
        DisableEcoQoS(GetCurrentProcess());
        if (browserPid != 0) {
            HANDLE hBrowser = OpenProcess(PROCESS_SET_INFORMATION, FALSE, browserPid);
            if (hBrowser) {
                DisableEcoQoS(hBrowser);
                CloseHandle(hBrowser);
            }
            SetProcessTreeEcoQoS(browserPid, false);
        }
        return;
    }

    controller->put_IsVisible(FALSE);
    m_isBackgrounded = true;
    m_isAudioPlaybackBackgrounded = false;
    if (m_isSuspended || m_suspendPending) return;

    if (browserPid != 0) SetProcessTreeEcoQoS(browserPid, true);
    EnableEcoQoS(GetCurrentProcess());
    wil::com_ptr<ICoreWebView2_19> webView19;
    if (SUCCEEDED(webView->QueryInterface(IID_PPV_ARGS(&webView19))) && webView19) {
        webView19->put_MemoryUsageTargetLevel(COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_LOW);
    }

    wil::com_ptr<ICoreWebView2_3> webView3;
    if (SUCCEEDED(webView->QueryInterface(IID_PPV_ARGS(&webView3))) && webView3) {
        m_suspendPending = true;
        const auto generation = ++m_suspendGeneration;
        const HRESULT hr = webView3->TrySuspend(
            Microsoft::WRL::Callback<ICoreWebView2TrySuspendCompletedHandler>(
                [this, generation, webView3](HRESULT result, BOOL successful) -> HRESULT {
                    if (generation != m_suspendGeneration) {
                        // A restore raced this asynchronous completion. Do not
                        // apply the stale result or trim a now-active window.
                        if ((!m_isBackgrounded || m_isAudioPlaybackBackgrounded) &&
                            SUCCEEDED(result) && successful) {
                            webView3->Resume();
                        }
                        return S_OK;
                    }
                    m_suspendPending = false;
                    m_isSuspended = SUCCEEDED(result) && successful;
                    if (m_isSuspended) TrimWorkingSet();
                    return S_OK;
                }
            ).Get()
        );
        if (FAILED(hr) && generation == m_suspendGeneration) {
            m_suspendPending = false;
            m_isSuspended = false;
        }
    } else {
        TrimWorkingSet();
    }
}

void PowerManager::HandleWindowRestore(ICoreWebView2Controller* controller, ICoreWebView2* webView) {
    if (!controller || !webView) return;

    UINT32 browserPid = 0;
    webView->get_BrowserProcessId(&browserPid);

    // Invalidate pending callbacks before restoring foreground state.
    ++m_suspendGeneration;
    const bool needsResume = m_isSuspended || m_suspendPending;
    m_suspendPending = false;
    m_isBackgrounded = false;

    // 1. Restore visibility (DirectComposition rasterization resume)
    controller->put_IsVisible(TRUE);
    m_isAudioPlaybackBackgrounded = false;

    // 2. Resume renderer if suspended
    if (needsResume) {
        wil::com_ptr<ICoreWebView2_3> webView3;
        if (SUCCEEDED(webView->QueryInterface(IID_PPV_ARGS(&webView3))) && webView3) {
            webView3->Resume();
        }
        m_isSuspended = false;
    }

    // 3. Restore memory target level to NORMAL for responsiveness
    wil::com_ptr<ICoreWebView2_19> webView19;
    if (SUCCEEDED(webView->QueryInterface(IID_PPV_ARGS(&webView19))) && webView19) {
        webView19->put_MemoryUsageTargetLevel(COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_NORMAL);
    }

    // 4. Remove EcoQoS and return to normal/high performance scheduling on P-Cores
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
