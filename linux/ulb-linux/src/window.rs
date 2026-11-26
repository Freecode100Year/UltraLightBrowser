use crate::audio_watcher::AudioWatcher;
use crate::engine::{PlaceholderEngine, TabState, WebEngine};
use crate::menus::{build_main_menu, build_share_menu, build_tab_menu, MainMenuContext};
use crate::paint_bar::{compute_layout, hit_test, paint_toolbar, HitKind, HitResult, PaintParams, ToolbarLayout, TOPBAR_HEIGHT};
use crate::session::{start_ipc_server, IpcCommand, IpcGuard, SessionDir};
use gtk4::gdk;
use gtk4::gio::SimpleActionGroup;
use gtk4::glib;
use gtk4::prelude::*;
use gtk4::{
    Application, ApplicationWindow, Box as GtkBox, Button, DrawingArea, Entry, EventControllerKey,
    EventControllerMotion, GestureClick, Label, ListBox, ListBoxRow, Orientation, Overlay,
    Popover, PopoverMenu, ScrolledWindow, Stack, Widget,
};
use std::cell::RefCell;
use std::path::PathBuf;
use std::rc::Rc;
use ulb_core::config::{ConfigStore, DEFAULT_HOME_URL};
use ulb_core::extensions_store::ExtensionsStore;
use ulb_core::internal_pages::internal_url;
use ulb_core::library::{Library, BOOKMARKS_FOLDER, FAVORITES_FOLDER};
use ulb_core::line_store::LineStore;
use ulb_core::user_scripts::UserScriptStore;
use ulb_core::util::{domain_of, now_epoch_ms};
use ulb_core::warp_host::{load_encrypted_lines, save_encrypted_lines, WarpConfig, WarpStatus, WarpSupervisor};

/// Shared mutable browser paths/binaries needed across windows.
#[derive(Clone)]
pub struct AppPaths {
    pub config_dir: PathBuf,
    pub data_dir: PathBuf,
    pub warp_bin: Option<PathBuf>,
}

struct TabItem {
    state: TabState,
    engine: Box<dyn WebEngine>,
}

struct AppState {
    paths: AppPaths,
    config: ConfigStore,
    library: Library,
    lines: LineStore,
    extensions: ExtensionsStore,
    scripts: UserScriptStore,
    warp_status: WarpStatus,
    warp_sup: Option<WarpSupervisor>,
    audio_watcher: Option<AudioWatcher>,
    tabs: Vec<TabItem>,
    closed_urls: Vec<String>,
    next_tab_id: u64,
    active_id: u64,
    ctx_tab_id: u64,
    private_mode: bool,
    sidebar_visible: bool,
    overview_visible: bool,
    find_visible: bool,
    fullscreen: bool,
    address_focused: bool,
    traffic_hover: bool,
    hover: HitResult,
    layout: ToolbarLayout,
    _session: SessionDir,
    _ipc: Option<IpcGuard>,
    session_path_str: String,
}

pub struct BrowserWindow {
    window: ApplicationWindow,
    toolbar: DrawingArea,
    address_entry: Entry,
    suggest_popover: Popover,
    suggest_list: ListBox,
    find_bar: GtkBox,
    find_entry: Entry,
    sidebar_panel: GtkBox,
    content_stack: Stack,
    toast_label: Label,
    menu_popover: PopoverMenu,
    state: Rc<RefCell<AppState>>,
}

impl AppState {
    fn save_all(&mut self) {
        let _ = self.config.save();
        if !self.private_mode {
            let _ = self.library.save();
        }
        let _ = save_encrypted_lines(&self.paths.data_dir, &self.lines);
    }

    fn restart_warp(&mut self) {
        if let Some(mut s) = self.warp_sup.take() {
            s.stop();
        }
        self.warp_status = WarpStatus::default();
        if !self.config.settings.warp_enabled {
            return;
        }
        let Some(ref bin) = self.paths.warp_bin else {
            self.warp_status.error = "未找到 ulb-warp 组件".to_string();
            return;
        };
        let selected_id = self.lines.selected().to_string();
        let selected_links = self
            .lines
            .selected_links()
            .into_iter()
            .map(str::to_string)
            .collect();
        let cfg = WarpConfig {
            binary: bin.clone(),
            state_dir: self.paths.data_dir.join("warp-state"),
            fail_closed: self.config.settings.warp_fail_closed,
            use_line: self.config.settings.use_line,
            selected_id,
            selected_links,
        };
        if let Ok(sup) = WarpSupervisor::start(&cfg, |_| {}) {
            self.warp_status = sup.status();
            self.warp_sup = Some(sup);
        }
    }

    fn home_url(&self) -> String {
        if self.config.settings.new_tab_page == "blank" {
            "about:blank".to_string()
        } else if self.config.settings.start_url.is_empty()
            || self.config.settings.start_url == DEFAULT_HOME_URL
        {
            internal_url("start.html")
        } else {
            self.config.settings.start_url.clone()
        }
    }

