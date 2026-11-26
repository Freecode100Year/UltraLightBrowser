use crate::ffi::{sys, CefObject, CefStr};
use crate::scheme::{create_internal_scheme_factory, InternalSchemeState, RpcCallback};
use crate::stealth_headers::unpacked_extension_id;
use std::ffi::CString;
use std::os::raw::{c_char, c_int};
use std::path::PathBuf;
use std::ptr;
use std::sync::{Arc, Mutex};

/// Startup configuration for initializing Chromium/CEF dynamically from Arch system `/usr/lib/cef`.
pub struct CefConfig {
    pub session_dir: PathBuf,
    pub memorial_dir: PathBuf,
    pub extension_dirs: Vec<PathBuf>,
    pub user_agent: String,
    pub socks_port: u16,
    pub fail_closed: bool,
    pub rpc: RpcCallback,
}

#[derive(Clone)]
struct AppState {
    user_agent: String,
    extension_dirs: Vec<PathBuf>,
    socks_port: u16,
    fail_closed: bool,
}

/// Registers the exact C API version with `libcef.so` (required at start of every process).
fn ensure_api_hash() -> Result<(), String> {
    let ver: i32 = env!("ULB_CEF_API_VERSION")
        .parse()
        .map_err(|_| "invalid CEF API version".to_string())?;
    let hash_ptr = unsafe { sys::cef_api_hash(ver, 0) };
    if hash_ptr.is_null() {
        return Err("系统 libcef.so 版本与绑定头不匹配".to_string());
    }
    Ok(())
}

/// Runs CEF sub-processes (`--type=renderer`, `--type=gpu-process`, `--type=zygote`, etc.)
/// before GTK initialization. Returns `Some(exit_code)` if this invocation is a child helper.
pub fn try_execute_subprocess() -> Option<i32> {
    let args: Vec<String> = std::env::args().collect();
    if !args.iter().any(|a| a.starts_with("--type=")) {
        return None;
    }
    if ensure_api_hash().is_err() {
        return Some(1);
    }
    let cstrings: Vec<CString> = args
        .iter()
        .filter_map(|a| CString::new(a.as_str()).ok())
        .collect();
    let mut ptrs: Vec<*mut c_char> = cstrings
        .iter()
        .map(|s| s.as_ptr() as *mut c_char)
        .collect();
    let main_args = sys::cef_main_args_t {
        argc: ptrs.len() as c_int,
        argv: ptrs.as_mut_ptr(),
    };
    let app = create_app(AppState {
        user_agent: String::new(),
        extension_dirs: Vec::new(),
        socks_port: 0,
        fail_closed: false,
    });
    let code = unsafe { sys::cef_execute_process(&main_args, app, ptr::null_mut()) };
    if code >= 0 {
        Some(code)
    } else {
        None
    }
}

fn create_app(state: AppState) -> *mut sys::cef_app_t {
    CefObject::new(
        |app: &mut sys::cef_app_t| {
            app.on_before_command_line_processing = Some(on_before_command_line);
        },
        state,
    )
}

unsafe extern "C" fn on_before_command_line(
    self_: *mut sys::cef_app_t,
    _process_type: *const sys::cef_string_t,
    command_line: *mut sys::cef_command_line_t,
) {
    if command_line.is_null() {
        return;
    }
    let state: &AppState = CefObject::state_from(self_);
    let append_switch = |name: &str| {
        if let Some(f) = (*command_line).append_switch {
            let cs = CefStr::from_str(name);
            f(command_line, cs.as_ptr());
        }
    };
    let append_kv = |name: &str, val: &str| {
        if let Some(f) = (*command_line).append_switch_with_value {
            let k = CefStr::from_str(name);
            let v = CefStr::from_str(val);
            f(command_line, k.as_ptr(), v.as_ptr());
        }
    };

    append_switch("no-sandbox");
    append_switch("enable-smooth-scrolling");
    append_kv("disable-blink-features", "AutomationControlled");
    append_kv("disable-features", "HardwareMediaKeyHandling");
    append_kv("autoplay-policy", "no-user-gesture-required");

    if !state.user_agent.is_empty() {
        append_kv("user-agent", &state.user_agent);
    }

    if state.socks_port > 0 {
        append_kv("proxy-server", &format!("socks5://127.0.0.1:{}", state.socks_port));
        append_kv("proxy-bypass-list", "localhost,127.0.0.1,*.internal");
    } else if state.fail_closed {
        // Discard port fails outbound TCP while keeping *.internal functional.
        append_kv("proxy-server", "socks5://127.0.0.1:9");
        append_kv("proxy-bypass-list", "localhost,127.0.0.1,*.internal");
    }

    if !state.extension_dirs.is_empty() {
        let joined = state
            .extension_dirs
            .iter()
            .map(|p| p.to_string_lossy().into_owned())
            .collect::<Vec<_>>()
            .join(",");
        append_kv("load-extension", &joined);
    }
}

