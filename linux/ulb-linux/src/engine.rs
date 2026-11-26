use gtk4::prelude::*;
use gtk4::{Align, Box as GtkBox, Label, Orientation, Widget};
use std::cell::RefCell;
use std::rc::Rc;
use std::sync::{Arc, Mutex};
use ulb_cef::{BrowserEvent, BrowserPolicy, CefTabView};

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

/// Pluggable web rendering backend trait (`CefEngine` on live Wayland, `PlaceholderEngine` fallback/test).
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
    fn execute_js(&mut self, script: &str);
    fn poll_events(&mut self) -> Vec<BrowserEvent>;
    fn has_painted_frame(&self) -> bool;
}

/// Live Chromium/CEF backend wrapping `CefTabView`.
pub struct CefEngine {
    view: CefTabView,
}

impl CefEngine {
    pub fn new(initial_url: &str, policy: Arc<Mutex<BrowserPolicy>>) -> Self {
        Self {
            view: CefTabView::new(initial_url, policy),
        }
    }
}

impl WebEngine for CefEngine {
    fn widget(&self) -> Widget {
        self.view.widget()
    }

    fn navigate(&mut self, url: &str) {
        self.view.navigate(url);
    }

    fn go_back(&mut self) {
        self.view.go_back();
    }

    fn go_forward(&mut self) {
        self.view.go_forward();
    }

    fn reload(&mut self) {
        self.view.reload();
    }

    fn stop(&mut self) {
        self.view.stop();
    }

    fn set_zoom(&mut self, zoom: f64) {
        self.view.set_zoom(zoom);
    }

    fn set_muted(&mut self, muted: bool) {
        self.view.set_muted(muted);
    }

    fn find(&mut self, query: &str, forward: bool) {
        self.view.find(query, forward);
    }

    fn stop_find(&mut self) {
        self.view.stop_find();
    }

    fn execute_js(&mut self, script: &str) {
        self.view.execute_js(script);
    }

    fn poll_events(&mut self) -> Vec<BrowserEvent> {
        self.view.poll_repaint_and_events()
    }

    fn has_painted_frame(&self) -> bool {
        self.view.has_painted_frame()
    }
}

/// Fallback GTK4 widget when CEF could not initialize (e.g. headless diagnostic environments).
pub struct PlaceholderEngine {
    root: GtkBox,
    inner: Rc<RefCell<Label>>,
}

impl PlaceholderEngine {
    pub fn new(initial_url: &str, reason: &str) -> Self {
        let root = GtkBox::builder()
            .orientation(Orientation::Vertical)
            .spacing(12)
            .halign(Align::Center)
            .valign(Align::Center)
            .hexpand(true)
            .vexpand(true)
            .build();
        let url_label = Label::builder().label(initial_url).selectable(true).build();
        let info = Label::builder().label(reason).wrap(true).build();
        root.append(&url_label);
        root.append(&info);
        Self {
            root,
            inner: Rc::new(RefCell::new(url_label)),
        }
    }
}

impl WebEngine for PlaceholderEngine {
    fn widget(&self) -> Widget {
        self.root.clone().upcast()
    }

    fn navigate(&mut self, url: &str) {
        self.inner.borrow().set_text(url);
    }

    fn go_back(&mut self) {}
    fn go_forward(&mut self) {}
    fn reload(&mut self) {}
    fn stop(&mut self) {}
    fn set_zoom(&mut self, _zoom: f64) {}
    fn set_muted(&mut self, _muted: bool) {}
    fn find(&mut self, _query: &str, _forward: bool) {}
    fn stop_find(&mut self) {}
    fn execute_js(&mut self, _script: &str) {}
    fn poll_events(&mut self) -> Vec<BrowserEvent> {
        Vec::new()
    }

    fn has_painted_frame(&self) -> bool {
        false
    }
}
