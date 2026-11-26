use gtk4::prelude::*;
use gtk4::{Align, Box as GtkBox, Label, Orientation, Widget};
use std::cell::RefCell;
use std::rc::Rc;

/// Snapshot of a browser tab exposed to UI shell, painting, and RPC bridge.
#[derive(Debug, Clone)]
pub struct TabState {
    pub id: u64,
    pub title: String,
    pub url: String,
    pub loading: bool,
    pub load_started_ms: i64,
    pub can_go_back: bool,
    pub can_go_forward: bool,
    pub audio: bool,
    pub muted: bool,
    pub reader_available: bool,
    pub reader_source: String,
    pub zoom: f64,
}

/// Pluggable web rendering backend trait (`PlaceholderEngine` now, `CefEngine` in Stage 2).
pub trait WebEngine {
    fn widget(&self) -> Widget;
    fn navigate(&mut self, url: &str);
    fn go_back(&mut self);
    fn go_forward(&mut self);
    fn reload(&mut self);
    fn stop(&mut self);
    fn set_zoom(&mut self, zoom: f64);
    fn set_muted(&mut self, muted: bool);
    fn find(&mut self, query: &str, forward: bool);
    fn stop_find(&mut self);
    #[allow(dead_code)]
    fn execute_js(&mut self, script: &str);
    fn apply_user_agent(&mut self, profile: &str, mac_platform_version: &str);
    fn set_proxy_port(&mut self, socks_port: u16);
}

struct PlaceholderInner {
    url_label: Label,
    status_label: Label,
    ua_profile: String,
    mac_ver: String,
    socks_port: u16,
    zoom: f64,
    muted: bool,
}

/// Native GTK4 engine placeholder used when `extra/cef` is not installed yet.
pub struct PlaceholderEngine {
    root: GtkBox,
    inner: Rc<RefCell<PlaceholderInner>>,
}

impl PlaceholderEngine {
    pub fn new(initial_url: &str, ua_profile: &str, mac_ver: &str, socks_port: u16) -> Self {
        let root = GtkBox::builder()
            .orientation(Orientation::Vertical)
            .spacing(12)
            .halign(Align::Center)
            .valign(Align::Center)
            .hexpand(true)
            .vexpand(true)
            .margin_start(32)
            .margin_end(32)
            .build();

        let title = Label::builder()
            .label("UltraLightBrowser · Omarchy Native Shell")
            .css_classes(["title-2"])
            .build();
        let url_label = Label::builder()
            .label(initial_url)
            .selectable(true)
            .wrap(true)
            .build();
        let status_label = Label::builder()
            .wrap(true)
            .justify(gtk4::Justification::Center)
            .build();
        let hint = Label::builder()
            .label("浏览器外壳、Safari 紧凑标签栏、macOS Edge UA、WARP 守护、广告过滤、书签/历史及扩展库已就绪。\n安装系统 CEF 后（sudo pacman -S cef）即可接入 Stage 2 完整 Chromium + CRX 扩展内核。")
            .wrap(true)
            .justify(gtk4::Justification::Center)
            .build();

        root.append(&title);
        root.append(&url_label);
        root.append(&status_label);
        root.append(&hint);

        let inner = Rc::new(RefCell::new(PlaceholderInner {
            url_label,
            status_label,
            ua_profile: ua_profile.to_string(),
            mac_ver: mac_ver.to_string(),
            socks_port,
            zoom: 1.0,
            muted: false,
        }));
        let eng = Self { root, inner };
        eng.refresh_status();
        eng
    }

    fn refresh_status(&self) {
        let inner = self.inner.borrow();
        let sample_base =
            "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/142.0.0.0 Safari/537.36";
        let ua_str =
            ulb_core::user_agent::build_macos_edge_user_agent(sample_base, &inner.ua_profile);
        let proxy_txt = if inner.socks_port > 0 {
            format!("SOCKS5 127.0.0.1:{}", inner.socks_port)
        } else {
            "Direct".to_string()
        };
        inner.status_label.set_text(&format!(
            "UA: {} ({}) | Proxy: {} | Zoom: {:.0}% | Muted: {}",
            ua_str,
            inner.mac_ver,
            proxy_txt,
            inner.zoom * 100.0,
            if inner.muted { "Yes" } else { "No" }
        ));
    }
}

impl WebEngine for PlaceholderEngine {
    fn widget(&self) -> Widget {
        self.root.clone().upcast()
    }

    fn navigate(&mut self, url: &str) {
        self.inner.borrow().url_label.set_text(url);
    }

    fn go_back(&mut self) {}
    fn go_forward(&mut self) {}
    fn reload(&mut self) {}
    fn stop(&mut self) {}

    fn set_zoom(&mut self, zoom: f64) {
        self.inner.borrow_mut().zoom = zoom;
        self.refresh_status();
    }

    fn set_muted(&mut self, muted: bool) {
        self.inner.borrow_mut().muted = muted;
        self.refresh_status();
    }

    fn find(&mut self, _query: &str, _forward: bool) {}
    fn stop_find(&mut self) {}
    fn execute_js(&mut self, _script: &str) {}

    fn apply_user_agent(&mut self, profile: &str, mac_platform_version: &str) {
        {
            let mut inner = self.inner.borrow_mut();
            inner.ua_profile = profile.to_string();
            inner.mac_ver = mac_platform_version.to_string();
        }
        self.refresh_status();
    }

    fn set_proxy_port(&mut self, socks_port: u16) {
        self.inner.borrow_mut().socks_port = socks_port;
        self.refresh_status();
    }
}