    fn resolve_address(&self, raw: &str) -> String {
        let trimmed = raw.trim();
        if trimmed.is_empty() {
            return self.home_url();
        }
        if trimmed.contains("://")
            || trimmed.starts_with("about:")
            || trimmed.starts_with("chrome-extension://")
        {
            return trimmed.to_string();
        }
        if !trimmed.contains(' ') && (trimmed.contains('.') || trimmed.starts_with("localhost")) {
            return format!("https://{trimmed}");
        }
        let q: String = url_encode(trimmed);
        match self.config.settings.search_engine.as_str() {
            "bing" => format!("https://www.bing.com/search?q={q}"),
            "duckduckgo" => format!("https://duckduckgo.com/?q={q}"),
            "baidu" => format!("https://www.baidu.com/s?wd={q}"),
            _ => format!("https://www.google.com/search?q={q}"),
        }
    }

    fn tab_snapshots(&self) -> Vec<TabState> {
        self.tabs.iter().map(|t| t.state.clone()).collect()
    }
}

fn url_encode(input: &str) -> String {
    let mut out = String::with_capacity(input.len() * 2);
    for b in input.as_bytes() {
        if b.is_ascii_alphanumeric() || matches!(b, b'-' | b'_' | b'.' | b'~') {
            out.push(*b as char);
        } else if *b == b' ' {
            out.push('+');
        } else {
            out.push_str(&format!("%{b:02X}"));
        }
    }
    out
}

pub fn run_browser(app: &Application, paths: AppPaths, session: SessionDir, initial_urls: Vec<String>, private_mode: bool) {
    let mut config = ConfigStore::new(paths.config_dir.join("config.json"));
    config.load();
    if config.settings.user_agent_profile == "default" {
        // Default profile on Linux uses macOS Edge stealth identity unless user overrides.
        config.settings.user_agent_profile = "macos-edge".to_string();
    }

    let mut library = Library::new(paths.data_dir.clone());
    library.load();

    let mut lines = LineStore::new();
    load_encrypted_lines(&paths.data_dir, &mut lines);

    let mut extensions = ExtensionsStore::new(&paths.data_dir);
    extensions.load_index();

    let mut scripts = UserScriptStore::new(paths.data_dir.join("scripts"), "2.10.1");
    scripts.load();

    let (ipc_tx, ipc_rx) = std::sync::mpsc::channel::<IpcCommand>();
    let ipc_guard = start_ipc_server(session.runtime_root(), move |cmd| {
        let _ = ipc_tx.send(cmd);
    })
    .ok();

    let mut state = AppState {
        paths,
        config,
        library,
        lines,
        extensions,
        scripts,
        warp_status: WarpStatus::default(),
        warp_sup: None,
        audio_watcher: None,
        tabs: Vec::new(),
        closed_urls: Vec::new(),
        next_tab_id: 1,
        active_id: 0,
        ctx_tab_id: 0,
        private_mode,
        sidebar_visible: false,
        overview_visible: false,
        find_visible: false,
        fullscreen: false,
        address_focused: false,
        traffic_hover: false,
        hover: HitResult::default(),
        layout: ToolbarLayout::default(),
        session_path_str: session.path().display().to_string(),
        _session: session,
        _ipc: ipc_guard,
    };
    state.restart_warp();
    let state = Rc::new(RefCell::new(state));

    let audio_watcher = AudioWatcher::start(|_headphones| {});
    state.borrow_mut().audio_watcher = Some(audio_watcher);

    let window = ApplicationWindow::builder()
        .application(app)
        .title(if private_mode { "UltraLightBrowser · 无痕浏览" } else { "UltraLightBrowser" })
        .default_width(1280)
        .default_height(820)
        .decorated(false)
        .build();

    let root_box = GtkBox::new(Orientation::Vertical, 0);

    // Topbar Overlay: Cairo toolbar + floating Address Entry
    let topbar_overlay = Overlay::new();
    let toolbar = DrawingArea::new();
    toolbar.set_content_height(TOPBAR_HEIGHT as i32);
    toolbar.set_hexpand(true);

    let address_entry = Entry::builder()
        .visible(false)
        .has_frame(false)
        .placeholder_text("搜索或输入网站名称")
        .build();
    address_entry.set_halign(gtk4::Align::Start);
    address_entry.set_valign(gtk4::Align::Start);

    topbar_overlay.set_child(Some(&toolbar));
    topbar_overlay.add_overlay(&address_entry);
    root_box.append(&topbar_overlay);

    // Suggestions popover anchored under address_entry
    let suggest_list = ListBox::new();
    let suggest_scroll = ScrolledWindow::builder()
        .min_content_width(420)
        .max_content_height(260)
        .child(&suggest_list)
        .build();
    let suggest_popover = Popover::builder()
        .child(&suggest_scroll)
        .autohide(true)
        .build();
    suggest_popover.set_parent(&address_entry);

    // Find bar (hidden by default)
    let find_bar = GtkBox::builder()
        .orientation(Orientation::Horizontal)
        .spacing(6)
        .margin_start(12)
        .margin_end(12)
        .margin_top(6)
        .margin_bottom(6)
        .visible(false)
        .build();
    let find_entry = Entry::builder()
        .placeholder_text("在页面中查找…")
        .hexpand(true)
        .build();
    let find_prev = Button::with_label("上一个");
    let find_next = Button::with_label("下一个");
    let find_done = Button::with_label("完成");
    find_bar.append(&find_entry);
    find_bar.append(&find_prev);
    find_bar.append(&find_next);
    find_bar.append(&find_done);
    root_box.append(&find_bar);

    // Main content row: Collapsible Sidebar + Web Stack + Toast Overlay
    let content_row = GtkBox::builder()
        .orientation(Orientation::Horizontal)
        .hexpand(true)
        .vexpand(true)
        .build();
    let sidebar_panel = GtkBox::builder()
        .orientation(Orientation::Vertical)
        .width_request(260)
        .visible(false)
        .spacing(6)
        .margin_start(10)
        .margin_end(10)
        .margin_top(10)
        .build();
    sidebar_panel.append(&Label::builder().label("边栏 · 收藏与标签组").build());
    content_row.append(&sidebar_panel);

    let content_overlay = Overlay::new();
    let content_stack = Stack::builder().hexpand(true).vexpand(true).build();
    let toast_label = Label::builder()
        .visible(false)
        .halign(gtk4::Align::Center)
        .valign(gtk4::Align::End)
        .margin_bottom(24)
        .css_classes(["osd"])
        .build();
    content_overlay.set_child(Some(&content_stack));
    content_overlay.add_overlay(&toast_label);
    content_row.append(&content_overlay);
    root_box.append(&content_row);

    let menu_popover = PopoverMenu::from_model(Option::<&gtk4::gio::MenuModel>::None);
    menu_popover.set_parent(&toolbar);

    window.set_child(Some(&root_box));

    let bw = Rc::new(BrowserWindow {
        window: window.clone(),
        toolbar: toolbar.clone(),
        address_entry: address_entry.clone(),
        suggest_popover,
        suggest_list: suggest_list.clone(),
        find_bar,
        find_entry: find_entry.clone(),
        sidebar_panel,
        content_stack,
        toast_label,
        menu_popover,
        state: Rc::clone(&state),
    });

    bw.install_actions();
    bw.connect_events(&find_prev, &find_next, &find_done);

    if initial_urls.is_empty() {
        let start = bw.state.borrow().home_url();
        bw.new_tab(&start, true);
    } else {
        for (i, u) in initial_urls.iter().enumerate() {
            bw.new_tab(u, i == 0);
        }
    }

    // Periodic timer (~150ms): refresh WARP stdout status + animate loading progress + drain IPC commands.
    let bw_tick = Rc::clone(&bw);
    glib::timeout_add_local(std::time::Duration::from_millis(150), move || {
        while let Ok(cmd) = ipc_rx.try_recv() {
            match cmd {
                IpcCommand::Raise => bw_tick.window.present(),
                IpcCommand::Open(url) => {
                    bw_tick.new_tab(&url, true);
                    bw_tick.window.present();
                }
            }
        }
        {
            let mut st = bw_tick.state.borrow_mut();
            if let Some(ref sup) = st.warp_sup {
                let latest = sup.status();
                if latest != st.warp_status {
                    let port = latest.port;
                    st.warp_status = latest;
                    for t in &mut st.tabs {
                        t.engine.set_proxy_port(port);
                    }
                }
            }
        }
        bw_tick.toolbar.queue_draw();
        glib::ControlFlow::Continue
    });

    window.connect_close_request({
        let state_close = Rc::clone(&state);
        move |_| {
            let mut st = state_close.borrow_mut();
            st.save_all();
            if let Some(mut w) = st.warp_sup.take() {
                w.stop();
            }
            if let Some(mut a) = st.audio_watcher.take() {
                a.stop();
            }
            glib::Propagation::Proceed
        }
    });

    window.present();
}

