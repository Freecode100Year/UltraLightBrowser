use crate::ffi::{cef_release, read_cef_str, sys, take_userfree_str, CefObject, CefStr};
use crate::scheme::make_static_resource_handler;
use crate::stealth_headers::{macos_edge_request_headers, map_cef_resource_type};
use gtk4::cairo::{Format, ImageSurface};
use gtk4::gdk;
use gtk4::prelude::*;
use gtk4::{DrawingArea, EventControllerKey, EventControllerMotion, EventControllerScroll, EventControllerScrollFlags, GestureClick, Widget};
use std::cell::RefCell;
use std::os::raw::c_int;
use std::ptr;
use std::rc::Rc;
use std::sync::{Arc, Mutex};
use ulb_core::adblock::{cosmetic_script, evaluate, extract_host, ResourceKind};
use ulb_core::internal_pages::{is_internal_url, VIDEO_DIAGNOSTICS_JS};
use ulb_core::user_agent::{build_mac_stealth_script, MAC_DEVICE_PROFILES};

/// Host-side shared policy configuration queried from network/load callbacks.
#[derive(Clone)]
pub struct BrowserPolicy {
    pub ua_profile: String,
    pub user_agent_string: String,
    pub mac_platform_version: String,
    pub mac_profile_idx: usize,
    pub early_scripts_js: Vec<String>,
    pub idle_scripts_js: Vec<String>,
    pub audio_dsp_js: String,
    pub adblock_enabled: bool,
    pub adblock_disabled_hosts: Vec<String>,
}

/// Asynchronous browser events forwarded to `ulb-linux`'s tab strip on the GTK loop.
#[derive(Debug, Clone)]
pub enum BrowserEvent {
    TitleChanged(String),
    UrlChanged(String),
    LoadingChanged {
        loading: bool,
        can_go_back: bool,
        can_go_forward: bool,
    },
    AudioChanged(bool),
    FindResult {
        count: i32,
        active_ordinal: i32,
    },
    OpenPopup(String),
    BlockedResource,
}

struct FrameBuffer {
    width: i32,
    height: i32,
    bgra: Vec<u8>,
    scale: f32,
    dirty: bool,
}

struct TabInner {
    drawing: DrawingArea,
    frame: Arc<Mutex<FrameBuffer>>,
    browser: Arc<Mutex<usize>>,
    events: Arc<Mutex<Vec<BrowserEvent>>>,
}

/// Wayland-native GTK4 offscreen CEF Chromium tab instance.
pub struct CefTabView {
    inner: Rc<RefCell<TabInner>>,
}

impl CefTabView {
    pub fn new(initial_url: &str, policy: Arc<Mutex<BrowserPolicy>>) -> Self {
        let frame = Arc::new(Mutex::new(FrameBuffer {
            width: 0,
            height: 0,
            bgra: Vec::new(),
            scale: 1.0,
            dirty: false,
        }));
        let browser = Arc::new(Mutex::new(0usize));
        let events = Arc::new(Mutex::new(Vec::new()));

        let drawing = DrawingArea::builder()
            .hexpand(true)
            .vexpand(true)
            .focusable(true)
            .can_focus(true)
            .build();

        let view_state = Arc::new(ClientState {
            frame: Arc::clone(&frame),
            browser: Arc::clone(&browser),
            policy: Arc::clone(&policy),
            events: Arc::clone(&events),
            logical_size: Mutex::new((1024, 720)),
            scale_factor: Mutex::new(1.0),
        });

        // Paint rendered Chromium framebuffer cleanly to GTK4 Cairo surface.
        let frame_draw = Arc::clone(&frame);
        let vs_resize = Arc::clone(&view_state);
        drawing.set_draw_func(move |area, cr, w, h| {
            let scale = area.scale_factor().max(1) as f32;
            let scale_changed = {
                let mut sc = vs_resize.scale_factor.lock().unwrap();
                if (*sc - scale).abs() > 0.01 {
                    *sc = scale;
                    true
                } else {
                    false
                }
            };
            let resized = {
                let mut sz = vs_resize.logical_size.lock().unwrap();
                if *sz != (w.max(1), h.max(1)) {
                    *sz = (w.max(1), h.max(1));
                    true
                } else {
                    false
                }
            };
            if resized || scale_changed {
                let ptr_raw = *vs_resize.browser.lock().unwrap();
                if ptr_raw != 0 {
                    unsafe {
                        let b = ptr_raw as *mut sys::cef_browser_t;
                        if let Some(get_host) = (*b).get_host {
                            let host = get_host(b);
                            if !host.is_null() {
                                if scale_changed {
                                    if let Some(notify) = (*host).notify_screen_info_changed {
                                        notify(host);
                                    }
                                }
                                if let Some(was_resized) = (*host).was_resized {
                                    was_resized(host);
                                }
                                cef_release(host);
                            }
                        }
                    }
                }
            }

            let fb = frame_draw.lock().unwrap();
            if fb.width <= 0 || fb.height <= 0 || fb.bgra.is_empty() {
                cr.set_source_rgb(0.09, 0.09, 0.11);
                let _ = cr.paint();
                return;
            }
            let fw = fb.width;
            let fh = fb.height;
            let s = if fb.scale > 0.0 { fb.scale } else { scale };
            let copy = fb.bgra.clone();
            drop(fb);

            if let Ok(surf) = ImageSurface::create_for_data(copy, Format::ARgb32, fw, fh, fw * 4) {
                let _ = cr.save();
                cr.scale(1.0 / f64::from(s), 1.0 / f64::from(s));
                let _ = cr.set_source_surface(&surf, 0.0, 0.0);
                let _ = cr.paint();
                let _ = cr.restore();
            }
        });

        let inner = Rc::new(RefCell::new(TabInner {
            drawing: drawing.clone(),
            frame,
            browser,
            events,
        }));

        Self::connect_input(&drawing, &view_state);

        // Spawn CEF browser host inside windowless OSR container
        let win_info = sys::cef_window_info_t {
            size: std::mem::size_of::<sys::cef_window_info_t>(),
            windowless_rendering_enabled: 1,
            runtime_style: sys::cef_runtime_style_t_CEF_RUNTIME_STYLE_ALLOY,
            ..Default::default()
        };
        let b_settings = sys::cef_browser_settings_t {
            size: std::mem::size_of::<sys::cef_browser_settings_t>(),
            windowless_frame_rate: 60,
            ..Default::default()
        };
        let client = create_client(view_state);
        let url_cs = CefStr::from_str(initial_url);
        unsafe {
            sys::cef_browser_host_create_browser(
                &win_info,
                client,
                url_cs.as_ptr(),
                &b_settings,
                ptr::null_mut(),
                ptr::null_mut(),
            );
        }

        Self { inner }
    }

