#pragma once

// Pure classification of a Windows audio render endpoint; tested on Linux by
// tests/audio_endpoint_test.cpp.

#include <cwctype>
#include <string>

namespace UltraLight {

// EndpointFormFactor values (mmdeviceapi.h).
constexpr unsigned kFormFactorHeadphones = 3;
constexpr unsigned kFormFactorHeadset = 5;
constexpr unsigned kFormFactorHandset = 6;

// formFactor < 0: property missing. Names are the endpoint's friendly name
// ("Headphones (Realtek(R) Audio)") and its device interface name ("WH-1000XM5").
// Many USB DACs, Bluetooth headsets and rear-panel jacks report Speakers,
// LineLevel or Unknown, so a non-headphone form factor still falls through to
// the names.
inline bool IsHeadphoneEndpoint(long formFactor, const std::wstring& name, const std::wstring& interfaceName = L"") {
    if (formFactor == static_cast<long>(kFormFactorHeadphones) ||
        formFactor == static_cast<long>(kFormFactorHeadset) ||
        formFactor == static_cast<long>(kFormFactorHandset)) {
        return true;
    }
    std::wstring lower = name + L"\n" + interfaceName;
    for (auto& ch : lower) ch = static_cast<wchar_t>(std::towlower(ch));
    // Substrings that only occur in headphone names. Short or generic tokens
    // ("iem", "qc", "pro") are left out: they appear inside other words.
    static const wchar_t* const kKeywords[] = {
        L"headphone", L"headset", L"earphone", L"earbud", L"airpods", L"buds",
        L"wh-1000", L"wf-1000", L"hands-free", L"耳机" /* 耳机 */,
        L"耳麦" /* 耳麦 */, L"头戴" /* 头戴 */
    };
    for (const wchar_t* kw : kKeywords) {
        if (lower.find(kw) != std::wstring::npos) return true;
    }
    return false;
}

} // namespace UltraLight