impl BrowserWindow {
    fn show_toast(self: &Rc<Self>, msg: &str) {
        self.toast_label.set_text(msg);
        self.toast_label.set_visible(true);
        let lbl = self.toast_label.clone();
        glib::timeout_add_local_once(std::time::Duration::from_millis(1800), move || {
            lbl.set_visible(false);
        });
    }

    fn new_tab(self: &Rc<Self>, url: &str, activate: bool) {
        let child_widget: Widget;
        let id = {
            let mut st = self.state.borrow_mut();
            let id = st.next_tab_id;
            st.next_tab_id += 1;
            let engine = PlaceholderEngine::new(
                url,
                &st.config.settings.user_agent_profile,
                &st.config.settings.mac_platform_version,
                st.warp_status.port,
            );
            child_widget = engine.widget();
            let title = if url == internal_url("start.html") {
                "起始页".to_string()
            } else {
                domain_of(url)
            };
            st.tabs.push(TabItem {
                state: TabState {
                    id,
                    title,
                    url: url.to_string(),
                    loading: false,
                    load_started_ms: now_epoch_ms(),
                    can_go_back: false,
                    can_go_forward: false,
                    audio: false,
                    muted: false,
                    reader_available: false,
                    reader_source: String::new(),
                    zoom: 1.0,
                },
                engine: Box::new(engine),
            });
            if activate {
                st.active_id = id;
            }
            id
        };
        self.content_stack.add_named(&child_widget, Some(&format!("tab-{id}")));
        if activate {
            self.activate_tab(id);
        } else {
            self.toolbar.queue_draw();
        }
    }

    fn activate_tab(self: &Rc<Self>, id: u64) {
        {
            let mut st = self.state.borrow_mut();
            if !st.tabs.iter().any(|t| t.state.id == id) {
                return;
            }
            st.active_id = id;
            st.address_focused = false;
        }
        self.address_entry.set_visible(false);
        self.content_stack.set_visible_child_name(&format!("tab-{id}"));
        self.toolbar.queue_draw();
    }