    pub fn widget(&self) -> Widget {
        self.inner.borrow().drawing.clone().upcast()
    }

    pub fn poll_repaint_and_events(&self) -> Vec<BrowserEvent> {
        let inner = self.inner.borrow();
        let should_draw = {
            let mut fb = inner.frame.lock().unwrap();
            let d = fb.dirty && !fb.bgra.is_empty();
            fb.dirty = false;
            d
        };
        if should_draw {
            inner.drawing.queue_draw();
        }
        let mut evs = inner.events.lock().unwrap();
        std::mem::take(&mut *evs)
    }

    pub fn has_painted_frame(&self) -> bool {
        let inner = self.inner.borrow();
        let fb = inner.frame.lock().unwrap();
        fb.width > 0 && fb.height > 0 && !fb.bgra.is_empty()
    }

    fn with_browser<F: FnOnce(*mut sys::cef_browser_t, *mut sys::cef_browser_host_t)>(&self, f: F) {
        let raw = *self.inner.borrow().browser.lock().unwrap();
        if raw == 0 {
            return;
        }
        unsafe {
            let b = raw as *mut sys::cef_browser_t;
            let host = (*b)
                .get_host
                .map(|g| g(b))
                .unwrap_or(ptr::null_mut());
            f(b, host);
            if !host.is_null() {
                cef_release(host);
            }
        }
    }

    pub fn navigate(&self, url: &str) {
        self.with_browser(|b, _| unsafe {
            if let Some(get_main) = (*b).get_main_frame {
                let frame = get_main(b);
                if !frame.is_null() {
                    if let Some(load) = (*frame).load_url {
                        let cs = CefStr::from_str(url);
                        load(frame, cs.as_ptr());
                    }
                    cef_release(frame);
                }
            }
        });
    }

    pub fn go_back(&self) {
        self.with_browser(|b, _| unsafe {
            if let Some(f) = (*b).go_back { f(b); }
        });
    }

    pub fn go_forward(&self) {
        self.with_browser(|b, _| unsafe {
            if let Some(f) = (*b).go_forward { f(b); }
        });
    }

    pub fn reload(&self) {
        self.with_browser(|b, _| unsafe {
            if let Some(f) = (*b).reload { f(b); }
        });
    }

    pub fn stop(&self) {
        self.with_browser(|b, _| unsafe {
            if let Some(f) = (*b).stop_load { f(b); }
        });
    }

    pub fn set_zoom(&self, zoom: f64) {
        self.with_browser(|_, host| unsafe {
            if !host.is_null() {
                if let Some(f) = (*host).set_zoom_level {
                    // CEF zoom_level: 0.0 = 100%, log_{1.2}(factor)
                    let level = zoom.max(0.25).ln() / 1.2_f64.ln();
                    f(host, level);
                }
            }
        });
    }

