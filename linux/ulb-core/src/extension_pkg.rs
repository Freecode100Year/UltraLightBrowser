use serde_json::Value;
use std::fs;
use std::path::Path;

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct ExtensionManifest {
    pub name: String,
    pub version: String,
    pub description: String,
    pub manifest_version: i64,
    pub popup: String,
    pub options: String,
    pub proxy: bool,
    pub error: String,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct RecommendedExtension {
    pub key: &'static str,
    pub name: &'static str,
    pub summary: &'static str,
    pub note: &'static str,
    pub homepage: &'static str,
    pub store_id: &'static str,
    pub url: &'static str,
    pub sha256: &'static str,
    pub proxy: bool,
}

pub fn store_download_url(id: &str) -> String {
    format!(
        "https://clients2.google.com/service/update2/crx?response=redirect&prodversion=142.0&acceptformat=crx3&x=id%3D{id}%26uc"
    )
}

const RECOMMENDED: [RecommendedExtension; 2] = [
    RecommendedExtension {
        key: "global-speed",
        name: "Global Speed：视频速度控制",
        summary: "调整所有网站视频和音频的播放速度，可用快捷键加速、减速、跳过。",
        note: "",
        homepage: "https://chromewebstore.google.com/detail/jpbjcnkcffbooppibceonlgknpkniiff",
        store_id: "jpbjcnkcffbooppibceonlgknpkniiff",
        url: "",
        sha256: "",
        proxy: false,
    },
    RecommendedExtension {
        key: "proton-vpn",
        name: "Proton VPN",
        summary: "瑞士 Proton 公司的开源（GPL-3.0）VPN 扩展，有免费线路，需要 Proton 账号。",
        note: "它会接管浏览器的网络代理：开着它时，ULB 自己的 WARP / 我的线路不再生效，「线路不通时不直连」的保护也由它决定。不用时请在这里关掉。",
        homepage: "https://protonvpn.com/download-browser-extension",
        store_id: "jplgfhpmjnbigmhklmmbgecoobifkmpa",
        url: "",
        sha256: "",
        proxy: true,
    },
];

pub fn recommended_extensions() -> &'static [RecommendedExtension] {
    &RECOMMENDED
}

pub fn find_recommended_extension(key: &str) -> Option<&'static RecommendedExtension> {
    RECOMMENDED.iter().find(|e| e.key == key)
}

/// Strips JSON `//` and `/* */` comments outside string literals, as Chrome
/// accepts comments in `manifest.json`.
fn strip_json_comments(input: &str) -> String {
    let mut out = String::with_capacity(input.len());
    let b = input.as_bytes();
    let mut i = 0;
    let mut in_str = false;
    while i < b.len() {
        if in_str {
            let ch = input[i..].chars().next().unwrap();
            out.push(ch);
            if ch == '\\' && i + 1 < b.len() {
                i += 1;
                let next = input[i..].chars().next().unwrap();
                out.push(next);
                i += next.len_utf8();
                continue;
            }
            if ch == '"' {
                in_str = false;
            }
            i += ch.len_utf8();
            continue;
        }
        if b[i] == b'"' {
            in_str = true;
            out.push('"');
            i += 1;
        } else if i + 1 < b.len() && b[i] == b'/' && b[i + 1] == b'/' {
            i += 2;
            while i < b.len() && b[i] != b'\n' {
                i += 1;
            }
        } else if i + 1 < b.len() && b[i] == b'/' && b[i + 1] == b'*' {
            i += 2;
            while i + 1 < b.len() && !(b[i] == b'*' && b[i + 1] == b'/') {
                i += 1;
            }
            i = (i + 2).min(b.len());
        } else {
            let ch = input[i..].chars().next().unwrap();
            out.push(ch);
            i += ch.len_utf8();
        }
    }
    out
}

fn read_json_lenient(path: &Path) -> Option<Value> {
    let raw = fs::read_to_string(path).ok()?;
    let trimmed = raw.strip_prefix('\u{FEFF}').unwrap_or(&raw);
    if let Ok(v) = serde_json::from_str::<Value>(trimmed) {
        return Some(v);
    }
    serde_json::from_str::<Value>(&strip_json_comments(trimmed)).ok()
}

fn localize(dir: &Path, value: &str, default_locale: &str) -> String {
    let Some(inner) = value
        .strip_prefix("__MSG_")
        .and_then(|s| s.strip_suffix("__"))
    else {
        return value.to_string();
    };
    let key = inner.to_ascii_lowercase();
    for locale in ["zh_CN", "zh", default_locale, "en"] {
        if locale.is_empty() {
            continue;
        }
        let path = dir.join("_locales").join(locale).join("messages.json");
        let Some(Value::Object(map)) = read_json_lenient(&path) else {
            continue;
        };
        for (k, v) in map {
            if k.to_ascii_lowercase() == key {
                if let Some(msg) = v.get("message").and_then(Value::as_str) {
                    if !msg.is_empty() {
                        return msg.to_string();
                    }
                }
            }
        }
    }
    value.to_string()
}

/// Validates and normalizes an extension-internal page path.
pub fn extension_page_path(page: &str) -> String {
    let p = page.trim_start_matches(['/', '\\']);
    if p.is_empty() || p.len() > 300 {
        return String::new();
    }
    let normalized = p.replace('\\', "/");
    for part in normalized.split('/') {
        if part == ".." || part.contains(':') {
            return String::new();
        }
    }
    normalized
}

