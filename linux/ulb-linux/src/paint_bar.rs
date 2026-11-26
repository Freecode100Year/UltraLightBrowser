use crate::engine::TabState;
use crate::icons::{draw_icon, IconKind};
use gtk4::cairo::{self, Context};
use std::f64::consts::PI;
use ulb_core::internal_pages::{is_internal_url, page_name};
use ulb_core::library::Library;
use ulb_core::util::now_epoch_ms;

pub const TOPBAR_HEIGHT: f64 = 46.0;

#[derive(Debug, Clone, Copy, Default, PartialEq)]
pub struct Rect {
    pub x: f64,
    pub y: f64,
    pub w: f64,
    pub h: f64,
}

impl Rect {
    pub fn contains(&self, px: f64, py: f64) -> bool {
        self.w > 0.0
            && self.h > 0.0
            && px >= self.x
            && px <= self.x + self.w
            && py >= self.y
            && py <= self.y + self.h
    }

    pub fn right(&self) -> f64 {
        self.x + self.w
    }

    pub fn bottom(&self) -> f64 {
        self.y + self.h
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum HitKind {
    #[default]
    None,
    TrafficClose,
    TrafficMin,
    TrafficMax,
    Sidebar,
    Back,
    Forward,
    Share,
    NewTab,
    Overview,
    Menu,
    Tab,
    TabClose,
    TabAudio,
    Overflow,
    Address,
    Reader,
    Reload,
    DragArea,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct HitResult {
    pub kind: HitKind,
    pub tab_id: u64,
}

#[derive(Debug, Clone)]
pub struct TabSlot {
    pub id: u64,
    pub rc: Rect,
    pub close: Rect,
    pub audio: Rect,
}

#[derive(Debug, Clone, Default)]
pub struct ToolbarLayout {
    pub width: f64,
    pub traffic_group: Rect,
    pub traffic_close: Rect,
    pub traffic_min: Rect,
    pub traffic_max: Rect,
    pub sidebar_btn: Rect,
    pub back_btn: Rect,
    pub forward_btn: Rect,
    pub private_badge: Rect,
    pub strip: Rect,
    pub overflow: Rect,
    pub overflow_count: usize,
    pub share_btn: Rect,
    pub new_tab_btn: Rect,
    pub overview_btn: Rect,
    pub menu_btn: Rect,
    pub active_pill: Rect,
    pub reader_btn: Rect,
    pub reload_btn: Rect,
    pub address_text: Rect,
    pub slots: Vec<TabSlot>,
}

/// Computes Safari-style single-row toolbar & compact tab pill coordinates.
pub fn compute_layout(
    width: f64,
    tabs: &[TabState],
    active_id: u64,
    private_mode: bool,
) -> ToolbarLayout {
    let mut lo = ToolbarLayout {
        width,
        ..ToolbarLayout::default()
    };
    if width <= 0.0 {
        return lo;
    }

    let cy = TOPBAR_HEIGHT / 2.0;
    let r = 6.0;
    let gap = 8.0;
    let close_x = 20.0;
    let min_x = close_x + 2.0 * r + gap;
    let max_x = min_x + 2.0 * r + gap;

    lo.traffic_close = Rect {
        x: close_x - r,
        y: cy - r,
        w: 2.0 * r,
        h: 2.0 * r,
    };
    lo.traffic_min = Rect {
        x: min_x - r,
        y: cy - r,
        w: 2.0 * r,
        h: 2.0 * r,
    };
    lo.traffic_max = Rect {
        x: max_x - r,
        y: cy - r,
        w: 2.0 * r,
        h: 2.0 * r,
    };
    lo.traffic_group = Rect {
        x: lo.traffic_close.x - 4.0,
        y: lo.traffic_close.y - 4.0,
        w: lo.traffic_max.right() - lo.traffic_close.x + 8.0,
        h: 2.0 * r + 8.0,
    };

    let bw = 30.0;
    let bh = 28.0;
    let by = (TOPBAR_HEIGHT - bh) / 2.0;
    let mut x = lo.traffic_max.right() + 16.0;
    lo.sidebar_btn = Rect { x, y: by, w: bw, h: bh };
    x += bw + 4.0;
    lo.back_btn = Rect { x, y: by, w: bw, h: bh };
    x += bw + 2.0;
    lo.forward_btn = Rect { x, y: by, w: bw, h: bh };
    x += bw + 10.0;

    if private_mode {
        lo.private_badge = Rect {
            x,
            y: cy - 11.0,
            w: 64.0,
            h: 22.0,
        };
        x += 72.0;
    }

    let mut rx = width - 12.0;
    lo.menu_btn = Rect { x: rx - bw, y: by, w: bw, h: bh };
    rx -= bw + 6.0;
    lo.overview_btn = Rect { x: rx - bw, y: by, w: bw, h: bh };
    rx -= bw + 6.0;
    lo.new_tab_btn = Rect { x: rx - bw, y: by, w: bw, h: bh };
    rx -= bw + 6.0;
    lo.share_btn = Rect { x: rx - bw, y: by, w: bw, h: bh };
    rx -= 10.0;

    let th = 34.0;
    let ty = (TOPBAR_HEIGHT - th) / 2.0;
    lo.strip = Rect {
        x,
        y: ty,
        w: (rx - x).max(0.0),
        h: th,
    };

    let n = tabs.len();
    let strip_w = lo.strip.w;
    if n == 0 || strip_w <= 0.0 {
        return lo;
    }

    let tgap = 6.0;
    let active_idx = tabs.iter().position(|t| t.id == active_id).unwrap_or(0);
    let active_w: f64;
    let mut inactive_w = 0.0;
    let mut first = 0usize;
    let mut last = n - 1;

    if n == 1 {
        active_w = strip_w.min(680.0);
    } else {
        let nf = (n - 1) as f64;
        let min_active = strip_w.min(300.0);
        active_w = (strip_w - nf * (180.0 + tgap)).clamp(min_active, min_active.max(680.0));
        inactive_w = ((strip_w - active_w - nf * tgap) / nf).clamp(36.0, 240.0);
        if active_w + nf * (inactive_w + tgap) > strip_w {
            let chip_w = 44.0;
            let fit = (((strip_w - active_w - chip_w - tgap) / (inactive_w + tgap)).floor() as isize)
                .max(0) as usize;
            let mut before = active_idx.min(fit / 2);
            let after = (n - 1 - active_idx).min(fit.saturating_sub(before));
            before = active_idx.min(fit.saturating_sub(after));
            first = active_idx - before;
            last = active_idx + after;
            lo.overflow_count = n - (last - first + 1);
        }
    }

    let mut cx = if n == 1 {
        lo.strip.x + (strip_w - active_w) / 2.0
    } else {
        lo.strip.x
    };

    for t in &tabs[first..=last] {
        let is_active = t.id == active_id;
        let w = if is_active { active_w } else { inactive_w };
        let rc = Rect { x: cx, y: ty, w, h: th };
        let icon_box = 20.0;
        let close = Rect {
            x: rc.x + 6.0,
            y: ty + (th - icon_box) / 2.0,
            w: icon_box,
            h: icon_box,
        };
        let mut audio = if t.audio || t.muted {
            Rect {
                x: rc.right() - 6.0 - icon_box,
                y: close.y,
                w: icon_box,
                h: icon_box,
            }
        } else {
            Rect::default()
        };

        if is_active {
            lo.active_pill = rc;
            lo.reader_btn = Rect {
                x: rc.x + 6.0,
                y: ty + 5.0,
                w: 26.0,
                h: th - 10.0,
            };
            lo.reload_btn = Rect {
                x: rc.right() - 32.0,
                y: ty + 5.0,
                w: 26.0,
                h: th - 10.0,
            };
            if t.audio || t.muted {
                audio = Rect {
                    x: lo.reload_btn.x - 26.0,
                    y: lo.reload_btn.y,
                    w: 24.0,
                    h: lo.reload_btn.h,
                };
            }
            let text_right = if audio.w > 0.0 {
                audio.x - 4.0
            } else {
                lo.reload_btn.x - 4.0
            };
            lo.address_text = Rect {
                x: lo.reader_btn.right() + 4.0,
                y: ty + 4.0,
                w: (text_right - (lo.reader_btn.right() + 4.0)).max(0.0),
                h: th - 8.0,
            };
        }

        lo.slots.push(TabSlot {
            id: t.id,
            rc,
            close,
            audio,
        });
        cx += w + tgap;
    }

    if lo.overflow_count > 0 {
        lo.overflow = Rect {
            x: cx,
            y: ty + 3.0,
            w: 44.0,
            h: th - 6.0,
        };
    }
    lo
}

pub fn hit_test(lo: &ToolbarLayout, active_id: u64, address_focused: bool, x: f64, y: f64) -> HitResult {
    if !(0.0..=TOPBAR_HEIGHT).contains(&y) {
        return HitResult::default();
    }
    if lo.traffic_close.contains(x, y) {
        return HitResult { kind: HitKind::TrafficClose, tab_id: 0 };
    }
    if lo.traffic_min.contains(x, y) {
        return HitResult { kind: HitKind::TrafficMin, tab_id: 0 };
    }
    if lo.traffic_max.contains(x, y) {
        return HitResult { kind: HitKind::TrafficMax, tab_id: 0 };
    }
    if lo.sidebar_btn.contains(x, y) {
        return HitResult { kind: HitKind::Sidebar, tab_id: 0 };
    }
    if lo.back_btn.contains(x, y) {
        return HitResult { kind: HitKind::Back, tab_id: 0 };
    }
    if lo.forward_btn.contains(x, y) {
        return HitResult { kind: HitKind::Forward, tab_id: 0 };
    }
    if lo.share_btn.contains(x, y) {
        return HitResult { kind: HitKind::Share, tab_id: 0 };
    }
    if lo.new_tab_btn.contains(x, y) {
        return HitResult { kind: HitKind::NewTab, tab_id: 0 };
    }
    if lo.overview_btn.contains(x, y) {
        return HitResult { kind: HitKind::Overview, tab_id: 0 };
    }
    if lo.menu_btn.contains(x, y) {
        return HitResult { kind: HitKind::Menu, tab_id: 0 };
    }
    if lo.overflow_count > 0 && lo.overflow.contains(x, y) {
        return HitResult { kind: HitKind::Overflow, tab_id: 0 };
    }
    for slot in &lo.slots {
        if !slot.rc.contains(x, y) {
            continue;
        }
        if slot.audio.contains(x, y) {
            return HitResult { kind: HitKind::TabAudio, tab_id: slot.id };
        }
        if slot.id == active_id {
            if address_focused {
                return HitResult { kind: HitKind::Address, tab_id: slot.id };
            }
            if lo.reader_btn.contains(x, y) {
                return HitResult { kind: HitKind::Reader, tab_id: slot.id };
            }
            if lo.reload_btn.contains(x, y) {
                return HitResult { kind: HitKind::Reload, tab_id: slot.id };
            }
            return HitResult { kind: HitKind::Address, tab_id: slot.id };
        }
        if slot.close.contains(x, y) {
            return HitResult { kind: HitKind::TabClose, tab_id: slot.id };
        }
        return HitResult { kind: HitKind::Tab, tab_id: slot.id };
    }
    HitResult { kind: HitKind::DragArea, tab_id: 0 }
}

fn round_rect(cr: &Context, r: Rect, radius: f64) {
    let rad = radius.min(r.w * 0.5).min(r.h * 0.5);
    cr.new_sub_path();
    cr.arc(r.x + r.w - rad, r.y + rad, rad, -PI * 0.5, 0.0);
    cr.arc(r.x + r.w - rad, r.y + r.h - rad, rad, 0.0, PI * 0.5);
    cr.arc(r.x + rad, r.y + r.h - rad, rad, PI * 0.5, PI);
    cr.arc(r.x + rad, r.y + rad, rad, PI, PI * 1.5);
    cr.close_path();
}

fn fill_round(cr: &Context, r: Rect, rad: f64, rgba: (f64, f64, f64, f64)) {
    round_rect(cr, r, rad);
    cr.set_source_rgba(rgba.0, rgba.1, rgba.2, rgba.3);
    let _ = cr.fill();
}

fn stroke_round(cr: &Context, r: Rect, rad: f64, rgba: (f64, f64, f64, f64), width: f64) {
    round_rect(cr, r, rad);
    cr.set_source_rgba(rgba.0, rgba.1, rgba.2, rgba.3);
    cr.set_line_width(width);
    let _ = cr.stroke();
}

fn draw_centered_text(cr: &Context, r: Rect, text: &str, size: f64, rgba: (f64, f64, f64, f64), center_h: bool) {
    if text.is_empty() || r.w <= 4.0 {
        return;
    }
    let _ = cr.save();
    cr.rectangle(r.x, r.y, r.w, r.h);
    cr.clip();
    cr.select_font_face("Sans", cairo::FontSlant::Normal, cairo::FontWeight::Normal);
    cr.set_font_size(size);
    cr.set_source_rgba(rgba.0, rgba.1, rgba.2, rgba.3);
    if let Ok(ext) = cr.text_extents(text) {
        let ty = r.y + (r.h - ext.height()) / 2.0 - ext.y_bearing();
        let tx = if center_h {
            r.x + ((r.w - ext.width()) / 2.0).max(0.0)
        } else {
            r.x
        };
        cr.move_to(tx, ty);
        let _ = cr.show_text(text);
    }
    let _ = cr.restore();
}

fn display_url(tab: &TabState) -> &str {
    if !tab.reader_source.is_empty() {
        return &tab.reader_source;
    }
    if is_internal_url(&tab.url) || ulb_core::config::is_default_home_url(&tab.url) {
        return "";
    }
    &tab.url
}

/// Visual paint parameters passed to Cairo toolbar renderer.
pub struct PaintParams<'a> {
    pub layout: &'a ToolbarLayout,
    pub tabs: &'a [TabState],
    pub active_id: u64,
    pub private_mode: bool,
    pub sidebar_visible: bool,
    pub overview_visible: bool,
    pub address_focused: bool,
    pub window_active: bool,
    pub traffic_hover: bool,
    pub hover: HitResult,
}

pub fn paint_toolbar(cr: &Context, p: &PaintParams<'_>) {
    let w = p.layout.width;
    let h = TOPBAR_HEIGHT;

    let grad = cairo::LinearGradient::new(0.0, 0.0, 0.0, h);
    if p.private_mode {
        grad.add_color_stop_rgb(0.0, 40.0 / 255.0, 34.0 / 255.0, 52.0 / 255.0);
        grad.add_color_stop_rgb(1.0, 32.0 / 255.0, 27.0 / 255.0, 43.0 / 255.0);
    } else {
        grad.add_color_stop_rgb(0.0, 44.0 / 255.0, 46.0 / 255.0, 52.0 / 255.0);
        grad.add_color_stop_rgb(1.0, 38.0 / 255.0, 40.0 / 255.0, 45.0 / 255.0);
    }
    let _ = cr.set_source(&grad);
    cr.rectangle(0.0, 0.0, w, h);
    let _ = cr.fill();

    cr.set_source_rgb(21.0 / 255.0, 22.0 / 255.0, 26.0 / 255.0);
    cr.set_line_width(1.0);
    cr.move_to(0.0, h - 0.5);
    cr.line_to(w, h - 0.5);
    let _ = cr.stroke();

    // Traffic lights
    let lit = p.window_active || p.traffic_hover;
    let lights = [
        (p.layout.traffic_close, (1.0, 95.0 / 255.0, 87.0 / 255.0), HitKind::TrafficClose),
        (p.layout.traffic_min, (254.0 / 255.0, 188.0 / 255.0, 46.0 / 255.0), HitKind::TrafficMin),
        (p.layout.traffic_max, (40.0 / 255.0, 200.0 / 255.0, 64.0 / 255.0), HitKind::TrafficMax),
    ];
    for (rc, rgb, kind) in lights {
        let c = if !lit {
            (78.0 / 255.0, 78.0 / 255.0, 84.0 / 255.0)
        } else {
            rgb
        };
        cr.set_source_rgb(c.0, c.1, c.2);
        cr.arc(rc.x + rc.w / 2.0, rc.y + rc.h / 2.0, rc.w / 2.0, 0.0, 2.0 * PI);
        let _ = cr.fill();
        if p.traffic_hover {
            let cx = rc.x + rc.w / 2.0;
            let cy = rc.y + rc.h / 2.0;
            let a = rc.w * 0.24;
            cr.set_source_rgba(0.15, 0.08, 0.05, 0.85);
            cr.set_line_width(1.2);
            match kind {
                HitKind::TrafficClose => {
                    cr.move_to(cx - a, cy - a);
                    cr.line_to(cx + a, cy + a);
                    cr.move_to(cx - a, cy + a);
                    cr.line_to(cx + a, cy - a);
                    let _ = cr.stroke();
                }
                HitKind::TrafficMin => {
                    cr.move_to(cx - a * 1.15, cy);
                    cr.line_to(cx + a * 1.15, cy);
                    let _ = cr.stroke();
                }
                HitKind::TrafficMax => {
                    cr.move_to(cx - a, cy - a * 0.2);
                    cr.line_to(cx + a, cy + a * 0.2);
                    let _ = cr.stroke();
                }
                _ => {}
            }
        }
    }

    let active = p.tabs.iter().find(|t| t.id == p.active_id);
    let draw_btn = |rc: Rect, icon: IconKind, kind: HitKind, enabled: bool, on: bool| {
        let hover = enabled && p.hover.kind == kind;
        if hover || on {
            fill_round(cr, rc, 6.0, (58.0 / 255.0, 61.0 / 255.0, 68.0 / 255.0, 1.0));
        }
        let col = if enabled {
            (200.0 / 255.0, 202.0 / 255.0, 208.0 / 255.0, 1.0)
        } else {
            (200.0 / 255.0, 202.0 / 255.0, 208.0 / 255.0, 0.35)
        };
        let sz = 17.0;
        draw_icon(cr, icon, rc.x + (rc.w - sz) / 2.0, rc.y + (rc.h - sz) / 2.0, sz, col);
    };

    draw_btn(p.layout.sidebar_btn, IconKind::Sidebar, HitKind::Sidebar, true, p.sidebar_visible);
    draw_btn(p.layout.back_btn, IconKind::Back, HitKind::Back, active.is_some_and(|t| t.can_go_back), false);
    draw_btn(p.layout.forward_btn, IconKind::Forward, HitKind::Forward, active.is_some_and(|t| t.can_go_forward), false);
    draw_btn(p.layout.share_btn, IconKind::Share, HitKind::Share, active.is_some_and(|t| !display_url(t).is_empty()), false);
    draw_btn(p.layout.new_tab_btn, IconKind::Plus, HitKind::NewTab, true, false);
    draw_btn(p.layout.overview_btn, IconKind::Grid, HitKind::Overview, true, p.overview_visible);
    draw_btn(p.layout.menu_btn, IconKind::More, HitKind::Menu, true, false);

    if p.private_mode {
        fill_round(cr, p.layout.private_badge, 11.0, (92.0 / 255.0, 62.0 / 255.0, 150.0 / 255.0, 1.0));
        draw_centered_text(cr, p.layout.private_badge, "无痕浏览", 11.5, (0.94, 0.92, 1.0, 1.0), true);
    }

    // Tabs + active capsule
    for slot in &p.layout.slots {
        let Some(tab) = p.tabs.iter().find(|t| t.id == slot.id) else {
            continue;
        };
        let is_active = slot.id == p.active_id;
        let hover = matches!(p.hover.kind, HitKind::Tab | HitKind::TabClose | HitKind::TabAudio)
            && p.hover.tab_id == slot.id;

        if is_active {
            let fill = if p.address_focused {
                (30.0 / 255.0, 32.0 / 255.0, 37.0 / 255.0, 1.0)
            } else if p.private_mode {
                (58.0 / 255.0, 50.0 / 255.0, 74.0 / 255.0, 1.0)
            } else {
                (59.0 / 255.0, 63.0 / 255.0, 71.0 / 255.0, 1.0)
            };
            let stroke = if p.address_focused {
                (61.0 / 255.0, 139.0 / 255.0, 253.0 / 255.0, 1.0)
            } else {
                (75.0 / 255.0, 79.0 / 255.0, 88.0 / 255.0, 1.0)
            };
            fill_round(cr, slot.rc, 8.0, fill);
            stroke_round(cr, slot.rc, 8.0, stroke, if p.address_focused { 1.6 } else { 1.0 });

            if !p.address_focused {
                let reader_on = !tab.reader_source.is_empty();
                let reader_hover = p.hover.kind == HitKind::Reader;
                if reader_on || reader_hover {
                    fill_round(
                        cr,
                        p.layout.reader_btn,
                        5.0,
                        if reader_on { (0.24, 0.44, 0.94, 0.35) } else { (1.0, 1.0, 1.0, 0.09) },
                    );
                }
                let rcol = if reader_on {
                    (0.47, 0.67, 1.0, 1.0)
                } else if tab.reader_available {
                    (0.78, 0.79, 0.82, 1.0)
                } else {
                    (0.78, 0.79, 0.82, 0.28)
                };
                draw_icon(cr, IconKind::Reader, p.layout.reader_btn.x + 5.0, p.layout.reader_btn.y + 4.0, 16.0, rcol);

                let durl = display_url(tab);
                let label = if durl.is_empty() {
                    if is_internal_url(&tab.url)
                        && !tab.title.is_empty()
                        && page_name(&tab.url).as_deref() != Some("start.html")
                    {
                        tab.title.clone()
                    } else {
                        "搜索或输入网站名称".to_string()
                    }
                } else {
                    let host = Library::host_of(durl);
                    let trimmed = host.strip_prefix("www.").unwrap_or(&host);
                    if trimmed.is_empty() {
                        durl.to_string()
                    } else {
                        trimmed.to_string()
                    }
                };
                let secure = durl.starts_with("https://");
                let text_col = if durl.is_empty() {
                    (0.59, 0.60, 0.63, 1.0)
                } else {
                    (0.94, 0.94, 0.96, 1.0)
                };
                let tb = p.layout.address_text;
                if secure {
                    draw_icon(cr, IconKind::Lock, tb.x + 4.0, tb.y + (tb.h - 13.0) / 2.0, 13.0, (0.60, 0.82, 0.54, 1.0));
                }
                draw_centered_text(cr, tb, &label, 13.0, text_col, true);

                if p.hover.kind == HitKind::Reload {
                    fill_round(cr, p.layout.reload_btn, 5.0, (1.0, 1.0, 1.0, 0.09));
                }
                draw_icon(
                    cr,
                    if tab.loading { IconKind::Stop } else { IconKind::Reload },
                    p.layout.reload_btn.x + 5.0,
                    p.layout.reload_btn.y + 4.0,
                    16.0,
                    (0.67, 0.68, 0.71, 1.0),
                );
            }

            if tab.loading {
                let elapsed = (now_epoch_ms() - tab.load_started_ms).max(0) as f64;
                let progress = 0.12 + 0.78 * (1.0 - (-elapsed / 2500.0).exp());
                cr.set_source_rgb(61.0 / 255.0, 139.0 / 255.0, 253.0 / 255.0);
                cr.rectangle(slot.rc.x + 6.0, slot.rc.bottom() - 2.0, (slot.rc.w - 12.0) * progress, 2.0);
                let _ = cr.fill();
            }
        } else {
            let bg = if hover {
                (56.0 / 255.0, 59.0 / 255.0, 66.0 / 255.0, 1.0)
            } else if p.private_mode {
                (44.0 / 255.0, 38.0 / 255.0, 56.0 / 255.0, 1.0)
            } else {
                (47.0 / 255.0, 50.0 / 255.0, 56.0 / 255.0, 1.0)
            };
            fill_round(cr, slot.rc, 8.0, bg);

            if hover {
                if p.hover.kind == HitKind::TabClose && p.hover.tab_id == slot.id {
                    fill_round(cr, slot.close, 4.0, (1.0, 1.0, 1.0, 0.12));
                }
                draw_icon(cr, IconKind::Close, slot.close.x + 4.0, slot.close.y + 4.0, 12.0, (0.82, 0.83, 0.85, 1.0));
            } else {
                let badge = Rect { x: slot.close.x + 2.0, y: slot.close.y + 2.0, w: 16.0, h: 16.0 };
                let badge_col = if is_internal_url(&tab.url) {
                    (0.24, 0.44, 0.94, 1.0)
                } else {
                    (0.32, 0.48, 0.62, 1.0)
                };
                fill_round(cr, badge, 4.0, badge_col);
                let ch = if is_internal_url(&tab.url) {
                    "★".to_string()
                } else {
                    let host = Library::host_of(&tab.url);
                    let trimmed = host.strip_prefix("www.").unwrap_or(&host);
                    trimmed
                        .chars()
                        .next()
                        .map(|c| c.to_ascii_uppercase().to_string())
                        .unwrap_or_else(|| "•".to_string())
                };
                draw_centered_text(cr, badge, &ch, 10.5, (1.0, 1.0, 1.0, 1.0), true);
            }

            let text_left = slot.close.right() + 6.0;
            let text_right = if slot.audio.w > 0.0 { slot.audio.x } else { slot.rc.right() } - 8.0;
            if text_right > text_left + 10.0 {
                let title = if !tab.title.is_empty() {
                    tab.title.as_str()
                } else {
                    "新标签页"
                };
                draw_centered_text(
                    cr,
                    Rect { x: text_left, y: slot.rc.y, w: text_right - text_left, h: slot.rc.h },
                    title,
                    12.5,
                    if hover { (0.93, 0.93, 0.94, 1.0) } else { (0.66, 0.67, 0.70, 1.0) },
                    false,
                );
            }
        }

        if slot.audio.w > 0.0 && !(is_active && p.address_focused) {
            if p.hover.kind == HitKind::TabAudio && p.hover.tab_id == slot.id {
                fill_round(cr, slot.audio, 4.0, (1.0, 1.0, 1.0, 0.12));
            }
            draw_icon(
                cr,
                if tab.muted { IconKind::SpeakerMute } else { IconKind::Speaker },
                slot.audio.x + 3.0,
                slot.audio.y + 3.0,
                14.0,
                (0.61, 0.76, 1.0, 1.0),
            );
        }
    }

    if p.layout.overflow_count > 0 {
        let bg = if p.hover.kind == HitKind::Overflow {
            (62.0 / 255.0, 66.0 / 255.0, 74.0 / 255.0, 1.0)
        } else {
            (50.0 / 255.0, 53.0 / 255.0, 60.0 / 255.0, 1.0)
        };
        fill_round(cr, p.layout.overflow, 7.0, bg);
        draw_centered_text(
            cr,
            p.layout.overflow,
            &format!("+{}", p.layout.overflow_count),
            11.0,
            (0.78, 0.79, 0.82, 1.0),
            true,
        );
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sample_tab(id: u64, url: &str) -> TabState {
        TabState {
            id,
            title: "Example".to_string(),
            url: url.to_string(),
            loading: false,
            load_started_ms: 0,
            can_go_back: false,
            can_go_forward: false,
            audio: false,
            muted: false,
            reader_available: false,
            reader_source: String::new(),
            zoom: 1.0,
        }
    }

    #[test]
    fn computes_safari_pill_layout_and_hit_testing() {
        let tabs = vec![
            sample_tab(1, "https://example.com"),
            sample_tab(2, "https://archlinux.org"),
        ];
        let lo = compute_layout(1280.0, &tabs, 1, false);
        assert_eq!(lo.slots.len(), 2);
        assert!(lo.active_pill.w >= 300.0);
        assert_eq!(
            hit_test(&lo, 1, false, lo.menu_btn.x + 5.0, lo.menu_btn.y + 5.0).kind,
            HitKind::Menu
        );
        assert_eq!(
            hit_test(&lo, 1, false, lo.reload_btn.x + 5.0, lo.reload_btn.y + 5.0).kind,
            HitKind::Reload
        );
        assert_eq!(
            hit_test(&lo, 1, false, lo.slots[1].close.x + 3.0, lo.slots[1].close.y + 3.0).kind,
            HitKind::TabClose
        );
        assert_eq!(
            hit_test(&lo, 1, false, lo.slots[1].close.x + 40.0, lo.slots[1].rc.y + 10.0),
            HitResult { kind: HitKind::Tab, tab_id: 2 }
        );
    }
}