/// Live CEF runtime guard. Calls `cef_shutdown()` on Drop.
pub struct CefRuntime {
    initialized: bool,
    pub loaded_extension_ids: Vec<(PathBuf, String)>,
}

impl CefRuntime {
    /// Initializes system CEF (`156.x`), registers `ulb.internal`/`f8964.internal`, and prepares extensions.
    pub fn init(cfg: CefConfig) -> Result<Self, String> {
        ensure_api_hash()?;

        let _ = std::fs::create_dir_all(&cfg.session_dir);

        let args: Vec<String> = std::env::args().collect();
        let cstrings: Vec<CString> = args
            .iter()
            .filter_map(|a| CString::new(a.as_str()).ok())
            .collect();
        let mut ptrs: Vec<*mut c_char> = cstrings
            .iter()
            .map(|s| s.as_ptr() as *mut c_char)
            .collect();
        let main_args = sys::cef_main_args_t {
            argc: ptrs.len() as c_int,
            argv: ptrs.as_mut_ptr(),
        };

        let mut settings = sys::cef_settings_t {
            size: std::mem::size_of::<sys::cef_settings_t>(),
            no_sandbox: 1,
            multi_threaded_message_loop: 0,
            external_message_pump: 0,
            windowless_rendering_enabled: 1,
            log_severity: sys::cef_log_severity_t_LOGSEVERITY_WARNING,
            ..Default::default()
        };

        let cef_lib_dir = option_env!("ULB_CEF_LIB_DIR").unwrap_or("/usr/lib/cef");
        let locales_dir = format!("{cef_lib_dir}/locales");
        let cache_dir = cfg.session_dir.to_string_lossy().into_owned();
        let log_file = cfg.session_dir.join("cef.log").to_string_lossy().into_owned();

        CefStr::write_into(cef_lib_dir, &mut settings.resources_dir_path);
        CefStr::write_into(&locales_dir, &mut settings.locales_dir_path);
        CefStr::write_into(&cache_dir, &mut settings.cache_path);
        CefStr::write_into(&cache_dir, &mut settings.root_cache_path);
        CefStr::write_into(&log_file, &mut settings.log_file);
        if !cfg.user_agent.is_empty() {
            CefStr::write_into(&cfg.user_agent, &mut settings.user_agent);
        }

        let app = create_app(AppState {
            user_agent: cfg.user_agent,
            extension_dirs: cfg.extension_dirs.clone(),
            socks_port: cfg.socks_port,
            fail_closed: cfg.fail_closed,
        });

        let ok = unsafe { sys::cef_initialize(&main_args, &settings, app, ptr::null_mut()) };
        if ok == 0 {
            return Err("cef_initialize 失败（请检查系统 /usr/lib/cef 与无头/显示环境）".to_string());
        }

        let scheme_state = InternalSchemeState {
            rpc: cfg.rpc,
            memorial_dir: cfg.memorial_dir,
        };
        let https = CefStr::from_str("https");
        let ulb_dom = CefStr::from_str("ulb.internal");
        let f_dom = CefStr::from_str("f8964.internal");

        unsafe {
            let f1 = create_internal_scheme_factory(scheme_state.clone());
            sys::cef_register_scheme_handler_factory(https.as_ptr(), ulb_dom.as_ptr(), f1);
            let f2 = create_internal_scheme_factory(scheme_state);
            sys::cef_register_scheme_handler_factory(https.as_ptr(), f_dom.as_ptr(), f2);
        }

        let loaded_extension_ids = cfg
            .extension_dirs
            .iter()
            .map(|p| (p.clone(), unpacked_extension_id(p)))
            .collect();

        Ok(Self {
            initialized: true,
            loaded_extension_ids,
        })
    }

    /// Pumps pending CEF event loop work; call periodically (~15ms) from GTK main loop.
    pub fn pump(&self) {
        if self.initialized {
            unsafe {
                sys::cef_do_message_loop_work();
            }
        }
    }
}

impl Drop for CefRuntime {
    fn drop(&mut self) {
        if self.initialized {
            self.initialized = false;
            unsafe {
                sys::cef_shutdown();
            }
        }
    }
}

pub fn shared_rpc_stub() -> RpcCallback {
    Arc::new(Mutex::new(Box::new(|_| r#"{"error":"uninitialized"}"#.to_string())))
}
