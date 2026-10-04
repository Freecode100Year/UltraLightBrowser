#pragma once

// Built-in Cloudflare WARP. The bundled helper (warp/, Go) registers a free WARP
// device, runs a userspace WireGuard tunnel and serves a loopback SOCKS5 proxy;
// the browser environment is created with that proxy, so every request (and its
// DNS lookup) goes through Cloudflare, IPv6 first.

#include <windows.h>
#include <atomic>
#include <string>

namespace UltraLight {

class WarpManager {
public:
    enum class State { Off, Starting, Up, Down, Unavailable };

    static WarpManager& Instance();

    // Starts the helper (or joins the one of another running copy) and waits up to
    // a few seconds for its proxy port. Call before the WebView2 environment exists.
    void Start();
    void Stop();

    // Extra browser arguments: the proxy and a WebRTC policy that keeps WebRTC from
    // sending UDP outside the proxy (which would reveal the real address).
    std::wstring BrowserArguments() const;

    State GetState() const { return m_state.load(); }
    std::wstring StatusText() const;

private:
    WarpManager() = default;
    bool Extract(const std::wstring& exePath);
    bool Launch(int port);  // 0 = any free port
    void ReadOutput(HANDLE pipe);

    HANDLE m_process = nullptr;
    HANDLE m_job = nullptr;
    std::atomic<bool> m_stopping{false};
    HANDLE m_portEvent = nullptr;
    std::atomic<int> m_port{0};
    std::atomic<State> m_state{State::Off};
};

} // namespace UltraLight
