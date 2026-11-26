use crate::audio_watcher::AudioWatcher;
use crate::engine::{CefEngine, PlaceholderEngine, TabState, WebEngine};
use crate::menus::{build_main_menu, build_share_menu, build_tab_menu, MainMenuContext};
use crate::paint_bar::{
    compute_layout, hit_test, paint_toolbar, HitKind, HitResult, PaintParams, ToolbarLayout,
    TOPBAR_HEIGHT,
};
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
use std::sync::{Arc, Mutex};
use std::time::Duration;
use ulb_cef::{BrowserEvent, BrowserPolicy, CefConfig, CefRuntime};
use ulb_core::audio_dsp::{build_audio_dsp_script, build_audio_update_script};
use ulb_core::config::{ConfigStore, DEFAULT_HOME_URL};
use ulb_core::extension_pkg::{find_recommended_extension, store_download_url};
use ulb_core::extensions_store::ExtensionsStore;
use ulb_core::f8964_pack::extract_pack;
use ulb_core::internal_pages::internal_url;
use ulb_core::library::{Library, BOOKMARKS_FOLDER, FAVORITES_FOLDER};
use ulb_core::line_store::LineStore;
use ulb_core::ui_bridge::{dispatch_rpc, BridgeContext, SideEffect, TabView};
use ulb_core::user_agent::build_macos_edge_user_agent;
use ulb_core::user_scripts::UserScriptStore;
use ulb_core::util::{domain_of, now_epoch_ms};
use ulb_core::warp_host::{
    load_encrypted_lines, run_download_extension, run_fetch_subscription, run_probe,
    save_encrypted_lines, WarpConfig, WarpStatus, WarpSupervisor,
};

/// Shared mutable paths/binaries needed across the browser window lifetime.
#[derive(Clone)]
pub struct AppPaths {
    pub config_dir: PathBuf,
    pub data_dir: PathBuf,
    pub warp_bin: Option<PathBuf>,
}

/// Thread-safe core stores shared between GTK UI thread and CEF `ulb.internal/__rpc`.
struct HostStores {
    paths: AppPaths,
    config: ConfigStore,
    library: Library,
    lines: LineStore,
    extensions: ExtensionsStore,
    scripts: UserScriptStore,
    warp_status: WarpStatus,
    tabs_view: Vec<TabView>,
    active_tab_id: u64,
    private_mode: bool,
    total_blocked_ads: u64,
    rpc_calls: u64,
    pending_fx: Vec<SideEffect>,
}

impl HostStores {
    fn save_all(&mut self) {
        let _ = self.config.save();
        if !self.private_mode {
            let _ = self.library.save();
        }
        let _ = save_encrypted_lines(&self.paths.data_dir, &self.lines);
    }

    fn build_policy(&mut self, headphones_active: bool) -> BrowserPolicy {
        let base_linux_ua = "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/156.0.0.0 Safari/537.36";
        let ua_str = build_macos_edge_user_agent(
            base_linux_ua,
            &self.config.settings.user_agent_profile,
        );
        self.scripts.load();
        let early_scripts_js = self.scripts.injection_scripts();
        let idle_scripts_js = Vec::new();

        BrowserPolicy {
            ua_profile: self.config.settings.user_agent_profile.clone(),
            user_agent_string: ua_str,
            mac_platform_version: self.config.settings.mac_platform_version.clone(),
            mac_profile_idx: 0,
            early_scripts_js,
            idle_scripts_js,
            audio_dsp_js: build_audio_dsp_script(&self.config.settings, headphones_active),
            adblock_enabled: self.config.settings.enable_ad_block,
            adblock_disabled_hosts: self.library.hosts_with_adblock_off(),
        }
    }
}

struct TabItem {
    state: TabState,
    engine: Box<dyn WebEngine>,
}

struct AppState {
    stores: Arc<Mutex<HostStores>>,
    policy: Arc<Mutex<BrowserPolicy>>,
    cef_active: bool,
    cef_error: String,
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
    _session: SessionDir,
    _ipc: Option<IpcGuard>,
    session_path_str: String,
}

impl AppState {
    fn sync_stores_tabs(&self) {
        let mut s = self.stores.lock().unwrap();
        s.active_tab_id = self.active_id;
        s.tabs_view = self
            .tabs
            .iter()
            .map(|t| TabView {
                id: t.state.id,
                title: t.state.title.clone(),
                url: t.state.url.clone(),
                loading: t.state.loading,
                audio: t.state.audio,
                muted: t.state.muted,
                active: t.state.id == self.active_id,
                reader_source: t.state.reader_source.clone(),
            })
            .collect();
    }

