// Toolbar layout, painting and mouse handling for the Safari-style title bar:
// traffic lights, sidebar/back/forward, compact tab strip (the active tab is the
// address field), share, new tab, tab overview and the single "⋯" menu.

#include "MainWindow.hpp"
#include "InternalPages.hpp"
#include "StringUtils.hpp"

#include <windowsx.h>
#include <cmath>

using namespace Gdiplus;

namespace UltraLight {

namespace {

Color C(BYTE r, BYTE g, BYTE b, BYTE a = 255) { return Color(a, r, g, b); }

RectF F(const RECT& r) {
    return RectF(static_cast<REAL>(r.left), static_cast<REAL>(r.top),
                 static_cast<REAL>(r.right - r.left), static_cast<REAL>(r.bottom - r.top));
}

void RoundRectPath(GraphicsPath& path, const RectF& r, REAL radius) {
    const REAL d = radius * 2;
    if (d <= 0 || r.Width < d || r.Height < d) {
        path.AddRectangle(r);
        return;
    }
    path.AddArc(r.X, r.Y, d, d, 180, 90);
    path.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    path.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    path.CloseFigure();
}

void FillRound(Graphics& g, const RectF& r, REAL radius, const Color& fill) {
    GraphicsPath path;
    RoundRectPath(path, r, radius);
    SolidBrush brush(fill);
    g.FillPath(&brush, &path);
}

void StrokeRound(Graphics& g, const RectF& r, REAL radius, const Color& color, REAL width = 1.0f) {
    GraphicsPath path;
    RoundRectPath(path, r, radius);
    Pen pen(color, width);
    g.DrawPath(&pen, &path);
}

void Text(Graphics& g, const std::wstring& s, Font& font, const RectF& r, const Color& color,
          StringAlignment align = StringAlignmentNear) {
    if (s.empty() || r.Width <= 2) return;
    StringFormat fmt(StringFormatFlagsNoWrap);
    fmt.SetAlignment(align);
    fmt.SetLineAlignment(StringAlignmentCenter);
    fmt.SetTrimming(StringTrimmingEllipsisCharacter);
    SolidBrush brush(color);
    g.DrawString(s.c_str(), static_cast<INT>(s.size()), &font, r, &fmt, &brush);
}

unsigned Hue(const std::wstring& s) {
    unsigned h = 0;
    for (wchar_t c : s) h = h * 31 + c;
    return h % 360;
}

Color HslColor(unsigned hue, double sat, double light) {
    const double c = (1 - std::fabs(2 * light - 1)) * sat;
    const double hp = hue / 60.0;
    const double x = c * (1 - std::fabs(std::fmod(hp, 2) - 1));
    double r = 0, g = 0, b = 0;
    if (hp < 1) { r = c; g = x; } else if (hp < 2) { r = x; g = c; } else if (hp < 3) { g = c; b = x; }
    else if (hp < 4) { g = x; b = c; } else if (hp < 5) { r = x; b = c; } else { r = c; b = x; }
    const double m = light - c / 2;
    return C(static_cast<BYTE>((r + m) * 255), static_cast<BYTE>((g + m) * 255), static_cast<BYTE>((b + m) * 255));
}

std::wstring HostOf(const std::wstring& url) {
    return StringUtils::Utf8ToWide(Library::HostOf(StringUtils::WideToUtf8(url)));
}

} // namespace

RECT MainWindow::ContentRect() const {
    RECT client{};
    GetClientRect(m_hWnd, &client);
    if (m_isFullScreen) return client;
    RECT rc{client.left, m_topbarHeight, client.right, client.bottom};
    if (m_sidebarVisible) rc.left += S(260);
    if (m_findVisible) rc.top += S(40);
    if (rc.right < rc.left) rc.right = rc.left;
    if (rc.bottom < rc.top) rc.bottom = rc.top;
    return rc;
}

void MainWindow::LayoutToolbar(int width) {
    const int topH = m_topbarHeight;
    const int cy = topH / 2;

    // Traffic lights
    const int r = S(6);
    const int gap = S(8);
    const int closeX = S(20);
    const int minX = closeX + 2 * r + gap;
    const int maxX = minX + 2 * r + gap;
    m_rcTrafficClose = {closeX - r, cy - r, closeX + r, cy + r};
    m_rcTrafficMin = {minX - r, cy - r, minX + r, cy + r};
    m_rcTrafficMax = {maxX - r, cy - r, maxX + r, cy + r};
    m_rcTrafficGroup = {m_rcTrafficClose.left - 4, m_rcTrafficClose.top - 4, m_rcTrafficMax.right + 4, m_rcTrafficMax.bottom + 4};

    const int bw = S(30), bh = S(28), by = (topH - bh) / 2;
    int x = m_rcTrafficMax.right + S(16);
    m_rcSidebarBtn = {x, by, x + bw, by + bh}; x += bw + S(4);
    m_rcBack = {x, by, x + bw, by + bh}; x += bw + S(2);
    m_rcForward = {x, by, x + bw, by + bh}; x += bw + S(10);
    if (m_private) {
        m_rcPrivateBadge = {x, cy - S(11), x + S(46), cy + S(11)};
        x += S(54);
    } else {
        m_rcPrivateBadge = {};
    }

    int rx = width - S(12);
    m_rcMenuBtn = {rx - bw, by, rx, by + bh}; rx -= bw + S(6);
    m_rcOverviewBtn = {rx - bw, by, rx, by + bh}; rx -= bw + S(6);
    m_rcNewTab = {rx - bw, by, rx, by + bh}; rx -= bw + S(6);
    m_rcShare = {rx - bw, by, rx, by + bh}; rx -= S(10);

    const int th = S(34);
    const int ty = (topH - th) / 2;
    m_rcStrip = {x, ty, (std::max)(x, rx), ty + th};

    // Tab slots
    m_slots.clear();
    m_overflowCount = 0;
    m_rcOverflow = {};
    m_rcActive = {};
    const int n = static_cast<int>(m_tabs.size());
    const int W = m_rcStrip.right - m_rcStrip.left;
    if (n == 0 || W <= 0) return;
    const int tgap = S(6);
    const int activeIdx = (std::max)(0, IndexOf(m_activeId));

    int activeW = 0, inactiveW = 0;
    int first = 0, last = n - 1;
    if (n == 1) {
        activeW = (std::min)(W, S(680));
    } else {
        const int minActive = (std::min)(W, S(300));
        activeW = std::clamp(W - (n - 1) * (S(180) + tgap), minActive, (std::max)(minActive, S(680)));
        inactiveW = (W - activeW - (n - 1) * tgap) / (n - 1);
        inactiveW = std::clamp(inactiveW, S(36), S(240));
        if (activeW + (n - 1) * (inactiveW + tgap) > W) {
            // Too many tabs: keep a window of tabs around the active one plus a "+N" chip.
            const int chipW = S(44);
            const int fit = (std::max)(0, (W - activeW - chipW - tgap) / (inactiveW + tgap));
            int before = (std::min)(activeIdx, fit / 2);
            int after = (std::min)(n - 1 - activeIdx, fit - before);
            before = (std::min)(activeIdx, fit - after);
            first = activeIdx - before;
            last = activeIdx + after;
            m_overflowCount = n - (last - first + 1);
        }
    }

    int cx = m_rcStrip.left;
    if (n == 1) cx = m_rcStrip.left + (W - activeW) / 2;
    for (int i = first; i <= last; ++i) {
        const Tab& t = *m_tabs[static_cast<size_t>(i)];
        const bool active = t.id == m_activeId;
        const int w = active ? activeW : inactiveW;
        TabSlot slot;
        slot.id = t.id;
        slot.rc = {cx, ty, cx + w, ty + th};
        const int iconBox = S(20);
        slot.close = {slot.rc.left + S(6), ty + (th - iconBox) / 2, slot.rc.left + S(6) + iconBox, ty + (th + iconBox) / 2};
        if (t.audio || t.muted) {
            slot.audio = {slot.rc.right - S(6) - iconBox, slot.close.top, slot.rc.right - S(6), slot.close.bottom};
        }
        if (active) {
            m_rcActive = slot.rc;
            m_rcReader = {slot.rc.left + S(6), ty + S(5), slot.rc.left + S(32), ty + th - S(5)};
            m_rcReload = {slot.rc.right - S(32), ty + S(5), slot.rc.right - S(6), ty + th - S(5)};
            if (t.audio || t.muted) {
                slot.audio = {m_rcReload.left - S(26), m_rcReload.top, m_rcReload.left - S(2), m_rcReload.bottom};
            }
            const int textRight = (slot.audio.right > slot.audio.left ? slot.audio.left : m_rcReload.left) - S(4);
            m_rcAddressText = {m_rcReader.right + S(4), ty + S(4), textRight, ty + th - S(4)};
        }
        m_slots.push_back(slot);
        cx += w + tgap;
    }
    if (m_overflowCount > 0) {
        m_rcOverflow = {cx, ty + S(3), cx + S(44), ty + th - S(3)};
    }
}

void MainWindow::UpdateLayout() {
    if (!m_hWnd) return;
    RECT client{};
    GetClientRect(m_hWnd, &client);
    const int width = client.right;
    if (width <= 0 || client.bottom <= 0) return;

    if (!m_isFullScreen) LayoutToolbar(width);

    // Address edit sits inside the active tab capsule.
    if (m_hEditAddress) {
        if (m_isAddressFocused && !m_isFullScreen && m_rcAddressText.right > m_rcAddressText.left) {
            const int h = S(20);
            const int y = (m_rcAddressText.top + m_rcAddressText.bottom - h) / 2;
            SetWindowPos(m_hEditAddress, HWND_TOP, m_rcAddressText.left, y,
                         m_rcAddressText.right - m_rcAddressText.left, h, SWP_SHOWWINDOW);
        } else {
            ShowWindow(m_hEditAddress, SW_HIDE);
        }
    }

    const RECT content = ContentRect();
    if (m_findVisible && !m_isFullScreen) {
        m_rcFindBar = {content.left, m_topbarHeight, content.right, m_topbarHeight + S(40)};
        const int bh = S(26);
        const int by = m_rcFindBar.top + (S(40) - bh) / 2;
        m_rcFindDone = {m_rcFindBar.right - S(12) - S(56), by, m_rcFindBar.right - S(12), by + bh};
        m_rcFindNext = {m_rcFindDone.left - S(10) - S(28), by, m_rcFindDone.left - S(10), by + bh};
        m_rcFindPrev = {m_rcFindNext.left - S(2) - S(28), by, m_rcFindNext.left - S(2), by + bh};
        const int editW = (std::min)(S(320), (std::max)(S(120), static_cast<int>(m_rcFindPrev.left - m_rcFindBar.left) - S(140)));
        m_rcFindStatus = {m_rcFindPrev.left - S(10) - S(100), by, m_rcFindPrev.left - S(10), by + bh};
        if (m_hFindEdit) {
            SetWindowPos(m_hFindEdit, HWND_TOP, m_rcFindStatus.left - S(8) - editW, by + S(3), editW, bh - S(6), SWP_SHOWWINDOW);
        }
    } else {
        m_rcFindBar = {};
        if (m_hFindEdit) ShowWindow(m_hFindEdit, SW_HIDE);
    }

    for (const auto& tab : m_tabs) {
        if (tab->view) tab->view->Resize(content);
    }
    LayoutPanels();
    InvalidateToolbar();
    if (m_findVisible) InvalidateRect(m_hWnd, &m_rcFindBar, FALSE);
}

void MainWindow::InvalidateToolbar() {
    if (!m_hWnd) return;
    RECT client{};
    GetClientRect(m_hWnd, &client);
    RECT rc{0, 0, client.right, m_topbarHeight};
    InvalidateRect(m_hWnd, &rc, FALSE);
}

std::wstring MainWindow::DisplayUrl(const Tab& tab) const {
    if (!tab.readerSource.empty()) return tab.readerSource;
    if (InternalPages::IsInternal(tab.url)) return L"";
    return tab.url;
}

void MainWindow::PaintToolbar(HDC hdc, int width) {
    const int topH = m_topbarHeight;
    Graphics g(hdc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);

    // Background
    {
        const Color top = m_private ? C(40, 34, 52) : C(44, 46, 52);
        const Color bottom = m_private ? C(32, 27, 43) : C(38, 40, 45);
        LinearGradientBrush bg(PointF(0, 0), PointF(0, static_cast<REAL>(topH)), top, bottom);
        g.FillRectangle(&bg, 0, 0, width, topH);
        Pen sep(C(21, 22, 26), 1);
        g.DrawLine(&sep, 0.0f, topH - 0.5f, static_cast<REAL>(width), topH - 0.5f);
    }

    // Traffic lights
    const bool lit = m_isWindowActive || m_isTrafficGroupHovered;
    const struct { const RECT* rc; Color fill; Color pressed; Color glyph; int id; } lights[] = {
        {&m_rcTrafficClose, C(255, 95, 87), C(214, 69, 61), C(77, 0, 0), 1},
        {&m_rcTrafficMin, C(254, 188, 46), C(214, 153, 30), C(153, 87, 0), 2},
        {&m_rcTrafficMax, C(40, 200, 64), C(30, 160, 48), C(0, 100, 0), 3},
    };
    for (const auto& l : lights) {
        const Color fill = !lit ? C(78, 78, 84) : (m_trafficPressedBtn == l.id ? l.pressed : l.fill);
        SolidBrush brush(fill);
        const RectF rf = F(*l.rc);
        g.FillEllipse(&brush, rf);
        if (m_isTrafficGroupHovered) {
            Pen pen(l.glyph, static_cast<REAL>(S(1)) * 1.2f);
            pen.SetStartCap(LineCapRound);
            pen.SetEndCap(LineCapRound);
            const REAL cx = rf.X + rf.Width / 2, cy = rf.Y + rf.Height / 2, a = rf.Width * 0.24f;
            if (l.id == 1) {
                g.DrawLine(&pen, cx - a, cy - a, cx + a, cy + a);
                g.DrawLine(&pen, cx - a, cy + a, cx + a, cy - a);
            } else if (l.id == 2) {
                g.DrawLine(&pen, cx - a * 1.2f, cy, cx + a * 1.2f, cy);
            } else {
                PointF up[] = {PointF(cx - a, cy + a * 0.2f), PointF(cx - a, cy - a), PointF(cx + a * 0.2f, cy - a)};
                PointF dn[] = {PointF(cx + a, cy - a * 0.2f), PointF(cx + a, cy + a), PointF(cx - a * 0.2f, cy + a)};
                SolidBrush gb(l.glyph);
                g.FillPolygon(&gb, up, 3);
                g.FillPolygon(&gb, dn, 3);
            }
        }
    }

    const Tab* active = ActiveTab();
    auto button = [&](const RECT& rc, Icon icon, Hit kind, bool enabled, bool on = false) {
        const bool hover = m_hover.kind == kind && enabled;
        const bool pressed = m_pressed.kind == kind && hover;
        if (pressed) FillRound(g, F(rc), static_cast<REAL>(S(6)), C(70, 74, 82));
        else if (hover || on) FillRound(g, F(rc), static_cast<REAL>(S(6)), C(58, 61, 68));
        const Color col = enabled ? C(200, 202, 208) : C(200, 202, 208, 90);
        RectF box = F(rc);
        const REAL inset = (box.Height - S(17)) / 2;
        box.Inflate(-inset, -inset);
        Icons::Draw(g, icon, box, col);
    };
    button(m_rcSidebarBtn, Icon::Sidebar, Hit::Sidebar, true, m_sidebarVisible);
    button(m_rcBack, Icon::Back, Hit::Back, active && active->canGoBack);
    button(m_rcForward, Icon::Forward, Hit::Forward, active && active->canGoForward);
    button(m_rcShare, Icon::Share, Hit::Share, active && !DisplayUrl(*active).empty());
    button(m_rcNewTab, Icon::Plus, Hit::NewTab, true);
    button(m_rcOverviewBtn, Icon::Grid, Hit::Overview, true, m_overviewVisible);
    button(m_rcMenuBtn, Icon::More, Hit::Menu, true);

    Font uiFont(hdc, m_hUiFont);
    Font addressFont(hdc, m_hAddressFont);
    Font smallFont(hdc, m_hSmallFont);

    if (m_private) {
        FillRound(g, F(m_rcPrivateBadge), static_cast<REAL>(S(11)), C(92, 62, 150));
        Text(g, L"无痕", smallFont, F(m_rcPrivateBadge), C(240, 236, 255), StringAlignmentCenter);
    }

    // Tabs
    const REAL radius = static_cast<REAL>(S(8));
    for (const auto& slot : m_slots) {
        const Tab* tab = FindTab(slot.id);
        if (!tab) continue;
        RECT rc = slot.rc;
        if (m_dragging && slot.id == m_dragTabId) continue;  // drawn last, under the cursor
        const bool isActive = slot.id == m_activeId;
        const bool hover = (m_hover.kind == Hit::Tab || m_hover.kind == Hit::TabClose || m_hover.kind == Hit::TabAudio) && m_hover.tabId == slot.id;
        const RectF rf = F(rc);
        if (isActive) {
            FillRound(g, rf, radius, m_isAddressFocused ? C(30, 32, 37) : (m_private ? C(58, 50, 74) : C(59, 63, 71)));
            StrokeRound(g, rf, radius, m_isAddressFocused ? C(61, 139, 253) : C(75, 79, 88), m_isAddressFocused ? 1.6f : 1.0f);
            if (!m_isAddressFocused) {
                // Reader button
                const bool readerOn = !tab->readerSource.empty();
                const bool readerHover = m_hover.kind == Hit::Reader;
                if (readerOn || readerHover) FillRound(g, F(m_rcReader), static_cast<REAL>(S(5)), readerOn ? C(61, 111, 240, 90) : C(255, 255, 255, 22));
                RectF rb = F(m_rcReader);
                rb.Inflate(-static_cast<REAL>(S(5)), -static_cast<REAL>(S(5)));
                Icons::Draw(g, Icon::Reader, rb,
                    readerOn ? C(120, 170, 255) : (tab->readerAvailable ? C(200, 202, 208) : C(200, 202, 208, 70)));
                // Address text
                const std::wstring url = DisplayUrl(*tab);
                std::wstring label;
                bool secure = false;
                if (url.empty()) {
                    label = InternalPages::IsInternal(tab->url) && !tab->title.empty() && InternalPages::PageName(tab->url) != L"start.html"
                        ? tab->title : L"搜索或输入网站名称";
                } else {
                    label = HostOf(url);
                    if (label.rfind(L"www.", 0) == 0) label = label.substr(4);
                    if (label.empty()) label = url;
                    secure = url.rfind(L"https://", 0) == 0;
                }
                const RectF textBox = F(m_rcAddressText);
                // Centre "lock + host" inside the capsule.
                RectF measured;
                StringFormat measureFmt(StringFormatFlagsNoWrap);
                g.MeasureString(label.c_str(), static_cast<INT>(label.size()), &addressFont, textBox, &measureFmt, &measured);
                const REAL lockW = secure ? static_cast<REAL>(S(16)) : 0;
                const REAL total = (std::min)(textBox.Width, measured.Width + lockW);
                REAL startX = textBox.X + (textBox.Width - total) / 2;
                if (secure) {
                    Icons::Draw(g, Icon::Lock, RectF(startX, textBox.Y + (textBox.Height - S(13)) / 2, static_cast<REAL>(S(13)), static_cast<REAL>(S(13))),
                                C(154, 208, 138));
                    startX += lockW;
                }
                Text(g, label, addressFont, RectF(startX, textBox.Y, total - lockW + 2, textBox.Height),
                     url.empty() ? C(150, 152, 160) : C(240, 240, 244));
                // Reload / stop
                if (m_hover.kind == Hit::Reload) FillRound(g, F(m_rcReload), static_cast<REAL>(S(5)), C(255, 255, 255, 22));
                RectF rr = F(m_rcReload);
                rr.Inflate(-static_cast<REAL>(S(5)), -static_cast<REAL>(S(5)));
                Icons::Draw(g, tab->loading ? Icon::Stop : Icon::Reload, rr, C(170, 174, 182));
            }
            if (tab->loading) {
                const double elapsed = static_cast<double>(GetTickCount64() - tab->loadStarted);
                const double p = 0.12 + 0.78 * (1.0 - std::exp(-elapsed / 2500.0));
                const REAL barW = static_cast<REAL>((rc.right - rc.left - 2 * S(6)) * p);
                SolidBrush blue(C(61, 139, 253));
                g.FillRectangle(&blue, static_cast<REAL>(rc.left + S(6)), static_cast<REAL>(rc.bottom - S(2)), barW, static_cast<REAL>(S(2)));
            }
        } else {
            FillRound(g, rf, radius, hover ? C(56, 59, 66) : (m_private ? C(44, 38, 56) : C(47, 50, 56)));
        }

        if (!isActive || m_isAddressFocused) {
            if (isActive) continue;
            // Favicon or close button, then the title
            const bool closeHover = m_hover.kind == Hit::TabClose && m_hover.tabId == slot.id;
            const RectF iconBox(static_cast<REAL>(slot.close.left + S(2)), static_cast<REAL>(slot.close.top + S(2)),
                                static_cast<REAL>(S(16)), static_cast<REAL>(S(16)));
            if (hover) {
                if (closeHover) FillRound(g, F(slot.close), static_cast<REAL>(S(4)), C(255, 255, 255, 30));
                RectF cb = iconBox;
                cb.Inflate(-static_cast<REAL>(S(2)), -static_cast<REAL>(S(2)));
                Icons::Draw(g, Icon::Close, cb, C(210, 212, 218));
            } else if (tab->favicon) {
                g.DrawImage(tab->favicon.get(), iconBox);
            } else {
                const std::wstring host = HostOf(tab->url);
                const std::wstring letterSrc = host.rfind(L"www.", 0) == 0 ? host.substr(4) : host;
                std::wstring letter = letterSrc.empty() ? L"•" : std::wstring(1, static_cast<wchar_t>(towupper(letterSrc[0])));
                if (InternalPages::IsInternal(tab->url)) letter = L"★";
                FillRound(g, iconBox, static_cast<REAL>(S(4)), InternalPages::IsInternal(tab->url) ? C(61, 111, 240) : HslColor(Hue(host), 0.45, 0.42));
                Text(g, letter, smallFont, iconBox, C(255, 255, 255), StringAlignmentCenter);
            }
            const int textLeft = slot.close.right + S(6);
            const int textRight = (slot.audio.right > slot.audio.left ? slot.audio.left : slot.rc.right) - S(8);
            if (textRight - textLeft > S(10)) {
                std::wstring title = tab->title;
                if (title.empty()) title = HostOf(tab->url);
                if (title.empty()) title = L"新标签页";
                Text(g, title, uiFont, RectF(static_cast<REAL>(textLeft), rf.Y, static_cast<REAL>(textRight - textLeft), rf.Height),
                     hover ? C(236, 236, 240) : C(169, 171, 178));
            }
        }
        if (slot.audio.right > slot.audio.left && !(isActive && m_isAddressFocused)) {
            if (m_hover.kind == Hit::TabAudio && m_hover.tabId == slot.id) {
                FillRound(g, F(slot.audio), static_cast<REAL>(S(4)), C(255, 255, 255, 30));
            }
            RectF ab = F(slot.audio);
            ab.Inflate(-static_cast<REAL>(S(3)), -static_cast<REAL>(S(3)));
            Icons::Draw(g, tab->muted ? Icon::SpeakerMute : Icon::Speaker, ab, C(156, 195, 255));
        }
    }

    // Dragged tab follows the cursor.
    if (m_dragging) {
        for (const auto& slot : m_slots) {
            if (slot.id != m_dragTabId) continue;
            const Tab* tab = FindTab(slot.id);
            POINT pt{};
            GetCursorPos(&pt);
            ScreenToClient(m_hWnd, &pt);
            const int w = slot.rc.right - slot.rc.left;
            const int left = std::clamp(static_cast<int>(pt.x - m_dragOffsetX), static_cast<int>(m_rcStrip.left), static_cast<int>(m_rcStrip.right - w));
            const RectF rf(static_cast<REAL>(left), static_cast<REAL>(slot.rc.top), static_cast<REAL>(w), static_cast<REAL>(slot.rc.bottom - slot.rc.top));
            FillRound(g, rf, radius, C(70, 74, 84));
            StrokeRound(g, rf, radius, C(95, 99, 110));
            if (tab) {
                std::wstring title = tab->title.empty() ? HostOf(tab->url) : tab->title;
                Text(g, title, uiFont, RectF(rf.X + S(30), rf.Y, rf.Width - S(38), rf.Height), C(236, 236, 240));
            }
        }
    }

    if (m_overflowCount > 0) {
        const bool hover = m_hover.kind == Hit::Overflow;
        FillRound(g, F(m_rcOverflow), static_cast<REAL>(S(7)), hover ? C(62, 66, 74) : C(50, 53, 60));
        Text(g, L"+" + std::to_wstring(m_overflowCount), smallFont, F(m_rcOverflow), C(200, 202, 208), StringAlignmentCenter);
    }
}

void MainWindow::PaintFindBar(HDC hdc) {
    if (!m_findVisible || m_rcFindBar.right <= m_rcFindBar.left) return;
    Graphics g(hdc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    SolidBrush bg(C(40, 42, 48));
    g.FillRectangle(&bg, F(m_rcFindBar));
    Pen sep(C(24, 25, 29), 1);
    g.DrawLine(&sep, static_cast<REAL>(m_rcFindBar.left), m_rcFindBar.bottom - 0.5f, static_cast<REAL>(m_rcFindBar.right), m_rcFindBar.bottom - 0.5f);

    Font uiFont(hdc, m_hUiFont);
    // Search field background
    if (m_hFindEdit) {
        RECT er{};
        GetWindowRect(m_hFindEdit, &er);
        MapWindowPoints(nullptr, m_hWnd, reinterpret_cast<POINT*>(&er), 2);
        RectF field(static_cast<REAL>(er.left - S(26)), static_cast<REAL>(m_rcFindPrev.top), static_cast<REAL>(er.right - er.left + S(32)),
                    static_cast<REAL>(m_rcFindPrev.bottom - m_rcFindPrev.top));
        FillRound(g, field, static_cast<REAL>(S(7)), C(30, 32, 37));
        StrokeRound(g, field, static_cast<REAL>(S(7)), GetFocus() == m_hFindEdit ? C(61, 139, 253) : C(70, 73, 80));
        Icons::Draw(g, Icon::Search, RectF(field.X + S(7), field.Y + (field.Height - S(14)) / 2, static_cast<REAL>(S(14)), static_cast<REAL>(S(14))),
                    C(150, 152, 160));
        Text(g, L"在页面中查找", uiFont, RectF(static_cast<REAL>(m_rcFindBar.left + S(14)), static_cast<REAL>(m_rcFindBar.top),
             field.X - m_rcFindBar.left - S(20), static_cast<REAL>(m_rcFindBar.bottom - m_rcFindBar.top)), C(150, 152, 160));
    }
    std::wstring status;
    if (!m_findQuery.empty()) {
        status = m_findCount == 0 ? L"未找到" : (std::to_wstring(m_findIndex) + L" / " + std::to_wstring(m_findCount));
    }
    Text(g, status, uiFont, F(m_rcFindStatus), m_findCount == 0 && !m_findQuery.empty() ? C(255, 140, 130) : C(170, 172, 180),
         StringAlignmentFar);
    auto btn = [&](const RECT& rc, Hit kind) {
        if (m_hover.kind == kind) FillRound(g, F(rc), static_cast<REAL>(S(6)), C(62, 66, 74));
        else FillRound(g, F(rc), static_cast<REAL>(S(6)), C(52, 55, 62));
    };
    btn(m_rcFindPrev, Hit::FindPrev);
    btn(m_rcFindNext, Hit::FindNext);
    btn(m_rcFindDone, Hit::FindDone);
    RectF pb = F(m_rcFindPrev);
    pb.Inflate(-static_cast<REAL>(S(6)), -static_cast<REAL>(S(5)));
    Icons::Draw(g, Icon::Up, pb, C(210, 212, 218));
    RectF nb = F(m_rcFindNext);
    nb.Inflate(-static_cast<REAL>(S(6)), -static_cast<REAL>(S(5)));
    Icons::Draw(g, Icon::Down, nb, C(210, 212, 218));
    Text(g, L"完成", uiFont, F(m_rcFindDone), C(230, 232, 236), StringAlignmentCenter);
}

MainWindow::HitResult MainWindow::HitTest(POINT pt) const {
    if (m_isFullScreen) return {};
    if (m_findVisible && PtInRect(&m_rcFindBar, pt)) {
        if (PtInRect(&m_rcFindPrev, pt)) return {Hit::FindPrev, 0};
        if (PtInRect(&m_rcFindNext, pt)) return {Hit::FindNext, 0};
        if (PtInRect(&m_rcFindDone, pt)) return {Hit::FindDone, 0};
        return {};
    }
    if (pt.y < 0 || pt.y >= m_topbarHeight) return {};
    if (PtInRect(&m_rcTrafficGroup, pt)) return {Hit::Traffic, 0};
    if (PtInRect(&m_rcSidebarBtn, pt)) return {Hit::Sidebar, 0};
    if (PtInRect(&m_rcBack, pt)) return {Hit::Back, 0};
    if (PtInRect(&m_rcForward, pt)) return {Hit::Forward, 0};
    if (PtInRect(&m_rcShare, pt)) return {Hit::Share, 0};
    if (PtInRect(&m_rcNewTab, pt)) return {Hit::NewTab, 0};
    if (PtInRect(&m_rcOverviewBtn, pt)) return {Hit::Overview, 0};
    if (PtInRect(&m_rcMenuBtn, pt)) return {Hit::Menu, 0};
    if (m_overflowCount > 0 && PtInRect(&m_rcOverflow, pt)) return {Hit::Overflow, 0};
    for (const auto& slot : m_slots) {
        if (!PtInRect(&slot.rc, pt)) continue;
        if (slot.audio.right > slot.audio.left && PtInRect(&slot.audio, pt)) return {Hit::TabAudio, slot.id};
        if (slot.id == m_activeId) {
            if (m_isAddressFocused) return {Hit::Address, slot.id};
            if (PtInRect(&m_rcReader, pt)) return {Hit::Reader, slot.id};
            if (PtInRect(&m_rcReload, pt)) return {Hit::Reload, slot.id};
            return {Hit::Address, slot.id};
        }
        if (PtInRect(&slot.close, pt)) return {Hit::TabClose, slot.id};
        return {Hit::Tab, slot.id};
    }
    return {};
}

void MainWindow::OnToolbarMouseMove(POINT pt) {
    if (!m_trackingMouse) {
        TRACKMOUSEEVENT tme{sizeof(TRACKMOUSEEVENT), TME_LEAVE, m_hWnd, 0};
        TrackMouseEvent(&tme);
        m_trackingMouse = true;
    }
    // Tab drag
    if (m_dragTabId && (GetKeyState(VK_LBUTTON) & 0x8000)) {
        if (!m_dragging && std::abs(pt.x - m_dragStartX) > S(6)) m_dragging = true;
        if (m_dragging) {
            int target = 0;
            for (const auto& slot : m_slots) {
                if (pt.x > (slot.rc.left + slot.rc.right) / 2) target = IndexOf(slot.id) + (slot.id == m_dragTabId ? 0 : 1);
            }
            const int from = IndexOf(m_dragTabId);
            if (target > from) --target;
            if (target != from && target >= 0) MoveTab(m_dragTabId, target);
            InvalidateToolbar();
            return;
        }
    }

    const HitResult hit = HitTest(pt);
    // Traffic light hover state
    const bool inTraffic = hit.kind == Hit::Traffic;
    int trafficBtn = 0;
    if (inTraffic) {
        if (PtInRect(&m_rcTrafficClose, pt)) trafficBtn = 1;
        else if (PtInRect(&m_rcTrafficMin, pt)) trafficBtn = 2;
        else if (PtInRect(&m_rcTrafficMax, pt)) trafficBtn = 3;
    }
    if (inTraffic != m_isTrafficGroupHovered || trafficBtn != m_trafficHoveredBtn ||
        hit.kind != m_hover.kind || hit.tabId != m_hover.tabId) {
        m_isTrafficGroupHovered = inTraffic;
        m_trafficHoveredBtn = trafficBtn;
        const bool findChange = m_hover.kind >= Hit::FindPrev || hit.kind >= Hit::FindPrev;
        m_hover = hit;
        InvalidateToolbar();
        if (findChange && m_findVisible) InvalidateRect(m_hWnd, &m_rcFindBar, FALSE);
    }
}

void MainWindow::OnToolbarButtonDown(POINT pt) {
    const HitResult hit = HitTest(pt);
    m_pressed = hit;
    if (hit.kind == Hit::Traffic) {
        if (PtInRect(&m_rcTrafficClose, pt)) m_trafficPressedBtn = 1;
        else if (PtInRect(&m_rcTrafficMin, pt)) m_trafficPressedBtn = 2;
        else if (PtInRect(&m_rcTrafficMax, pt)) m_trafficPressedBtn = 3;
    }
    if (hit.kind == Hit::Tab) {
        m_dragTabId = hit.tabId;
        m_dragStartX = pt.x;
        for (const auto& slot : m_slots) {
            if (slot.id == hit.tabId) m_dragOffsetX = pt.x - slot.rc.left;
        }
        m_dragging = false;
    }
    if (hit.kind != Hit::None) SetCapture(m_hWnd);
    InvalidateToolbar();
}

void MainWindow::OnToolbarButtonUp(POINT pt) {
    if (GetCapture() == m_hWnd) ReleaseCapture();
    const HitResult pressed = m_pressed;
    m_pressed = {};
    const int pressedTraffic = m_trafficPressedBtn;
    m_trafficPressedBtn = 0;
    if (m_dragging) {
        m_dragging = false;
        m_dragTabId = 0;
        UpdateLayout();
        TabsChanged();
        return;
    }
    m_dragTabId = 0;
    const HitResult hit = HitTest(pt);
    InvalidateToolbar();
    if (hit.kind != pressed.kind || hit.tabId != pressed.tabId) return;

    switch (hit.kind) {
    case Hit::Traffic:
        if (pressedTraffic == 1 && PtInRect(&m_rcTrafficClose, pt)) PostMessageW(m_hWnd, WM_CLOSE, 0, 0);
        else if (pressedTraffic == 2 && PtInRect(&m_rcTrafficMin, pt)) ShowWindow(m_hWnd, SW_MINIMIZE);
        else if (pressedTraffic == 3 && PtInRect(&m_rcTrafficMax, pt)) ShowWindow(m_hWnd, IsZoomed(m_hWnd) ? SW_RESTORE : SW_MAXIMIZE);
        break;
    case Hit::Sidebar: OnCommand(IDM_SIDEBAR); break;
    case Hit::Back: OnCommand(IDM_BACK); break;
    case Hit::Forward: OnCommand(IDM_FORWARD); break;
    case Hit::Share: ShowShareMenu(); break;
    case Hit::NewTab: OnCommand(IDM_NEW_TAB); break;
    case Hit::Overview:
    case Hit::Overflow: OnCommand(IDM_OVERVIEW); break;
    case Hit::Menu: ShowMainMenu(); break;
    case Hit::Tab: ActivateTab(hit.tabId); break;
    case Hit::TabClose: CloseTab(hit.tabId); break;
    case Hit::TabAudio:
        if (Tab* t = FindTab(hit.tabId)) SetTabMuted(*t, !t->muted);
        break;
    case Hit::Reader: OnCommand(IDM_READER); break;
    case Hit::Reload: OnCommand(ActiveTab() && ActiveTab()->loading ? IDM_STOP : IDM_RELOAD); break;
    case Hit::Address: {
        const Tab* t = ActiveTab();
        BeginAddressEdit(t ? DisplayUrl(*t) : L"", true);
        break;
    }
    case Hit::FindPrev: RunFind(L"prev"); break;
    case Hit::FindNext: RunFind(L"next"); break;
    case Hit::FindDone: HideFindBar(); break;
    default: break;
    }
}

void MainWindow::OnToolbarMiddleUp(POINT pt) {
    const HitResult hit = HitTest(pt);
    if (hit.kind == Hit::Tab || hit.kind == Hit::TabClose || hit.kind == Hit::TabAudio ||
        (hit.kind == Hit::Address && m_tabs.size() > 1) || hit.kind == Hit::Reader || hit.kind == Hit::Reload) {
        CloseTab(hit.tabId);
    } else if (hit.kind == Hit::None && pt.y < m_topbarHeight && pt.x > m_rcStrip.left && pt.x < m_rcStrip.right) {
        OnCommand(IDM_NEW_TAB);
    }
}

void MainWindow::OnToolbarRightUp(POINT pt) {
    const HitResult hit = HitTest(pt);
    if (hit.tabId != 0) {
        POINT screen = pt;
        ClientToScreen(m_hWnd, &screen);
        ShowTabMenu(hit.tabId, screen);
    }
}

} // namespace UltraLight
