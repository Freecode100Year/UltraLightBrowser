use crate::line_store::{LineEntry, LineStore};
use crate::util::{now_epoch_ms, write_file_atomic};
use chacha20poly1305::aead::{Aead, KeyInit};
use chacha20poly1305::{ChaCha20Poly1305, Key, Nonce};
use std::fs;
use std::io::{BufRead, BufReader, Write};
use std::net::TcpListener;
use std::os::unix::fs::PermissionsExt;
use std::path::{Path, PathBuf};
use std::process::{Child, ChildStdin, Command, Stdio};
use std::sync::{Arc, Mutex};
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};

const LINES_FILE_MAGIC: &[u8] = b"ULBLN1";
const PUBLIC_LINE_ROTATE: &str = "2001:470:1f07:5f5::/64";

/// Reconstructs the built-in public fallback line at runtime (matching `WarpManager::PublicLine`).
pub fn public_line() -> String {
    let parts = [
        "ss://",
        "Y2hhY2hhMjAtaWV0Zi1wb2x5MTMwNTo1MzU0NzY3OA==",
        "@",
        "d39b83df2f179c97b04843d4c0576bf796",
        "140301815bb28e8825525bd5ff389515",
        ".cf68684214.top",
        ":19465",
        "#%E5%85%AC%E7%9B%8A%E7%BA%BF%E8%B7%AF",
    ];
    parts.concat()
}

/// Live status reported by the supervised `ulb-warp` helper.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct WarpStatus {
    pub running: bool,
    pub up: bool,
    pub port: u16,
    pub endpoint: String,
    pub line: String,
    pub reason: String,
    pub error: String,
    pub mode: String,
    pub probing: bool,
}

impl Default for WarpStatus {
    fn default() -> Self {
        Self {
            running: false,
            up: false,
            port: 0,
            endpoint: String::new(),
            line: String::new(),
            reason: String::new(),
            error: String::new(),
            mode: "WARP".to_string(),
            probing: false,
        }
    }
}

/// Applies one stdout status line from `ulb-warp` into `WarpStatus`.
pub fn parse_warp_line(raw: &str, status: &mut WarpStatus) {
    let line = raw.trim();
    if let Some(rest) = line.strip_prefix("PORT ") {
        if let Ok(port) = rest.trim().parse::<u16>() {
            status.port = port;
        }
    } else if let Some(rest) = line.strip_prefix("STATE ") {
        status.up = rest.trim() == "up";
        if status.up {
            status.error.clear();
        }
    } else if let Some(rest) = line.strip_prefix("ENDPOINT ") {
        status.endpoint = rest.trim().to_string();
    } else if line.starts_with("SCAN ") {
        status.probing = line == "SCAN start";
    } else if let Some(rest) = line.strip_prefix("LINE ") {
        status.line = rest.trim().to_string();
    } else if let Some(rest) = line.strip_prefix("REASON ") {
        status.reason = rest.trim().to_string();
    } else if let Some(rest) = line.strip_prefix("ERROR ") {
        status.error = rest.trim().to_string();
    }
}

/// Reserves an ephemeral loopback port from the OS.
pub fn reserve_loopback_port() -> std::io::Result<u16> {
    let listener = TcpListener::bind("127.0.0.1:0")?;
    Ok(listener.local_addr()?.port())
}

fn load_or_create_key(key_path: &Path) -> Result<[u8; 32], String> {
    if let Ok(bytes) = fs::read(key_path) {
        if bytes.len() == 32 {
            let mut key = [0u8; 32];
            key.copy_from_slice(&bytes);
            return Ok(key);
        }
    }
    let mut key = [0u8; 32];
    getrandom::getrandom(&mut key).map_err(|e| format!("无法生成密钥: {e}"))?;
    write_file_atomic(key_path, &key).map_err(|e| format!("无法保存密钥: {e}"))?;
    let _ = fs::set_permissions(key_path, fs::Permissions::from_mode(0o600));
    Ok(key)
}

/// Loads `lines.dat` (encrypted with `ChaCha20Poly1305` and per-profile key `lines.key`).
pub fn load_encrypted_lines(data_dir: &Path, store: &mut LineStore) -> bool {
    let dat_path = data_dir.join("lines.dat");
    let key_path = data_dir.join("lines.key");
    let Ok(raw) = fs::read(&dat_path) else {
        return false;
    };
    if !raw.starts_with(LINES_FILE_MAGIC) || raw.len() <= LINES_FILE_MAGIC.len() + 12 {
        return false;
    }
    let Ok(key_bytes) = load_or_create_key(&key_path) else {
        return false;
    };
    let cipher = ChaCha20Poly1305::new(Key::from_slice(&key_bytes));
    let nonce_start = LINES_FILE_MAGIC.len();
    let nonce = Nonce::from_slice(&raw[nonce_start..nonce_start + 12]);
    let Ok(plain) = cipher.decrypt(nonce, &raw[nonce_start + 12..]) else {
        return false;
    };
    let Ok(text) = std::str::from_utf8(&plain) else {
        return false;
    };
    store.load(text)
}

