#pragma once

#include <windows.h>
#include <string>
#include <string_view>

namespace UltraLight::StringUtils {

inline std::wstring Utf8ToWide(std::string_view str) {
    if (str.empty()) return L"";
    int size = MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), nullptr, 0);
    if (size <= 0) return L"";
    std::wstring out(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), out.data(), size);
    return out;
}

inline std::string WideToUtf8(std::wstring_view wstr) {
    if (wstr.empty()) return "";
    int size = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return "";
    std::string out(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), out.data(), size, nullptr, nullptr);
    return out;
}

} // namespace UltraLight::StringUtils
