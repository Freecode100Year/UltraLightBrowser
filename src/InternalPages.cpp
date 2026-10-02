#include "InternalPages.hpp"
#include "Config.hpp"
#include "StringUtils.hpp"

#include <wil/com.h>
#include <fstream>
#include <iterator>
#include <string_view>

#ifndef ULB_VERSION
#define ULB_VERSION "dev"
#endif

namespace UltraLight::InternalPages {

namespace {

struct UiFile {
    int id;
    const char* name;
};

// Keep in sync with resources/resource.rc (checked by tests/ui_resources_test.py).
constexpr UiFile kUiFiles[] = {
    {3001, "ui.css"},
    {3002, "ui.js"},
    {3003, "start.html"},
    {3004, "start.css"},
    {3005, "start.js"},
    {3006, "history.html"},
    {3007, "history.js"},
    {3008, "bookmarks.html"},
    {3009, "bookmarks.js"},
    {3010, "settings.html"},
    {3011, "settings.js"},
    {3012, "privacy.html"},
    {3013, "privacy.js"},
    {3014, "overview.html"},
    {3015, "overview.js"},
    {3016, "sidebar.html"},
    {3017, "sidebar.js"},
    {3018, "reader.html"},
    {3019, "reader.css"},
    {3020, "reader.js"},
    {3021, "reader-extract.js"},
    {3022, "find.js"},
    {3023, "vendor/Readability.js"},
    {3024, "vendor/Readability-readerable.js"},
};

std::string_view Resource(int id) {
    HMODULE module = GetModuleHandleW(nullptr);
    HRSRC res = FindResourceW(module, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!res) return {};
    HGLOBAL data = LoadResource(module, res);
    if (!data) return {};
    const void* ptr = LockResource(data);
    const DWORD size = SizeofResource(module, res);
    if (!ptr || size == 0) return {};
    return {static_cast<const char*>(ptr), size};
}

std::string_view ResourceByName(const char* name) {
    for (const auto& f : kUiFiles) {
        if (std::string_view(f.name) == name) return Resource(f.id);
    }
    return {};
}

std::wstring WrapScript(std::initializer_list<const char*> parts, std::string_view tail) {
    std::string js = "(() => {\n";
    for (const char* p : parts) {
        js.append(ResourceByName(p));
        js += "\n;\n";
    }
    js.append(tail);
    js += "\n})()";
    return StringUtils::Utf8ToWide(js);
}

std::wstring JsString(const std::wstring& s) {
    std::wstring out = L"\"";
    for (wchar_t c : s) {
        switch (c) {
        case L'"': out += L"\\\""; break;
        case L'\\': out += L"\\\\"; break;
        case L'\n': out += L"\\n"; break;
        case L'\r': out += L"\\r"; break;
        case L'\t': out += L"\\t"; break;
        case 0x2028: out += L"\\u2028"; break;
        case 0x2029: out += L"\\u2029"; break;
        default:
            if (c < 0x20) {
                wchar_t buf[8];
                swprintf_s(buf, L"\\u%04x", static_cast<unsigned>(c));
                out += buf;
            } else {
                out += c;
            }
        }
    }
    return out + L"\"";
}

} // namespace

std::filesystem::path Folder() {
    return Config::Instance().GetAppDataPath() / "ui";
}

std::filesystem::path FaviconCacheDir() {
    return Folder() / "cache" / "fav";
}

void Extract() {
    std::error_code ec;
    const auto root = Folder();
    std::filesystem::create_directories(root / "vendor", ec);
    std::filesystem::create_directories(FaviconCacheDir(), ec);
    // Skip rewriting the pages when this build already extracted them.
    std::string stamp = ULB_VERSION;
    for (const auto& f : kUiFiles) stamp += "|" + std::to_string(Resource(f.id).size());
    {
        std::ifstream in(root / ".stamp", std::ios::binary);
        std::string existing((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        bool complete = existing == stamp;
        for (const auto& f : kUiFiles) {
            if (!complete) break;
            complete = std::filesystem::exists(root / std::filesystem::path(StringUtils::Utf8ToWide(f.name)), ec);
        }
        if (complete) return;
    }
    for (const auto& f : kUiFiles) {
        const auto data = Resource(f.id);
        if (data.empty()) continue;
        const auto path = root / std::filesystem::path(StringUtils::Utf8ToWide(f.name));
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
    }
    std::ofstream(root / ".stamp", std::ios::binary | std::ios::trunc) << stamp;
}

std::wstring Url(const std::wstring& page) {
    return std::wstring(kOrigin) + page;
}

bool IsInternal(const std::wstring& uri) {
    return uri.rfind(kOrigin, 0) == 0 || uri == L"https://ulb.internal";
}

std::wstring PageName(const std::wstring& uri) {
    if (!IsInternal(uri)) return {};
    std::wstring rest = uri.size() > wcslen(kOrigin) ? uri.substr(wcslen(kOrigin)) : L"";
    const auto cut = rest.find_first_of(L"?#");
    if (cut != std::wstring::npos) rest = rest.substr(0, cut);
    return rest;
}

void MapToWebView(ICoreWebView2* webView) {
    if (!webView) return;
    wil::com_ptr<ICoreWebView2_3> webView3;
    if (SUCCEEDED(webView->QueryInterface(IID_PPV_ARGS(&webView3))) && webView3) {
        webView3->SetVirtualHostNameToFolderMapping(kHost, Folder().wstring().c_str(),
            COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY);
    }
}

const std::wstring& ReaderableScript() {
    static const std::wstring script = WrapScript({"vendor/Readability-readerable.js"},
        "try { return document.body ? isProbablyReaderable(document, { minContentLength: 140, minScore: 20 }) : false; } catch (e) { return false; }");
    return script;
}

const std::wstring& ExtractScript() {
    static const std::wstring script = WrapScript({"vendor/Readability.js"}, ResourceByName("reader-extract.js"));
    return script;
}

std::wstring FindScript(const std::wstring& action, const std::wstring& query) {
    static const std::wstring fn = StringUtils::Utf8ToWide(ResourceByName("find.js"));
    return L"(" + fn + L")(" + JsString(action) + L", " + JsString(query) + L")";
}

} // namespace UltraLight::InternalPages