/// Saves `LineStore` encrypted to `lines.dat` (`0600`).
pub fn save_encrypted_lines(data_dir: &Path, store: &LineStore) -> Result<(), String> {
    let key_bytes = load_or_create_key(&data_dir.join("lines.key"))?;
    let cipher = ChaCha20Poly1305::new(Key::from_slice(&key_bytes));
    let mut nonce_bytes = [0u8; 12];
    getrandom::getrandom(&mut nonce_bytes).map_err(|e| format!("nonce error: {e}"))?;
    let nonce = Nonce::from_slice(&nonce_bytes);
    let plain = store.save();
    let cipher_bytes = cipher
        .encrypt(nonce, plain.as_bytes())
        .map_err(|_| "加密线路配置失败".to_string())?;
    let mut out = Vec::with_capacity(LINES_FILE_MAGIC.len() + 12 + cipher_bytes.len());
    out.extend_from_slice(LINES_FILE_MAGIC);
    out.extend_from_slice(&nonce_bytes);
    out.extend_from_slice(&cipher_bytes);
    let dat_path = data_dir.join("lines.dat");
    write_file_atomic(&dat_path, &out).map_err(|e| format!("写入 lines.dat 失败: {e}"))?;
    let _ = fs::set_permissions(&dat_path, fs::Permissions::from_mode(0o600));
    Ok(())
}

/// Options to start the supervised `ulb-warp` helper.
#[derive(Debug, Clone)]
pub struct WarpConfig {
    pub binary: PathBuf,
    pub state_dir: PathBuf,
    pub fail_closed: bool,
    pub use_line: bool,
    pub selected_id: String,
    pub selected_links: Vec<String>,
}

/// Supervises a running `ulb-warp` child process and tracks its live `WarpStatus`.
pub struct WarpSupervisor {
    child: Option<Child>,
    stdin: Option<ChildStdin>,
    reader_thread: Option<JoinHandle<()>>,
    status: Arc<Mutex<WarpStatus>>,
}

impl WarpSupervisor {
    pub fn start<F>(cfg: &WarpConfig, on_change: F) -> std::io::Result<Self>
    where
        F: Fn(&WarpStatus) + Send + Sync + 'static,
    {
        fs::create_dir_all(&cfg.state_dir)?;
        let mode = if !cfg.use_line {
            "WARP"
        } else if cfg.selected_id == LineStore::PUBLIC {
            "公益线路"
        } else if cfg.selected_id == LineStore::AUTO {
            "自选线路 · 自动"
        } else {
            "自选线路"
        };

        let status = Arc::new(Mutex::new(WarpStatus {
            running: true,
            mode: mode.to_string(),
            ..WarpStatus::default()
        }));

        let mut cmd = Command::new(&cfg.binary);
        cmd.arg("-state")
            .arg(&cfg.state_dir)
            .arg("-listen")
            .arg("127.0.0.1:0")
            .arg("-parent")
            .arg(std::process::id().to_string())
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::null());
        if cfg.fail_closed {
            cmd.arg("-fail-closed");
        }

        if cfg.use_line {
            if cfg.selected_id == LineStore::PUBLIC {
                cmd.env("ULB_LINE", public_line());
                cmd.env("ULB_LINE_ROTATE", PUBLIC_LINE_ROTATE);
            } else if cfg.selected_id == LineStore::AUTO {
                cmd.env("ULB_LINES", cfg.selected_links.join("\n"));
            } else if let Some(first) = cfg.selected_links.first() {
                cmd.env("ULB_LINE", first);
            }
        }

        let mut child = cmd.spawn()?;
        let stdin = child.stdin.take();
        let stdout = child.stdout.take();
        let status_bg = Arc::clone(&status);

        let reader_thread = stdout.map(|out| {
            thread::spawn(move || {
                let reader = BufReader::new(out);
                for line in reader.lines().map_while(Result::ok) {
                    let snapshot = {
                        let mut guard = status_bg.lock().unwrap();
                        parse_warp_line(&line, &mut guard);
                        guard.clone()
                    };
                    on_change(&snapshot);
                }
                let snapshot = {
                    let mut guard = status_bg.lock().unwrap();
                    guard.running = false;
                    guard.up = false;
                    guard.clone()
                };
                on_change(&snapshot);
            })
        });