    fn sync_policy(&mut self) {
        let hp = self
            .audio_watcher
            .as_ref()
            .is_some_and(|w| w.is_headphones_active());
        let next_policy = self.stores.lock().unwrap().build_policy(hp);
        *self.policy.lock().unwrap() = next_policy;
        let audio_update = {
            let st = self.stores.lock().unwrap();
            build_audio_update_script(&st.config.settings, hp)
        };
        for t in &mut self.tabs {
            t.engine.execute_js(&audio_update);
        }
    }

    fn restart_warp(&mut self) {
        if let Some(mut s) = self.warp_sup.take() {
            s.stop();
        }
        let cfg_opt = {
            let mut st = self.stores.lock().unwrap();
            st.warp_status = WarpStatus::default();
            if !st.config.settings.warp_enabled {
                None
            } else if let Some(ref bin) = st.paths.warp_bin {
                let selected_id = st.lines.selected().to_string();
                let selected_links = st
                    .lines
                    .selected_links()
                    .into_iter()
                    .map(str::to_string)
                    .collect();
                Some(WarpConfig {
                    binary: bin.clone(),
                    state_dir: st.paths.data_dir.join("warp-state"),
                    fail_closed: st.config.settings.warp_fail_closed,
                    use_line: st.config.settings.use_line,
                    selected_id,
                    selected_links,
                })
            } else {
                st.warp_status.error = "未找到 ulb-warp 组件".to_string();
                None
            }
        };

        if let Some(cfg) = cfg_opt {
            if let Ok(sup) = WarpSupervisor::start(&cfg, |_| {}) {
                let status = sup.status();
                self.stores.lock().unwrap().warp_status = status;
                self.warp_sup = Some(sup);
            }
        }
    }

