use crate::util::{clip_utf8, decode_base64};
use serde::{Deserialize, Serialize};
use serde_json::Value;

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct LineEntry {
    pub id: String,
    pub name: String,
    pub link: String,
    #[serde(default)]
    pub sub: String,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct SubEntry {
    pub id: String,
    pub name: String,
    pub url: String,
    #[serde(default)]
    pub updated: i64,
    #[serde(default)]
    pub count: usize,
}

#[derive(Debug, Clone)]
pub struct LineStore {
    lines: Vec<LineEntry>,
    subs: Vec<SubEntry>,
    selected: String,
    next_id: u64,
}

impl Default for LineStore {
    fn default() -> Self {
        Self::new()
    }
}

fn percent_decode(s: &str) -> String {
    let bytes = s.as_bytes();
    let mut out = Vec::with_capacity(bytes.len());
    let mut i = 0;
    while i < bytes.len() {
        if bytes[i] == b'%' && i + 2 < bytes.len() {
            if let Ok(hex) = std::str::from_utf8(&bytes[i + 1..i + 3]) {
                if let Ok(val) = u8::from_str_radix(hex, 16) {
                    out.push(val);
                    i += 3;
                    continue;
                }
            }
        }
        out.push(if bytes[i] == b'+' { b' ' } else { bytes[i] });
        i += 1;
    }
    String::from_utf8_lossy(&out).into_owned()
}

fn clean_label(s: &str) -> String {
    let filtered: String = s
        .chars()
        .filter(|&c| c != '\r' && c != '\n' && c != '\t')
        .collect();
    let trimmed = filtered.trim();
    clip_utf8(trimmed, 80).to_string()
}

fn scheme(link: &str) -> String {
    link.split_once("://")
        .map(|(s, _)| s.to_ascii_lowercase())
        .unwrap_or_default()
}

fn vmess_json(link: &str) -> Option<serde_json::Map<String, Value>> {
    let (_, rest) = link.split_once("://")?;
    let bytes = decode_base64(rest)?;
    let Value::Object(map) = serde_json::from_slice::<Value>(&bytes).ok()? else {
        return None;
    };
    Some(map)
}

fn json_str(map: &serde_json::Map<String, Value>, key: &str) -> String {
    match map.get(key) {
        Some(Value::String(s)) => s.clone(),
        Some(Value::Number(n)) => n.to_string(),
        _ => String::new(),
    }
}

impl LineStore {
    pub const PUBLIC: &'static str = "public";
    pub const AUTO: &'static str = "auto";
    pub const MAX_LINES: usize = 500;
    pub const AUTO_MAX: usize = 20;

    pub fn new() -> Self {
        Self {
            lines: Vec::new(),
            subs: Vec::new(),
            selected: Self::PUBLIC.to_string(),
            next_id: 1,
        }
    }

    pub fn lines(&self) -> &[LineEntry] {
        &self.lines
    }

    pub fn subs(&self) -> &[SubEntry] {
        &self.subs
    }

    pub fn selected(&self) -> &str {
        &self.selected
    }

    pub fn find(&self, id: &str) -> Option<&LineEntry> {
        self.lines.iter().find(|l| l.id == id)
    }

    fn next_id(&mut self, prefix: char) -> String {
        let id = format!("{prefix}{}", self.next_id);
        self.next_id += 1;
        id
    }

    pub fn load(&mut self, text: &str) -> bool {
        *self = Self::new();
        let Ok(Value::Object(root)) = serde_json::from_str::<Value>(text) else {
            return false;
        };
        let get_str = |obj: &serde_json::Map<String, Value>, k: &str| {
            obj.get(k)
                .and_then(Value::as_str)
                .unwrap_or("")
                .to_string()
        };
        if let Some(Value::Array(arr)) = root.get("lines") {
            for item in arr {
                if let Value::Object(o) = item {
                    let entry = LineEntry {
                        id: get_str(o, "id"),
                        name: get_str(o, "name"),
                        link: get_str(o, "link"),
                        sub: get_str(o, "sub"),
                    };
                    if !entry.id.is_empty()
                        && !entry.link.is_empty()
                        && self.lines.len() < Self::MAX_LINES
                    {
                        self.lines.push(entry);
                    }
                }
            }
        }
        if let Some(Value::Array(arr)) = root.get("subs") {
            for item in arr {
                if let Value::Object(o) = item {
                    let sub = SubEntry {
                        id: get_str(o, "id"),
                        name: get_str(o, "name"),
                        url: get_str(o, "url"),
                        updated: o.get("updated").and_then(Value::as_i64).unwrap_or(0),
                        count: o.get("count").and_then(Value::as_u64).unwrap_or(0) as usize,
                    };
                    if !sub.id.is_empty() && !sub.url.is_empty() {
                        self.subs.push(sub);
                    }
                }
            }
        }
        self.next_id = root
            .get("next")
            .and_then(Value::as_u64)
            .unwrap_or(1)
            .max(1);
        let sel = root
            .get("selected")
            .and_then(Value::as_str)
            .unwrap_or("");
        self.selected = Self::PUBLIC.to_string();
        if !sel.is_empty() {
            self.select(sel);
        }
        true
    }

    pub fn save(&self) -> String {
        serde_json::json!({
            "lines": self.lines,
            "subs": self.subs,
            "selected": self.selected,
            "next": self.next_id,
        })
        .to_string()
    }

    pub fn add(&mut self, links: &[&str], sub: &str) -> usize {
        let mut added = 0;
        for raw in links {
            let link = raw.trim();
            if link.is_empty()
                || link.len() > 8192
                || scheme(link).is_empty()
                || self.lines.len() >= Self::MAX_LINES
            {
                continue;
            }
            if self.lines.iter().any(|l| l.link == link) {
                continue;
            }
            let id = self.next_id('l');
            self.lines.push(LineEntry {
                id,
                name: Self::name_of(link),
                link: link.to_string(),
                sub: sub.to_string(),
            });
            added += 1;
        }
        added
    }

    pub fn remove(&mut self, id: &str) -> bool {
        let Some(pos) = self.lines.iter().position(|l| l.id == id) else {
            return false;
        };
        self.lines.remove(pos);
        if self.selected == id || (self.selected == Self::AUTO && self.lines.is_empty()) {
            self.selected = Self::PUBLIC.to_string();
        }
        true
    }

    pub fn rename(&mut self, id: &str, name: &str) -> bool {
        let Some(line) = self.lines.iter_mut().find(|l| l.id == id) else {
            return false;
        };
        let cleaned = clean_label(name);
        line.name = if cleaned.is_empty() {
            Self::name_of(&line.link)
        } else {
            cleaned
        };
        true
    }

    pub fn select(&mut self, id: &str) -> bool {
        if id == Self::PUBLIC
            || (id == Self::AUTO && !self.lines.is_empty())
            || self.find(id).is_some()
        {
            self.selected = id.to_string();
            true
        } else {
            false
        }
    }

    pub fn add_sub(&mut self, name: &str, url: &str) -> String {
        if let Some(existing) = self.subs.iter().find(|s| s.url == url) {
            return existing.id.clone();
        }
        let id = self.next_id('s');
        let mut cleaned = clean_label(name);
        if cleaned.is_empty() {
            cleaned = format!("订阅 {}", self.subs.len() + 1);
        }
        self.subs.push(SubEntry {
            id: id.clone(),
            name: cleaned,
            url: url.to_string(),
            updated: 0,
            count: 0,
        });
        id
    }

    pub fn remove_sub(&mut self, id: &str) -> bool {
        let Some(pos) = self.subs.iter().position(|s| s.id == id) else {
            return false;
        };
        self.subs.remove(pos);
        let doomed: Vec<String> = self
            .lines
            .iter()
            .filter(|l| l.sub == id)
            .map(|l| l.id.clone())
            .collect();
        for d in doomed {
            self.remove(&d);
        }
        true
    }

    pub fn replace_sub(&mut self, id: &str, links: &[&str], now: i64) -> bool {
        if !self.subs.iter().any(|s| s.id == id) {
            return false;
        }
        let keep_link = self
            .find(&self.selected)
            .filter(|l| l.sub == id)
            .map(|l| l.link.clone());
        let prev_selected = self.selected.clone();
        self.lines.retain(|l| l.sub != id);
        self.add(links, id);
        let count = self.lines.iter().filter(|l| l.sub == id).count();
        if let Some(sub) = self.subs.iter_mut().find(|s| s.id == id) {
            sub.updated = now;
            sub.count = count;
        }
        if let Some(keep) = keep_link {
            self.selected = self
                .lines
                .iter()
                .find(|l| l.link == keep)
                .map(|l| l.id.clone())
                .unwrap_or_else(|| Self::PUBLIC.to_string());
        } else if !self.select(&prev_selected) {
            self.selected = Self::PUBLIC.to_string();
        }
        true
    }

    pub fn selected_links(&self) -> Vec<&str> {
        if self.selected == Self::AUTO {
            self.lines
                .iter()
                .take(Self::AUTO_MAX)
                .map(|l| l.link.as_str())
                .collect()
        } else if let Some(l) = self.find(&self.selected) {
            vec![l.link.as_str()]
        } else {
            Vec::new()
        }
    }

    pub fn protocol_of(link: &str) -> String {
        match scheme(link).as_str() {
            "vless" => "VLESS".into(),
            "trojan" => "Trojan".into(),
            "vmess" => "VMess".into(),
            "ss" => "Shadowsocks".into(),
            "hysteria2" | "hy2" => "Hysteria2".into(),
            "wireguard" => "WireGuard".into(),
            other => other.to_string(),
        }
    }

    pub fn server_of(link: &str) -> String {
        let s = scheme(link);
        if s.is_empty() {
            return String::new();
        }
        if s == "vmess" {
            let Some(v) = vmess_json(link) else {
                return String::new();
            };
            let host = json_str(&v, "add");
            let port = json_str(&v, "port");
            return if host.is_empty() {
                String::new()
            } else {
                format!("{host}:{port}")
            };
        }
        let mut rest = link.split_once("://").map(|(_, r)| r).unwrap_or("");
        if let Some((before_hash, _)) = rest.split_once('#') {
            rest = before_hash;
        }
        let decoded;
        if s == "ss" && !rest.contains('@') {
            let b64_part = rest.split('?').next().unwrap_or(rest);
            let Some(bytes) = decode_base64(b64_part) else {
                return String::new();
            };
            decoded = String::from_utf8_lossy(&bytes).into_owned();
            rest = &decoded;
        }
        let Some((_, hp)) = rest.rsplit_once('@') else {
            return String::new();
        };
        let hp = hp.split(['/', '?']).next().unwrap_or("");
        clean_label(hp)
    }

    pub fn name_of(link: &str) -> String {
        if scheme(link) == "vmess" {
            if let Some(v) = vmess_json(link) {
                let ps = clean_label(&json_str(&v, "ps"));
                if !ps.is_empty() {
                    return ps;
                }
            }
        } else if let Some((_, frag)) = link.split_once('#') {
            let name = clean_label(&percent_decode(frag));
            if !name.is_empty() {
                return name;
            }
        }
        let server = Self::server_of(link);
        let proto = Self::protocol_of(link);
        if server.is_empty() {
            proto
        } else {
            format!("{proto} {server}")
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const VLESS: &str = "vless://8f4c3a2e-1b2d-4c5e-9f6a-7b8c9d0e1f2a@[2001:db8::1]:443?security=reality&pbk=x&sid=1#%E9%A6%99%E6%B8%AF%201";
    const TROJAN: &str = "trojan://secret@example.com:8443?sni=example.com";
    const VMESS: &str = "vmess://eyJ2IjoiMiIsInBzIjoi5Lic5LqsIiwiYWRkIjoiMTkyLjAuMi43IiwicG9ydCI6IjQ0MyIsImlkIjoieCJ9";
    const SS_LEGACY: &str = "ss://YWVzLTI1Ni1nY206cHdAMTkyLjAuMi45OjgzODg=#old";

    #[test]
    fn line_store_display_subscriptions_and_persistence() {
        assert_eq!(LineStore::name_of(VLESS), "香港 1");
        assert_eq!(LineStore::server_of(VLESS), "[2001:db8::1]:443");
        assert_eq!(LineStore::protocol_of(VLESS), "VLESS");
        assert_eq!(LineStore::name_of(TROJAN), "Trojan example.com:8443");
        assert_eq!(LineStore::name_of(VMESS), "东京");
        assert_eq!(LineStore::server_of(VMESS), "192.0.2.7:443");
        assert_eq!(LineStore::server_of(SS_LEGACY), "192.0.2.9:8388");
        assert_eq!(
            LineStore::protocol_of("hysteria2://pw@example.com:443"),
            "Hysteria2"
        );
        assert_eq!(
            LineStore::protocol_of("hy2://pw@example.com:443"),
            "Hysteria2"
        );
        assert_eq!(LineStore::protocol_of("wireguard://abc"), "WireGuard");
        assert_eq!(LineStore::name_of("vless://x@h:1#%0A%0A"), "VLESS h:1");

        let mut s = LineStore::new();
        assert_eq!(s.selected(), LineStore::PUBLIC);
        assert!(!s.select(LineStore::AUTO));
        assert_eq!(
            s.add(&[VLESS, &format!(" {TROJAN}\r"), VLESS, "", "not a link"], ""),
            2
        );
        let id = s.lines()[1].id.clone();
        assert!(s.select(&id));
        assert_eq!(s.selected_links(), vec![TROJAN]);
        assert!(s.rename(&id, "  我的服务器\n "));
        assert_eq!(s.find(&id).unwrap().name, "我的服务器");
        assert!(s.rename(&id, ""));
        assert_eq!(s.find(&id).unwrap().name, "Trojan example.com:8443");
        assert!(s.select(LineStore::AUTO));
        assert_eq!(s.selected_links().len(), 2);
        assert!(s.select(&id) && s.remove(&id));
        assert_eq!(s.selected(), LineStore::PUBLIC);

        let sub = s.add_sub("机场", "https://sub.example.com/x");
        assert_eq!(s.add_sub("again", "https://sub.example.com/x"), sub);
        assert!(s.replace_sub(&sub, &[VLESS, VMESS], 1000));
        let vmess_id = s
            .lines()
            .iter()
            .find(|l| l.link == VMESS)
            .unwrap()
            .id
            .clone();
        assert!(s.select(&vmess_id));
        assert!(s.replace_sub(&sub, &[VMESS, SS_LEGACY], 2000));
        assert_eq!(s.selected_links(), vec![VMESS]);
        assert!(s.replace_sub(&sub, &[SS_LEGACY], 3000));
        assert_eq!(s.selected(), LineStore::PUBLIC);

        let saved = s.save();
        let mut t = LineStore::new();
        assert!(t.load(&saved));
        assert_eq!(t.save(), saved);
        assert!(!t.load("not json"));
        assert!(t.lines().is_empty() && t.selected() == LineStore::PUBLIC);
    }
}