        Ok(Self {
            child: Some(child),
            stdin,
            reader_thread,
            status,
        })
    }

    pub fn status(&self) -> WarpStatus {
        self.status.lock().unwrap().clone()
    }

    pub fn rescan(&mut self) -> bool {
        let Some(stdin) = self.stdin.as_mut() else {
            return false;
        };
        stdin.write_all(b"RESCAN\n").is_ok() && stdin.flush().is_ok()
    }

    pub fn stop(&mut self) {
        self.stdin.take();
        if let Some(mut child) = self.child.take() {
            let _ = child.kill();
            let _ = child.wait();
        }
        if let Some(handle) = self.reader_thread.take() {
            let _ = handle.join();
        }
    }
}

impl Drop for WarpSupervisor {
    fn drop(&mut self) {
        self.stop();
    }
}

/// Result for one line probed via `ulb-warp -probe`.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ProbeOutcome {
    pub id: String,
    pub latency_ms: i32,
}

fn run_oneshot_stdout(
    mut cmd: Command,
    env_pair: Option<(&str, String)>,
    timeout: Duration,
) -> Result<(i32, String), String> {
    if let Some((k, v)) = env_pair {
        cmd.env(k, v);
    }
    cmd.stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::null());
    let mut child = cmd.spawn().map_err(|e| format!("启动 ulb-warp 失败: {e}"))?;
    let start = Instant::now();
    let stdout = child.stdout.take();

    let reader = stdout.map(|out| {
        thread::spawn(move || {
            let mut buf = String::new();
            let _ = std::io::Read::read_to_string(&mut BufReader::new(out), &mut buf);
            buf
        })
    });

    let code = loop {
        match child.try_wait() {
            Ok(Some(status)) => break status.code().unwrap_or(1),
            Ok(None) => {
                if start.elapsed() >= timeout {
                    let _ = child.kill();
                    let _ = child.wait();
                    if let Some(r) = reader {
                        let _ = r.join();
                    }
                    return Err("操作超时".to_string());
                }
                thread::sleep(Duration::from_millis(25));
            }
            Err(e) => return Err(format!("等待 ulb-warp 失败: {e}")),
        }
    };

    let output = reader.and_then(|r| r.join().ok()).unwrap_or_default();
    Ok((code, output))
}

/// Runs `ulb-warp -probe` over a slice of lines and returns per-line RTT in ms (`-1` = unreachable).
pub fn run_probe(
    binary: &Path,
    lines: &[LineEntry],
    timeout: Duration,
) -> Result<Vec<ProbeOutcome>, String> {
    let mut cmd = Command::new(binary);
    cmd.arg("-probe");
    let payload: String = lines
        .iter()
        .map(|l| l.link.as_str())
        .collect::<Vec<_>>()
        .join("\n");
    let (_, stdout) = run_oneshot_stdout(cmd, Some(("ULB_PROBE", payload)), timeout)?;
    let mut results: Vec<ProbeOutcome> = lines
        .iter()
        .map(|l| ProbeOutcome {
            id: l.id.clone(),
            latency_ms: -1,
        })
        .collect();

    for line in stdout.lines() {
        let Some(rest) = line.strip_prefix("PROBE ") else {
            continue;
        };
        let mut parts = rest.splitn(2, ' ');
        let Some(idx_str) = parts.next() else { continue };
        let Some(ms_str) = parts.next() else { continue };
        let Ok(idx) = idx_str.parse::<usize>() else { continue };
        let Ok(ms) = ms_str.trim().parse::<i32>() else { continue };
        if let Some(item) = results.get_mut(idx) {
            item.latency_ms = ms;
        }
    }
    Ok(results)
}

/// Result of checking line syntax via `ulb-warp -probe -parse-only`.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ParseCheck {
    pub index: usize,
    pub ok: bool,
    pub error: String,
}

/// Validates line syntax via `ulb-warp -probe -parse-only`.
pub fn run_parse_only(binary: &Path, lines: &[&str], timeout: Duration) -> Result<Vec<ParseCheck>, String> {
    let mut cmd = Command::new(binary);
    cmd.arg("-probe").arg("-parse-only");
    let (_, stdout) = run_oneshot_stdout(cmd, Some(("ULB_PROBE", lines.join("\n"))), timeout)?;
    let mut checks = Vec::new();
    for line in stdout.lines() {
        let Some(rest) = line.strip_prefix("PROBE ") else {
            continue;
        };
        let mut parts = rest.splitn(3, ' ');
        let Some(idx_str) = parts.next() else { continue };
        let Some(status) = parts.next() else { continue };
        let Ok(index) = idx_str.parse::<usize>() else { continue };
        let ok = status == "ok";
        let error = if ok {
            String::new()
        } else {
            parts.next().unwrap_or(status).to_string()
        };
        checks.push(ParseCheck { index, ok, error });
    }
    Ok(checks)
}

