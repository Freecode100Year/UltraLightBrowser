#include "MacDeviceProfile.hpp"
#include "MacStealth.hpp"
#include <array>
#include <cassert>
#include <iostream>

int main() {
    using namespace UltraLight;
    constexpr std::array<unsigned, 4> cores{8, 8, 8, 10};
    constexpr std::array<const char*, 4> chips{"Apple M1", "Apple M2", "Apple M3", "Apple M4"};
    assert(MacDeviceProfileCount() == cores.size());
    for (std::size_t i = 0; i < cores.size(); ++i) {
        const auto& profile = MacDeviceProfileForIndex(i);
        assert(profile.hardwareConcurrency == cores[i]);
        assert(profile.renderer.find(chips[i]) != std::string_view::npos);
        assert(&MacDeviceProfileForIndex(i) == &MacDeviceProfileForIndex(i));
    }
    assert(MacDeviceProfileForIndex(cores.size()).renderer == MacDeviceProfileForIndex(0).renderer);
    assert(MacDeviceProfileIndex(false, 3) == 0);
    assert(MacDeviceProfileIndex(true, 0) == 0);
    assert(MacDeviceProfileIndex(true, 5) == 1);

    const auto script = BuildMacStealthScript(MacDeviceProfileForIndex(2));
    assert(script.find(L"__ULB_MAC_GPU_RENDERER__") == std::wstring::npos);
    assert(script.find(L"__ULB_MAC_HARDWARE_CONCURRENCY__") == std::wstring::npos);
    assert(script.find(L"Apple M3") != std::wstring::npos);
    assert(script.find(L"const HARDWARE_CONCURRENCY = 8;") != std::wstring::npos);
    assert(script.find(L"const HARDWARE_CONCURRENCY = 10;") == std::wstring::npos);
    assert(script.find(L"toDataURL") == std::wstring::npos);
    assert(script.find(L"getImageData") == std::wstring::npos);
    assert(script.find(L"copyFromChannel") == std::wstring::npos);
    assert(script.find(L"getChannelData") == std::wstring::npos);
    std::cout << "Mac device profile tests passed\n";
}