    pub fn set_muted(&self, muted: bool) {
        self.with_browser(|_, host| unsafe {
            if !host.is_null() {
                if let Some(f) = (*host).set_audio_muted {
                    f(host, i32::from(muted));
                }
            }
        });
    }

    pub fn find(&self, query: &str, forward: bool) {
        self.with_browser(|_, host| unsafe {
            if !host.is_null() {
                if let Some(f) = (*host).find {
                    let cs = CefStr::from_str(query);
                    f(host, cs.as_ptr(), i32::from(forward), 0, 1);
                }
            }
        });
    }

    pub fn stop_find(&self) {
        self.with_browser(|_, host| unsafe {
            if !host.is_null() {
                if let Some(f) = (*host).stop_finding {
                    f(host, 1);
                }
            }
        });
    }

    pub fn execute_js(&self, js: &str) {
        self.with_browser(|b, _| unsafe {
            if let Some(get_main) = (*b).get_main_frame {
                let frame = get_main(b);
                if !frame.is_null() {
                    if let Some(exec) = (*frame).execute_java_script {
                        let code = CefStr::from_str(js);
                        let src = CefStr::from_str("");
                        exec(frame, code.as_ptr(), src.as_ptr(), 1);
                    }
                    cef_release(frame);
                }
            }
        });
    }

    fn connect_input(drawing: &DrawingArea, vs: &Arc<ClientState>) {
        let motion = EventControllerMotion::new();
        let vs_mv = Arc::clone(vs);
        motion.connect_motion(move |_, x, y| {
            let ptr_raw = *vs_mv.browser.lock().unwrap();
            if ptr_raw == 0 {
                return;
            }
            unsafe {
                let b = ptr_raw as *mut sys::cef_browser_t;
                if let Some(get_host) = (*b).get_host {
                    let host = get_host(b);
                    if !host.is_null() {
                        if let Some(mv) = (*host).send_mouse_move_event {
                            let ev = sys::cef_mouse_event_t {
                                x: x as c_int,
                                y: y as c_int,
                                modifiers: 0,
                            };
                            mv(host, &ev, 0);
                        }
                        cef_release(host);
                    }
                }
            }
        });
        drawing.add_controller(motion);

        let click = GestureClick::builder().button(0).build();
        let vs_down = Arc::clone(vs);
        let area_focus = drawing.clone();
        click.connect_pressed(move |g, n, x, y| {
            area_focus.grab_focus();
            send_click(&vs_down, g.current_button(), n, x, y, false);
        });
        let vs_up = Arc::clone(vs);
        click.connect_released(move |g, n, x, y| {
            send_click(&vs_up, g.current_button(), n, x, y, true);
        });
        drawing.add_controller(click);

        let scroll = EventControllerScroll::new(EventControllerScrollFlags::BOTH_AXES);
        let vs_sc = Arc::clone(vs);
        scroll.connect_scroll(move |_, dx, dy| {
            let ptr_raw = *vs_sc.browser.lock().unwrap();
            if ptr_raw == 0 {
                return gtk4::glib::Propagation::Proceed;
            }
            unsafe {
                let b = ptr_raw as *mut sys::cef_browser_t;
                if let Some(get_host) = (*b).get_host {
                    let host = get_host(b);
                    if !host.is_null() {
                        if let Some(wheel) = (*host).send_mouse_wheel_event {
                            let ev = sys::cef_mouse_event_t { x: 0, y: 0, modifiers: 0 };
                            wheel(host, &ev, (-dx * 40.0) as c_int, (-dy * 40.0) as c_int);
                        }
                        cef_release(host);
                    }
                }
            }
            gtk4::glib::Propagation::Stop
        });
        drawing.add_controller(scroll);

        let key = EventControllerKey::new();
        let vs_kd = Arc::clone(vs);
        key.connect_key_pressed(move |_, kv, code, mods| {
            send_key(&vs_kd, kv, code, mods, true);
            gtk4::glib::Propagation::Stop
        });
        let vs_ku = Arc::clone(vs);
        key.connect_key_released(move |_, kv, code, mods| {
            send_key(&vs_ku, kv, code, mods, false);
        });
        drawing.add_controller(key);
    }
}

impl Drop for CefTabView {
    fn drop(&mut self) {
        self.with_browser(|_, host| unsafe {
            if !host.is_null() {
                if let Some(close) = (*host).close_browser {
                    close(host, 1);
                }
            }
        });
    }
}

