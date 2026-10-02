#pragma once
#include <array>
#include <string>
#include <utility>
#include <vector>
namespace UltraLight {
// Preserve the installed runtime's browser versions; only replace OS identity.
inline std::wstring BuildUserAgent(const std::wstring& original, const std::string& profile) {
    if (profile != "macos-edge") return original;
    const auto start = original.find(L'(');
    const auto end = original.find(L')', start);
    if (start == std::wstring::npos || end == std::wstring::npos) return {};
    return original.substr(0, start) + L"(Macintosh; Intel Mac OS X 10_15_7)" + original.substr(end + 1);
}

// Version following "<token>/" in a UA string, e.g. "Chrome/" -> "141.0.0.0".
inline std::string UserAgentToken(const std::wstring& ua, const std::wstring& token) {
    const auto pos = ua.find(token + L"/");
    if (pos == std::wstring::npos) return {};
    std::string out;
    for (size_t i = pos + token.size() + 1; i < ua.size() && ((ua[i] >= L'0' && ua[i] <= L'9') || ua[i] == L'.'); ++i) {
        out += static_cast<char>(ua[i]);
    }
    return out;
}

// Chromium's GREASE brand list (components/embedder_support/user_agent_utils.cc).
// Used only when the runtime's real list cannot be captured.
inline std::vector<std::pair<std::string, std::string>> GreasedBrandList(
    int seed, const std::string& brand, const std::string& brandVersion, const std::string& chromiumVersion) {
    static constexpr std::array<std::array<int, 3>, 6> orders{{
        {0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}}};
    static const std::array<const char*, 11> chars{" ", "(", ":", "-", ".", "/", ")", ";", "=", "?", "_"};
    static const std::array<const char*, 3> versions{"8", "99", "24"};
    if (seed < 0) seed = 0;
    const auto& order = orders[seed % 6];
    std::vector<std::pair<std::string, std::string>> list(3);
    list[order[0]] = {std::string("Not") + chars[seed % chars.size()] + "A" + chars[(seed + 1) % chars.size()] + "Brand",
                      versions[seed % versions.size()]};
    list[order[1]] = {"Chromium", chromiumVersion};
    list[order[2]] = {brand, brandVersion};
    return list;
}
}
