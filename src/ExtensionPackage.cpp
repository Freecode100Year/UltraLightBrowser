#include "ExtensionPackage.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>

namespace UltraLight {

using json = nlohmann::json;

namespace {

json ReadJson(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in.is_open()) return nullptr;
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (text.rfind("\xEF\xBB\xBF", 0) == 0) text.erase(0, 3);
    // Chrome accepts comments in manifest.json; nlohmann can skip them.
    return json::parse(text, nullptr, false, true);
}

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string Str(const json& o, const char* key) {
    if (!o.is_object()) return {};
    const auto it = o.find(key);
    return it != o.end() && it->is_string() ? it->get<std::string>() : std::string();
}

// "__MSG_appName__" -> the message in the first locale that has it.
std::string Localize(const std::filesystem::path& dir, const std::string& value, const std::string& defaultLocale) {
    if (value.size() < 9 || value.rfind("__MSG_", 0) != 0 || value.substr(value.size() - 2) != "__") return value;
    const std::string key = Lower(value.substr(6, value.size() - 8));
    for (const std::string& locale : {std::string("zh_CN"), std::string("zh"), defaultLocale, std::string("en")}) {
        if (locale.empty()) continue;
        const json messages = ReadJson(dir / "_locales" / locale / "messages.json");
        if (!messages.is_object()) continue;
        for (const auto& [k, v] : messages.items()) {
            if (Lower(k) == key) {
                const std::string m = Str(v, "message");
                if (!m.empty()) return m;
            }
        }
    }
    return value;
}

std::uint32_t U32(const std::string& b, size_t at) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(b[at])) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(b[at + 1])) << 8 |
           static_cast<std::uint32_t>(static_cast<unsigned char>(b[at + 2])) << 16 |
           static_cast<std::uint32_t>(static_cast<unsigned char>(b[at + 3])) << 24;
}

} // namespace

ExtensionManifest ReadExtensionManifest(const std::filesystem::path& dir) {
    ExtensionManifest m;
    const json root = ReadJson(dir / "manifest.json");
    if (!root.is_object()) {
        m.error = "文件夹里没有有效的 manifest.json（请选择解压后的扩展文件夹）";
        return m;
    }
    m.manifestVersion = root.value("manifest_version", 0);
    const std::string locale = Str(root, "default_locale");
    m.name = Localize(dir, Str(root, "name"), locale);
    m.version = Str(root, "version");
    m.description = Localize(dir, Str(root, "description"), locale);
    for (const char* key : {"action", "browser_action", "page_action"}) {
        if (const auto it = root.find(key); it != root.end() && m.popup.empty()) m.popup = Str(*it, "default_popup");
    }
    if (const auto it = root.find("options_ui"); it != root.end()) m.options = Str(*it, "page");
    if (m.options.empty()) m.options = Str(root, "options_page");
    for (const char* key : {"permissions", "optional_permissions"}) {
        if (const auto it = root.find(key); it != root.end() && it->is_array()) {
            for (const auto& p : *it) m.proxy = m.proxy || (p.is_string() && p.get<std::string>() == "proxy");
        }
    }
    m.popup = ExtensionPagePath(m.popup);
    m.options = ExtensionPagePath(m.options);
    if (m.name.empty() || m.version.empty()) m.error = "manifest.json 缺少 name 或 version";
    else if (m.manifestVersion != 2 && m.manifestVersion != 3) m.error = "不支持的 manifest_version";
    return m;
}

size_t CrxZipOffset(const std::string& b) {
    if (b.size() >= 4 && b.compare(0, 4, "PK\x03\x04") == 0) return 0;
    if (b.size() < 16 || b.compare(0, 4, "Cr24") != 0) return std::string::npos;
    const std::uint32_t version = U32(b, 4);
    size_t offset;
    if (version == 3) {
        offset = 12 + static_cast<size_t>(U32(b, 8));  // header size
    } else if (version == 2) {
        offset = 16 + static_cast<size_t>(U32(b, 8)) + static_cast<size_t>(U32(b, 12));  // key + signature
    } else {
        return std::string::npos;
    }
    if (offset + 4 > b.size() || b.compare(offset, 4, "PK\x03\x04") != 0) return std::string::npos;
    return offset;
}

std::string ExtensionPagePath(const std::string& page) {
    std::string p = page;
    while (!p.empty() && (p.front() == '/' || p.front() == '\\')) p.erase(0, 1);
    if (p.empty() || p.size() > 300) return {};
    std::replace(p.begin(), p.end(), '\\', '/');
    size_t start = 0;
    while (start <= p.size()) {
        const size_t slash = p.find('/', start);
        const std::string part = p.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (part == ".." || part.find(':') != std::string::npos) return {};
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return p;
}

} // namespace UltraLight
