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

void Extract();                                   // write the embedded files (startup)
std::filesystem::path Folder();
std::filesystem::path FaviconCacheDir();
std::wstring Url(const std::wstring& page);       // page relative to the origin
bool IsInternal(const std::wstring& uri);
std::wstring PageName(const std::wstring& uri);   // "start.html" for https://ulb.internal/start.html#x
void MapToWebView(ICoreWebView2* webView);

const std::wstring& ReaderableScript();           // evaluates to true/false
const std::wstring& ExtractScript();              // evaluates to article JSON string or null
std::wstring FindScript(const std::wstring& action, const std::wstring& query);

} // namespace UltraLight::InternalPages