pub fn read_extension_manifest(dir: &Path) -> ExtensionManifest {
    let mut m = ExtensionManifest::default();
    let Some(root) = read_json_lenient(&dir.join("manifest.json")).filter(Value::is_object) else {
        m.error = "文件夹里没有有效的 manifest.json（请选择解压后的扩展文件夹）".to_string();
        return m;
    };
    let str_field = |obj: &Value, k: &str| {
        obj.get(k)
            .and_then(Value::as_str)
            .unwrap_or("")
            .to_string()
    };
    m.manifest_version = root.get("manifest_version").and_then(Value::as_i64).unwrap_or(0);
    let locale = str_field(&root, "default_locale");
    m.name = localize(dir, &str_field(&root, "name"), &locale);
    m.version = str_field(&root, "version");
    m.description = localize(dir, &str_field(&root, "description"), &locale);
    for key in ["action", "browser_action", "page_action"] {
        if m.popup.is_empty() {
            if let Some(v) = root.get(key) {
                m.popup = str_field(v, "default_popup");
            }
        }
    }
    if let Some(v) = root.get("options_ui") {
        m.options = str_field(v, "page");
    }
    if m.options.is_empty() {
        m.options = str_field(&root, "options_page");
    }
    for key in ["permissions", "optional_permissions"] {
        if let Some(arr) = root.get(key).and_then(Value::as_array) {
            if arr.iter().any(|p| p.as_str() == Some("proxy")) {
                m.proxy = true;
            }
        }
    }
    m.popup = extension_page_path(&m.popup);
    m.options = extension_page_path(&m.options);
    if m.name.is_empty() || m.version.is_empty() {
        m.error = "manifest.json 缺少 name 或 version".to_string();
    } else if m.manifest_version != 2 && m.manifest_version != 3 {
        m.error = "不支持的 manifest_version".to_string();
    }
    m
}

/// Returns the ZIP archive offset inside a CRX2/CRX3 container (or `0` for plain ZIP).
pub fn crx_zip_offset(bytes: &[u8]) -> Option<usize> {
    if bytes.starts_with(b"PK\x03\x04") {
        return Some(0);
    }
    if bytes.len() < 16 || !bytes.starts_with(b"Cr24") {
        return None;
    }
    let u32_at = |at: usize| -> usize {
        u32::from_le_bytes(bytes[at..at + 4].try_into().unwrap()) as usize
    };
    let version = u32_at(4);
    let offset = match version {
        3 => 12usize.checked_add(u32_at(8))?,
        2 => 16usize.checked_add(u32_at(8))?.checked_add(u32_at(12))?,
        _ => return None,
    };
    if bytes.get(offset..offset.checked_add(4)?) == Some(b"PK\x03\x04") {
        Some(offset)
    } else {
        None
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::HashSet;

    fn write(path: &Path, text: &str) {
        fs::create_dir_all(path.parent().unwrap()).unwrap();
        fs::write(path, text).unwrap();
    }

    #[test]
    fn manifest_crx_paths_and_recommended() {
        let dir = std::env::temp_dir().join("ulb-core-ext-test");
        let _ = fs::remove_dir_all(&dir);
        write(
            &dir.join("manifest.json"),
            "\u{FEFF}{\n // comment\n \"manifest_version\": 3, \"name\": \"__MSG_appName__\", \"version\": \"1.0.2\",\n \"default_locale\": \"en\", \"action\": {\"default_popup\": \"/popup/index.html\"},\n \"options_ui\": {\"page\": \"options.html\"},\n \"permissions\": [\"storage\", \"proxy\"]}",
        );
        write(
            &dir.join("_locales/en/messages.json"),
            r#"{"APPNAME": {"message": "Blocker"}}"#,
        );
        write(
            &dir.join("_locales/zh_CN/messages.json"),
            r#"{"appName": {"message": "拦截器"}}"#,
        );

        let m = read_extension_manifest(&dir);
        assert!(m.error.is_empty());
        assert_eq!(m.name, "拦截器");
        assert_eq!(m.popup, "popup/index.html");
        assert_eq!(m.options, "options.html");
        assert!(m.proxy);

        fs::remove_file(dir.join("_locales/zh_CN/messages.json")).unwrap();
        assert_eq!(read_extension_manifest(&dir).name, "Blocker");

        let zip = b"PK\x03\x04rest";
        assert_eq!(crx_zip_offset(zip), Some(0));
        let mut crx3 = b"Cr24".to_vec();
        crx3.extend_from_slice(&3u32.to_le_bytes());
        crx3.extend_from_slice(&5u32.to_le_bytes());
        crx3.extend_from_slice(b"hdr12");
        crx3.extend_from_slice(zip);
        assert_eq!(crx_zip_offset(&crx3), Some(17));
        assert_eq!(crx_zip_offset(b"<html>"), None);

        assert_eq!(extension_page_path("/a\\b.html"), "a/b.html");
        assert!(extension_page_path("a/../../b.html").is_empty());
        assert!(extension_page_path("https://evil/").is_empty());

        let mut keys = HashSet::new();
        for r in recommended_extensions() {
            assert!(keys.insert(r.key));
            assert!(find_recommended_extension(r.key).is_some());
            assert!(r.homepage.starts_with("https://"));
            assert_ne!(r.store_id.is_empty(), r.url.is_empty());
            if r.proxy {
                assert!(r.note.contains("代理"));
            }
        }
        let _ = fs::remove_dir_all(&dir);
    }
}
