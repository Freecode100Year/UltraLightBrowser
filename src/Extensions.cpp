#include "Extensions.hpp"
#include "Config.hpp"
#include "ExtensionPackage.hpp"
#include "StringUtils.hpp"

#include <wrl.h>
#include <nlohmann/json.hpp>

#include <cctype>
#include <fstream>
#include <iterator>
#include <stdexcept>

using json = nlohmann::json;
using namespace Microsoft::WRL;

namespace UltraLight {

namespace {

constexpr std::uintmax_t kMaxBytes = 200ull * 1024 * 1024;
constexpr size_t kMaxFiles = 20000;
constexpr size_t kMaxExtensions = 50;

std::string Hex(HRESULT hr) {
    char buf[16];
    snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
    return buf;
}

// A folder name from the extension's name: ASCII letters and digits only.
std::string Slug(const std::string& name) {
    std::string out;
    for (char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c)) && static_cast<unsigned char>(c) < 0x80) out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        else if (!out.empty() && out.back() != '-') out.push_back('-');
        if (out.size() >= 32) break;
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out.empty() ? "ext" : out;
}

// Size and file count of a folder; throws when it is too big for an extension.
void CheckSize(const std::filesystem::path& dir) {
    std::error_code ec;
    std::uintmax_t bytes = 0;
    size_t files = 0;
    for (auto it = std::filesystem::recursive_directory_iterator(dir, std::filesystem::directory_options::skip_permission_denied, ec);
         it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (it->is_regular_file(ec)) {
            bytes += it->file_size(ec);
            ++files;
        }
        if (bytes > kMaxBytes || files > kMaxFiles) throw std::runtime_error("扩展太大（超过 200 MB 或 2 万个文件）");
    }
}

// Runs Windows' own tar.exe (Windows 10 1803 and later) to unpack a ZIP.
bool UnpackZip(const std::filesystem::path& zip, const std::filesystem::path& dest) {
    wchar_t sys[MAX_PATH];
    if (!GetSystemDirectoryW(sys, MAX_PATH)) return false;
    const std::wstring tar = std::wstring(sys) + L"\\tar.exe";
    std::wstring cmd = L"\"" + tar + L"\" -xf \"" + zip.wstring() + L"\" -C \"" + dest.wstring() + L"\"";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(tar.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;
    CloseHandle(pi.hThread);
    const bool finished = WaitForSingleObject(pi.hProcess, 120000) == WAIT_OBJECT_0;
    DWORD code = 1;
    if (finished) GetExitCodeProcess(pi.hProcess, &code);
    else TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hProcess);
    return finished && code == 0;
}

std::filesystem::path StagingPath(const char* kind) {
    return Extensions::Folder() / (std::string(".staging-") + kind + "-" + std::to_string(GetTickCount64()));
}

} // namespace

Extensions& Extensions::Instance() {
    static Extensions s_instance;
    return s_instance;
}

std::filesystem::path Extensions::Folder() {
    return Config::Instance().GetAppDataPath() / "extensions";
}

void Extensions::LoadIndex() {
    if (m_indexLoaded) return;
    m_indexLoaded = true;
    m_entries.clear();
    std::error_code ec;
    // Leftovers of an interrupted install or of a removal while the folder was in use.
    for (const auto& e : std::filesystem::directory_iterator(Folder(), ec)) {
        const std::string n = e.path().filename().string();
        if (n.rfind(".staging-", 0) == 0 || n.rfind(".trash-", 0) == 0) std::filesystem::remove_all(e.path(), ec);
    }
    std::ifstream in(Folder() / "index.json", std::ios::binary);
    const json root = in.is_open() ? json::parse(in, nullptr, false) : json(nullptr);
    if (!root.is_object() || !root.contains("extensions") || !root["extensions"].is_array()) return;
    for (const auto& j : root["extensions"]) {
        if (!j.is_object()) continue;
        Entry e;
        e.dir = j.value("dir", "");
        if (e.dir.empty() || e.dir.find_first_of("/\\:.") != std::string::npos) continue;
        e.enabled = j.value("enabled", true);
        e.source = j.value("source", "");
        const ExtensionManifest m = ReadExtensionManifest(Folder() / e.dir);
        if (!m.error.empty()) continue;  // folder gone or broken
        e.name = m.name;
        e.version = m.version;
        e.description = m.description;
        e.popup = m.popup;
        e.options = m.options;
        e.proxy = m.proxy;
        m_entries.push_back(std::move(e));
    }
}

