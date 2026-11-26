use std::fs;
use std::io::Write;
use std::path::Path;
use std::time::{SystemTime, UNIX_EPOCH};

/// Current Unix timestamp in milliseconds.
pub fn now_epoch_ms() -> i64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_millis() as i64)
        .unwrap_or(0)
}

/// Extracts lowercase hostname (without userinfo or port) from a URL, or empty if none.
pub fn domain_of(url: &str) -> String {
    let Some((_, after_scheme)) = url.split_once("://") else {
        return String::new();
    };
    let authority = after_scheme
        .split(&['/', '?', '#'][..])
        .next()
        .unwrap_or("");
    let host_port = authority.rsplit('@').next().unwrap_or("");
    let host = if let Some(inner) = host_port.strip_prefix('[') {
        inner.split(']').next().unwrap_or("")
    } else {
        host_port.split(':').next().unwrap_or("")
    };
    host.to_ascii_lowercase()
}

/// Clips a UTF-8 string to at most `max_bytes` without splitting a code point.
pub fn clip_utf8(s: &str, max_bytes: usize) -> &str {
    if s.len() <= max_bytes {
        return s;
    }
    let mut cut = max_bytes;
    while cut > 0 && !s.is_char_boundary(cut) {
        cut -= 1;
    }
    &s[..cut]
}

/// Writes `data` to `path` via a sibling `.tmp` file and atomic rename.
pub fn write_file_atomic(path: &Path, data: &[u8]) -> std::io::Result<()> {
    if let Some(parent) = path.parent() {
        if !parent.as_os_str().is_empty() {
            fs::create_dir_all(parent)?;
        }
    }
    let mut tmp = path.as_os_str().to_os_string();
    tmp.push(".tmp");
    let tmp_path = Path::new(&tmp);
    let write_res = (|| -> std::io::Result<()> {
        let mut f = fs::File::create(tmp_path)?;
        f.write_all(data)?;
        f.flush()?;
        Ok(())
    })();
    if let Err(e) = write_res {
        let _ = fs::remove_file(tmp_path);
        return Err(e);
    }
    if let Err(e) = fs::rename(tmp_path, path) {
        let _ = fs::remove_file(tmp_path);
        return Err(e);
    }
    Ok(())
}

/// Decodes standard or URL-safe Base64 (padding optional, ignoring ASCII whitespace).
pub fn decode_base64(input: &str) -> Option<Vec<u8>> {
    let mut out = Vec::with_capacity(input.len() * 3 / 4);
    let mut val: u32 = 0;
    let mut bits: i32 = -8;
    for b in input.bytes() {
        let d = match b {
            b'A'..=b'Z' => (b - b'A') as u32,
            b'a'..=b'z' => (b - b'a' + 26) as u32,
            b'0'..=b'9' => (b - b'0' + 52) as u32,
            b'+' | b'-' => 62,
            b'/' | b'_' => 63,
            b'=' | b'\n' | b'\r' | b' ' => continue,
            _ => return None,
        };
        val = (val << 6) | d;
        bits += 6;
        if bits >= 0 {
            out.push(((val >> bits) & 0xFF) as u8);
            bits -= 8;
        }
    }
    Some(out)
}