    fn close_tab(self: &Rc<Self>, id: u64) {
        let next_active: Option<u64>;
        let widget_to_remove: Option<Widget>;
        {
            let mut st = self.state.borrow_mut();
            if st.tabs.len() <= 1 {
                if let Some(t) = st.tabs.first_mut() {
                    let home = internal_url("start.html");
                    t.state.url = home.clone();
                    t.state.title = "起始页".to_string();
                    t.engine.navigate(&home);
                }
                next_active = Some(st.active_id);
                widget_to_remove = None;
            } else {
                let Some(pos) = st.tabs.iter().position(|t| t.state.id == id) else {
                    return;
                };
                let removed = st.tabs.remove(pos);
                if !removed.state.url.is_empty() {
                    st.closed_urls.push(removed.state.url.clone());
                }
                widget_to_remove = Some(removed.engine.widget());
                if st.active_id == id {
                    let next_pos = pos.min(st.tabs.len() - 1);
                    st.active_id = st.tabs[next_pos].state.id;
                }
                next_active = Some(st.active_id);
            }
        }
        if let Some(w) = widget_to_remove {
            self.content_stack.remove(&w);
        }
        if let Some(nid) = next_active {
            self.content_stack.set_visible_child_name(&format!("tab-{nid}"));
        }
        self.toolbar.queue_draw();
    }

    fn navigate_active(self: &Rc<Self>, raw: &str) {
        let resolved = self.state.borrow().resolve_address(raw);
        {
            let mut st = self.state.borrow_mut();
            let active_id = st.active_id;
            let save_history = !st.private_mode && st.config.settings.save_history;
            let title = if resolved.starts_with("https://ulb.internal/") {
                ulb_core::internal_pages::page_name(&resolved).unwrap_or_default()
            } else {
                domain_of(&resolved)
            };
            if let Some(t) = st.tabs.iter_mut().find(|t| t.state.id == active_id) {
                t.state.url = resolved.clone();
                t.state.title = title.clone();
                t.engine.navigate(&resolved);
            }
            if save_history && resolved.starts_with("http") && !resolved.contains("ulb.internal") {
                st.library.record_visit(&resolved, &title, now_epoch_ms());
            }
            st.address_focused = false;
        }
        self.address_entry.set_visible(false);
        self.suggest_popover.popdown();
        self.toolbar.queue_draw();
    }

    fn begin_address_edit(self: &Rc<Self>) {
        let (rect, current_url) = {
            let mut st = self.state.borrow_mut();
            st.address_focused = true;
            let active_id = st.active_id;
            let u = st
                .tabs
                .iter()
                .find(|t| t.state.id == active_id)
                .map(|t| t.state.url.clone())
                .unwrap_or_default();
            (st.layout.address_text, u)
        };
        if rect.w > 10.0 {
            self.address_entry.set_margin_start(rect.x.max(0.0) as i32);
            self.address_entry.set_margin_top(rect.y.max(0.0) as i32);
            self.address_entry.set_width_request(rect.w as i32);
            self.address_entry.set_height_request(rect.h as i32);
        }
        self.address_entry.set_text(
            if current_url == internal_url("start.html") { "" } else { &current_url },
        );
        self.address_entry.set_visible(true);
        self.address_entry.grab_focus();
        self.address_entry.select_region(0, -1);
        self.toolbar.queue_draw();
    }

    fn show_main_menu(self: &Rc<Self>) {
        let mut st = self.state.borrow_mut();
        let active_id = st.active_id;
        let active_opt = st.tabs.iter().find(|t| t.state.id == active_id).map(|t| t.state.clone());
        let web_page = active_opt
            .as_ref()
            .is_some_and(|t| t.url.starts_with("http") && !t.url.contains("ulb.internal"));
        let zoom = active_opt.as_ref().map(|t| t.zoom).unwrap_or(1.0);
        let reader_active = active_opt.as_ref().is_some_and(|t| !t.reader_source.is_empty());
        let host = active_opt.as_ref().map(|t| domain_of(&t.url)).unwrap_or_default();
        let host_adblock = st.library.site(&host).adblock;
        let extension_count = st.extensions.list().len();
        let script_count = st.scripts.scripts().len();
        let headphones_detected = st
            .audio_watcher
            .as_ref()
            .is_some_and(|w| w.is_headphones_active());
        let menu = build_main_menu(&MainMenuContext {
            settings: &st.config.settings,
            warp: &st.warp_status,
            sidebar_visible: st.sidebar_visible,
            fullscreen: st.fullscreen,
            reader_active,
            web_page,
            tab_zoom: zoom,
            host_adblock_on: host_adblock,
            tab_count: st.tabs.len(),
            extension_count,
            script_count,
            headphones_detected,
        });
        let r = st.layout.menu_btn;
        drop(st);
        self.menu_popover.set_menu_model(Some(&menu));
        self.menu_popover.set_pointing_to(Some(&gdk::Rectangle::new(
            r.x as i32,
            r.y as i32,
            r.w as i32,
            r.h as i32,
        )));
        self.menu_popover.popup();
    }