fn send_click(vs: &ClientState, button: u32, count: i32, x: f64, y: f64, up: bool) {
    let ptr_raw = *vs.browser.lock().unwrap();
    if ptr_raw == 0 {
        return;
    }
    let mbt = match button {
        2 => sys::cef_mouse_button_type_t_MBT_MIDDLE,
        3 => sys::cef_mouse_button_type_t_MBT_RIGHT,
        _ => sys::cef_mouse_button_type_t_MBT_LEFT,
    };
    unsafe {
        let b = ptr_raw as *mut sys::cef_browser_t;
        if let Some(get_host) = (*b).get_host {
            let host = get_host(b);
            if !host.is_null() {
                if let Some(click) = (*host).send_mouse_click_event {
                    let ev = sys::cef_mouse_event_t {
                        x: x as c_int,
                        y: y as c_int,
                        modifiers: 0,
                    };
                    click(host, &ev, mbt, i32::from(up), count.max(1));
                }
                cef_release(host);
            }
        }
    }
}

fn map_gdk_vk(kv: gdk::Key) -> c_int {
    if let Some(ch) = kv.to_unicode() {
        if ch.is_ascii_alphanumeric() {
            return ch.to_ascii_uppercase() as c_int;
        }
    }
    match kv {
        gdk::Key::BackSpace => 0x08,
        gdk::Key::Tab => 0x09,
        gdk::Key::Return | gdk::Key::KP_Enter => 0x0D,
        gdk::Key::Escape => 0x1B,
        gdk::Key::space => 0x20,
        gdk::Key::Page_Up => 0x21,
        gdk::Key::Page_Down => 0x22,
        gdk::Key::End => 0x23,
        gdk::Key::Home => 0x24,
        gdk::Key::Left => 0x25,
        gdk::Key::Up => 0x26,
        gdk::Key::Right => 0x27,
        gdk::Key::Down => 0x28,
        gdk::Key::Delete => 0x2E,
        _ => 0,
    }
}

fn cef_modifiers(mods: gdk::ModifierType) -> u32 {
    let mut f = 0u32;
    if mods.contains(gdk::ModifierType::SHIFT_MASK) {
        f |= sys::cef_event_flags_t_EVENTFLAG_SHIFT_DOWN;
    }
    if mods.contains(gdk::ModifierType::CONTROL_MASK) {
        f |= sys::cef_event_flags_t_EVENTFLAG_CONTROL_DOWN;
    }
    if mods.contains(gdk::ModifierType::ALT_MASK) {
        f |= sys::cef_event_flags_t_EVENTFLAG_ALT_DOWN;
    }
    f
}

fn send_key(vs: &ClientState, kv: gdk::Key, hw_code: u32, mods: gdk::ModifierType, pressed: bool) {
    let ptr_raw = *vs.browser.lock().unwrap();
    if ptr_raw == 0 {
        return;
    }
    let vk = map_gdk_vk(kv);
    let cef_mods = cef_modifiers(mods);
    unsafe {
        let b = ptr_raw as *mut sys::cef_browser_t;
        if let Some(get_host) = (*b).get_host {
            let host = get_host(b);
            if !host.is_null() {
                if let Some(send) = (*host).send_key_event {
                    let mut ev = sys::cef_key_event_t {
                        size: std::mem::size_of::<sys::cef_key_event_t>(),
                        type_: if pressed {
                            sys::cef_key_event_type_t_KEYEVENT_RAWKEYDOWN
                        } else {
                            sys::cef_key_event_type_t_KEYEVENT_KEYUP
                        },
                        modifiers: cef_mods,
                        windows_key_code: vk,
                        native_key_code: hw_code as c_int,
                        is_system_key: 0,
                        character: 0,
                        unmodified_character: 0,
                        focus_on_editable_field: 1,
                    };
                    send(host, &ev);
                    if pressed && !mods.contains(gdk::ModifierType::CONTROL_MASK) {
                        if let Some(ch) = kv.to_unicode() {
                            if !ch.is_control() || ch == '\r' || ch == '\n' {
                                ev.type_ = sys::cef_key_event_type_t_KEYEVENT_CHAR;
                                ev.character = ch as u16;
                                ev.unmodified_character = ch as u16;
                                send(host, &ev);
                            }
                        }
                    }
                }
                cef_release(host);
            }
        }
    }
}

struct ClientState {
    frame: Arc<Mutex<FrameBuffer>>,
    browser: Arc<Mutex<usize>>,
    policy: Arc<Mutex<BrowserPolicy>>,
    events: Arc<Mutex<Vec<BrowserEvent>>>,
    logical_size: Mutex<(i32, i32)>,
    scale_factor: Mutex<f32>,
}

fn create_client(state: Arc<ClientState>) -> *mut sys::cef_client_t {
    CefObject::new(
        |c: &mut sys::cef_client_t| {
            c.get_render_handler = Some(client_get_render);
            c.get_life_span_handler = Some(client_get_lifespan);
            c.get_display_handler = Some(client_get_display);
            c.get_load_handler = Some(client_get_load);
            c.get_request_handler = Some(client_get_request);
            c.get_audio_handler = Some(client_get_audio);
            c.get_find_handler = Some(client_get_find);
        },
        state,
    )
}

