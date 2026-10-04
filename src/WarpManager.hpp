#pragma once

// Built-in Cloudflare WARP. The bundled helper (warp/, Go) registers a free WARP
// device, runs a userspace WireGuard tunnel and serves a loopback SOCKS5 proxy;
// the browser environment is created with that proxy, so every request (and its
// DNS lookup) goes through Cloudflare, IPv6 first.

#include <windows.h>
#include <atomic>
#include <mutex>
#include <string>

namespace UltraLight {

class WarpManager {
public:
    enum class State { Off, Starting, Up, Down, Unavailable, Shared };  // Shared: helper run by another browser process

    static WarpManager& Instance();

    // Starts the helper (or joins the one of another running copy) and waits up to
    // a few seconds for its proxy port. Call before the WebView2 environment exists.
    void Start();
    void Stop();

    // Extra browser arguments: the proxy. With "阻止联网" on and no helper, a proxy
    // port that refuses every connection, so nothing goes direct.
    std::wstring BrowserArguments() const;

    State GetState() const { return m_state.load(); }
    std::wstring StatusText() const;
    // Endpoint in use, e.g. "188.114.97.22:7152（13 ms）"; empty before it is known.
    std::wstring EndpointText() const;
    bool Scanning() const { return m_scanning.load(); }
    // Optimise the WARP endpoint now ("重新优选 IP").
    bool Rescan();  // false when this process does not run the helper

private:
    WarpManager() = default;
    bool Extract(const std::wstring& exePath);
    bool Launch(int port);  // 0 = any free port
    void ReadOutput(HANDLE pipe);
    void BlockIfFailClosed();

    // m_process, m_job and m_stdin are replaced by the reader thread when the helper
    // restarts and released by Stop() on the UI thread.
    mutable std::mutex m_procMutex;
    HANDLE m_process = nullptr;
    HANDLE m_job = nullptr;
    HANDLE m_stdin = nullptr;
    std::atomic<bool> m_stopping{false};
    std::atomic<bool> m_scanning{false};
    mutable SRWLOCK m_lock = SRWLOCK_INIT;  // m_endpoint
    UINT_PTR m_blockSocket = ~UINT_PTR(0);  // bound, never listening: refuses connections
    std::atomic<int> m_blockPort{0};
    std::wstring m_endpoint;
    HANDLE m_portEvent = nullptr;
    std::atomic<int> m_port{0};
    int m_quickExits = 0;  // helper exits soon after start, in a row (reader thread only)
    std::atomic<State> m_state{State::Off};
};

} // namespace UltraLight
