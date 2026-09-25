#include "PowerManager.hpp"
#include <tlhelp32.h>
#include <psapi.h>
#include <iostream>

namespace UltraLight {

PowerManager& PowerManager::Instance() {
    static PowerManager s_instance;
    return s_instance;
}

void PowerManager::DisableEcoQoS(HANDLE hProcess) {
    // Disable Windows 11 EcoQoS (Power Throttling)
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

void PowerManager::OptimizeProcessTree(DWORD parentPid) {
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
                    DisableEcoQoS(hChild);
                    CloseHandle(hChild);
                }
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
}

void PowerManager::HandleWindowMinimize(ICoreWebView2* webView) {
    if (!webView || m_isSuspended) return;

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
}

void PowerManager::HandleWindowRestore(ICoreWebView2* webView) {
    if (!webView || !m_isSuspended) return;

    wil::com_ptr<ICoreWebView2_3> webView3;
    if (SUCCEEDED(webView->QueryInterface(IID_PPV_ARGS(&webView3))) && webView3) {
        webView3->Resume();
        m_isSuspended = false;
    }
}

void PowerManager::HandleInactivitySuspend(ICoreWebView2* webView) {
    HandleWindowMinimize(webView);
}

void PowerManager::HandleActivityResume(ICoreWebView2* webView) {
    HandleWindowRestore(webView);
}

void PowerManager::TrimWorkingSet() {
    // Release physical memory back to system down to ~20MB
    SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));
}

} // namespace UltraLight
