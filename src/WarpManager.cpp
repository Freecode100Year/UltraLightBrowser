#include <winsock2.h>
#include "WarpManager.hpp"
#include "AppShell.hpp"
#include "Config.hpp"
#include "StringUtils.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#pragma comment(lib, "ws2_32.lib")

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

} // namespace

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

void WarpManager::Start() {
    const auto& settings = Config::Instance().GetSettings();
    if (!settings.warpEnabled) {
        m_state = State::Off;
        return;
    }
    const std::filesystem::path folder = WarpFolder();
    const std::filesystem::path portFile = folder / "port";

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
                    if (!m_stopping && !m_process && !PortOpen(port) && Launch(port)) {
                        std::ofstream(WarpFolder() / "port", std::ios::trunc) << port;
                    }
                }
            }).detach();
            return;
        }
    }

    const std::wstring exe = (folder / "ulb-warp.exe").wstring();
    if (!Extract(exe)) {
        m_state = State::Unavailable;
        return;
    }
    m_portEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_state = State::Starting;
    if (!Launch(0)) {
        m_state = State::Unavailable;
        return;
    }
    // The proxy listens almost at once; the tunnel itself comes up in the background
    // and connections wait for it (or go direct if it cannot be reached).
    if (m_portEvent) WaitForSingleObject(m_portEvent, 3000);
    if (m_port == 0) {
        Stop();
        m_state = State::Unavailable;
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

    std::wstring cmd = L"\"" + exe + L"\" -state \"" + (folder / "account.json").wstring() +
                       L"\" -listen 127.0.0.1:" + std::to_wstring(port) + L" -parent " + std::to_wstring(GetCurrentProcessId());
    if (Config::Instance().GetSettings().warpFailClosed) cmd += L" -fail-closed";

    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;
    si.hStdInput = cmdRead;
    PROCESS_INFORMATION pi{};
    const BOOL started = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, folder.wstring().c_str(), &si, &pi);
    CloseHandle(writePipe);
    CloseHandle(cmdRead);
    if (!started) {
        CloseHandle(readPipe);
        CloseHandle(cmdWrite);
        return false;
    }
    AcquireSRWLockExclusive(&m_lock);
    if (m_stdin) CloseHandle(m_stdin);
    m_stdin = cmdWrite;
    ReleaseSRWLockExclusive(&m_lock);
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
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    if (m_process) CloseHandle(m_process);
    m_process = pi.hProcess;
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
            if (line.rfind("PORT ", 0) == 0) {
                m_port = std::atoi(line.c_str() + 5);
                if (m_portEvent) SetEvent(m_portEvent);
            } else if (line == "STATE up") {
                m_state = State::Up;
            } else if (line == "STATE down") {
                m_state = State::Down;
            } else if (line == "SCAN start") {
                m_scanning = true;
            } else if (line == "SCAN end" || line == "SCAN none") {
                m_scanning = false;
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
    if (m_job) {
        CloseHandle(m_job);  // kills the helper
        m_job = nullptr;
    } else if (m_process) {
        TerminateProcess(m_process, 0);
    }
    if (m_process) {
        WaitForSingleObject(m_process, 1000);
        CloseHandle(m_process);
        m_process = nullptr;
    }
}

std::wstring WarpManager::BrowserArguments() const {
    const int port = m_port.load();
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

void WarpManager::Rescan() {
    AcquireSRWLockShared(&m_lock);
    HANDLE pipe = m_stdin;
    ReleaseSRWLockShared(&m_lock);
    if (!pipe) return;
    m_scanning = true;
    static const char kCommand[] = "RESCAN\n";
    DWORD written = 0;
    WriteFile(pipe, kCommand, sizeof(kCommand) - 1, &written, nullptr);
}

std::wstring WarpManager::StatusText() const {
    switch (m_state.load()) {
    case State::Off: return L"已关闭";
    case State::Starting: return L"正在连接…";
    case State::Up: return L"已连接";
    case State::Down: return Config::Instance().GetSettings().warpFailClosed ? L"未连接（已阻止联网）" : L"未连接（直接连接）";
    case State::Unavailable: return L"不可用（直接连接）";
    case State::Shared: return L"已开启（由另一个浏览器进程运行）";
    }
    return {};
}

} // namespace UltraLight
