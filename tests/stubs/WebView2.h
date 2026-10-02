#pragma once
#include <windows.h>
#include <functional>
struct ICoreWebView2TrySuspendCompletedHandler {
    virtual ~ICoreWebView2TrySuspendCompletedHandler() = default;
    virtual HRESULT Invoke(HRESULT, BOOL) = 0;
};
constexpr int COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_LOW = 0;
constexpr int COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_NORMAL = 1;
struct ICoreWebView2_3 {
    HRESULT suspendResult = S_OK;
    HRESULT completionResult = S_OK;
    BOOL successful = TRUE;
    int suspends = 0, resumes = 0;
    std::function<void()> beforeCompletion;
    HRESULT TrySuspend(ICoreWebView2TrySuspendCompletedHandler* cb) {
        ++suspends;
        if (suspendResult < 0) return suspendResult;
        if (beforeCompletion) beforeCompletion();
        cb->Invoke(completionResult, successful);
        return suspendResult;
    }
    HRESULT Resume() { ++resumes; return S_OK; }
};
struct ICoreWebView2_19 {
    int target = COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_NORMAL;
    HRESULT put_MemoryUsageTargetLevel(int level) { target = level; return S_OK; }
};
struct ICoreWebView2Controller {
    BOOL visible = TRUE;
    HRESULT put_IsVisible(BOOL value) { visible = value; return S_OK; }
};
struct ICoreWebView2 {
    ICoreWebView2_3 lifecycle;
    ICoreWebView2_19 memory;
    HRESULT get_BrowserProcessId(UINT32* pid) { *pid = 10; return S_OK; }
    HRESULT QueryInterface(ICoreWebView2_3** value) { *value = &lifecycle; return S_OK; }
    HRESULT QueryInterface(ICoreWebView2_19** value) { *value = &memory; return S_OK; }
};
