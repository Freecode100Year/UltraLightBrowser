#pragma once

#include <windows.h>
#include <functional>

namespace UltraLight {

// Follows the default audio output device (headphones plugged in, Bluetooth
// headset connected, output switched in Windows). Core Audio reports changes on
// its own thread; they are handed to the UI thread through a message-only window
// and coalesced for 250 ms, so a burst of notifications triggers one callback.
class AudioEndpointWatcher {
public:
    static AudioEndpointWatcher& Instance();

    // Call on the UI thread after COM is initialised. onChanged runs on the UI thread.
    void Start(HINSTANCE instance, std::function<void()> onChanged);
    void Stop();

private:
    AudioEndpointWatcher() = default;
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    HWND m_hwnd = nullptr;
    struct Client;
    Client* m_client = nullptr;
    void* m_enumerator = nullptr;  // IMMDeviceEnumerator*
    std::function<void()> m_onChanged;
};

} // namespace UltraLight
