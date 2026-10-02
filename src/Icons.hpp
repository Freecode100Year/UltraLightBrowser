#pragma once

#include "GdiPlus.hpp"

namespace UltraLight {

enum class Icon {
    Sidebar, Back, Forward, Share, Plus, Grid, More, Reader, Lock, Reload, Stop, Close,
    Speaker, SpeakerMute, Star, Bookmark, Clock, Download, ReadingList, Search, Zoom,
    Shield, Gear, Info, Globe, Monitor, Bolt, Private, Window, Block, Sound, Fullscreen,
    Copy, Open, Home, Print, Up, Down
};

namespace Icons {

// Draws a line icon designed on a 24x24 grid into `box`.
void Draw(Gdiplus::Graphics& g, Icon icon, const Gdiplus::RectF& box, const Gdiplus::Color& color, float strokeScale = 1.0f);

// 32bpp premultiplied bitmap for menu items (cached per icon/size; owned by the cache).
HBITMAP MenuBitmap(Icon icon, int size);
void ReleaseMenuBitmaps();

} // namespace Icons
} // namespace UltraLight
