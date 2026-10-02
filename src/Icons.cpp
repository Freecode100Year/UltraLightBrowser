#include "Icons.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>

using namespace Gdiplus;

namespace UltraLight::Icons {

namespace {

// Maps the 24x24 design grid onto the target box.
struct Grid {
    RectF box;
    float s;
    PointF P(float x, float y) const { return PointF(box.X + x * s, box.Y + y * s); }
    float L(float v) const { return v * s; }
};

void Line(Graphics& g, const Pen& pen, const Grid& d, float x1, float y1, float x2, float y2) {
    g.DrawLine(&pen, d.P(x1, y1), d.P(x2, y2));
}

struct XY {
    float x, y;
};

void Poly(Graphics& g, const Pen& pen, const Grid& d, std::initializer_list<XY> pts, bool closed = false) {
    PointF buf[16];
    int n = 0;
    for (const auto& p : pts) {
        if (n < 16) buf[n++] = d.P(p.x, p.y);
    }
    if (closed) g.DrawPolygon(&pen, buf, n);
    else g.DrawLines(&pen, buf, n);
}

void RoundRectPath(GraphicsPath& path, const Grid& d, float x, float y, float w, float h, float r) {
    const float rr = d.L(r) * 2;
    const PointF tl = d.P(x, y);
    const float W = d.L(w), H = d.L(h);
    path.AddArc(tl.X, tl.Y, rr, rr, 180, 90);
    path.AddArc(tl.X + W - rr, tl.Y, rr, rr, 270, 90);
    path.AddArc(tl.X + W - rr, tl.Y + H - rr, rr, rr, 0, 90);
    path.AddArc(tl.X, tl.Y + H - rr, rr, rr, 90, 90);
    path.CloseFigure();
}

void RoundRect(Graphics& g, const Pen& pen, const Grid& d, float x, float y, float w, float h, float r) {
    GraphicsPath path;
    RoundRectPath(path, d, x, y, w, h, r);
    g.DrawPath(&pen, &path);
}

void Circle(Graphics& g, const Pen& pen, const Grid& d, float cx, float cy, float r) {
    const PointF c = d.P(cx - r, cy - r);
    g.DrawEllipse(&pen, c.X, c.Y, d.L(r * 2), d.L(r * 2));
}

void Dot(Graphics& g, const Brush& brush, const Grid& d, float cx, float cy, float r) {
    const PointF c = d.P(cx - r, cy - r);
    g.FillEllipse(&brush, c.X, c.Y, d.L(r * 2), d.L(r * 2));
}

void Arc(Graphics& g, const Pen& pen, const Grid& d, float cx, float cy, float r, float start, float sweep) {
    const PointF c = d.P(cx - r, cy - r);
    g.DrawArc(&pen, c.X, c.Y, d.L(r * 2), d.L(r * 2), start, sweep);
}

void Speaker(Graphics& g, const Pen& pen, const Grid& d) {
    Poly(g, pen, d, {{11, 5}, {6, 9}, {3, 9}, {3, 15}, {6, 15}, {11, 19}}, true);
}

} // namespace

void Draw(Graphics& g, Icon icon, const RectF& box, const Color& color, float strokeScale) {
    const float size = box.Width < box.Height ? box.Width : box.Height;
    Grid d{RectF(box.X + (box.Width - size) / 2, box.Y + (box.Height - size) / 2, size, size), size / 24.0f};
    Pen pen(color, (std::max)(1.0f, d.L(1.7f) * strokeScale));
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    pen.SetLineJoin(LineJoinRound);
    SolidBrush brush(color);

    switch (icon) {
    case Icon::Sidebar:
        RoundRect(g, pen, d, 3, 4.5f, 18, 15, 3);
        Line(g, pen, d, 9, 4.5f, 9, 19.5f);
        break;
    case Icon::Back:
        Poly(g, pen, d, {{15, 5.5f}, {8.5f, 12}, {15, 18.5f}});
        break;
    case Icon::Forward:
        Poly(g, pen, d, {{9, 5.5f}, {15.5f, 12}, {9, 18.5f}});
        break;
    case Icon::Share:
        Line(g, pen, d, 12, 15, 12, 3.5f);
        Poly(g, pen, d, {{8, 7.5f}, {12, 3.5f}, {16, 7.5f}});
        Poly(g, pen, d, {{8.5f, 10}, {5.5f, 10}, {5.5f, 20.5f}, {18.5f, 20.5f}, {18.5f, 10}, {15.5f, 10}});
        break;
    case Icon::Plus:
        Line(g, pen, d, 12, 5, 12, 19);
        Line(g, pen, d, 5, 12, 19, 12);
        break;
    case Icon::Grid:
        RoundRect(g, pen, d, 4, 4, 6.5f, 6.5f, 1.5f);
        RoundRect(g, pen, d, 13.5f, 4, 6.5f, 6.5f, 1.5f);
        RoundRect(g, pen, d, 4, 13.5f, 6.5f, 6.5f, 1.5f);
        RoundRect(g, pen, d, 13.5f, 13.5f, 6.5f, 6.5f, 1.5f);
        break;
    case Icon::More:
        Dot(g, brush, d, 5.5f, 12, 1.6f);
        Dot(g, brush, d, 12, 12, 1.6f);
        Dot(g, brush, d, 18.5f, 12, 1.6f);
        break;
    case Icon::Reader:
    case Icon::ReadingList:
        Line(g, pen, d, 4, 6, 20, 6);
        Line(g, pen, d, 4, 10, 20, 10);
        Line(g, pen, d, 4, 14, 14, 14);
        Line(g, pen, d, 4, 18, 16, 18);
        if (icon == Icon::ReadingList) {
            Circle(g, pen, d, 18.5f, 16.5f, 2.6f);
        }
        break;
    case Icon::Lock:
        RoundRect(g, pen, d, 5.5f, 11, 13, 9, 2);
        Arc(g, pen, d, 12, 8.5f, 3.6f, 180, 180);
        Line(g, pen, d, 8.4f, 8.5f, 8.4f, 11);
        Line(g, pen, d, 15.6f, 8.5f, 15.6f, 11);
        break;
    case Icon::Reload:
        Arc(g, pen, d, 12, 12, 7, -40, 300);
        Poly(g, pen, d, {{19.5f, 4.5f}, {18.8f, 8.6f}, {14.8f, 7.6f}});
        break;
    case Icon::Stop:
    case Icon::Close:
        Line(g, pen, d, 6.5f, 6.5f, 17.5f, 17.5f);
        Line(g, pen, d, 17.5f, 6.5f, 6.5f, 17.5f);
        break;
    case Icon::Speaker:
    case Icon::Sound:
        Speaker(g, pen, d);
        Arc(g, pen, d, 12, 12, 4.5f, -45, 90);
        Arc(g, pen, d, 12, 12, 8, -48, 96);
        break;
    case Icon::SpeakerMute:
        Speaker(g, pen, d);
        Line(g, pen, d, 15, 9, 21, 15);
        Line(g, pen, d, 21, 9, 15, 15);
        break;
    case Icon::Star:
        Poly(g, pen, d, {{12, 3.5f}, {14.6f, 9}, {20.5f, 9.6f}, {16, 13.6f}, {17.3f, 19.6f}, {12, 16.6f},
                         {6.7f, 19.6f}, {8, 13.6f}, {3.5f, 9.6f}, {9.4f, 9}}, true);
        break;
    case Icon::Bookmark:
        Poly(g, pen, d, {{6.5f, 3.5f}, {17.5f, 3.5f}, {17.5f, 20.5f}, {12, 16.5f}, {6.5f, 20.5f}}, true);
        break;
    case Icon::Clock:
        Circle(g, pen, d, 12, 12, 8.5f);
        Poly(g, pen, d, {{12, 7}, {12, 12}, {15.5f, 14}});
        break;
    case Icon::Download:
        Line(g, pen, d, 12, 4, 12, 16);
        Poly(g, pen, d, {{7.5f, 11.5f}, {12, 16}, {16.5f, 11.5f}});
        Line(g, pen, d, 5, 20, 19, 20);
        break;
    case Icon::Search:
        Circle(g, pen, d, 10.5f, 10.5f, 6);
        Line(g, pen, d, 15, 15, 20, 20);
        break;
    case Icon::Zoom:
        Circle(g, pen, d, 10.5f, 10.5f, 6);
        Line(g, pen, d, 15, 15, 20, 20);
        Line(g, pen, d, 8, 10.5f, 13, 10.5f);
        Line(g, pen, d, 10.5f, 8, 10.5f, 13);
        break;
    case Icon::Shield:
        Poly(g, pen, d, {{12, 3}, {19.5f, 6}, {19.5f, 12}, {16.5f, 18}, {12, 21}, {7.5f, 18}, {4.5f, 12}, {4.5f, 6}}, true);
        break;
    case Icon::Gear: {
        Circle(g, pen, d, 12, 12, 3.2f);
        Circle(g, pen, d, 12, 12, 7);
        for (int i = 0; i < 8; ++i) {
            const float a = 3.14159265f * i / 4;
            Line(g, pen, d, 12 + 7 * cosf(a), 12 + 7 * sinf(a), 12 + 9.4f * cosf(a), 12 + 9.4f * sinf(a));
        }
        break;
    }
    case Icon::Info:
        Circle(g, pen, d, 12, 12, 8.5f);
        Line(g, pen, d, 12, 11, 12, 16.5f);
        Dot(g, brush, d, 12, 7.8f, 1.1f);
        break;
    case Icon::Globe:
        Circle(g, pen, d, 12, 12, 8.5f);
        Line(g, pen, d, 3.5f, 12, 20.5f, 12);
        {
            const PointF tl = d.P(8, 3.5f);
            g.DrawEllipse(&pen, tl.X, tl.Y, d.L(8), d.L(17));
        }
        break;
    case Icon::Monitor:
        RoundRect(g, pen, d, 3.5f, 4.5f, 17, 12, 2);
        Line(g, pen, d, 8, 20, 16, 20);
        Line(g, pen, d, 12, 16.5f, 12, 20);
        break;
    case Icon::Bolt:
        Poly(g, pen, d, {{13, 3}, {5.5f, 13.5f}, {11.5f, 13.5f}, {10.5f, 21}, {18.5f, 10.5f}, {12.5f, 10.5f}}, true);
        break;
    case Icon::Private:
        Arc(g, pen, d, 12, 16, 9, 200, 140);
        Arc(g, pen, d, 12, 8, 9, 20, 140);
        Line(g, pen, d, 4.5f, 4.5f, 19.5f, 19.5f);
        break;
    case Icon::Window:
        RoundRect(g, pen, d, 3.5f, 5, 17, 14, 2);
        Line(g, pen, d, 3.5f, 9, 20.5f, 9);
        break;
    case Icon::Block:
        Circle(g, pen, d, 12, 12, 8.5f);
        Line(g, pen, d, 6, 6, 18, 18);
        break;
    case Icon::Fullscreen:
        Poly(g, pen, d, {{4, 9}, {4, 4}, {9, 4}});
        Poly(g, pen, d, {{15, 4}, {20, 4}, {20, 9}});
        Poly(g, pen, d, {{20, 15}, {20, 20}, {15, 20}});
        Poly(g, pen, d, {{9, 20}, {4, 20}, {4, 15}});
        break;
    case Icon::Copy:
        RoundRect(g, pen, d, 8, 8, 12, 12, 2);
        Poly(g, pen, d, {{16, 8}, {16, 4}, {4, 4}, {4, 16}, {8, 16}});
        break;
    case Icon::Open:
        Poly(g, pen, d, {{13, 4}, {20, 4}, {20, 11}});
        Line(g, pen, d, 20, 4, 11, 13);
        Poly(g, pen, d, {{17, 14}, {17, 20}, {4, 20}, {4, 7}, {10, 7}});
        break;
    case Icon::Home:
        Poly(g, pen, d, {{4, 11}, {12, 4}, {20, 11}});
        Poly(g, pen, d, {{6.5f, 9.5f}, {6.5f, 20}, {17.5f, 20}, {17.5f, 9.5f}});
        break;
    case Icon::Up:
        Poly(g, pen, d, {{5.5f, 15}, {12, 8.5f}, {18.5f, 15}});
        break;
    case Icon::Down:
        Poly(g, pen, d, {{5.5f, 9}, {12, 15.5f}, {18.5f, 9}});
        break;
    case Icon::Print:
        Poly(g, pen, d, {{7, 9}, {7, 4}, {17, 4}, {17, 9}});
        RoundRect(g, pen, d, 3.5f, 9, 17, 8, 2);
        RoundRect(g, pen, d, 7, 14, 10, 6.5f, 0.5f);
        break;
    }
}

namespace {
std::map<std::pair<int, int>, HBITMAP>& Cache() {
    static std::map<std::pair<int, int>, HBITMAP> cache;
    return cache;
}
} // namespace

HBITMAP MenuBitmap(Icon icon, int size) {
    auto& cache = Cache();
    const auto key = std::make_pair(static_cast<int>(icon), size);
    const auto it = cache.find(key);
    if (it != cache.end()) return it->second;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) return nullptr;
    ZeroMemory(bits, static_cast<size_t>(size) * size * 4);
    {
        Bitmap canvas(size, size, size * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(bits));
        Graphics g(&canvas);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        Draw(g, icon, RectF(0, 0, static_cast<REAL>(size), static_cast<REAL>(size)), Color(255, 205, 207, 213), 0.95f);
    }
    cache[key] = bmp;
    return bmp;
}

void ReleaseMenuBitmaps() {
    for (auto& [key, bmp] : Cache()) DeleteObject(bmp);
    Cache().clear();
}

} // namespace UltraLight::Icons