/// Fetches a subscription URL through `ulb-warp -fetch` (optionally via SOCKS5 port).
pub fn run_fetch_subscription(
    binary: &Path,
    url: &str,
    socks_port: u16,
    timeout: Duration,
) -> Result<Vec<String>, String> {
    let mut cmd = Command::new(binary);
    cmd.arg("-fetch");
    if socks_port > 0 {
        cmd.arg("-socks").arg(format!("127.0.0.1:{socks_port}"));
    }
    let (code, stdout) = run_oneshot_stdout(cmd, Some(("ULB_FETCH", url.to_string())), timeout)?;
    let mut links = Vec::new();
    let mut err_msg = None;
    for line in stdout.lines() {
        if let Some(link) = line.strip_prefix("LINK ") {
            links.push(link.to_string());
        } else if let Some(err) = line.strip_prefix("ERROR ") {
            err_msg = Some(err.to_string());
        }
    }
    if let Some(e) = err_msg {
        return Err(e);
    }
    if code != 0 && links.is_empty() {
        return Err(format!("更新订阅失败 (exit {code})"));
    }
    Ok(links)
}

/// Downloads a verified extension CRX archive via `ulb-warp -download` (SHA-256 + CRX3 signer verified in Go).
pub fn run_download_extension(
    binary: &Path,
    url: &str,
    dest: &Path,
    crx_id: &str,
    sha256_hex: &str,
    socks_port: u16,
    timeout: Duration,
) -> Result<u64, String> {
    let mut cmd = Command::new(binary);
    cmd.arg("-download");
    if socks_port > 0 {
        cmd.arg("-socks").arg(format!("127.0.0.1:{socks_port}"));
    }
    cmd.env("ULB_DOWNLOAD_TO", dest)
        .env("ULB_DOWNLOAD_CRXID", crx_id)
        .env("ULB_DOWNLOAD_SHA256", sha256_hex);

    let (code, stdout) = run_oneshot_stdout(cmd, Some(("ULB_DOWNLOAD", url.to_string())), timeout)?;
    let mut bytes = None;
    let mut err_msg = None;
    for line in stdout.lines() {
        if let Some(n) = line.strip_prefix("DOWNLOADED ") {
            bytes = n.trim().parse::<u64>().ok();
        } else if let Some(err) = line.strip_prefix("ERROR ") {
            err_msg = Some(err.to_string());
        }
    }
    if let Some(e) = err_msg {
        return Err(e);
    }
    if code != 0 {
        return Err(format!("下载扩展失败 (exit {code})"));
    }
    bytes.ok_or_else(|| format!("下载未完成 ({})", now_epoch_ms()))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_warp_stdout_lines() {
        let mut s = WarpStatus::default();
        parse_warp_line("PORT 54321", &mut s);
        parse_warp_line("ENDPOINT 162.159.192.1:2408", &mut s);
        parse_warp_line("SCAN start", &mut s);
        assert!(s.probing);
        parse_warp_line("SCAN done", &mut s);
        assert!(!s.probing);
        parse_warp_line("STATE up", &mut s);
        parse_warp_line("LINE ss://...", &mut s);
        parse_warp_line("REASON connected", &mut s);

        assert_eq!(s.port, 54321);
        assert!(s.up);
        assert_eq!(s.endpoint, "162.159.192.1:2408");
        assert_eq!(s.line, "ss://...");
        assert_eq!(s.reason, "connected");
    }

    #[test]
    fn encrypts_and_decrypts_lines_dat() {
        let dir = std::env::temp_dir().join(format!("ulb-warp-test-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();

        let mut store = LineStore::new();
        let added = store.add(&["vless://11111111-1111-1111-1111-111111111111@example.com:443#JP"], "");
        assert_eq!(added, 1);
        save_encrypted_lines(&dir, &store).unwrap();

        let raw = fs::read(dir.join("lines.dat")).unwrap();
        assert!(raw.starts_with(LINES_FILE_MAGIC));
        assert!(String::from_utf8_lossy(&raw).find("example.com").is_none());

        let mut loaded = LineStore::new();
        assert!(load_encrypted_lines(&dir, &mut loaded));
        assert_eq!(loaded.lines().len(), 1);
        assert_eq!(loaded.lines()[0].name, "JP");

        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn public_line_starts_with_ss_and_resolves() {
        let line = public_line();
        assert!(line.starts_with("ss://"));
        assert!(line.contains(":19465#"));
    }
}