bool Extensions::SaveIndex() const {
    json list = json::array();
    for (const auto& e : m_entries) {
        json j = {{"dir", e.dir}, {"enabled", e.enabled}};
        if (!e.source.empty()) j["source"] = e.source;
        list.push_back(std::move(j));
    }
    std::error_code ec;
    std::filesystem::create_directories(Folder(), ec);
    const auto tmp = Folder() / "index.json.tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << json{{"extensions", list}}.dump(1, ' ', false, json::error_handler_t::replace);
        if (!out) return false;
    }
    return MoveFileExW(tmp.c_str(), (Folder() / "index.json").c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
}

std::vector<Extensions::Entry> Extensions::List() const {
    const_cast<Extensions*>(this)->LoadIndex();
    return m_entries;
}

std::string Extensions::InstallFolder(const std::filesystem::path& source) {
    LoadIndex();
    if (!ReadExtensionManifest(source).error.empty()) throw std::runtime_error(ReadExtensionManifest(source).error);
    CheckSize(source);
    const auto staged = StagingPath("dir");
    std::error_code ec;
    std::filesystem::create_directories(staged, ec);
    std::filesystem::copy(source, staged, std::filesystem::copy_options::recursive | std::filesystem::copy_options::skip_symlinks, ec);
    if (ec) {
        std::filesystem::remove_all(staged, ec);
        throw std::runtime_error("复制扩展文件失败");
    }
    return Install(staged);
}