unsafe extern "C" fn client_get_render(self_: *mut sys::cef_client_t) -> *mut sys::cef_render_handler_t {
    let st: &Arc<ClientState> = CefObject::state_from(self_);
    let st = Arc::clone(st);
    CefObject::new(
        |h: &mut sys::cef_render_handler_t| {
            h.get_view_rect = Some(render_get_view_rect);
            h.get_screen_info = Some(render_get_screen_info);
            h.on_paint = Some(render_on_paint);
        },
        st,
    )
}

unsafe extern "C" fn client_get_lifespan(self_: *mut sys::cef_client_t) -> *mut sys::cef_life_span_handler_t {
    let st: &Arc<ClientState> = CefObject::state_from(self_);
    let st = Arc::clone(st);
    CefObject::new(
        |h: &mut sys::cef_life_span_handler_t| {
            h.on_after_created = Some(life_on_after_created);
            h.on_before_popup = Some(life_on_before_popup);
            h.on_before_close = Some(life_on_before_close);
        },
        st,
    )
}

unsafe extern "C" fn client_get_display(self_: *mut sys::cef_client_t) -> *mut sys::cef_display_handler_t {
    let st: &Arc<ClientState> = CefObject::state_from(self_);
    let st = Arc::clone(st);
    CefObject::new(
        |h: &mut sys::cef_display_handler_t| {
            h.on_address_change = Some(disp_on_address);
            h.on_title_change = Some(disp_on_title);
        },
        st,
    )
}

unsafe extern "C" fn client_get_load(self_: *mut sys::cef_client_t) -> *mut sys::cef_load_handler_t {
    let st: &Arc<ClientState> = CefObject::state_from(self_);
    let st = Arc::clone(st);
    CefObject::new(
        |h: &mut sys::cef_load_handler_t| {
            h.on_loading_state_change = Some(load_state_change);
            h.on_load_start = Some(load_start);
            h.on_load_end = Some(load_end);
        },
        st,
    )
}

unsafe extern "C" fn client_get_request(self_: *mut sys::cef_client_t) -> *mut sys::cef_request_handler_t {
    let st: &Arc<ClientState> = CefObject::state_from(self_);
    let st = Arc::clone(st);
    CefObject::new(
        |h: &mut sys::cef_request_handler_t| {
            h.get_resource_request_handler = Some(req_get_resource_handler);
        },
        st,
    )
}

unsafe extern "C" fn client_get_audio(self_: *mut sys::cef_client_t) -> *mut sys::cef_audio_handler_t {
    let st: &Arc<ClientState> = CefObject::state_from(self_);
    let st = Arc::clone(st);
    CefObject::new(
        |h: &mut sys::cef_audio_handler_t| {
            h.on_audio_stream_started = Some(audio_started);
            h.on_audio_stream_stopped = Some(audio_stopped);
        },
        st,
    )
}

unsafe extern "C" fn client_get_find(self_: *mut sys::cef_client_t) -> *mut sys::cef_find_handler_t {
    let st: &Arc<ClientState> = CefObject::state_from(self_);
    let st = Arc::clone(st);
    CefObject::new(
        |h: &mut sys::cef_find_handler_t| {
            h.on_find_result = Some(find_result);
        },
        st,
    )
}

unsafe extern "C" fn render_get_view_rect(
    self_: *mut sys::cef_render_handler_t,
    _browser: *mut sys::cef_browser_t,
    rect: *mut sys::cef_rect_t,
) {
    let st: &Arc<ClientState> = CefObject::state_from(self_);
    let (w, h) = *st.logical_size.lock().unwrap();
    if !rect.is_null() {
        (*rect) = sys::cef_rect_t { x: 0, y: 0, width: w.max(1), height: h.max(1) };
    }
}

unsafe extern "C" fn render_get_screen_info(
    self_: *mut sys::cef_render_handler_t,
    _browser: *mut sys::cef_browser_t,
    info: *mut sys::cef_screen_info_t,
) -> c_int {
    let st: &Arc<ClientState> = CefObject::state_from(self_);
    let (w, h) = *st.logical_size.lock().unwrap();
    let scale = (*st.scale_factor.lock().unwrap()).max(1.0);
    if !info.is_null() {
        (*info).size = std::mem::size_of::<sys::cef_screen_info_t>();
        (*info).device_scale_factor = scale;
        (*info).depth = 24;
        (*info).depth_per_component = 8;
        (*info).rect = sys::cef_rect_t { x: 0, y: 0, width: w.max(1), height: h.max(1) };
        (*info).available_rect = (*info).rect;
    }
    1
}

