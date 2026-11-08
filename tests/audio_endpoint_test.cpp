// g++ -std=c++20 -Wall -Wextra -Werror -Isrc tests/audio_endpoint_test.cpp -o /tmp/endpoint-tests
#include "AudioEndpoint.hpp"
#include <cassert>
#include <cstdio>

using UltraLight::IsHeadphoneEndpoint;

int main() {
    // Form factor alone: Headphones, Headset, Handset.
    assert(IsHeadphoneEndpoint(3, L"Output"));
    assert(IsHeadphoneEndpoint(5, L""));
    assert(IsHeadphoneEndpoint(6, L""));
    // Speakers / LineLevel / Unknown still check the names (this used to stop at "Speakers").
    assert(IsHeadphoneEndpoint(1, L"Headphones (Realtek(R) Audio)"));
    assert(IsHeadphoneEndpoint(10, L"Speakers", L"WH-1000XM5"));
    assert(IsHeadphoneEndpoint(1, L"Speakers (AirPods Pro)"));
    assert(IsHeadphoneEndpoint(2, L"Galaxy Buds2 Pro Stereo"));
    assert(IsHeadphoneEndpoint(1, L"耳机 (USB Audio)"));
    assert(IsHeadphoneEndpoint(-1, L"HEADSET EARPHONE"));
    // Real speakers and DACs without a headphone name stay speakers.
    assert(!IsHeadphoneEndpoint(1, L"Speakers (Realtek(R) Audio)"));
    assert(!IsHeadphoneEndpoint(2, L"Line (FiiO K7)", L"FiiO K7"));
    assert(!IsHeadphoneEndpoint(1, L"Premium Speakers (Siemens)"));
    assert(!IsHeadphoneEndpoint(-1, L""));
    std::puts("Audio endpoint tests passed");
    return 0;
}
