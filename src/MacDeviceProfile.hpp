#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace UltraLight {

struct MacDeviceProfile {
    std::string_view renderer;
    unsigned hardwareConcurrency;
};

inline constexpr std::array<MacDeviceProfile, 4> kMacDeviceProfiles{{
    {"ANGLE (Apple, ANGLE Metal Renderer: Apple M1, Unspecified Version)", 8},
    {"ANGLE (Apple, ANGLE Metal Renderer: Apple M2, Unspecified Version)", 8},
    {"ANGLE (Apple, ANGLE Metal Renderer: Apple M3, Unspecified Version)", 8},
    {"ANGLE (Apple, ANGLE Metal Renderer: Apple M4, Unspecified Version)", 10},
}};

inline std::size_t MacDeviceProfileCount() {
    return kMacDeviceProfiles.size();
}

inline const MacDeviceProfile& MacDeviceProfileForIndex(std::size_t index) {
    return kMacDeviceProfiles[index % kMacDeviceProfiles.size()];
}

inline std::size_t MacDeviceProfileIndex(bool rngSucceeded, std::uint32_t randomValue) {
    return rngSucceeded ? randomValue % kMacDeviceProfiles.size() : 0;
}

template <typename RandomSource>
inline const MacDeviceProfile& SessionMacDeviceProfileFor(RandomSource&& randomSource) {
    static const std::size_t profileIndex = [&randomSource] {
        std::uint32_t randomValue = 0;
        const bool succeeded = randomSource(randomValue);
        return MacDeviceProfileIndex(succeeded, randomValue);
    }();
    return MacDeviceProfileForIndex(profileIndex);
}

} // namespace UltraLight
