#pragma once
#include <string>
namespace UltraLight {
// Preserve the installed runtime's browser versions; only replace OS identity.
inline std::wstring BuildUserAgent(const std::wstring& original, const std::string& profile) {
    if (profile != "macos-edge") return original;
    const auto start = original.find(L'(');
    const auto end = original.find(L')', start);
    if (start == std::wstring::npos || end == std::wstring::npos) return {};
    return original.substr(0, start) + L"(Macintosh; Intel Mac OS X 10_15_7)" + original.substr(end + 1);
}
}
