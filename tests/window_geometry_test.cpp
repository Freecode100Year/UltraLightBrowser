#include "WindowGeometry.hpp"
#include <iostream>
struct Rect { int left, top, right, bottom; bool operator==(const Rect&) const = default; };
int main() {
    const Rect monitor{-1920, 0, 0, 1080};
    const Rect work{-1920, 40, 0, 1040};
    const Rect proposed{-1800, 100, -800, 900};
    int failures = 0;
    auto check = [&](Rect actual, Rect expected, const char* name) {
        if (actual != expected) { std::cerr << "FAIL: " << name << '\n'; ++failures; }
    };
    check(UltraLight::CalculateClientBounds(proposed, monitor, work, true, true), monitor,
          "fullscreen from maximized must cover taskbar and full monitor");
    check(UltraLight::CalculateClientBounds(proposed, monitor, work, true, false), monitor,
          "fullscreen from normal window must cover full monitor");
    check(UltraLight::CalculateClientBounds(proposed, monitor, work, false, true), work,
          "maximized outside fullscreen must respect taskbar");
    check(UltraLight::CalculateClientBounds(proposed, monitor, work, false, false), proposed,
          "normal window must keep proposed bounds");
    if (!failures) std::cout << "Window geometry tests passed\n";
    return failures ? 1 : 0;
}
