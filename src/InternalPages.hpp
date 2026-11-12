#pragma once

#include <windows.h>
#include <objbase.h>
#include <WebView2.h>
#include <filesystem>
#include <string>

namespace UltraLight::InternalPages {

// Built-in pages are served from the app data folder through a WebView2 virtual
// host. ".internal" is reserved for private use, so it never collides with the web.
inline constexpr const wchar_t* kHost = L"ulb.internal";
inline constexpr const wchar_t* kOrigin = L"https://ulb.internal/";

// Offline snapshot of freedom8964.com ("六四事件（离线版）" on the start page), built by
// tools/pack_f8964.py and embedded as one pack. It has its own host so that, unlike
// the built-in pages, it cannot send messages to the browser (IsInternal is false).
inline constexpr const wchar_t* kMemorialHost = L"f8964.internal";
inline constexpr const wchar_t* kMemorialStart = L"https://f8964.internal/index.html";

void Extract();                                   // write the embedded files (startup)
std::filesystem::path Folder();
std::filesystem::path FaviconCacheDir();
std::filesystem::path MemorialFolder();             // extracted freedom8964.com snapshot
std::wstring Url(const std::wstring& page);       // page relative to the origin
bool IsInternal(const std::wstring& uri);
std::wstring PageName(const std::wstring& uri);   // "start.html" for https://ulb.internal/start.html#x
void MapToWebView(ICoreWebView2* webView);

const std::wstring& ReaderableScript();           // evaluates to true/false
const std::wstring& ExtractScript();              // evaluates to article JSON string or null
std::wstring FindScript(const std::wstring& action, const std::wstring& query);

} // namespace UltraLight::InternalPages