    fn show_share_menu(self: &Rc<Self>) {
        let r = self.state.borrow().layout.share_btn;
        let menu = build_share_menu();
        self.menu_popover.set_menu_model(Some(&menu));
        self.menu_popover.set_pointing_to(Some(&gdk::Rectangle::new(
            r.x as i32,
            r.y as i32,
            r.w as i32,
            r.h as i32,
        )));
        self.menu_popover.popup();
    }

    fn install_actions(self: &Rc<Self>) {
        type ActionCb = Box<dyn Fn(&Rc<BrowserWindow>)>;
        let group = SimpleActionGroup::new();
        let add = |name: &str, f: ActionCb| {
            let bw = Rc::clone(self);
            let act = gtk4::gio::SimpleAction::new(name, None);
            act.connect_activate(move |_, _| f(&bw));
            group.add_action(&act);
        };

        add("memorial", Box::new(|bw| bw.new_tab(&internal_url("memorial/"), true)));
        add("new-tab", Box::new(|bw| { let u = bw.state.borrow().home_url(); bw.new_tab(&u, true); }));
        add("new-window", Box::new(|bw| { let u = bw.state.borrow().home_url(); bw.new_tab(&u, true); }));
        add("new-private", Box::new(|bw| bw.show_toast("可通过 --private 参数启动无痕窗口")));
        add("bookmarks", Box::new(|bw| bw.new_tab(&internal_url("bookmarks.html"), true)));
        add("history", Box::new(|bw| bw.new_tab(&internal_url("history.html"), true)));
        add("reading-list", Box::new(|bw| bw.new_tab(&internal_url("bookmarks.html#reading"), true)));
        add("overview", Box::new(|bw| bw.new_tab(&internal_url("overview.html"), true)));
        add("extensions", Box::new(|bw| bw.new_tab(&internal_url("settings.html#extensions"), true)));
        add("manage-lines", Box::new(|bw| bw.new_tab(&internal_url("settings.html#warp"), true)));
        add("settings", Box::new(|bw| bw.new_tab(&internal_url("settings.html"), true)));
        add("ua-selftest", Box::new(|bw| bw.new_tab(&internal_url("selftest.html"), true)));
        add("about", Box::new(|bw| {
            let tmp = bw.state.borrow().session_path_str.clone();
            bw.show_toast(&format!("UltraLightBrowser · Omarchy ({tmp})"));
        }));
        add("toggle-sidebar", Box::new(|bw| {
            let vis = { let mut st = bw.state.borrow_mut(); st.sidebar_visible = !st.sidebar_visible; st.sidebar_visible };
            bw.sidebar_panel.set_visible(vis);
            bw.toolbar.queue_draw();
        }));
        add("find", Box::new(|bw| {
            bw.state.borrow_mut().find_visible = true;
            bw.find_bar.set_visible(true);
            bw.find_entry.grab_focus();
        }));
        add("toggle-fullscreen", Box::new(|bw| {
            let fs = { let mut st = bw.state.borrow_mut(); st.fullscreen = !st.fullscreen; st.fullscreen };
            if fs { bw.window.fullscreen(); } else { bw.window.unfullscreen(); }
            bw.toolbar.queue_draw();
        }));
        add("zoom-in", Box::new(|bw| bw.adjust_zoom(0.1)));
        add("zoom-out", Box::new(|bw| bw.adjust_zoom(-0.1)));
        add("zoom-reset", Box::new(|bw| bw.set_zoom(1.0)));
        add("toggle-global-adblock", Box::new(|bw| {
            { let mut st = bw.state.borrow_mut(); st.config.settings.enable_ad_block = !st.config.settings.enable_ad_block; let _ = st.config.save(); }
        }));
        add("toggle-site-adblock", Box::new(|bw| {
            let mut st = bw.state.borrow_mut();
            let active_id = st.active_id;
            if let Some(url) = st.tabs.iter().find(|t| t.state.id == active_id).map(|t| t.state.url.clone()) {
                let host = domain_of(&url);
                if !host.is_empty() && !st.private_mode {
                    let mut site = st.library.site(&host);
                    site.adblock = !site.adblock;
                    st.library.set_site(&host, site);
                    let _ = st.library.save();
                }
            }
        }));
        add("audio-passthrough", Box::new(|bw| {
            let mut st = bw.state.borrow_mut();
            st.config.settings.system_audio_passthrough = true;
            let _ = st.config.save();
        }));
        add("audio-surround", Box::new(|bw| {
            let mut st = bw.state.borrow_mut();
            st.config.settings.system_audio_passthrough = false;
            st.config.settings.enable_surround_sound = true;
            let _ = st.config.save();
        }));
        add("audio-vocal", Box::new(|bw| {
            let mut st = bw.state.borrow_mut();
            st.config.settings.enable_vocal_boost = !st.config.settings.enable_vocal_boost;
            let _ = st.config.save();
        }));
        add("audio-night", Box::new(|bw| {
            let mut st = bw.state.borrow_mut();
            st.config.settings.enable_night_mode = !st.config.settings.enable_night_mode;
            let _ = st.config.save();
        }));
        add("warp-rescan", Box::new(|bw| {
            if let Some(ref mut sup) = bw.state.borrow_mut().warp_sup { sup.rescan(); }
        }));
        add("net-direct", Box::new(|bw| {
            let mut st = bw.state.borrow_mut();
            st.config.settings.warp_enabled = false;
            let _ = st.config.save();
            st.restart_warp();
        }));
        add("net-warp", Box::new(|bw| {
            let mut st = bw.state.borrow_mut();
            st.config.settings.warp_enabled = true;
            st.config.settings.use_line = false;
            let _ = st.config.save();
            st.restart_warp();
        }));
        add("net-line", Box::new(|bw| {
            let mut st = bw.state.borrow_mut();
            st.config.settings.warp_enabled = true;
            st.config.settings.use_line = true;
            let _ = st.config.save();
            st.restart_warp();
        }));
        add("warp-fail-closed", Box::new(|bw| {
            let mut st = bw.state.borrow_mut();
            st.config.settings.warp_fail_closed = !st.config.settings.warp_fail_closed;
            let _ = st.config.save();
            st.restart_warp();
        }));
        add("ua-default", Box::new(|bw| bw.set_ua("default")));
        add("ua-macos-edge", Box::new(|bw| bw.set_ua("macos-edge")));
        add("copy-url", Box::new(|bw| {
            let active_id = bw.state.borrow().active_id;
            let url = bw.state.borrow().tabs.iter().find(|t| t.state.id == active_id).map(|t| t.state.url.clone()).unwrap_or_default();
            if let Some(display) = gdk::Display::default() {
                display.clipboard().set_text(&url);
                bw.show_toast("已复制链接");
            }
        }));
        add("add-bookmark", Box::new(|bw| bw.bookmark_active(BOOKMARKS_FOLDER, "已添加书签")));
        add("add-favorite", Box::new(|bw| bw.bookmark_active(FAVORITES_FOLDER, "已添加到个人收藏")));
        add("add-reading", Box::new(|bw| {
            let mut st = bw.state.borrow_mut();
            let id = st.active_id;
            if let Some((title, url)) = st.tabs.iter().find(|t| t.state.id == id).map(|t| (t.state.title.clone(), t.state.url.clone())) {
                st.library.add_reading(&title, &url, now_epoch_ms());
                let _ = st.library.save();
            }
            drop(st);
            bw.show_toast("已添加到阅读列表");
        }));
        add("tab-ctx-reload", Box::new(|bw| {
            let mut st = bw.state.borrow_mut();
            let tid = st.ctx_tab_id;
            if let Some(t) = st.tabs.iter_mut().find(|t| t.state.id == tid) { t.engine.reload(); }
        }));
        add("tab-ctx-duplicate", Box::new(|bw| {
            let tid = bw.state.borrow().ctx_tab_id;
            let url = bw.state.borrow().tabs.iter().find(|t| t.state.id == tid).map(|t| t.state.url.clone());
            if let Some(u) = url { bw.new_tab(&u, true); }
        }));
        add("tab-ctx-mute", Box::new(|bw| {
            let mut st = bw.state.borrow_mut();
            let tid = st.ctx_tab_id;
            if let Some(t) = st.tabs.iter_mut().find(|t| t.state.id == tid) {
                t.state.muted = !t.state.muted;
                t.engine.set_muted(t.state.muted);
            }
            drop(st);
            bw.toolbar.queue_draw();
        }));
        add("tab-ctx-close", Box::new(|bw| { let tid = bw.state.borrow().ctx_tab_id; bw.close_tab(tid); }));
        add("reopen-tab", Box::new(|bw| {
            let last = bw.state.borrow_mut().closed_urls.pop();
            if let Some(u) = last { bw.new_tab(&u, true); }
        }));

        self.window.insert_action_group("win", Some(&group));
    }