unsafe extern "C" fn render_on_paint(
    self_: *mut sys::cef_render_handler_t,
    _browser: *mut sys::cef_browser_t,
    type_: sys::cef_paint_element_type_t,
    _dirty_count: usize,
    _dirty: *const sys::cef_rect_t,
    buffer: *const std::ffi::c_void,
    width: c_int,
    height: c_int,
) {
    if type_ != sys::cef_paint_element_type_t_PET_VIEW || buffer.is_null() || width <= 0 || height <= 0 {
        return;
    }
    let st: &Arc<ClientState> = CefObject::state_from(self_);
    let scale = (*st.scale_factor.lock().unwrap()).max(1.0);
    let byte_len = (width as usize) * (height as usize) * 4;
    let slice = std::slice::from_raw_parts(buffer.cast::<u8>(), byte_len);
    let mut fb = st.frame.lock().unwrap();
    fb.width = width;
    fb.height = height;
    fb.scale = scale;
    fb.dirty = true;
    fb.bgra.clear();
    fb.bgra.extend_from_slice(slice);
}

unsafe extern "C" fn life_on_after_created(
    self_: *mut sys::cef_life_span_handler_t,
    browser: *mut sys::cef_browser_t,
) {
    let st: &Arc<ClientState> = CefObject::state_from(self_);
    crate::ffi::cef_add_ref(browser);
    *st.browser.lock().unwrap() = browser as usize;
}

unsafe extern "C" fn life_on_before_popup(
    self_: *mut sys::cef_life_span_handler_t,
    _browser: *mut sys::cef_browser_t,
    _frame: *mut sys::cef_frame_t,
    _popup_id: c_int,
    target_url: *const sys::cef_string_t,
    _target_frame_name: *const sys::cef_string_t,
    _target_disposition: sys::cef_window_open_disposition_t,
    _user_gesture: c_int,
    _popup_features: *const sys::cef_popup_features_t,
    _window_info: *mut sys::cef_window_info_t,
    _client: *mut *mut sys::cef_client_t,
    _settings: *mut sys::cef_browser_settings_t,
    _extra_info: *mut *mut sys::cef_dictionary_value_t,
    _no_javascript_access: *mut c_int,
) -> c_int {
    let url = read_cef_str(target_url);
    if !url.is_empty() {
        let st: &Arc<ClientState> = CefObject::state_from(self_);
        st.events.lock().unwrap().push(BrowserEvent::OpenPopup(url));
    }
    1
}

unsafe extern "C" fn life_on_before_close(
    self_: *mut sys::cef_life_span_handler_t,
    _browser: *mut sys::cef_browser_t,
) {
    let st: &Arc<ClientState> = CefObject::state_from(self_);
    let raw = std::mem::take(&mut *st.browser.lock().unwrap());
    if raw != 0 {
        cef_release(raw as *mut sys::cef_browser_t);
    }
}

unsafe extern "C" fn disp_on_address(
    self_: *mut sys::cef_display_handler_t,
    _browser: *mut sys::cef_browser_t,
    frame: *mut sys::cef_frame_t,
    url: *const sys::cef_string_t,
) {
    if !frame.is_null() && (*frame).is_main.map(|f| f(frame)).unwrap_or(0) == 0 {
        return;
    }
    let u = read_cef_str(url);
    let st: &Arc<ClientState> = CefObject::state_from(self_);
    st.events.lock().unwrap().push(BrowserEvent::UrlChanged(u));
}

unsafe extern "C" fn disp_on_title(
    self_: *mut sys::cef_display_handler_t,
    _browser: *mut sys::cef_browser_t,
    title: *const sys::cef_string_t,
) {
    let t = read_cef_str(title);
    let st: &Arc<ClientState> = CefObject::state_from(self_);
    st.events.lock().unwrap().push(BrowserEvent::TitleChanged(t));
}

unsafe extern "C" fn load_state_change(
    self_: *mut sys::cef_load_handler_t,
    _browser: *mut sys::cef_browser_t,
    is_loading: c_int,
    can_go_back: c_int,
    can_go_forward: c_int,
) {
    let st: &Arc<ClientState> = CefObject::state_from(self_);
    st.events
        .lock()
        .unwrap()
        .push(BrowserEvent::LoadingChanged {
            loading: is_loading != 0,
            can_go_back: can_go_back != 0,
            can_go_forward: can_go_forward != 0,
        });
}

