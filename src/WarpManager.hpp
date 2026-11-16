#pragma once

// Built-in Cloudflare WARP. The bundled helper (warp/, Go) registers a free WARP
// device, runs a userspace WireGuard tunnel and serves a loopback SOCKS5 proxy;
// the browser environment is created with that proxy, so every request (and its
// DNS lookup) goes through Cloudflare, IPv6 first.
//
// Instead of WARP the same helper can run a line: one of the user's lines (share
// links kept DPAPI-encrypted in warp\lines.dat, see LineStore), the fastest of
// them ("自动选择"), or else the built-in public line. A line never falls back to
// direct connections.

#include <windows.h>
#include "LineStore.hpp"
#include <atomic>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

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
    bool Rescan();  // false when this process does not run the helper (or runs a line)

    // The user's lines. Changes take effect when the browser restarts.
    static LineStore LoadStore();
    static bool SaveStore(const LineStore& store);
    bool LineMode() const { return m_lineMode; }
    int Port() const { return m_port.load(); }  // the running helper's proxy, 0 if none

    // Runs the helper in one of its one-shot modes (-probe, -fetch) and returns
    // what it printed; vars are added to its environment. Blocks: call it from a
    // worker thread.
    static std::string RunTool(const std::wstring& args, const std::vector<std::pair<std::wstring, std::wstring>>& vars, DWORD timeoutMs);

private:
    WarpManager() = default;
    bool Extract(const std::wstring& exePath);
    bool Launch(int port);  // 0 = any free port
    void ReadOutput(HANDLE pipe);
    void BlockIfFailClosed();
    static std::vector<std::string> LoadLinks();  // selected line(s); the public line if none

    // m_process, m_job and m_stdin are replaced by the reader thread when the helper
    // restarts and released by Stop() on the UI thread.
    mutable std::mutex m_procMutex;
    HANDLE m_process = nullptr;
    HANDLE m_job = nullptr;
    HANDLE m_stdin = nullptr;
    std::atomic<bool> m_stopping{false};
    std::atomic<bool> m_scanning{false};
    // Why the line is down, from the helper's REASON line: 0 unknown, 1 no IPv6,
    // 2 server unreachable, 3 server answers but the tunnel fails.
    std::atomic<int> m_reason{0};
    mutable SRWLOCK m_lock = SRWLOCK_INIT;  // m_endpoint
    UINT_PTR m_blockSocket = ~UINT_PTR(0);  // bound, never listening: refuses connections
    std::atomic<int> m_blockPort{0};
    std::wstring m_endpoint;
    HANDLE m_portEvent = nullptr;
    std::atomic<int> m_port{0};
    bool m_lineMode = false;
    std::vector<std::string> m_links;  // in line mode: one link, or several for automatic selection
    int m_quickExits = 0;  // helper exits soon after start, in a row (reader thread only)
    std::atomic<State> m_state{State::Off};
};

} // namespace UltraLight
