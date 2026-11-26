use std::fs;
use std::io::{BufRead, BufReader, Write};
use std::os::unix::fs::PermissionsExt;
use std::os::unix::net::{UnixListener, UnixStream};
use std::path::{Path, PathBuf};
use std::thread;

/// Manages the ephemeral `tmpfs` session directory (`$XDG_RUNTIME_DIR/ulb-session-<pid>`).
pub struct SessionDir {
    runtime_root: PathBuf,
    path: PathBuf,
}

impl SessionDir {
    /// Creates `$XDG_RUNTIME_DIR/ulb-session-<pid>` (`0700`) and sweeps dead-PID leftovers.
    pub fn create(runtime_root: PathBuf) -> std::io::Result<Self> {
        fs::create_dir_all(&runtime_root)?;
        sweep_stale_sessions(&runtime_root, |pid| Path::new(&format!("/proc/{pid}")).exists());
        let path = runtime_root.join(format!("ulb-session-{}", std::process::id()));
        let _ = fs::remove_dir_all(&path);
        fs::create_dir_all(&path)?;
        let _ = fs::set_permissions(&path, fs::Permissions::from_mode(0o700));
        Ok(Self { runtime_root, path })
    }

    pub fn runtime_root(&self) -> &Path {
        &self.runtime_root
    }

    pub fn path(&self) -> &Path {
        &self.path
    }
}

impl Drop for SessionDir {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.path);
    }
}

/// Resolves `$XDG_RUNTIME_DIR` (falling back to a private temp subdir when unset).
pub fn resolve_runtime_dir() -> PathBuf {
    if let Some(dir) = std::env::var_os("XDG_RUNTIME_DIR") {
        if !dir.is_empty() {
            return PathBuf::from(dir);
        }
    }
    std::env::temp_dir().join("ulb-runtime")
}

/// Resolves `$XDG_CONFIG_HOME/ultralight` (falling back to `$HOME/.config/ultralight`).
pub fn resolve_config_dir() -> PathBuf {
    if let Some(dir) = std::env::var_os("XDG_CONFIG_HOME") {
        if !dir.is_empty() {
            return PathBuf::from(dir).join("ultralight");
        }
    }
    if let Some(home) = std::env::var_os("HOME") {
        return PathBuf::from(home).join(".config").join("ultralight");
    }
    std::env::temp_dir().join("ultralight-config")
}

/// Resolves `$XDG_DATA_HOME/ultralight` (falling back to `$HOME/.local/share/ultralight`).
pub fn resolve_data_dir() -> PathBuf {
    if let Some(dir) = std::env::var_os("XDG_DATA_HOME") {
        if !dir.is_empty() {
            return PathBuf::from(dir).join("ultralight");
        }
    }
    if let Some(home) = std::env::var_os("HOME") {
        return PathBuf::from(home)
            .join(".local")
            .join("share")
            .join("ultralight");
    }
    std::env::temp_dir().join("ultralight-data")
}

/// Removes `ulb-session-<pid>` folders belonging to dead PIDs.
pub fn sweep_stale_sessions<F>(runtime_root: &Path, is_alive: F)
where
    F: Fn(u32) -> bool,
{
    let Ok(rd) = fs::read_dir(runtime_root) else {
        return;
    };
    for entry in rd.flatten() {
        let Some(name) = entry.file_name().to_str().map(|s| s.to_string()) else {
            continue;
        };
        let Some(pid_str) = name.strip_prefix("ulb-session-") else {
            continue;
        };
        let Ok(pid) = pid_str.parse::<u32>() else {
            continue;
        };
        if pid != std::process::id() && !is_alive(pid) {
            let _ = fs::remove_dir_all(entry.path());
        }
    }
}

pub fn socket_path(runtime_root: &Path) -> PathBuf {
    runtime_root.join("ulb.sock")
}

/// Attempts to hand off URLs (or a blank raise signal) to an already running instance.
pub fn try_send_open(runtime_root: &Path, urls: &[String]) -> bool {
    let sock = socket_path(runtime_root);
    let Ok(mut stream) = UnixStream::connect(&sock) else {
        return false;
    };
    if urls.is_empty() {
        let _ = writeln!(stream, "RAISE");
    } else {
        for u in urls {
            let _ = writeln!(stream, "OPEN {u}");
        }
    }
    let _ = stream.flush();
    true
}

/// Commands received over the single-instance Unix domain socket.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum IpcCommand {
    Raise,
    Open(String),
}

/// Binds the single-instance Unix socket and dispatches commands in a background thread.
pub fn start_ipc_server<F>(runtime_root: &Path, on_cmd: F) -> std::io::Result<IpcGuard>
where
    F: Fn(IpcCommand) + Send + Sync + 'static,
{
    fs::create_dir_all(runtime_root)?;
    let sock = socket_path(runtime_root);
    if sock.exists() {
        let _ = fs::remove_file(&sock);
    }
    let listener = UnixListener::bind(&sock)?;
    let _ = fs::set_permissions(&sock, fs::Permissions::from_mode(0o600));
    let sock_cleanup = sock.clone();

    thread::spawn(move || {
        for conn in listener.incoming() {
            let Ok(stream) = conn else { break };
            let reader = BufReader::new(stream);
            for line in reader.lines().map_while(Result::ok) {
                if line == "RAISE" {
                    on_cmd(IpcCommand::Raise);
                } else if let Some(url) = line.strip_prefix("OPEN ") {
                    on_cmd(IpcCommand::Open(url.trim().to_string()));
                }
            }
        }
    });

    Ok(IpcGuard { sock: sock_cleanup })
}

pub struct IpcGuard {
    sock: PathBuf,
}

impl Drop for IpcGuard {
    fn drop(&mut self) {
        let _ = fs::remove_file(&self.sock);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::{Arc, Mutex};
    use std::time::Duration;

    #[test]
    fn sweeps_only_dead_session_dirs() {
        let root = std::env::temp_dir().join(format!("ulb-sess-test-{}", std::process::id()));
        let _ = fs::remove_dir_all(&root);
        fs::create_dir_all(root.join("ulb-session-11111")).unwrap();
        fs::create_dir_all(root.join("ulb-session-22222")).unwrap();

        sweep_stale_sessions(&root, |pid| pid == 22222);

        assert!(!root.join("ulb-session-11111").exists());
        assert!(root.join("ulb-session-22222").exists());
        let _ = fs::remove_dir_all(&root);
    }

    #[test]
    fn ipc_socket_roundtrip() {
        let root = std::env::temp_dir().join(format!("ulb-ipc-test-{}", std::process::id()));
        let _ = fs::remove_dir_all(&root);
        let received = Arc::new(Mutex::new(Vec::new()));
        let rec_cb = Arc::clone(&received);

        let _guard = start_ipc_server(&root, move |cmd| {
            rec_cb.lock().unwrap().push(cmd);
        })
        .unwrap();

        assert!(try_send_open(&root, &["https://archlinux.org".to_string()]));
        thread::sleep(Duration::from_millis(60));
        let got = received.lock().unwrap().clone();
        assert_eq!(got, vec![IpcCommand::Open("https://archlinux.org".to_string())]);
        let _ = fs::remove_dir_all(&root);
    }
}
