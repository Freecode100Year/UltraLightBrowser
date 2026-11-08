#include "AudioEndpointWatcher.hpp"

#include <mmdeviceapi.h>
#include <atomic>

namespace UltraLight {

namespace {
constexpr UINT kMsgEndpointChanged = WM_APP + 0x41;
constexpr UINT_PTR kDebounceTimer = 1;
constexpr UINT kDebounceMs = 250;
const wchar_t kClassName[] = L"UltraLightAudioEndpointWatcher";
}

// Runs on Core Audio's notification thread: only posts a message.
struct AudioEndpointWatcher::Client final : IMMNotificationClient {
    explicit Client(HWND target) : hwnd(target) {}

    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = --refs;
        if (left == 0) delete this;
        return left;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
            *out = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (flow == eRender && role == eMultimedia) Notify();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { Notify(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

    void Notify() {
        if (HWND target = hwnd.load()) PostMessageW(target, kMsgEndpointChanged, 0, 0);
    }

    std::atomic<HWND> hwnd;
    std::atomic<ULONG> refs{1};
};

AudioEndpointWatcher& AudioEndpointWatcher::Instance() {
    static AudioEndpointWatcher s_instance;
    return s_instance;
}

void AudioEndpointWatcher::Start(HINSTANCE instance, std::function<void()> onChanged) {
    if (m_hwnd) return;
    m_onChanged = std::move(onChanged);

    WNDCLASSW wc{};
    wc.lpfnWndProc = &AudioEndpointWatcher::WndProc;
    wc.hInstance = instance;
    wc.lpszClassName = kClassName;
    RegisterClassW(&wc);
    m_hwnd = CreateWindowExW(0, kClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
    if (!m_hwnd) return;

    IMMDeviceEnumerator* enumerator = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&enumerator))) || !enumerator) {
        return;
    }
    auto* client = new Client(m_hwnd);
    if (FAILED(enumerator->RegisterEndpointNotificationCallback(client))) {
        client->Release();
        enumerator->Release();
        return;
    }
    m_client = client;
    m_enumerator = enumerator;
}

void AudioEndpointWatcher::Stop() {
    if (m_client) {
        m_client->hwnd.store(nullptr);
        if (m_enumerator) static_cast<IMMDeviceEnumerator*>(m_enumerator)->UnregisterEndpointNotificationCallback(m_client);
        m_client->Release();
        m_client = nullptr;
    }
    if (m_enumerator) {
        static_cast<IMMDeviceEnumerator*>(m_enumerator)->Release();
        m_enumerator = nullptr;
    }
    if (m_hwnd) {
        KillTimer(m_hwnd, kDebounceTimer);
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
    m_onChanged = nullptr;
}

LRESULT CALLBACK AudioEndpointWatcher::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == kMsgEndpointChanged) {
        SetTimer(hwnd, kDebounceTimer, kDebounceMs, nullptr);  // restarts the window
        return 0;
    }
    if (msg == WM_TIMER && wParam == kDebounceTimer) {
        KillTimer(hwnd, kDebounceTimer);
        auto& self = Instance();
        if (self.m_hwnd == hwnd && self.m_onChanged) self.m_onChanged();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace UltraLight