unsafe extern "C" fn load_start(
    self_: *mut sys::cef_load_handler_t,
    _browser: *mut sys::cef_browser_t,
    frame: *mut sys::cef_frame_t,
    _transition_type: sys::cef_transition_type_t,
) {
    if frame.is_null() {
        return;
    }
    let url = (*frame).get_url.map(|f| take_userfree_str(f(frame))).unwrap_or_default();
    let is_selftest = url.contains("selftest.html");
    if (is_internal_url(&url) && !is_selftest) || url.starts_with("https://f8964.internal") {
        return;
    }
    let st: &Arc<ClientState> = CefObject::state_from(self_);
    let (stealth, cosmetic, early_scripts) = {
        let p = st.policy.lock().unwrap();
        let stealth = if p.ua_profile == "macos-edge" {
            let prof = &MAC_DEVICE_PROFILES[p.mac_profile_idx % MAC_DEVICE_PROFILES.len()];
            Some(build_mac_stealth_script(prof, Some(&p.user_agent_string)))
        } else {
            None
        };
        let cosmetic = if p.adblock_enabled && !is_selftest {
            Some(cosmetic_script(true, &p.adblock_disabled_hosts))
        } else {
            None
        };
        (
            stealth,
            cosmetic,
            if is_selftest {
                Vec::new()
            } else {
                p.early_scripts_js.clone()
            },
        )
    };
    if let Some(exec) = (*frame).execute_java_script {
        let src = CefStr::from_str("");
        if let Some(js) = stealth {
            let cs = CefStr::from_str(&js);
            exec(frame, cs.as_ptr(), src.as_ptr(), 1);
        }
        if let Some(cjs) = cosmetic {
            let cs = CefStr::from_str(&cjs);
            exec(frame, cs.as_ptr(), src.as_ptr(), 1);
        }
        for js in early_scripts {
            let cs = CefStr::from_str(&js);
            exec(frame, cs.as_ptr(), src.as_ptr(), 1);
        }
    }
}

unsafe extern "C" fn load_end(
    self_: *mut sys::cef_load_handler_t,
    _browser: *mut sys::cef_browser_t,
    frame: *mut sys::cef_frame_t,
    _http_status: c_int,
) {
    if frame.is_null() {
        return;
    }
    let url = (*frame).get_url.map(|f| take_userfree_str(f(frame))).unwrap_or_default();
    if is_internal_url(&url) || url.starts_with("https://f8964.internal") {
        return;
    }
    let st: &Arc<ClientState> = CefObject::state_from(self_);
    let (audio_js, idle_scripts) = {
        let p = st.policy.lock().unwrap();
        (p.audio_dsp_js.clone(), p.idle_scripts_js.clone())
    };
    if let Some(exec) = (*frame).execute_java_script {
        let src = CefStr::from_str("");
        if !audio_js.is_empty() {
            let cs = CefStr::from_str(&audio_js);
            exec(frame, cs.as_ptr(), src.as_ptr(), 1);
        }
        let diag = CefStr::from_str(VIDEO_DIAGNOSTICS_JS);
        exec(frame, diag.as_ptr(), src.as_ptr(), 1);
        for js in idle_scripts {
            let cs = CefStr::from_str(&js);
            exec(frame, cs.as_ptr(), src.as_ptr(), 1);
        }
    }
}

unsafe extern "C" fn req_get_resource_handler(
    self_: *mut sys::cef_request_handler_t,
    _browser: *mut sys::cef_browser_t,
    _frame: *mut sys::cef_frame_t,
    _request: *mut sys::cef_request_t,
    _is_navigation: c_int,
    _is_download: c_int,
    _request_initiator: *const sys::cef_string_t,
    _disable_default_handling: *mut c_int,
) -> *mut sys::cef_resource_request_handler_t {
    let st: &Arc<ClientState> = CefObject::state_from(self_);
    let st = Arc::clone(st);
    CefObject::new(
        |h: &mut sys::cef_resource_request_handler_t| {
            h.on_before_resource_load = Some(res_req_before_load);
            h.get_resource_handler = Some(res_req_get_handler);
        },
        st,
    )
}