    fn home_url(&self) -> String {
        let st = self.stores.lock().unwrap();
        if st.config.settings.new_tab_page == "blank" {
            "about:blank".to_string()
        } else if st.config.settings.start_url.is_empty()
            || st.config.settings.start_url == DEFAULT_HOME_URL
        {
            internal_url("start.html")
        } else {
            st.config.settings.start_url.clone()
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
        let q = url_encode(trimmed);
        let engine = self.stores.lock().unwrap().config.settings.search_engine.clone();
        match engine.as_str() {
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

pub struct BrowserWindow {
    window: ApplicationWindow,
    toolbar: DrawingArea,
    address_entry: Entry,
    suggest_popover: Popover,
    suggest_list: ListBox,
    find_bar: GtkBox,
    find_entry: Entry,
    find_status: Label,
    sidebar_panel: GtkBox,
    sidebar_engine: RefCell<Option<Box<dyn WebEngine>>>,
    content_stack: Stack,
    toast_label: Label,
    menu_popover: PopoverMenu,
    layout: RefCell<ToolbarLayout>,
    cef_runtime: RefCell<Option<CefRuntime>>,
    state: Rc<RefCell<AppState>>,
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

fn extract_memorial_pack(paths: &AppPaths, session_path: &std::path::Path) -> PathBuf {
    let dest = session_path.join("f8964");
    let mut candidates = vec![paths.data_dir.join("f8964.pack")];
    if let Ok(exe) = std::env::current_exe() {
        if let Some(dir) = exe.parent() {
            candidates.push(dir.join("f8964.pack"));
        }
    }
    for src in candidates {
        if let Ok(bytes) = std::fs::read(&src) {
            if extract_pack(&bytes, &dest).is_ok() {
                break;
            }
        }
    }
    dest
}

pub fn run_browser(
    app: &Application,
    paths: AppPaths,
    session: SessionDir,
    initial_urls: Vec<String>,
    private_mode: bool,
    smoke_test: bool,
) {
    let mut config = ConfigStore::new(&paths.config_dir);
    config.load();
    if config.settings.user_agent_profile == "default" {
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

    let mut host_stores = HostStores {
        paths: paths.clone(),
        config,
        library,
        lines,
        extensions,
        scripts,
        warp_status: WarpStatus::default(),
        tabs_view: Vec::new(),
        active_tab_id: 0,
        private_mode,
        total_blocked_ads: 0,
        rpc_calls: 0,
        pending_fx: Vec::new(),
    };

    let init_policy = host_stores.build_policy(false);
    let ext_folder = host_stores.extensions.folder().to_path_buf();
    let enabled_ext_dirs: Vec<PathBuf> = host_stores
        .extensions
        .list()
        .iter()
        .filter(|e| e.enabled)
        .map(|e| ext_folder.join(&e.dir))
        .collect();
    let ua_for_cef = init_policy.user_agent_string.clone();
    let fail_closed = host_stores.config.settings.warp_fail_closed;
    let warp_enabled = host_stores.config.settings.warp_enabled;
    let memorial_dir = extract_memorial_pack(&paths, session.path());

    let stores = Arc::new(Mutex::new(host_stores));
    let policy = Arc::new(Mutex::new(init_policy));

    let stores_rpc = Arc::clone(&stores);
    let rpc_cb: ulb_cef::RpcCallback = Arc::new(Mutex::new(Box::new(move |raw_json: &str| {
        let mut st = stores_rpc.lock().unwrap();
        let active_id = st.active_tab_id;
        let private = st.private_mode;
        let blocked = st.total_blocked_ads;
        let st_ptr = &mut *st;
        let mut ctx = BridgeContext {
            config: &mut st_ptr.config,
            library: &mut st_ptr.library,
            lines: &mut st_ptr.lines,
            extensions: &mut st_ptr.extensions,
            scripts: &mut st_ptr.scripts,
            warp: &st_ptr.warp_status,
            tabs: &st_ptr.tabs_view,
            reader: None,
            private_mode: private,
            active_tab_id: active_id,
            total_blocked_ads: blocked,
        };
        if let Some(outcome) = dispatch_rpc(&mut ctx, raw_json) {
            st.rpc_calls += 1;
            st.pending_fx.extend(outcome.side_effects);
            outcome.response_json
        } else {
            r#"{"error":"invalid rpc"}"#.to_string()
        }
    })));

    // Start WARP before CEF subprocesses/networking come up if enabled.
    let mut initial_warp_sup = None;
    if warp_enabled {
        let mut st = stores.lock().unwrap();
        if let Some(ref bin) = st.paths.warp_bin {
            let cfg = WarpConfig {
                binary: bin.clone(),
                state_dir: st.paths.data_dir.join("warp-state"),
                fail_closed: st.config.settings.warp_fail_closed,
                use_line: st.config.settings.use_line,
                selected_id: st.lines.selected().to_string(),
                selected_links: st
                    .lines
                    .selected_links()
                    .into_iter()
                    .map(str::to_string)
                    .collect(),
            };
            if let Ok(sup) = WarpSupervisor::start(&cfg, |_| {}) {
                st.warp_status = sup.status();
                initial_warp_sup = Some(sup);
            }
        }
    }
    let initial_socks_port = stores.lock().unwrap().warp_status.port;

    let (cef_runtime, cef_error) = match CefRuntime::init(CefConfig {
        session_dir: session.path().to_path_buf(),
        memorial_dir,
        extension_dirs: enabled_ext_dirs,
        user_agent: ua_for_cef,
        socks_port: initial_socks_port,
        fail_closed: warp_enabled && fail_closed,
        rpc: rpc_cb,
    }) {
        Ok(rt) => {
            let mut st = stores.lock().unwrap();
            let loaded_ids = rt.loaded_extension_ids.clone();
            let folder = st.extensions.folder().to_path_buf();
            for (path, id) in loaded_ids {
                if let Some(dir_name) = path.strip_prefix(&folder).ok().and_then(|p| p.to_str()) {
                    let dir_string = dir_name.to_string();
                    st.extensions.mark_runtime_status(&dir_string, &id, "");
                }
            }
            (Some(rt), String::new())
        }
        Err(e) => (None, e),
    };

    let session_path_str = session.path().display().to_string();
    let app_state = AppState {
        stores,
        policy,
        cef_active: cef_runtime.is_some(),
        cef_error,
        warp_sup: initial_warp_sup,
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
        session_path_str,
        _session: session,
        _ipc: ipc_guard,
    };

    let state = Rc::new(RefCell::new(app_state));
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

    let suggest_list = ListBox::new();
    let suggest_scroll = ScrolledWindow::builder()
        .min_content_width(420)
        .max_content_height(260)
        .child(&suggest_list)
        .build();
    let suggest_popover = Popover::builder().child(&suggest_scroll).autohide(true).build();
    suggest_popover.set_parent(&address_entry);

    let find_bar = GtkBox::builder()
        .orientation(Orientation::Horizontal)
        .spacing(6)
        .margin_start(12)
        .margin_end(12)
        .margin_top(6)
        .margin_bottom(6)
        .visible(false)
        .build();
    let find_entry = Entry::builder().placeholder_text("在页面中查找…").hexpand(true).build();
    let find_status = Label::builder().label("").build();
    let find_prev = Button::with_label("上一个");
    let find_next = Button::with_label("下一个");
    let find_done = Button::with_label("完成");
    find_bar.append(&find_entry);
    find_bar.append(&find_status);
    find_bar.append(&find_prev);
    find_bar.append(&find_next);
    find_bar.append(&find_done);
    root_box.append(&find_bar);

    let content_row = GtkBox::builder()
        .orientation(Orientation::Horizontal)
        .hexpand(true)
        .vexpand(true)
        .build();
    let sidebar_panel = GtkBox::builder()
        .orientation(Orientation::Vertical)
        .width_request(260)
        .visible(false)
        .build();
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
        find_status,
        sidebar_panel,
        sidebar_engine: RefCell::new(None),
        content_stack,
        toast_label,
        menu_popover,
        layout: RefCell::new(ToolbarLayout::default()),
        cef_runtime: RefCell::new(cef_runtime),
        state: Rc::clone(&state),
    });

    bw.install_actions();
    bw.connect_events(&find_prev, &find_next, &find_done);

    let smoke_headers: Arc<Mutex<Option<String>>> = Arc::new(Mutex::new(None));
    if smoke_test {
        let hdr_out = Arc::clone(&smoke_headers);
        let listener = std::net::TcpListener::bind("127.0.0.1:0").expect("smoke listener");
        let port = listener.local_addr().unwrap().port();
        std::thread::spawn(move || {
            use std::io::{Read, Write};
            for stream in listener.incoming().take(3) {
                let Ok(mut s) = stream else { break };
                let mut buf = [0u8; 4096];
                let n = s.read(&mut buf).unwrap_or(0);
                let req = String::from_utf8_lossy(&buf[..n]).to_string();
                let mut guard = hdr_out.lock().unwrap();
                if guard.is_none() && req.contains("User-Agent:") {
                    *guard = Some(req);
                }
                let body = "<!doctype html><title>SMOKE_HTTP_OK</title><body>smoke</body>";
                let _ = write!(
                    s,
                    "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}",
                    body.len(),
                    body
                );
            }
        });
        bw.new_tab(&internal_url("start.html"), false);
        bw.new_tab(&format!("http://127.0.0.1:{port}/"), true);
    } else if initial_urls.is_empty() {
        let start = bw.state.borrow().home_url();
        bw.new_tab(&start, true);
    } else {
        for (i, u) in initial_urls.iter().enumerate() {
            bw.new_tab(u, i == 0);
        }
    }

    // 16ms UI tick: pump CEF event loop, repaint active OSR frame, handle tab events & RPC side effects.
    let bw_tick = Rc::clone(&bw);
    let smoke_started = std::time::Instant::now();
    let smoke_hdr_tick = Arc::clone(&smoke_headers);
    glib::timeout_add_local(Duration::from_millis(16), move || {
        while let Ok(cmd) = ipc_rx.try_recv() {
            match cmd {
                IpcCommand::Raise => bw_tick.window.present(),
                IpcCommand::Open(url) => {
                    bw_tick.new_tab(&url, true);
                    bw_tick.window.present();
                }
            }
        }

        bw_tick.pump_and_process();

        if smoke_test {
            let (painted, rpc_cnt, warp_up) = {
                let st = bw_tick.state.borrow();
                let p = st.tabs.iter().any(|t| t.engine.has_painted_frame());
                let host = st.stores.lock().unwrap();
                (p, host.rpc_calls, host.warp_status.port)
            };
            let hdr_opt = smoke_hdr_tick.lock().unwrap().clone();
            if let Some(ref hdr) = hdr_opt {
                if painted && rpc_cnt >= 1 {
                    let has_mac = hdr.contains("Macintosh; Intel Mac OS X 10_15_7")
                        && hdr.contains("Edg/");
                    let has_ch = hdr.to_ascii_lowercase().contains("sec-ch-ua-platform: \"macos\"");
                    println!(
                        "SMOKE_PASS painted={painted} rpc_calls={rpc_cnt} warp_port={warp_up} mac_ua={has_mac} mac_ch={has_ch}"
                    );
                    bw_tick.window.close();
                    return glib::ControlFlow::Break;
                }
            }
            if smoke_started.elapsed() >= Duration::from_secs(7) {
                eprintln!(
                    "SMOKE_TIMEOUT painted={painted} rpc_calls={rpc_cnt} has_hdr={}",
                    hdr_opt.is_some()
                );
                bw_tick.window.close();
                return glib::ControlFlow::Break;
            }
        }
        glib::ControlFlow::Continue
    });

    window.connect_close_request({
        let bw_close = Rc::clone(&bw);
        move |_| {
            {
                let mut st = bw_close.state.borrow_mut();
                st.tabs.clear();
                st.stores.lock().unwrap().save_all();
                if let Some(mut w) = st.warp_sup.take() {
                    w.stop();
                }
                if let Some(mut a) = st.audio_watcher.take() {
                    a.stop();
                }
            }
            bw_close.sidebar_engine.borrow_mut().take();
            bw_close.cef_runtime.borrow_mut().take();
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
        glib::timeout_add_local_once(Duration::from_millis(1800), move || {
            lbl.set_visible(false);
        });
    }

    fn pump_and_process(self: &Rc<Self>) {
        if let Some(ref rt) = *self.cef_runtime.borrow() {
            rt.pump();
        }

        let mut popups = Vec::new();
        let mut blocked_count = 0u64;
        let mut find_upd = None;
        let mut need_redraw = false;

        {
            let mut st = self.state.borrow_mut();

            if let Some(ref sup) = st.warp_sup {
                let latest = sup.status();
                let mut host = st.stores.lock().unwrap();
                if latest != host.warp_status {
                    host.warp_status = latest;
                    need_redraw = true;
                }
            }

            let save_history = !st.private_mode && st.stores.lock().unwrap().config.settings.save_history;
            let mut visited = Vec::new();

            for item in &mut st.tabs {
                let evs = item.engine.poll_events();
                for ev in evs {
                    need_redraw = true;
                    match ev {
                        BrowserEvent::TitleChanged(t) => item.state.title = t,
                        BrowserEvent::UrlChanged(u) => {
                            item.state.url = u.clone();
                            if save_history && u.starts_with("http") && !u.contains(".internal") {
                                visited.push((u, item.state.title.clone()));
                            }
                        }
                        BrowserEvent::LoadingChanged {
                            loading,
                            can_go_back,
                            can_go_forward,
                        } => {
                            if loading && !item.state.loading {
                                item.state.load_started_ms = now_epoch_ms();
                            }
                            item.state.loading = loading;
                            item.state.can_go_back = can_go_back;
                            item.state.can_go_forward = can_go_forward;
                        }
                        BrowserEvent::AudioChanged(a) => item.state.audio = a,
                        BrowserEvent::FindResult { count, active_ordinal } => {
                            find_upd = Some((count, active_ordinal));
                        }
                        BrowserEvent::OpenPopup(u) => popups.push(u),
                        BrowserEvent::BlockedResource => blocked_count += 1,
                    }
                }
            }

            if blocked_count > 0 || !visited.is_empty() {
                let mut host = st.stores.lock().unwrap();
                host.total_blocked_ads += blocked_count;
                for (u, t) in visited {
                    host.library.record_visit(&u, &t, now_epoch_ms());
                }
            }
            st.sync_stores_tabs();
        }

        if let Some((count, active_ord)) = find_upd {
            self.find_status.set_text(&if count == 0 {
                "0 个匹配".to_string()
            } else {
                format!("{active_ord}/{count}")
            });
        }

        if let Some(ref mut sb) = *self.sidebar_engine.borrow_mut() {
            for ev in sb.poll_events() {
                if let BrowserEvent::OpenPopup(u) = ev {
                    popups.push(u);
                }
            }
        }

        for u in popups {
            self.new_tab(&u, true);
        }

        let fx_batch: Vec<SideEffect> = {
            let st = self.state.borrow();
            let mut host = st.stores.lock().unwrap();
            std::mem::take(&mut host.pending_fx)
        };
        for fx in fx_batch {
            self.apply_side_effect(fx);
        }

        if need_redraw || self.state.borrow().tabs.iter().any(|t| t.state.loading) {
            self.toolbar.queue_draw();
        }
    }

    fn apply_side_effect(self: &Rc<Self>, fx: SideEffect) {
        match fx {
            SideEffect::SaveConfig => {
                let _ = self.state.borrow().stores.lock().unwrap().config.save();
            }
            SideEffect::SaveLibrary => {
                let stores = Arc::clone(&self.state.borrow().stores);
                let mut st = stores.lock().unwrap();
                if !st.private_mode {
                    let _ = st.library.save();
                }
            }
            SideEffect::SaveLines => {
                let stores = Arc::clone(&self.state.borrow().stores);
                let st = stores.lock().unwrap();
                let _ = save_encrypted_lines(&st.paths.data_dir, &st.lines);
            }
            SideEffect::SyncAdblock => {
                self.state.borrow_mut().sync_policy();
            }
            SideEffect::RestartWarp => {
                self.state.borrow_mut().restart_warp();
                self.show_toast("网络线路配置已更新");
            }
            SideEffect::RescanWarp => {
                if let Some(ref mut sup) = self.state.borrow_mut().warp_sup {
                    sup.rescan();
                }
            }
            SideEffect::OpenUrl { url, new_tab, activate } => {
                if new_tab {
                    self.new_tab(&url, activate);
                } else {
                    self.navigate_active(&url);
                }
            }
            SideEffect::ActivateTab(id) => self.activate_tab(id),
            SideEffect::CloseTab(id) => self.close_tab(id),
            SideEffect::NewTab(act) => {
                let u = self.state.borrow().home_url();
                self.new_tab(&u, act);
            }
            SideEffect::OpenExtensionPage { dir, options } => {
                let url_opt = self
                    .state
                    .borrow()
                    .stores
                    .lock()
                    .unwrap()
                    .extensions
                    .page_url(&dir, options);
                if let Some(u) = url_opt {
                    self.new_tab(&u, true);
                } else {
                    self.show_toast("扩展未激活或未声明页面");
                }
            }
            SideEffect::ProbeLines(lines) => {
                let bin_opt = self.state.borrow().stores.lock().unwrap().paths.warp_bin.clone();
                if let Some(bin) = bin_opt {
                    std::thread::spawn(move || {
                        let _ = run_probe(&bin, &lines, Duration::from_secs(15));
                    });
                }
            }
            SideEffect::FetchSubscription { id, url } => {
                let stores = Arc::clone(&self.state.borrow().stores);
                let (bin_opt, socks) = {
                    let s = stores.lock().unwrap();
                    (s.paths.warp_bin.clone(), s.warp_status.port)
                };
                if let Some(bin) = bin_opt {
                    std::thread::spawn(move || {
                        if let Ok(links) = run_fetch_subscription(&bin, &url, socks, Duration::from_secs(20)) {
                            let refs: Vec<&str> = links.iter().map(String::as_str).collect();
                            let mut s = stores.lock().unwrap();
                            s.lines.replace_sub(&id, &refs, now_epoch_ms());
                            let _ = save_encrypted_lines(&s.paths.data_dir, &s.lines);
                        }
                    });
                }
            }
            SideEffect::DownloadRecommendedExtension(key) => {
                if let Some(rec) = find_recommended_extension(&key) {
                    let stores = Arc::clone(&self.state.borrow().stores);
                    let (bin_opt, socks, data_dir) = {
                        let s = stores.lock().unwrap();
                        (s.paths.warp_bin.clone(), s.warp_status.port, s.paths.data_dir.clone())
                    };
                    if let Some(bin) = bin_opt {
                        let url = if rec.url.is_empty() {
                            store_download_url(rec.store_id)
                        } else {
                            rec.url.to_string()
                        };
                        let crx_dest = data_dir.join(format!("{}.crx", rec.key));
                        std::thread::spawn(move || {
                            if run_download_extension(
                                &bin,
                                &url,
                                &crx_dest,
                                rec.store_id,
                                rec.sha256,
                                socks,
                                Duration::from_secs(45),
                            )
                            .is_ok()
                            {
                                let mut s = stores.lock().unwrap();
                                let _ = s.extensions.install_package(&crx_dest, rec.key);
                                let _ = std::fs::remove_file(&crx_dest);
                            }
                        });
                    }
                }
            }
            SideEffect::ReloadExtensionsRuntime => {
                self.show_toast("扩展列表已更新，将在下次启动时应用到 Chromium");
            }
            SideEffect::CloseOverview | SideEffect::ExitReader | SideEffect::NewPrivateWindow => {}
        }
    }

    fn new_tab(self: &Rc<Self>, url: &str, activate: bool) {
        let child_widget: Widget;
        let id = {
            let mut st = self.state.borrow_mut();
            let id = st.next_tab_id;
            st.next_tab_id += 1;
            let is_loading = st.cef_active;
            let engine: Box<dyn WebEngine> = if is_loading {
                Box::new(CefEngine::new(url, Arc::clone(&st.policy)))
            } else {
                Box::new(PlaceholderEngine::new(url, &st.cef_error))
            };
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
                    loading: is_loading,
                    load_started_ms: now_epoch_ms(),
                    can_go_back: false,
                    can_go_forward: false,
                    audio: false,
                    muted: false,
                    reader_available: false,
                    reader_source: String::new(),
                    zoom: 1.0,
                },
                engine,
            });
            if activate {
                st.active_id = id;
            }
            st.sync_stores_tabs();
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
            st.sync_stores_tabs();
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
            st.sync_stores_tabs();
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
            let save_history =
                !st.private_mode && st.stores.lock().unwrap().config.settings.save_history;
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
            if save_history && resolved.starts_with("http") && !resolved.contains(".internal") {
                st.stores.lock().unwrap().library.record_visit(&resolved, &title, now_epoch_ms());
            }
            st.address_focused = false;
            st.sync_stores_tabs();
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
            (self.layout.borrow().address_text, u)
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
        let st = self.state.borrow();
        let active = st.tabs.iter().find(|t| t.state.id == st.active_id);
        let web_page =
            active.is_some_and(|t| t.state.url.starts_with("http") && !t.state.url.contains(".internal"));
        let zoom = active.map(|t| t.state.zoom).unwrap_or(1.0);
        let reader_active = active.is_some_and(|t| !t.state.reader_source.is_empty());
        let host = active.map(|t| domain_of(&t.state.url)).unwrap_or_default();
        let headphones_detected = st
            .audio_watcher
            .as_ref()
            .is_some_and(|w| w.is_headphones_active());
        let mut stores = st.stores.lock().unwrap();
        let host_adblock = stores.library.site(&host).adblock;
        let extension_count = stores.extensions.list().len();
        let script_count = stores.scripts.scripts().len();
        let menu = build_main_menu(&MainMenuContext {
            settings: &stores.config.settings,
            warp: &stores.warp_status,
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
        drop(stores);
        let r = self.layout.borrow().menu_btn;
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
        let r = self.layout.borrow().share_btn;
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

        add("memorial", Box::new(|bw| bw.new_tab("https://f8964.internal/index.html", true)));
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
            bw.show_toast(&format!("UltraLightBrowser · Omarchy CEF 156 ({tmp})"));
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
            {
                let st = bw.state.borrow();
                let mut host = st.stores.lock().unwrap();
                host.config.settings.enable_ad_block = !host.config.settings.enable_ad_block;
                let _ = host.config.save();
            }
            bw.state.borrow_mut().sync_policy();
        }));
        add("toggle-site-adblock", Box::new(|bw| {
            {
                let st = bw.state.borrow();
                let active_id = st.active_id;
                if let Some(url) = st.tabs.iter().find(|t| t.state.id == active_id).map(|t| t.state.url.clone()) {
                    let dom = domain_of(&url);
                    let mut host = st.stores.lock().unwrap();
                    if !dom.is_empty() && !host.private_mode {
                        let mut site = host.library.site(&dom);
                        site.adblock = !site.adblock;
                        host.library.set_site(&dom, site);
                        let _ = host.library.save();
                    }
                }
            }
            bw.state.borrow_mut().sync_policy();
        }));
        add("audio-passthrough", Box::new(|bw| {
            {
                let st = bw.state.borrow();
                let mut host = st.stores.lock().unwrap();
                host.config.settings.system_audio_passthrough = true;
                let _ = host.config.save();
            }
            bw.state.borrow_mut().sync_policy();
        }));
        add("audio-surround", Box::new(|bw| {
            {
                let st = bw.state.borrow();
                let mut host = st.stores.lock().unwrap();
                host.config.settings.system_audio_passthrough = false;
                host.config.settings.enable_surround_sound = true;
                let _ = host.config.save();
            }
            bw.state.borrow_mut().sync_policy();
        }));
        add("audio-vocal", Box::new(|bw| {
            {
                let st = bw.state.borrow();
                let mut host = st.stores.lock().unwrap();
                host.config.settings.enable_vocal_boost = !host.config.settings.enable_vocal_boost;
                let _ = host.config.save();
            }
            bw.state.borrow_mut().sync_policy();
        }));
        add("audio-night", Box::new(|bw| {
            {
                let st = bw.state.borrow();
                let mut host = st.stores.lock().unwrap();
                host.config.settings.enable_night_mode = !host.config.settings.enable_night_mode;
                let _ = host.config.save();
            }
            bw.state.borrow_mut().sync_policy();
        }));
        add("warp-rescan", Box::new(|bw| {
            if let Some(ref mut sup) = bw.state.borrow_mut().warp_sup { sup.rescan(); }
        }));
        add("net-direct", Box::new(|bw| {
            {
                let st = bw.state.borrow();
                let mut host = st.stores.lock().unwrap();
                host.config.settings.warp_enabled = false;
                let _ = host.config.save();
            }
            bw.state.borrow_mut().restart_warp();
        }));
        add("net-warp", Box::new(|bw| {
            {
                let st = bw.state.borrow();
                let mut host = st.stores.lock().unwrap();
                host.config.settings.warp_enabled = true;
                host.config.settings.use_line = false;
                let _ = host.config.save();
            }
            bw.state.borrow_mut().restart_warp();
        }));
        add("net-line", Box::new(|bw| {
            {
                let st = bw.state.borrow();
                let mut host = st.stores.lock().unwrap();
                host.config.settings.warp_enabled = true;
                host.config.settings.use_line = true;
                let _ = host.config.save();
            }
            bw.state.borrow_mut().restart_warp();
        }));
        add("warp-fail-closed", Box::new(|bw| {
            {
                let st = bw.state.borrow();
                let mut host = st.stores.lock().unwrap();
                host.config.settings.warp_fail_closed = !host.config.settings.warp_fail_closed;
                let _ = host.config.save();
            }
            bw.state.borrow_mut().restart_warp();
        }));
        add("ua-default", Box::new(|bw| bw.set_ua("default")));
        add("ua-macos-edge", Box::new(|bw| bw.set_ua("macos-edge")));
        add("copy-url", Box::new(|bw| {
            let url = {
                let st = bw.state.borrow();
                st.tabs.iter().find(|t| t.state.id == st.active_id).map(|t| t.state.url.clone()).unwrap_or_default()
            };
            if let Some(display) = gdk::Display::default() {
                display.clipboard().set_text(&url);
                bw.show_toast("已复制链接");
            }
        }));
        add("add-bookmark", Box::new(|bw| bw.bookmark_active(BOOKMARKS_FOLDER, "已添加书签")));
        add("add-favorite", Box::new(|bw| bw.bookmark_active(FAVORITES_FOLDER, "已添加到个人收藏")));
        add("add-reading", Box::new(|bw| {
            {
                let st = bw.state.borrow();
                let id = st.active_id;
                if let Some((title, url)) = st.tabs.iter().find(|t| t.state.id == id).map(|t| (t.state.title.clone(), t.state.url.clone())) {
                    let mut host = st.stores.lock().unwrap();
                    host.library.add_reading(&title, &url, now_epoch_ms());
                    let _ = host.library.save();
                }
            }
            bw.show_toast("已添加到阅读列表");
        }));
        add("tab-ctx-reload", Box::new(|bw| {
            let mut st = bw.state.borrow_mut();
            let tid = st.ctx_tab_id;
            if let Some(t) = st.tabs.iter_mut().find(|t| t.state.id == tid) { t.engine.reload(); }
        }));
        add("tab-ctx-duplicate", Box::new(|bw| {
            let url = {
                let st = bw.state.borrow();
                st.tabs.iter().find(|t| t.state.id == st.ctx_tab_id).map(|t| t.state.url.clone())
            };
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
        {
            let st = self.state.borrow();
            let mut host = st.stores.lock().unwrap();
            host.config.settings.user_agent_profile = profile.to_string();
            let _ = host.config.save();
        }
        self.state.borrow_mut().sync_policy();
        self.show_toast("UA 配置已更新（刷新页面生效）");
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
            let st = self.state.borrow();
            let id = st.active_id;
            if let Some((title, url)) = st.tabs.iter().find(|t| t.state.id == id).map(|t| (t.state.title.clone(), t.state.url.clone())) {
                let mut host = st.stores.lock().unwrap();
                host.library.add_bookmark(&title, &url, folder, now_epoch_ms());
                let _ = host.library.save();
            }
        }
        self.show_toast(toast);
    }

    fn connect_events(self: &Rc<Self>, find_prev: &Button, find_next: &Button, find_done: &Button) {
        let bw_draw = Rc::clone(self);
        self.toolbar.set_draw_func(move |_area, cr, width, _height| {
            let Ok(st) = bw_draw.state.try_borrow() else {
                return;
            };
            let snaps = st.tab_snapshots();
            let layout = compute_layout(width as f64, &snaps, st.active_id, st.private_mode);
            let p = PaintParams {
                layout: &layout,
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
            *bw_draw.layout.borrow_mut() = layout;
        });

        let motion = EventControllerMotion::new();
        let bw_move = Rc::clone(self);
        motion.connect_motion(move |_, x, y| {
            let layout = bw_move.layout.borrow().clone();
            let mut st = bw_move.state.borrow_mut();
            let hit = hit_test(&layout, st.active_id, st.address_focused, x, y);
            let th = layout.traffic_group.contains(x, y);
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

        let click = GestureClick::builder().button(0).build();
        let bw_click = Rc::clone(self);
        click.connect_pressed(move |g, _n, x, y| {
            let btn = g.current_button();
            let hit = {
                let layout = bw_click.layout.borrow();
                let st = bw_click.state.borrow();
                hit_test(&layout, st.active_id, st.address_focused, x, y)
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
                    if vis && bw_click.sidebar_engine.borrow().is_none() {
                        let st = bw_click.state.borrow();
                        if st.cef_active {
                            let view = CefEngine::new(&internal_url("sidebar.html"), Arc::clone(&st.policy));
                            bw_click.sidebar_panel.append(&view.widget());
                            *bw_click.sidebar_engine.borrow_mut() = Some(Box::new(view));
                        } else if bw_click.sidebar_panel.first_child().is_none() {
                            bw_click.sidebar_panel.append(&Label::builder().label("边栏 · 收藏与标签组").build());
                        }
                    }
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
                HitKind::DragArea => {
                    if let Some(surface) = bw_click.window.surface() {
                        if let Ok(toplevel) = surface.downcast::<gdk::Toplevel>() {
                            if let Some(dev) = g.current_event().and_then(|e| e.device()) {
                                let ts = g.current_event_time();
                                toplevel.begin_move(&dev, btn as i32, x, y, ts);
                            }
                        }
                    }
                }
                HitKind::Overflow | HitKind::Reader | HitKind::None => {}
            }
        });
        self.toolbar.add_controller(click);

        let bw_act = Rc::clone(self);
        self.address_entry.connect_activate(move |e| {
            let text = e.text().to_string();
            bw_act.navigate_active(&text);
        });

        let focus_ctrl = gtk4::EventControllerFocus::new();
        let bw_blur = Rc::clone(self);
        focus_ctrl.connect_leave(move |_| {
            if !bw_blur.suggest_popover.is_visible() {
                bw_blur.state.borrow_mut().address_focused = false;
                bw_blur.address_entry.set_visible(false);
                bw_blur.toolbar.queue_draw();
            }
        });
        self.address_entry.add_controller(focus_ctrl);

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
            let sugs = bw_chg.state.borrow().stores.lock().unwrap().library.suggest(q.trim(), 8);
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
