use gtk4::cairo::{self, Context};
use std::f64::consts::PI;

/// Vector icon set matching `src/Icons.cpp` rendered cleanly on a 24x24 grid via Cairo.
#[allow(dead_code)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum IconKind {
    Sidebar,
    Back,
    Forward,
    Share,
    Plus,
    Grid,
    More,
    Reader,
    ReadingList,
    Lock,
    Reload,
    Stop,
    Close,
    Speaker,
    Sound,
    SpeakerMute,
    Star,
    Bookmark,
    Clock,
    Download,
    Search,
    Zoom,
    Shield,
    Gear,
    Info,
    Globe,
    Monitor,
    Bolt,
    Private,
    Window,
    Block,
    Fullscreen,
    Copy,
    Open,
    Home,
    Up,
    Down,
    Print,
}

impl IconKind {
    #[cfg(test)]
    pub const ALL: &'static [IconKind] = &[
        IconKind::Sidebar,
        IconKind::Back,
        IconKind::Forward,
        IconKind::Share,
        IconKind::Plus,
        IconKind::Grid,
        IconKind::More,
        IconKind::Reader,
        IconKind::ReadingList,
        IconKind::Lock,
        IconKind::Reload,
        IconKind::Stop,
        IconKind::Close,
        IconKind::Speaker,
        IconKind::Sound,
        IconKind::SpeakerMute,
        IconKind::Star,
        IconKind::Bookmark,
        IconKind::Clock,
        IconKind::Download,
        IconKind::Search,
        IconKind::Zoom,
        IconKind::Shield,
        IconKind::Gear,
        IconKind::Info,
        IconKind::Globe,
        IconKind::Monitor,
        IconKind::Bolt,
        IconKind::Private,
        IconKind::Window,
        IconKind::Block,
        IconKind::Fullscreen,
        IconKind::Copy,
        IconKind::Open,
        IconKind::Home,
        IconKind::Up,
        IconKind::Down,
        IconKind::Print,
    ];
}

fn round_rect_path(cr: &Context, x: f64, y: f64, w: f64, h: f64, r: f64) {
    let r = r.min(w * 0.5).min(h * 0.5);
    cr.new_sub_path();
    cr.arc(x + w - r, y + r, r, -PI * 0.5, 0.0);
    cr.arc(x + w - r, y + h - r, r, 0.0, PI * 0.5);
    cr.arc(x + r, y + h - r, r, PI * 0.5, PI);
    cr.arc(x + r, y + r, r, PI, PI * 1.5);
    cr.close_path();
}

