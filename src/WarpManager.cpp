#include <winsock2.h>
#include "WarpManager.hpp"
#include "AppShell.hpp"
#include "Config.hpp"
#include "StringUtils.hpp"

#include <wincrypt.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "crypt32.lib")

namespace UltraLight {

namespace {

constexpr int kHelperResource = 4001;

std::filesystem::path WarpFolder() {
    return Config::Instance().GetAppDataPath() / "warp";
}

std::string LoadHelperResource() {
    HMODULE module = GetModuleHandleW(nullptr);
    HRSRC res = FindResourceW(module, MAKEINTRESOURCEW(kHelperResource), RT_RCDATA);
    if (!res) return {};
    HGLOBAL data = LoadResource(module, res);
    const DWORD size = SizeofResource(module, res);
    const void* bytes = data ? LockResource(data) : nullptr;
    return bytes && size ? std::string(static_cast<const char*>(bytes), size) : std::string();
}

// Binds a loopback port without listening on it: connections to it are refused,
// and no other program can take the port while the socket stays open.
int ReserveRefusingPort(UINT_PTR& socketOut) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 0;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return 0;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int len = sizeof(addr);
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        closesocket(s);
        return 0;
    }
    socketOut = s;
    return ntohs(addr.sin_port);
}

bool PortOpen(int port) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    bool open = false;
    if (s != INVALID_SOCKET) {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<u_short>(port));
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        open = connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
        closesocket(s);
    }
    WSACleanup();
    return open;
}

std::filesystem::path LineFile() {
    return WarpFolder() / "line.dat";
}

// "host:port" of a vless://uuid@host:port?... link (IPv6 hosts without brackets).
std::string ServerOf(const std::string& link) {
    const size_t at = link.find('@');
    if (at == std::string::npos) return {};
    std::string rest = link.substr(at + 1, link.find_first_of("?#/", at + 1) - at - 1);
    if (!rest.empty() && rest[0] == '[') {
        const size_t close = rest.find(']');
        if (close == std::string::npos) return {};
        return rest.substr(1, close - 1) + rest.substr(close + 1);
    }
    return rest;
}

// The built-in public line, used while no link is imported. Assembled from
// parts so scrapers that collect share links from GitHub pass it over.
std::string PublicLine() {
    return std::string("vl") + "ess://" + "5680cee3-6b91-4739-86b8-5aded88cd227" + "@[" + "2001:470:1f07:5f5:3bcd:d73f:3e79:6b3e" + "]:443" +
           "?encryption=none&flow=xtls-rprx-vision&security=reality&sni=www.nvidia.com&fp=chrome" +
           "&pbk=" + "89rnPvfXmhlVxTRQot_US6EV7GWZLaNgTK2aDKINiW4" + "&sid=" + "14702eaec705a52e" + "&type=tcp";
}

} // namespace

bool WarpManager::SetLine(const std::string& input) {
    std::string link = input;
    while (!link.empty() && isspace(static_cast<unsigned char>(link.back()))) link.pop_back();
    while (!link.empty() && isspace(static_cast<unsigned char>(link.front()))) link.erase(0, 1);
    if (link.rfind("vless://", 0) != 0 || link.find("security=reality") == std::string::npos ||
        link.find("pbk=") == std::string::npos || ServerOf(link).empty() || link.size() > 4096) {
        return false;
    }
    DATA_BLOB in{static_cast<DWORD>(link.size()), reinterpret_cast<BYTE*>(link.data())};
    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"UltraLightBrowser line", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return false;
    std::error_code ec;
    std::filesystem::create_directories(WarpFolder(), ec);
    const std::filesystem::path tmp = LineFile().wstring() + L".tmp";
    bool ok;
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(out.pbData), out.cbData);
        ok = static_cast<bool>(f);
    }
    LocalFree(out.pbData);
    return ok && MoveFileExW(tmp.c_str(), LineFile().c_str(), MOVEFILE_REPLACE_EXISTING);
}

void WarpManager::ClearLine() {
    std::error_code ec;
    std::filesystem::remove(LineFile(), ec);
}

std::string WarpManager::LoadLine() {
    std::string link = LoadImported();
    return link.empty() ? PublicLine() : link;
}