unsafe extern "C" fn res_req_before_load(
    self_: *mut sys::cef_resource_request_handler_t,
    _browser: *mut sys::cef_browser_t,
    frame: *mut sys::cef_frame_t,
    request: *mut sys::cef_request_t,
    _callback: *mut sys::cef_callback_t,
) -> sys::cef_return_value_t {
    if request.is_null() {
        return sys::cef_return_value_t_RV_CONTINUE;
    }
    let url = (*request).get_url.map(|f| take_userfree_str(f(request))).unwrap_or_default();
    if is_internal_url(&url) || url.starts_with("https://f8964.internal") {
        return sys::cef_return_value_t_RV_CONTINUE;
    }

    let st: &Arc<ClientState> = CefObject::state_from(self_);
    let (profile, ver, adblock_on, disabled_hosts) = {
        let p = st.policy.lock().unwrap();
        (
            p.ua_profile.clone(),
            p.mac_platform_version.clone(),
            p.adblock_enabled,
            p.adblock_disabled_hosts.clone(),
        )
    };

    // 1. Adblock evaluation
    if adblock_on {
        let page_url = if !frame.is_null() {
            (*frame).get_url.map(|f| take_userfree_str(f(frame))).unwrap_or_default()
        } else {
            String::new()
        };
        let top_host = extract_host(&page_url);
        let site_disabled = !top_host.is_empty()
            && disabled_hosts
                .iter()
                .any(|h| h.eq_ignore_ascii_case(&top_host));
        if !site_disabled {
            let rt = (*request)
                .get_resource_type
                .map(|f| map_cef_resource_type(f(request)))
                .unwrap_or(ResourceKind::Other);
            let dec = evaluate(&url, &top_host, rt);
            if dec.block && dec.surrogate.is_none() {
                st.events.lock().unwrap().push(BrowserEvent::BlockedResource);
                return sys::cef_return_value_t_RV_CANCEL;
            }
        }
    }

    // 2. MacStealth HTTP User-Agent + Client Hints
    if profile == "macos-edge" {
        let ua_k = CefStr::from_str("User-Agent");
        let existing_ua = (*request)
            .get_header_by_name
            .map(|f| take_userfree_str(f(request, ua_k.as_ptr())))
            .unwrap_or_default();
        let base_ua = if existing_ua.is_empty() {
            "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/156.0.0.0 Safari/537.36".to_string()
        } else {
            existing_ua
        };
        if let Some(set_hdr) = (*request).set_header_by_name {
            for (k, v) in macos_edge_request_headers(&base_ua, &profile, &ver) {
                let ck = CefStr::from_str(&k);
                let cv = CefStr::from_str(&v);
                set_hdr(request, ck.as_ptr(), cv.as_ptr(), 1);
            }
        }
    }

    sys::cef_return_value_t_RV_CONTINUE
}

unsafe extern "C" fn res_req_get_handler(
    self_: *mut sys::cef_resource_request_handler_t,
    _browser: *mut sys::cef_browser_t,
    frame: *mut sys::cef_frame_t,
    request: *mut sys::cef_request_t,
) -> *mut sys::cef_resource_handler_t {
    if request.is_null() {
        return ptr::null_mut();
    }
    let st: &Arc<ClientState> = CefObject::state_from(self_);
    let (adblock_on, disabled_hosts) = {
        let p = st.policy.lock().unwrap();
        (p.adblock_enabled, p.adblock_disabled_hosts.clone())
    };
    if !adblock_on {
        return ptr::null_mut();
    }
    let url = (*request).get_url.map(|f| take_userfree_str(f(request))).unwrap_or_default();
    let page_url = if !frame.is_null() {
        (*frame).get_url.map(|f| take_userfree_str(f(frame))).unwrap_or_default()
    } else {
        String::new()
    };
    let top_host = extract_host(&page_url);
    if !top_host.is_empty()
        && disabled_hosts
            .iter()
            .any(|h| h.eq_ignore_ascii_case(&top_host))
    {
        return ptr::null_mut();
    }
    let rt = (*request)
        .get_resource_type
        .map(|f| map_cef_resource_type(f(request)))
        .unwrap_or(ResourceKind::Other);
    let dec = evaluate(&url, &top_host, rt);
    if let Some(js_body) = dec.surrogate {
        return make_static_resource_handler(200, "application/javascript", js_body.as_bytes().to_vec());
    }
    ptr::null_mut()
}

unsafe extern "C" fn audio_started(
    self_: *mut sys::cef_audio_handler_t,
    _browser: *mut sys::cef_browser_t,
    _params: *const sys::cef_audio_parameters_t,
    _channels: c_int,
) {
    let st: &Arc<ClientState> = CefObject::state_from(self_);
    st.events.lock().unwrap().push(BrowserEvent::AudioChanged(true));
}

unsafe extern "C" fn audio_stopped(
    self_: *mut sys::cef_audio_handler_t,
    _browser: *mut sys::cef_browser_t,
) {
    let st: &Arc<ClientState> = CefObject::state_from(self_);
    st.events.lock().unwrap().push(BrowserEvent::AudioChanged(false));
}

unsafe extern "C" fn find_result(
    self_: *mut sys::cef_find_handler_t,
    _browser: *mut sys::cef_browser_t,
    _identifier: c_int,
    count: c_int,
    _selection_rect: *const sys::cef_rect_t,
    active_match_ordinal: c_int,
    _final_update: c_int,
) {
    let st: &Arc<ClientState> = CefObject::state_from(self_);
    st.events.lock().unwrap().push(BrowserEvent::FindResult {
        count,
        active_ordinal: active_match_ordinal,
    });
}