    fn set_ua(self: &Rc<Self>, profile: &str) {
        let mut st = self.state.borrow_mut();
        st.config.settings.user_agent_profile = profile.to_string();
        let ver = st.config.settings.mac_platform_version.clone();
        let _ = st.config.save();
        for t in &mut st.tabs {
            t.engine.apply_user_agent(profile, &ver);
        }
    }

    fn set_zoom(self: &Rc<Self>, zoom: f64) {
        let mut st = self.state.borrow_mut();
        let active_id = st.active_id;
        if let Some(t) = st.tabs.iter_mut().find(|t| t.state.id == active_id) {
            t.state.zoom = zoom.clamp(0.5, 3.0);
            t.engine.set_zoom(t.state.zoom);
        }
    }

    fn adjust_zoom(self: &Rc<Self>, delta: f64) {
        let cur = {
            let st = self.state.borrow();
            st.tabs.iter().find(|t| t.state.id == st.active_id).map(|t| t.state.zoom).unwrap_or(1.0)
        };
        self.set_zoom(cur + delta);
    }

    fn bookmark_active(self: &Rc<Self>, folder: &str, toast: &str) {
        {
            let mut st = self.state.borrow_mut();
            let id = st.active_id;
            if let Some((title, url)) = st.tabs.iter().find(|t| t.state.id == id).map(|t| (t.state.title.clone(), t.state.url.clone())) {
                st.library.add_bookmark(&title, &url, folder, now_epoch_ms());
                let _ = st.library.save();
            }
        }
        self.show_toast(toast);
    }