bool WarpManager::LineImported() {
    return !LoadImported().empty();
}

std::string WarpManager::LoadImported() {
    std::ifstream f(LineFile(), std::ios::binary);
    std::string blob((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (blob.empty()) return {};
    DATA_BLOB in{static_cast<DWORD>(blob.size()), reinterpret_cast<BYTE*>(blob.data())};
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return {};
    std::string link(reinterpret_cast<const char*>(out.pbData), out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return link;
}

WarpManager& WarpManager::Instance() {
    static WarpManager s_instance;
    return s_instance;
}

bool WarpManager::Extract(const std::wstring& exePath) {
    const std::string helper = LoadHelperResource();
    if (helper.empty()) return false;
    std::error_code ec;
    if (std::filesystem::file_size(exePath, ec) == helper.size() && !ec) {
        std::ifstream in(exePath, std::ios::binary);
        std::string existing((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (existing == helper) return true;
    }
    std::filesystem::create_directories(std::filesystem::path(exePath).parent_path(), ec);
    const std::wstring tmp = exePath + L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) return false;
        out.write(helper.data(), static_cast<std::streamsize>(helper.size()));
        if (!out) return false;
    }
    return MoveFileExW(tmp.c_str(), exePath.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
}

void WarpManager::BlockIfFailClosed() {
    // "WARP 断开时阻止联网" must also hold when the helper cannot run at all.
    // A private line never goes direct.
    if ((!Config::Instance().GetSettings().warpFailClosed && !m_lineMode) || m_blockPort != 0) return;
    m_blockPort = ReserveRefusingPort(m_blockSocket);
}

void WarpManager::Start() {
    const auto& settings = Config::Instance().GetSettings();
    if (!settings.warpEnabled) {
        m_state = State::Off;
        return;
    }
    const std::filesystem::path folder = WarpFolder();
    const std::filesystem::path portFile = folder / "port";
    m_lineMode = settings.useLine;

    // A second copy of the browser shares the profile and must use the same
    // browser arguments, so it joins the helper that is already running.
    if (AppShell::AnotherInstanceRunning()) {
        std::ifstream in(portFile);
        int port = 0;
        if (in >> port && port > 0 && port < 65536) {
            m_port = port;
            m_state = State::Shared;
            // If the first copy exits, its helper goes with it: take over the port.
            std::thread([this, port]() {
                while (!m_stopping) {
                    Sleep(3000);
                    bool running;
                    {
                        std::lock_guard<std::mutex> lock(m_procMutex);
                        running = m_process != nullptr;
                    }
                    if (!m_stopping && !running && !PortOpen(port) && Launch(port)) {
                        std::ofstream(WarpFolder() / "port", std::ios::trunc) << port;
                    }
                }
            }).detach();
            return;
        }
    }

    if (m_lineMode) {
        m_link = LoadLine();
        if (m_link.empty()) {
            m_state = State::Unavailable;
            BlockIfFailClosed();
            return;
        }
    }
    const std::wstring exe = (folder / "ulb-warp.exe").wstring();
    if (!Extract(exe)) {
        m_state = State::Unavailable;
        BlockIfFailClosed();
        return;
    }
    m_portEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_state = State::Starting;
    if (!Launch(0)) {
        m_state = State::Unavailable;
        BlockIfFailClosed();
        return;
    }
    // The proxy listens almost at once (the first start of a freshly extracted helper
    // can take longer while antivirus scans it); the tunnel itself comes up in the
    // background and connections wait for it (or go direct if it cannot be reached).
    if (m_portEvent) WaitForSingleObject(m_portEvent, 8000);
    if (m_port == 0) {
        Stop();
        m_port = 0;
        m_state = State::Unavailable;
        BlockIfFailClosed();
        return;
    }
    std::ofstream(portFile, std::ios::trunc) << m_port.load();
}

bool WarpManager::Launch(int port) {
    const std::filesystem::path folder = WarpFolder();
    const std::wstring exe = (folder / "ulb-warp.exe").wstring();
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE readPipe = nullptr, writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) return false;
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
    HANDLE cmdRead = nullptr, cmdWrite = nullptr;  // control lines to the helper
    if (!CreatePipe(&cmdRead, &cmdWrite, &sa, 0)) {
        CloseHandle(readPipe);
        CloseHandle(writePipe);
        return false;
    }
    SetHandleInformation(cmdWrite, HANDLE_FLAG_INHERIT, 0);

    std::wstring cmd = L"\"" + exe + L"\"";
    if (!m_lineMode) cmd += L" -state \"" + (folder / "account.json").wstring() + L"\"";
    cmd += L" -listen 127.0.0.1:" + std::to_wstring(port) + L" -parent " + std::to_wstring(GetCurrentProcessId());
    if (Config::Instance().GetSettings().warpFailClosed) cmd += L" -fail-closed";

    // The link goes to the helper in its environment, not on the command line.
    std::wstring env;
    if (m_lineMode) {
        if (wchar_t* block = GetEnvironmentStringsW()) {
            for (const wchar_t* p = block; *p; p += wcslen(p) + 1) {
                if (_wcsnicmp(p, L"ULB_LINE=", 9) != 0) env.append(p).push_back(L'\0');
            }
            FreeEnvironmentStringsW(block);
        }
        env += L"ULB_LINE=" + StringUtils::Utf8ToWide(m_link);
        env.push_back(L'\0');
        env.push_back(L'\0');
    }

    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;
    si.hStdInput = cmdRead;
    PROCESS_INFORMATION pi{};
    const BOOL started = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
                                        env.empty() ? nullptr : env.data(), folder.wstring().c_str(), &si, &pi);
    if (!env.empty()) SecureZeroMemory(env.data(), env.size() * sizeof(wchar_t));
    CloseHandle(writePipe);
    CloseHandle(cmdRead);
    if (!started) {
        CloseHandle(readPipe);
        CloseHandle(cmdWrite);
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(m_procMutex);
        if (m_stopping) {
            // Stop() already ran: this helper must not outlive the browser.
            TerminateProcess(pi.hProcess, 0);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            CloseHandle(readPipe);
            CloseHandle(cmdWrite);
            return false;
        }
        if (m_stdin) CloseHandle(m_stdin);
        m_stdin = cmdWrite;
        // The helper dies with the browser, even if the browser crashes.
        if (!m_job) {
            m_job = CreateJobObjectW(nullptr, nullptr);
            if (m_job) {
                JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
                limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                SetInformationJobObject(m_job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
            }
        }
        if (m_job) AssignProcessToJobObject(m_job, pi.hProcess);
        if (m_process) CloseHandle(m_process);
        m_process = pi.hProcess;
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    std::thread([this, readPipe]() { ReadOutput(readPipe); }).detach();
    return true;
}

void WarpManager::ReadOutput(HANDLE pipe) {
    const ULONGLONG started = GetTickCount64();
    std::string pending;
    char buf[512];
    DWORD read = 0;
    while (ReadFile(pipe, buf, sizeof(buf), &read, nullptr) && read > 0) {
        pending.append(buf, read);
        size_t nl;
        while ((nl = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, nl);
            pending.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.rfind("PORT ", 0) == 0 && !m_stopping) {
                m_port = std::atoi(line.c_str() + 5);
                if (m_portEvent) SetEvent(m_portEvent);
            } else if (line == "STATE up") {
                m_reason = 0;
                m_state = State::Up;
            } else if (line == "STATE down") {
                m_state = State::Down;
            } else if (line.rfind("REASON ", 0) == 0) {
                const std::string why = line.substr(7);
                m_reason = why == "noipv6" ? 1 : why == "unreachable" ? 2 : why == "handshake" ? 3 : 0;
            } else if (line == "SCAN start") {
                m_scanning = true;
            } else if (line == "SCAN end" || line == "SCAN none") {
                m_scanning = false;
            } else if (line.rfind("LINE ", 0) == 0) {
                AcquireSRWLockExclusive(&m_lock);
                m_endpoint = StringUtils::Utf8ToWide(line.substr(5));
                ReleaseSRWLockExclusive(&m_lock);
            } else if (line.rfind("ENDPOINT ", 0) == 0) {
                // "ENDPOINT <ip:port> <rtt ms>"
                const std::string rest = line.substr(9);
                const size_t space = rest.rfind(' ');
                std::wstring text = StringUtils::Utf8ToWide(rest.substr(0, space));
                const int ms = space == std::string::npos ? 0 : std::atoi(rest.c_str() + space + 1);
                if (ms > 0) text += L"（" + std::to_wstring(ms) + L" ms）";
                AcquireSRWLockExclusive(&m_lock);
                m_endpoint = std::move(text);
                ReleaseSRWLockExclusive(&m_lock);
            }
        }
    }
    CloseHandle(pipe);
    if (m_portEvent) SetEvent(m_portEvent);
    if (m_stopping) return;
    // The browser keeps using this proxy port, so a helper that died is restarted
    // on the same port; until then requests fail rather than leave the proxy.
    m_state = State::Down;
    const int port = m_port.load();
    if (port == 0) return;
    // A helper that keeps exiting at once (e.g. another program took the port) is
    // retried with growing pauses instead of every second.
    m_quickExits = GetTickCount64() - started < 10000 ? (std::min)(m_quickExits + 1, 30) : 0;
    for (DWORD waited = 0, pause = 1000u * (1 + m_quickExits); waited < pause && !m_stopping; waited += 250) Sleep(250);
    while (!m_stopping && !Launch(port)) Sleep(5000);
}

void WarpManager::Stop() {
    m_stopping = true;
    // Another copy of the browser may still be using this helper (or its own on the
    // same port); the port file stays for it and for copies started later.
    std::error_code ec;
    if (!AppShell::AnotherInstanceRunning()) std::filesystem::remove(WarpFolder() / "port", ec);
    HANDLE process;
    {
        std::lock_guard<std::mutex> lock(m_procMutex);
        if (m_job) {
            CloseHandle(m_job);  // kills the helper
            m_job = nullptr;
        } else if (m_process) {
            TerminateProcess(m_process, 0);
        }
        process = m_process;
        m_process = nullptr;
        if (m_stdin) CloseHandle(m_stdin);
        m_stdin = nullptr;
    }
    if (process) {
        WaitForSingleObject(process, 1000);
        CloseHandle(process);
    }
}

std::wstring WarpManager::BrowserArguments() const {
    int port = m_port.load();
    if (port == 0) port = m_blockPort.load();
    if (port == 0) return {};
    // WebRTC is kept inside the proxy by a profile preference (see CreateEnvironment);
    // WebView2 ignores Chrome's --force-webrtc-ip-handling-policy switch.
    return L" --proxy-server=socks5://127.0.0.1:" + std::to_wstring(port);
}

std::wstring WarpManager::EndpointText() const {
    AcquireSRWLockShared(&m_lock);
    std::wstring text = m_endpoint;
    ReleaseSRWLockShared(&m_lock);
    return text;
}

bool WarpManager::Rescan() {
    static const char kCommand[] = "RESCAN\n";
    if (m_lineMode) return false;
    DWORD written = 0;
    {
        // Held while writing so a restart cannot close the pipe underneath.
        std::lock_guard<std::mutex> lock(m_procMutex);
        if (!m_stdin || !WriteFile(m_stdin, kCommand, sizeof(kCommand) - 1, &written, nullptr)) return false;
    }
    m_scanning = true;
    return true;
}

std::wstring WarpManager::StatusText() const {
    switch (m_state.load()) {
    case State::Off: return L"已关闭";
    case State::Starting: return L"正在连接…";
    case State::Up: return L"已连接";
    case State::Down: {
        std::wstring text = Config::Instance().GetSettings().warpFailClosed || m_lineMode ? L"未连接（已阻止联网）" : L"未连接（直接连接）";
        switch (m_reason.load()) {
        case 1: text += L"：本机没有 IPv6 网络，这条线路需要 IPv6"; break;
        case 2: text += L"：连不到入口服务器"; break;
        case 3: text += L"：入口服务器拒绝了连接，链接可能已失效"; break;
        }
        return text;
    }
    case State::Unavailable: return m_blockPort != 0 ? L"不可用（已阻止联网）" : L"不可用（直接连接）";
    case State::Shared: return L"已开启（由另一个浏览器进程运行）";
    }
    return {};
}

} // namespace UltraLight
