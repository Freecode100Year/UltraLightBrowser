#pragma once
namespace UltraLight {
// Keep client bounds selection independent of Win32 for regression testing.
template<class Rect>
Rect CalculateClientBounds(Rect proposed, Rect monitor, Rect work, bool fullscreen, bool maximized) {
    if (fullscreen) return monitor;
    return maximized ? work : proposed;
}
}