    fn connect_events(self: &Rc<Self>, find_prev: &Button, find_next: &Button, find_done: &Button) {
        // Draw toolbar
        let st_draw = Rc::clone(&self.state);
        self.toolbar.set_draw_func(move |_area, cr, width, _height| {
            let mut st = st_draw.borrow_mut();
            let snaps = st.tab_snapshots();
            st.layout = compute_layout(width as f64, &snaps, st.active_id, st.private_mode);
            let p = PaintParams {
                layout: &st.layout,
                tabs: &snaps,
                active_id: st.active_id,
                private_mode: st.private_mode,
                sidebar_visible: st.sidebar_visible,
                overview_visible: st.overview_visible,
                address_focused: st.address_focused,
                window_active: true,
                traffic_hover: st.traffic_hover,
                hover: st.hover,
            };
            paint_toolbar(cr, &p);
        });

        // Hover tracking
        let motion = EventControllerMotion::new();
        let bw_move = Rc::clone(self);
        motion.connect_motion(move |_, x, y| {
            let mut st = bw_move.state.borrow_mut();
            let hit = hit_test(&st.layout, st.active_id, st.address_focused, x, y);
            let th = st.layout.traffic_group.contains(x, y);
            if hit != st.hover || th != st.traffic_hover {
                st.hover = hit;
                st.traffic_hover = th;
                drop(st);
                bw_move.toolbar.queue_draw();
            }
        });
        let bw_leave = Rc::clone(self);
        motion.connect_leave(move |_| {
            let mut st = bw_leave.state.borrow_mut();
            st.hover = HitResult::default();
            st.traffic_hover = false;
            drop(st);
            bw_leave.toolbar.queue_draw();
        });
        self.toolbar.add_controller(motion);

        // Left & right click handling
        let click = GestureClick::builder().button(0).build();
        let bw_click = Rc::clone(self);
        click.connect_pressed(move |g, _n, x, y| {
            let btn = g.current_button();
            let hit = {
                let st = bw_click.state.borrow();
                hit_test(&st.layout, st.active_id, st.address_focused, x, y)
            };
            if btn == 3 && matches!(hit.kind, HitKind::Tab | HitKind::Address | HitKind::TabAudio) {
                let muted = {
                    let mut st = bw_click.state.borrow_mut();
                    st.ctx_tab_id = hit.tab_id;
                    st.tabs.iter().find(|t| t.state.id == hit.tab_id).is_some_and(|t| t.state.muted)
                };
                let (can_others, has_closed) = {
                    let st = bw_click.state.borrow();
                    (st.tabs.len() > 1, !st.closed_urls.is_empty())
                };
                let menu = build_tab_menu(muted, can_others, has_closed);
                bw_click.menu_popover.set_menu_model(Some(&menu));
                bw_click.menu_popover.set_pointing_to(Some(&gdk::Rectangle::new(x as i32, y as i32, 1, 1)));
                bw_click.menu_popover.popup();
                return;
            }
            if btn != 1 {
                return;
            }
            match hit.kind {
                HitKind::TrafficClose => bw_click.window.close(),
                HitKind::TrafficMin => bw_click.window.minimize(),
                HitKind::TrafficMax => {
                    if bw_click.window.is_maximized() {
                        bw_click.window.unmaximize();
                    } else {
                        bw_click.window.maximize();
                    }
                }
                HitKind::Sidebar => {
                    let vis = {
                        let mut st = bw_click.state.borrow_mut();
                        st.sidebar_visible = !st.sidebar_visible;
                        st.sidebar_visible
                    };
                    bw_click.sidebar_panel.set_visible(vis);
                    bw_click.toolbar.queue_draw();
                }
                HitKind::Back => {
                    let mut st = bw_click.state.borrow_mut();
                    let id = st.active_id;
                    if let Some(t) = st.tabs.iter_mut().find(|t| t.state.id == id) { t.engine.go_back(); }
                }
                HitKind::Forward => {
                    let mut st = bw_click.state.borrow_mut();
                    let id = st.active_id;
                    if let Some(t) = st.tabs.iter_mut().find(|t| t.state.id == id) { t.engine.go_forward(); }
                }
                HitKind::Reload => {
                    let mut st = bw_click.state.borrow_mut();
                    let id = st.active_id;
                    if let Some(t) = st.tabs.iter_mut().find(|t| t.state.id == id) {
                        if t.state.loading {
                            t.engine.stop();
                        } else {
                            t.engine.reload();
                        }
                    }
                }
                HitKind::NewTab => {
                    let u = bw_click.state.borrow().home_url();
                    bw_click.new_tab(&u, true);
                }
                HitKind::Overview => bw_click.new_tab(&internal_url("overview.html"), true),
                HitKind::Menu => bw_click.show_main_menu(),
                HitKind::Share => bw_click.show_share_menu(),
                HitKind::Tab => bw_click.activate_tab(hit.tab_id),
                HitKind::TabClose => bw_click.close_tab(hit.tab_id),
                HitKind::TabAudio => {
                    let mut st = bw_click.state.borrow_mut();
                    if let Some(t) = st.tabs.iter_mut().find(|t| t.state.id == hit.tab_id) {
                        t.state.muted = !t.state.muted;
                        t.engine.set_muted(t.state.muted);
                    }
                    drop(st);
                    bw_click.toolbar.queue_draw();
                }
                HitKind::Address => bw_click.begin_address_edit(),
                HitKind::Reader => bw_click.show_toast("阅读器模式将在 Chromium 接入后激活"),
                HitKind::DragArea | HitKind::Overflow => {}
                HitKind::None => {}
            }
        });
        self.toolbar.add_controller(click);

        // Address entry submit & live suggestions
        let bw_act = Rc::clone(self);
        self.address_entry.connect_activate(move |e| {
            let text = e.text().to_string();
            bw_act.navigate_active(&text);
        });

        let bw_chg = Rc::clone(self);
        self.address_entry.connect_changed(move |e| {
            let q = e.text().to_string();
            while let Some(child) = bw_chg.suggest_list.first_child() {
                bw_chg.suggest_list.remove(&child);
            }
            if q.trim().is_empty() {
                bw_chg.suggest_popover.popdown();
                return;
            }
            let sugs = bw_chg.state.borrow().library.suggest(q.trim(), 8);
            if sugs.is_empty() {
                bw_chg.suggest_popover.popdown();
                return;
            }
            for s in sugs {
                let row = ListBoxRow::new();
                let lbl = Label::builder()
                    .label(format!("{} — {}", s.title, s.url))
                    .halign(gtk4::Align::Start)
                    .margin_start(8)
                    .margin_end(8)
                    .margin_top(4)
                    .margin_bottom(4)
                    .build();
                lbl.set_widget_name(&s.url);
                row.set_child(Some(&lbl));
                bw_chg.suggest_list.append(&row);
            }
            bw_chg.suggest_popover.popup();
        });

        let bw_row = Rc::clone(self);
        self.suggest_list.connect_row_activated(move |_, row| {
            if let Some(child) = row.child() {
                let url = child.widget_name().to_string();
                bw_row.navigate_active(&url);
            }
        });

        // Find bar buttons
        let bw_fp = Rc::clone(self);
        find_prev.connect_clicked(move |_| {
            let q = bw_fp.find_entry.text().to_string();
            let mut st = bw_fp.state.borrow_mut();
            let id = st.active_id;
            if let Some(t) = st.tabs.iter_mut().find(|t| t.state.id == id) { t.engine.find(&q, false); }
        });
        let bw_fn = Rc::clone(self);
        find_next.connect_clicked(move |_| {
            let q = bw_fn.find_entry.text().to_string();
            let mut st = bw_fn.state.borrow_mut();
            let id = st.active_id;
            if let Some(t) = st.tabs.iter_mut().find(|t| t.state.id == id) { t.engine.find(&q, true); }
        });
        let bw_fd = Rc::clone(self);
        find_done.connect_clicked(move |_| {
            let mut st = bw_fd.state.borrow_mut();
            st.find_visible = false;
            let id = st.active_id;
            if let Some(t) = st.tabs.iter_mut().find(|t| t.state.id == id) {
                t.engine.stop_find();
            }
            drop(st);
            bw_fd.find_bar.set_visible(false);
        });

        // Keyboard shortcuts
        let key_ctrl = EventControllerKey::new();
        let bw_key = Rc::clone(self);
        key_ctrl.connect_key_pressed(move |_, key, _code, mods| {
            let ctrl = mods.contains(gdk::ModifierType::CONTROL_MASK);
            let shift = mods.contains(gdk::ModifierType::SHIFT_MASK);
            if ctrl && key == gdk::Key::t {
                let u = bw_key.state.borrow().home_url();
                bw_key.new_tab(&u, true);
                return glib::Propagation::Stop;
            }
            if ctrl && shift && key == gdk::Key::T {
                let last = bw_key.state.borrow_mut().closed_urls.pop();
                if let Some(u) = last { bw_key.new_tab(&u, true); }
                return glib::Propagation::Stop;
            }
            if ctrl && key == gdk::Key::w {
                let id = bw_key.state.borrow().active_id;
                bw_key.close_tab(id);
                return glib::Propagation::Stop;
            }
            if ctrl && key == gdk::Key::l {
                bw_key.begin_address_edit();
                return glib::Propagation::Stop;
            }
            if ctrl && key == gdk::Key::f {
                bw_key.state.borrow_mut().find_visible = true;
                bw_key.find_bar.set_visible(true);
                bw_key.find_entry.grab_focus();
                return glib::Propagation::Stop;
            }
            if (ctrl && key == gdk::Key::r) || key == gdk::Key::F5 {
                let mut st = bw_key.state.borrow_mut();
                let id = st.active_id;
                if let Some(t) = st.tabs.iter_mut().find(|t| t.state.id == id) { t.engine.reload(); }
                return glib::Propagation::Stop;
            }
            if key == gdk::Key::F11 {
                let fs = { let mut st = bw_key.state.borrow_mut(); st.fullscreen = !st.fullscreen; st.fullscreen };
                if fs { bw_key.window.fullscreen(); } else { bw_key.window.unfullscreen(); }
                return glib::Propagation::Stop;
            }
            if key == gdk::Key::Escape {
                bw_key.state.borrow_mut().address_focused = false;
                bw_key.address_entry.set_visible(false);
                bw_key.suggest_popover.popdown();
                bw_key.toolbar.queue_draw();
                return glib::Propagation::Stop;
            }
            glib::Propagation::Proceed
        });
        self.window.add_controller(key_ctrl);
    }
}