/// Draws `icon` inside `(x, y, size, size)` with RGBA color `(r, g, b, a)` (`0.0..=1.0`).
pub fn draw_icon(cr: &Context, icon: IconKind, x: f64, y: f64, size: f64, rgba: (f64, f64, f64, f64)) {
    if size <= 1.0 {
        return;
    }
    let _ = cr.save();
    cr.translate(x, y);
    let scale = size / 24.0;
    cr.scale(scale, scale);
    cr.set_source_rgba(rgba.0, rgba.1, rgba.2, rgba.3);
    cr.set_line_width(1.65);
    cr.set_line_cap(cairo::LineCap::Round);
    cr.set_line_join(cairo::LineJoin::Round);

    match icon {
        IconKind::Sidebar => {
            round_rect_path(cr, 3.0, 4.0, 18.0, 16.0, 2.5);
            let _ = cr.stroke();
            cr.move_to(9.0, 4.0);
            cr.line_to(9.0, 20.0);
            let _ = cr.stroke();
        }
        IconKind::Back => {
            cr.move_to(15.0, 5.0);
            cr.line_to(8.0, 12.0);
            cr.line_to(15.0, 19.0);
            let _ = cr.stroke();
        }
        IconKind::Forward => {
            cr.move_to(9.0, 5.0);
            cr.line_to(16.0, 12.0);
            cr.line_to(9.0, 19.0);
            let _ = cr.stroke();
        }
        IconKind::Share => {
            round_rect_path(cr, 5.0, 10.0, 14.0, 10.0, 2.0);
            let _ = cr.stroke();
            cr.move_to(12.0, 14.0);
            cr.line_to(12.0, 3.5);
            cr.move_to(8.5, 7.0);
            cr.line_to(12.0, 3.5);
            cr.line_to(15.5, 7.0);
            let _ = cr.stroke();
        }
        IconKind::Plus => {
            cr.move_to(12.0, 5.0);
            cr.line_to(12.0, 19.0);
            cr.move_to(5.0, 12.0);
            cr.line_to(19.0, 12.0);
            let _ = cr.stroke();
        }
        IconKind::Grid => {
            for (rx, ry) in [(4.0, 4.0), (14.0, 4.0), (4.0, 14.0), (14.0, 14.0)] {
                round_rect_path(cr, rx, ry, 6.0, 6.0, 1.5);
                let _ = cr.stroke();
            }
        }
        IconKind::More => {
            for cx in [5.5, 12.0, 18.5] {
                cr.arc(cx, 12.0, 1.5, 0.0, 2.0 * PI);
                let _ = cr.fill();
            }
        }
        IconKind::Reader => {
            for (y0, x1) in [(6.0, 20.0), (10.0, 20.0), (14.0, 20.0), (18.0, 15.0)] {
                cr.move_to(4.0, y0);
                cr.line_to(x1, y0);
            }
            let _ = cr.stroke();
        }
        IconKind::ReadingList => {
            cr.arc(7.0, 12.0, 3.5, 0.0, 2.0 * PI);
            let _ = cr.stroke();
            cr.arc(17.0, 12.0, 3.5, 0.0, 2.0 * PI);
            let _ = cr.stroke();
            cr.move_to(10.5, 11.5);
            cr.line_to(13.5, 11.5);
            let _ = cr.stroke();
        }
        IconKind::Lock => {
            round_rect_path(cr, 5.0, 10.5, 14.0, 9.5, 2.2);
            let _ = cr.stroke();
            cr.arc(12.0, 8.5, 3.8, PI, 2.0 * PI);
            cr.line_to(15.8, 10.5);
            cr.move_to(8.2, 8.5);
            cr.line_to(8.2, 10.5);
            let _ = cr.stroke();
        }
        IconKind::Reload => {
            cr.arc(12.0, 12.0, 6.8, -0.35 * PI, 1.35 * PI);
            let _ = cr.stroke();
            cr.move_to(15.5, 3.5);
            cr.line_to(18.5, 6.5);
            cr.line_to(15.0, 9.0);
            let _ = cr.stroke();
        }
        IconKind::Stop | IconKind::Close => {
            let inset = if icon == IconKind::Stop { 6.5 } else { 6.0 };
            cr.move_to(inset, inset);
            cr.line_to(24.0 - inset, 24.0 - inset);
            cr.move_to(24.0 - inset, inset);
            cr.line_to(inset, 24.0 - inset);
            let _ = cr.stroke();
        }
        IconKind::Speaker | IconKind::Sound => {
            cr.move_to(4.0, 9.5);
            cr.line_to(7.5, 9.5);
            cr.line_to(12.5, 5.5);
            cr.line_to(12.5, 18.5);
            cr.line_to(7.5, 14.5);
            cr.line_to(4.0, 14.5);
            cr.close_path();
            let _ = cr.stroke();
            cr.arc(14.0, 12.0, 3.2, -0.35 * PI, 0.35 * PI);
            let _ = cr.stroke();
            if icon == IconKind::Sound {
                cr.arc(14.0, 12.0, 6.0, -0.38 * PI, 0.38 * PI);
                let _ = cr.stroke();
            }
        }
        IconKind::SpeakerMute => {
            cr.move_to(4.0, 9.5);
            cr.line_to(7.5, 9.5);
            cr.line_to(12.5, 5.5);
            cr.line_to(12.5, 18.5);
            cr.line_to(7.5, 14.5);
            cr.line_to(4.0, 14.5);
            cr.close_path();
            let _ = cr.stroke();
            cr.move_to(15.5, 9.0);
            cr.line_to(20.5, 15.0);
            cr.move_to(20.5, 9.0);
            cr.line_to(15.5, 15.0);
            let _ = cr.stroke();
        }
        IconKind::Star => {
            for i in 0..10 {
                let angle = -PI * 0.5 + (i as f64) * PI / 5.0;
                let r = if i % 2 == 0 { 8.5 } else { 3.8 };
                let px = 12.0 + r * angle.cos();
                let py = 12.0 + r * angle.sin();
                if i == 0 {
                    cr.move_to(px, py);
                } else {
                    cr.line_to(px, py);
                }
            }
            cr.close_path();
            let _ = cr.stroke();
        }
        IconKind::Bookmark => {
            cr.move_to(6.5, 4.0);
            cr.line_to(17.5, 4.0);
            cr.line_to(17.5, 20.0);
            cr.line_to(12.0, 15.8);
            cr.line_to(6.5, 20.0);
            cr.close_path();
            let _ = cr.stroke();
        }
        IconKind::Clock => {
            cr.arc(12.0, 12.0, 8.0, 0.0, 2.0 * PI);
            let _ = cr.stroke();
            cr.move_to(12.0, 7.5);
            cr.line_to(12.0, 12.0);
            cr.line_to(15.5, 14.0);
            let _ = cr.stroke();
        }
        IconKind::Download => {
            cr.move_to(12.0, 4.0);
            cr.line_to(12.0, 15.0);
            cr.move_to(8.0, 11.5);
            cr.line_to(12.0, 15.5);
            cr.line_to(16.0, 11.5);
            cr.move_to(5.0, 19.0);
            cr.line_to(19.0, 19.0);
            let _ = cr.stroke();
        }
        IconKind::Search | IconKind::Zoom => {
            cr.arc(10.5, 10.5, 6.2, 0.0, 2.0 * PI);
            let _ = cr.stroke();
            cr.move_to(15.0, 15.0);
            cr.line_to(19.5, 19.5);
            let _ = cr.stroke();
            if icon == IconKind::Zoom {
                cr.move_to(8.0, 10.5);
                cr.line_to(13.0, 10.5);
                cr.move_to(10.5, 8.0);
                cr.line_to(10.5, 13.0);
                let _ = cr.stroke();
            }
        }
        IconKind::Shield => {
            cr.move_to(12.0, 3.5);
            cr.line_to(19.0, 6.5);
            cr.line_to(18.5, 12.5);
            cr.curve_to(18.0, 16.5, 15.0, 19.0, 12.0, 20.5);
            cr.curve_to(9.0, 19.0, 6.0, 16.5, 5.5, 12.5);
            cr.line_to(5.0, 6.5);
            cr.close_path();
            let _ = cr.stroke();
        }
        IconKind::Gear => {
            cr.arc(12.0, 12.0, 3.5, 0.0, 2.0 * PI);
            let _ = cr.stroke();
            cr.arc(12.0, 12.0, 7.2, 0.0, 2.0 * PI);
            let _ = cr.stroke();
        }
        IconKind::Info => {
            cr.arc(12.0, 12.0, 8.2, 0.0, 2.0 * PI);
            let _ = cr.stroke();
            cr.move_to(12.0, 11.0);
            cr.line_to(12.0, 16.5);
            let _ = cr.stroke();
            cr.arc(12.0, 7.8, 0.8, 0.0, 2.0 * PI);
            let _ = cr.fill();
        }
        IconKind::Globe => {
            cr.arc(12.0, 12.0, 8.2, 0.0, 2.0 * PI);
            let _ = cr.stroke();
            cr.move_to(4.0, 12.0);
            cr.line_to(20.0, 12.0);
            cr.move_to(12.0, 3.8);
            cr.line_to(12.0, 20.2);
            let _ = cr.stroke();
        }
        IconKind::Monitor => {
            round_rect_path(cr, 3.5, 4.5, 17.0, 12.0, 2.0);
            let _ = cr.stroke();
            cr.move_to(8.5, 19.5);
            cr.line_to(15.5, 19.5);
            cr.move_to(12.0, 16.5);
            cr.line_to(12.0, 19.5);
            let _ = cr.stroke();
        }
        IconKind::Bolt => {
            cr.move_to(13.5, 3.5);
            cr.line_to(6.5, 13.0);
            cr.line_to(11.5, 13.0);
            cr.line_to(10.5, 20.5);
            cr.line_to(17.5, 11.0);
            cr.line_to(12.5, 11.0);
            cr.close_path();
            let _ = cr.stroke();
        }
        IconKind::Private => {
            round_rect_path(cr, 4.0, 9.0, 16.0, 10.5, 3.5);
            let _ = cr.stroke();
            cr.arc(9.0, 14.0, 1.8, 0.0, 2.0 * PI);
            let _ = cr.stroke();
            cr.arc(15.0, 14.0, 1.8, 0.0, 2.0 * PI);
            let _ = cr.stroke();
        }
        IconKind::Window => {
            round_rect_path(cr, 3.5, 4.5, 17.0, 15.0, 2.2);
            let _ = cr.stroke();
            cr.move_to(3.5, 9.0);
            cr.line_to(20.5, 9.0);
            let _ = cr.stroke();
        }
        IconKind::Block => {
            cr.arc(12.0, 12.0, 8.2, 0.0, 2.0 * PI);
            let _ = cr.stroke();
            cr.move_to(6.5, 6.5);
            cr.line_to(17.5, 17.5);
            let _ = cr.stroke();
        }
        IconKind::Fullscreen => {
            for (sx, sy, dx, dy) in [
                (4.5, 4.5, 4.5, 4.5),
                (19.5, 4.5, -4.5, 4.5),
                (4.5, 19.5, 4.5, -4.5),
                (19.5, 19.5, -4.5, -4.5),
            ] {
                cr.move_to(sx + dx, sy);
                cr.line_to(sx, sy);
                cr.line_to(sx, sy + dy);
            }
            let _ = cr.stroke();
        }
        IconKind::Copy => {
            round_rect_path(cr, 8.5, 8.5, 11.0, 11.5, 1.8);
            let _ = cr.stroke();
            round_rect_path(cr, 4.5, 4.5, 11.0, 11.5, 1.8);
            let _ = cr.stroke();
        }
        IconKind::Open => {
            round_rect_path(cr, 4.5, 6.5, 13.0, 13.0, 2.0);
            let _ = cr.stroke();
            cr.move_to(12.0, 12.0);
            cr.line_to(19.5, 4.5);
            cr.move_to(14.5, 4.5);
            cr.line_to(19.5, 4.5);
            cr.line_to(19.5, 9.5);
            let _ = cr.stroke();
        }
        IconKind::Home => {
            cr.move_to(4.0, 11.0);
            cr.line_to(12.0, 4.5);
            cr.line_to(20.0, 11.0);
            cr.move_to(6.5, 9.5);
            cr.line_to(6.5, 19.5);
            cr.line_to(17.5, 19.5);
            cr.line_to(17.5, 9.5);
            let _ = cr.stroke();
        }
        IconKind::Up => {
            cr.move_to(6.0, 15.0);
            cr.line_to(12.0, 9.0);
            cr.line_to(18.0, 15.0);
            let _ = cr.stroke();
        }
        IconKind::Down => {
            cr.move_to(6.0, 9.0);
            cr.line_to(12.0, 15.0);
            cr.line_to(18.0, 9.0);
            let _ = cr.stroke();
        }
        IconKind::Print => {
            round_rect_path(cr, 4.5, 9.0, 15.0, 8.0, 1.5);
            let _ = cr.stroke();
            round_rect_path(cr, 7.0, 4.5, 10.0, 4.5, 1.0);
            let _ = cr.stroke();
            round_rect_path(cr, 7.0, 14.0, 10.0, 5.5, 1.0);
            let _ = cr.stroke();
        }
    }

    let _ = cr.restore();
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn renders_all_icons_to_cairo_surface() {
        let surface = cairo::ImageSurface::create(cairo::Format::ARgb32, 32, 32).unwrap();
        let cr = Context::new(&surface).unwrap();
        for icon in IconKind::ALL {
            draw_icon(&cr, *icon, 4.0, 4.0, 24.0, (0.9, 0.9, 0.95, 1.0));
        }
    }
}