std::string Extensions::InstallPackage(const std::filesystem::path& file, const std::string& source) {
    LoadIndex();
    std::error_code ec;
    if (std::filesystem::file_size(file, ec) > kMaxBytes) throw std::runtime_error("扩展文件超过 200 MB");
    std::ifstream in(file, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const size_t offset = CrxZipOffset(bytes);
    if (offset == std::string::npos) throw std::runtime_error("不是 .crx 或 .zip 扩展包");
    std::filesystem::create_directories(Folder(), ec);
    const auto zip = StagingPath("zip").wstring() + L".zip";
    {
        std::ofstream out(zip, std::ios::binary | std::ios::trunc);
        out.write(bytes.data() + offset, static_cast<std::streamsize>(bytes.size() - offset));
        if (!out) throw std::runtime_error("无法写入临时文件");
    }
    const auto staged = StagingPath("pkg");
    std::filesystem::create_directories(staged, ec);
    const bool ok = UnpackZip(zip, staged);
    std::filesystem::remove(zip, ec);
    if (!ok) {
        std::filesystem::remove_all(staged, ec);
        throw std::runtime_error("解压扩展包失败");
    }
    // A ZIP often holds one top folder with the extension inside.
    std::filesystem::path root = staged;
    if (!std::filesystem::exists(staged / "manifest.json")) {
        std::vector<std::filesystem::path> dirs;
        for (const auto& e : std::filesystem::directory_iterator(staged, ec)) if (e.is_directory(ec)) dirs.push_back(e.path());
        if (dirs.size() == 1 && std::filesystem::exists(dirs[0] / "manifest.json")) root = dirs[0];
    }
    try {
        CheckSize(root);
        if (root == staged) return Install(staged, source);
        const auto inner = StagingPath("dir");
        std::filesystem::rename(root, inner, ec);
        std::filesystem::remove_all(staged, ec);
        if (ec) throw std::runtime_error("整理扩展文件失败");
        return Install(inner, source);
    } catch (...) {
        std::filesystem::remove_all(staged, ec);
        throw;
    }
}

std::string Extensions::Install(const std::filesystem::path& staged, const std::string& source) {
    std::error_code ec;
    const ExtensionManifest m = ReadExtensionManifest(staged);
    if (!m.error.empty()) {
        std::filesystem::remove_all(staged, ec);
        throw std::runtime_error(m.error);
    }
    // The same extension again is an update: the old copy goes.
    for (const auto& e : std::vector<Entry>(m_entries)) {
        if (e.name == m.name || (!source.empty() && e.source == source)) Remove(e.dir);
    }
    if (m_entries.size() >= kMaxExtensions) {
        std::filesystem::remove_all(staged, ec);
        throw std::runtime_error("扩展太多了（最多 50 个）");
    }
    std::string dir = Slug(m.name);
    for (int n = 2; std::filesystem::exists(Folder() / dir, ec); ++n) dir = Slug(m.name) + "-" + std::to_string(n);
    std::filesystem::rename(staged, Folder() / dir, ec);
    if (ec) {
        std::filesystem::remove_all(staged, ec);
        throw std::runtime_error("无法保存扩展");
    }
    Entry e;
    e.dir = dir;
    e.name = m.name;
    e.version = m.version;
    e.description = m.description;
    e.popup = m.popup;
    e.options = m.options;
    e.proxy = m.proxy;
    e.source = source;
    m_entries.push_back(e);
    SaveIndex();
    AddToProfile(dir);
    return m.name;
}

void Extensions::AddToProfile(const std::string& dir) {
    if (!m_profile) return;
    const std::wstring path = (Folder() / dir).wstring();
    m_profile->AddBrowserExtension(path.c_str(),
        Callback<ICoreWebView2ProfileAddBrowserExtensionCompletedHandler>(
            [this, dir](HRESULT result, ICoreWebView2BrowserExtension* ext) -> HRESULT {
                for (auto& e : m_entries) {
                    if (e.dir != dir) continue;
                    if (FAILED(result) || !ext) {
                        e.error = "WebView2 无法加载此扩展（" + Hex(result) + "），它可能用到了 WebView2 不支持的功能";
                        return S_OK;
                    }
                    e.error.clear();
                    m_loaded[dir] = ext;
                    wil::unique_cotaskmem_string id;
                    if (SUCCEEDED(ext->get_Id(&id)) && id) e.id = StringUtils::WideToUtf8(id.get());
                }
                return S_OK;
            }).Get());
}

void Extensions::LoadAll(ICoreWebView2* webView) {
    if (m_profile || !webView) return;
    wil::com_ptr<ICoreWebView2_13> wv13;
    wil::com_ptr<ICoreWebView2Profile> profile;
    if (FAILED(webView->QueryInterface(IID_PPV_ARGS(&wv13))) || !wv13 || FAILED(wv13->get_Profile(&profile)) || !profile) return;
    if (FAILED(profile->QueryInterface(IID_PPV_ARGS(&m_profile))) || !m_profile) return;  // WebView2 Runtime too old
    LoadIndex();
    for (const auto& e : m_entries) {
        if (e.enabled) AddToProfile(e.dir);
    }
}

bool Extensions::SetEnabled(const std::string& dir, bool enabled) {
    LoadIndex();
    for (auto& e : m_entries) {
        if (e.dir != dir) continue;
        e.enabled = enabled;
        SaveIndex();
        if (const auto it = m_loaded.find(dir); it != m_loaded.end()) {
            it->second->Enable(enabled ? TRUE : FALSE,
                Callback<ICoreWebView2BrowserExtensionEnableCompletedHandler>([](HRESULT) -> HRESULT { return S_OK; }).Get());
        } else if (enabled) {
            AddToProfile(dir);
        }
        return true;
    }
    return false;
}

bool Extensions::Remove(const std::string& dir) {
    LoadIndex();
    const auto it = std::find_if(m_entries.begin(), m_entries.end(), [&](const Entry& e) { return e.dir == dir; });
    if (it == m_entries.end()) return false;
    if (const auto loaded = m_loaded.find(dir); loaded != m_loaded.end()) {
        loaded->second->Remove(Callback<ICoreWebView2BrowserExtensionRemoveCompletedHandler>([](HRESULT) -> HRESULT { return S_OK; }).Get());
        m_loaded.erase(loaded);
    }
    m_entries.erase(it);
    SaveIndex();
    // The browser may still hold files open: move the folder aside, delete it now
    // if possible, otherwise at the next start.
    std::error_code ec;
    const auto trash = Folder() / (".trash-" + dir + "-" + std::to_string(GetTickCount64()));
    std::filesystem::rename(Folder() / dir, trash, ec);
    std::filesystem::remove_all(ec ? Folder() / dir : trash, ec);
    return true;
}

std::wstring Extensions::PageUrl(const std::string& dir, bool options) const {
    for (const auto& e : m_entries) {
        if (e.dir != dir || e.id.empty()) continue;
        const std::string page = options ? e.options : e.popup;
        if (page.empty()) return {};
        return StringUtils::Utf8ToWide("chrome-extension://" + e.id + "/" + page);
    }
    return {};
}

} // namespace UltraLight
