mod audio_watcher;
mod engine;
mod icons;
mod menus;
mod paint_bar;
mod session;
mod window;

use gtk4::prelude::*;
use gtk4::Application;
use session::{
    resolve_config_dir, resolve_data_dir, resolve_runtime_dir, try_send_open, SessionDir,
};
use std::path::PathBuf;
use window::{run_browser, AppPaths};

fn find_warp_binary() -> Option<PathBuf> {
    if let Some(p) = std::env::var_os("ULB_WARP_BIN") {
        let pb = PathBuf::from(p);
        if pb.exists() {
            return Some(pb);
        }
    }
    if let Ok(exe) = std::env::current_exe() {
        if let Some(dir) = exe.parent() {
            let sibling = dir.join("ulb-warp");
            if sibling.exists() {
                return Some(sibling);
            }
        }
    }
    if let Some(path_env) = std::env::var_os("PATH") {
        for dir in std::env::split_paths(&path_env) {
            let candidate = dir.join("ulb-warp");
            if candidate.exists() {
                return Some(candidate);
            }
        }
    }
    None
}

fn main() {
    // CEF spawns helper processes (--type=renderer/gpu/zygote) from the same binary.
    if let Some(code) = ulb_cef::try_execute_subprocess() {
        std::process::exit(code);
    }

    let args: Vec<String> = std::env::args().skip(1).collect();
    let private_mode = args.iter().any(|a| a == "--private");
    let headless_check = args.iter().any(|a| a == "--check");
    let smoke_test = args.iter().any(|a| a == "--smoke-test");
    let urls: Vec<String> = args
        .into_iter()
        .filter(|a| !a.starts_with("--"))
        .collect();

    if headless_check {
        let ua = ulb_core::user_agent::build_macos_edge_user_agent(
            "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/156.0.0.0 Safari/537.36",
            "macos-edge",
        );
        println!("ulb-linux ok | ua={ua} | cef=linked");
        return;
    }

    let runtime_root = resolve_runtime_dir();
    if !private_mode && !smoke_test && try_send_open(&runtime_root, &urls) {
        return;
    }

    let app = Application::builder()
        .application_id(if smoke_test {
            "com.ultralight.browser.smoke"
        } else {
            "com.ultralight.browser"
        })
        .build();

    let paths = AppPaths {
        config_dir: resolve_config_dir(),
        data_dir: resolve_data_dir(),
        warp_bin: find_warp_binary(),
    };

    let urls_ref = std::cell::RefCell::new(Some(urls));
    app.connect_activate(move |app| {
        let Ok(session) = SessionDir::create(runtime_root.clone()) else {
            eprintln!("Failed to create runtime session directory");
            return;
        };
        let initial = urls_ref.borrow_mut().take().unwrap_or_default();
        run_browser(
            app,
            paths.clone(),
            session,
            initial,
            private_mode || smoke_test,
            smoke_test,
        );
    });

    app.run_with_args::<&str>(&[]);
}
