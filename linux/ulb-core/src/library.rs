use crate::util::{clip_utf8, write_file_atomic};
use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::collections::{BTreeMap, HashMap, HashSet};
use std::fs;
use std::path::{Path, PathBuf};

pub const FAVORITES_FOLDER: &str = "个人收藏";
pub const BOOKMARKS_FOLDER: &str = "书签";

const MAX_HISTORY: usize = 5000;
const HISTORY_RETENTION_MS: i64 = 90 * 24 * 3600 * 1000;
const MAX_TITLE: usize = 512;
const MAX_URL: usize = 8192;

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct Bookmark {
    pub id: String,
    pub title: String,
    pub url: String,
    pub folder: String,
    pub added: i64,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ReadingItem {
    pub id: String,
    pub title: String,
    pub url: String,
    pub added: i64,
    pub read: bool,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct HistoryEntry {
    pub url: String,
    pub title: String,
    pub last: i64,
    pub visits: i32,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct SavedTab {
    pub title: String,
    pub url: String,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct TabGroup {
    pub id: String,
    pub name: String,
    pub tabs: Vec<SavedTab>,
    pub updated: i64,
}

#[derive(Debug, Clone, PartialEq)]
pub struct SiteSettings {
    pub zoom: f64,
    pub auto_reader: bool,
    pub camera: String,
    pub microphone: String,
    pub location: String,
    pub popups: String,
    pub adblock: bool,
}

impl Default for SiteSettings {
    fn default() -> Self {
        Self {
            zoom: 0.0,
            auto_reader: false,
            camera: "ask".to_string(),
            microphone: "ask".to_string(),
            location: "ask".to_string(),
            popups: "block".to_string(),
            adblock: true,
        }
    }
}

impl SiteSettings {
    pub fn is_default(&self) -> bool {
        self.zoom == 0.0
            && !self.auto_reader
            && self.camera == "ask"
            && self.microphone == "ask"
            && self.location == "ask"
            && self.popups == "block"
            && self.adblock
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Suggestion {
    pub title: String,
    pub url: String,
    pub bookmark: bool,
}

fn html_escape(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    for ch in s.chars() {
        match ch {
            '&' => out.push_str("&amp;"),
            '<' => out.push_str("&lt;"),
            '>' => out.push_str("&gt;"),
            '"' => out.push_str("&quot;"),
            _ => out.push(ch),
        }
    }
    out
}

fn html_unescape(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    let mut i = 0;
    let bytes = s.as_bytes();
    while i < bytes.len() {
        if bytes[i] != b'&' {
            let ch = s[i..].chars().next().unwrap();
            out.push(ch);
            i += ch.len_utf8();
            continue;
        }
        let Some(rel_semi) = s[i..].find(';') else {
            out.push('&');
            i += 1;
            continue;
        };
        if rel_semi > 10 {
            out.push('&');
            i += 1;
            continue;
        }
        let ent = &s[i + 1..i + rel_semi];
        match ent {
            "amp" => out.push('&'),
            "lt" => out.push('<'),
            "gt" => out.push('>'),
            "quot" => out.push('"'),
            "apos" => out.push('\''),
            "nbsp" => out.push(' '),
            _ if ent.starts_with('#') => {
                let code = if let Some(hex) = ent[1..].strip_prefix(['x', 'X']) {
                    u32::from_str_radix(hex, 16).ok()
                } else {
                    ent[1..].parse::<u32>().ok()
                };
                if let Some(ch) = code.and_then(char::from_u32) {
                    out.push(ch);
                } else {
                    out.push_str(&s[i..i + rel_semi + 1]);
                }
            }
            _ => out.push_str(&s[i..i + rel_semi + 1]),
        }
        i += rel_semi + 1;
    }
    out
}

fn find_ci(hay: &str, needle_lower: &str, from: usize) -> Option<usize> {
    let h = hay.as_bytes();
    let n = needle_lower.as_bytes();
    if n.is_empty() || h.len() < from + n.len() {
        return None;
    }
    for i in from..=h.len() - n.len() {
        if h[i..i + n.len()]
            .iter()
            .zip(n.iter())
            .all(|(a, b)| a.to_ascii_lowercase() == *b)
        {
            return Some(i);
        }
    }
    None
}

fn attr_value<'a>(tag: &'a str, name: &str) -> &'a str {
    let is_ws = |b: u8| matches!(b, b' ' | b'\t' | b'\r' | b'\n' | 0x0c);
    let b = tag.as_bytes();
    let mut pos = match b.iter().position(|&c| is_ws(c)) {
        Some(p) => p,
        None => return "",
    };
    while pos < b.len() {
        while pos < b.len() && is_ws(b[pos]) {
            pos += 1;
        }
        if pos >= b.len() {
            break;
        }
        let start = pos;
        while pos < b.len() && b[pos] != b'=' && !is_ws(b[pos]) {
            pos += 1;
        }
        if pos >= b.len() {
            break;
        }
        let attr = tag[start..pos].to_ascii_lowercase();
        while pos < b.len() && is_ws(b[pos]) {
            pos += 1;
        }
        if pos >= b.len() {
            break;
        }
        if b[pos] != b'=' {
            continue;
        }
        pos += 1;
        while pos < b.len() && is_ws(b[pos]) {
            pos += 1;
        }
        if pos >= b.len() {
            break;
        }
        let quote = b[pos];
        let quoted = quote == b'"' || quote == b'\'';
        let val_start = pos + usize::from(quoted);
        let end = if quoted {
            match b[val_start..].iter().position(|&c| c == quote) {
                Some(rel) => val_start + rel,
                None => break,
            }
        } else {
            b[val_start..]
                .iter()
                .position(|&c| is_ws(c))
                .map(|rel| val_start + rel)
                .unwrap_or(b.len())
        };
        if attr == name {
            return &tag[val_start..end];
        }
        pos = end + usize::from(quoted);
    }
    ""
}

/// Manages persistent bookmarks/reading list/tab groups and in-memory-only
/// session history and per-site settings.
pub struct Library {
    dir: PathBuf,
    folders: Vec<String>,
    bookmarks: Vec<Bookmark>,
    reading: Vec<ReadingItem>,
    groups: Vec<TabGroup>,
    sites: BTreeMap<String, SiteSettings>,
    history: Vec<HistoryEntry>,
    history_index: HashMap<String, usize>,
    id_counter: u64,
    dirty: bool,
}

impl Library {
    pub fn new(dir: impl AsRef<Path>) -> Self {
        Self {
            dir: dir.as_ref().to_path_buf(),
            folders: vec![FAVORITES_FOLDER.to_string(), BOOKMARKS_FOLDER.to_string()],
            bookmarks: Vec::new(),
            reading: Vec::new(),
            groups: Vec::new(),
            sites: BTreeMap::new(),
            history: Vec::new(),
            history_index: HashMap::new(),
            id_counter: 0,
            dirty: false,
        }
    }

    pub fn host_of(url: &str) -> String {
        let Some((_, rest)) = url.split_once("://") else {
            return String::new();
        };
        let end = rest.find(['/', '?', '#']).unwrap_or(rest.len());
        let authority = &rest[..end];
        let authority = authority.rsplit_once('@').map_or(authority, |(_, h)| h);
        if authority.starts_with('[') {
            let end_b = authority
                .find(']')
                .map(|i| i + 1)
                .unwrap_or(authority.len());
            return authority[..end_b].to_ascii_lowercase();
        }
        let host = authority.split(':').next().unwrap_or("");
        host.trim_end_matches('.').to_ascii_lowercase()
    }

    pub fn is_web_url(url: &str) -> bool {
        let lower: String = url.chars().take(8).collect::<String>().to_ascii_lowercase();
        lower.starts_with("http://") || lower.starts_with("https://")
    }

    fn new_id(&mut self, now: i64) -> String {
        self.id_counter += 1;
        format!("{now:x}-{}", self.id_counter)
    }

    pub fn is_dirty(&self) -> bool {
        self.dirty
    }

    pub fn load(&mut self) {
        self.dirty = false;
        self.load_library_file();
        // History and old privacy/session files are never loaded or kept on disk.
        for name in ["history.json", "privacy.json", "session.json"] {
            let _ = fs::remove_file(self.dir.join(name));
            let _ = fs::remove_file(self.dir.join(format!("{name}.tmp")));
        }
    }

    fn load_library_file(&mut self) {
        let Ok(raw) = fs::read_to_string(self.dir.join("library.json")) else {
            return;
        };
        let Ok(Value::Object(root)) = serde_json::from_str::<Value>(&raw) else {
            return;
        };
        let str_field = |v: &Value, k: &str| {
            v.get(k)
                .and_then(Value::as_str)
                .unwrap_or("")
                .to_string()
        };
        if let Some(Value::Array(arr)) = root.get("folders") {
            for f in arr {
                if let Some(name) = f.as_str() {
                    self.add_folder(name);
                }
            }
        }
        if let Some(Value::Array(arr)) = root.get("bookmarks") {
            for b in arr {
                let id = str_field(b, "id");
                let url = clip_utf8(&str_field(b, "url"), MAX_URL).to_string();
                if id.is_empty() || url.is_empty() {
                    continue;
                }
                let mut folder = str_field(b, "folder");
                if !self.folders.iter().any(|f| f == &folder) {
                    folder = BOOKMARKS_FOLDER.to_string();
                }
                self.bookmarks.push(Bookmark {
                    id,
                    title: clip_utf8(&str_field(b, "title"), MAX_TITLE).to_string(),
                    url,
                    folder,
                    added: b.get("added").and_then(Value::as_i64).unwrap_or(0),
                });
            }
        }
        if let Some(Value::Array(arr)) = root.get("reading") {
            for r in arr {
                let id = str_field(r, "id");
                let url = clip_utf8(&str_field(r, "url"), MAX_URL).to_string();
                if id.is_empty() || url.is_empty() {
                    continue;
                }
                self.reading.push(ReadingItem {
                    id,
                    title: clip_utf8(&str_field(r, "title"), MAX_TITLE).to_string(),
                    url,
                    added: r.get("added").and_then(Value::as_i64).unwrap_or(0),
                    read: r.get("read").and_then(Value::as_bool).unwrap_or(false),
                });
            }
        }
        if let Some(Value::Array(arr)) = root.get("groups") {
            for g in arr {
                let id = str_field(g, "id");
                if id.is_empty() {
                    continue;
                }
                let mut tabs = Vec::new();
                if let Some(Value::Array(tarr)) = g.get("tabs") {
                    for t in tarr {
                        let url = clip_utf8(&str_field(t, "url"), MAX_URL).to_string();
                        if !url.is_empty() {
                            tabs.push(SavedTab {
                                title: clip_utf8(&str_field(t, "title"), MAX_TITLE).to_string(),
                                url,
                            });
                        }
                    }
                }
                self.groups.push(TabGroup {
                    id,
                    name: clip_utf8(&str_field(g, "name"), MAX_TITLE).to_string(),
                    tabs,
                    updated: g.get("updated").and_then(Value::as_i64).unwrap_or(0),
                });
            }
        }
        self.dirty = root.contains_key("sites");
    }

    pub fn save(&mut self) -> bool {
        if !self.dirty {
            return true;
        }
        let body = serde_json::json!({
            "folders": self.folders,
            "bookmarks": self.bookmarks,
            "reading": self.reading,
            "groups": self.groups,
        })
        .to_string();
        if write_file_atomic(&self.dir.join("library.json"), body.as_bytes()).is_ok() {
            self.dirty = false;
            true
        } else {
            self.dirty = true;
            false
        }
    }

    // ---- Bookmarks ----
    pub fn folders(&self) -> &[String] {
        &self.folders
    }

    pub fn add_folder(&mut self, raw_name: &str) -> bool {
        let name = clip_utf8(raw_name.trim(), 128).to_string();
        if name.is_empty() || self.folders.iter().any(|f| f == &name) {
            return false;
        }
        self.folders.push(name);
        self.dirty = true;
        true
    }

    pub fn rename_folder(&mut self, from: &str, raw_to: &str) -> bool {
        let to = clip_utf8(raw_to.trim(), 128).to_string();
        if from == FAVORITES_FOLDER
            || from == BOOKMARKS_FOLDER
            || to.is_empty()
            || self.folders.iter().any(|f| f == &to)
        {
            return false;
        }
        let Some(pos) = self.folders.iter().position(|f| f == from) else {
            return false;
        };
        self.folders[pos] = to.clone();
        for b in &mut self.bookmarks {
            if b.folder == from {
                b.folder = to.clone();
            }
        }
        self.dirty = true;
        true
    }

    pub fn remove_folder(&mut self, name: &str) -> bool {
        if name == FAVORITES_FOLDER || name == BOOKMARKS_FOLDER {
            return false;
        }
        let Some(pos) = self.folders.iter().position(|f| f == name) else {
            return false;
        };
        self.folders.remove(pos);
        for b in &mut self.bookmarks {
            if b.folder == name {
                b.folder = BOOKMARKS_FOLDER.to_string();
            }
        }
        self.dirty = true;
        true
    }

    pub fn bookmarks(&self) -> &[Bookmark] {
        &self.bookmarks
    }

    pub fn bookmarks_in(&self, folder: &str) -> Vec<&Bookmark> {
        self.bookmarks
            .iter()
            .filter(|b| b.folder == folder)
            .collect()
    }

    pub fn add_bookmark(&mut self, title: &str, url: &str, folder: &str, now: i64) -> String {
        if url.is_empty() || url.len() > MAX_URL {
            return String::new();
        }
        let id = self.new_id(now);
        let t = title.trim();
        let chosen_title = clip_utf8(if t.is_empty() { url } else { t }, MAX_TITLE).to_string();
        let chosen_folder = if self.folders.iter().any(|f| f == folder) {
            folder.to_string()
        } else {
            BOOKMARKS_FOLDER.to_string()
        };
        self.bookmarks.push(Bookmark {
            id: id.clone(),
            title: chosen_title,
            url: url.to_string(),
            folder: chosen_folder,
            added: now,
        });
        self.dirty = true;
        id
    }

    pub fn update_bookmark(&mut self, id: &str, title: &str, url: &str, folder: &str) -> bool {
        let folder_ok = self.folders.iter().any(|f| f == folder);
        let Some(b) = self.bookmarks.iter_mut().find(|b| b.id == id) else {
            return false;
        };
        if !url.is_empty() && url.len() <= MAX_URL {
            b.url = url.to_string();
        }
        if !title.trim().is_empty() {
            b.title = clip_utf8(title.trim(), MAX_TITLE).to_string();
        }
        if folder_ok {
            b.folder = folder.to_string();
        }
        self.dirty = true;
        true
    }

    pub fn remove_bookmark(&mut self, id: &str) -> bool {
        let Some(pos) = self.bookmarks.iter().position(|b| b.id == id) else {
            return false;
        };
        self.bookmarks.remove(pos);
        self.dirty = true;
        true
    }

    pub fn move_bookmark(&mut self, id: &str, before_id: &str) -> bool {
        if id == before_id {
            return false;
        }
        let Some(pos) = self.bookmarks.iter().position(|b| b.id == id) else {
            return false;
        };
        let mut moved = self.bookmarks.remove(pos);
        let target = self.bookmarks.iter().position(|b| b.id == before_id);
        match target {
            Some(idx) => {
                moved.folder = self.bookmarks[idx].folder.clone();
                self.bookmarks.insert(idx, moved);
            }
            None => self.bookmarks.push(moved),
        }
        self.dirty = true;
        true
    }

    pub fn import_netscape_html(&mut self, html: &str, now: i64) -> usize {
        let mut folder_stack: Vec<String> = Vec::new();
        let mut pending_folder = String::new();
        let mut existing: HashSet<String> = self
            .bookmarks
            .iter()
            .map(|b| format!("{}\n{}", b.folder, b.url))
            .collect();
        let mut imported = 0;
        let mut pos = 0;
        let bytes = html.as_bytes();
        while pos < bytes.len() {
            let Some(rel_lt) = html[pos..].find('<') else {
                break;
            };
            let lt = pos + rel_lt;
            if html[lt..].starts_with("<!--") {
                let Some(rel_end) = html[lt + 4..].find("-->") else {
                    break;
                };
                pos = lt + 4 + rel_end + 3;
                continue;
            }
            let mut gt = lt + 1;
            let mut quote = 0u8;
            while gt < bytes.len() {
                let c = bytes[gt];
                if quote != 0 {
                    if c == quote {
                        quote = 0;
                    }
                } else if c == b'"' || c == b'\'' {
                    quote = c;
                } else if c == b'>' {
                    break;
                }
                gt += 1;
            }
            if gt == bytes.len() {
                break;
            }
            let tag = &html[lt + 1..gt];
            let name = tag
                .split([' ', '\t', '\r', '\n'])
                .next()
                .unwrap_or("")
                .to_ascii_lowercase();
            pos = gt + 1;
            if name == "h3" {
                let Some(close) = find_ci(html, "</h3>", pos) else {
                    break;
                };
                pending_folder =
                    clip_utf8(html_unescape(&html[pos..close]).trim(), 128).to_string();
                if attr_value(tag, "personal_toolbar_folder").eq_ignore_ascii_case("true") {
                    pending_folder = FAVORITES_FOLDER.to_string();
                }
                pos = close + 5;
            } else if name == "dl" {
                folder_stack.push(std::mem::take(&mut pending_folder));
            } else if name == "/dl" {
                folder_stack.pop();
            } else if name == "a" {
                let Some(close) = find_ci(html, "</a>", pos) else {
                    break;
                };
                let url = html_unescape(attr_value(tag, "href"));
                let title = html_unescape(&html[pos..close]).trim().to_string();
                pos = close + 4;
                if !Self::is_web_url(&url) || url.len() > MAX_URL {
                    continue;
                }
                let mut folder = folder_stack
                    .iter()
                    .rev()
                    .find(|s| !s.is_empty())
                    .cloned()
                    .unwrap_or_else(|| BOOKMARKS_FOLDER.to_string());
                let lower = folder.to_ascii_lowercase();
                if matches!(
                    lower.as_str(),
                    "favorites bar" | "bookmarks bar" | "favorites"
                ) || matches!(folder.as_str(), "收藏夹栏" | "书签栏" | "个人收藏")
                {
                    folder = FAVORITES_FOLDER.to_string();
                }
                self.add_folder(&folder);
                if !existing.insert(format!("{folder}\n{url}")) {
                    continue;
                }
                if !self.add_bookmark(&title, &url, &folder, now).is_empty() {
                    imported += 1;
                }
            }
        }
        imported
    }

    pub fn export_netscape_html(&self) -> String {
        let mut out = String::from(
            "<!DOCTYPE NETSCAPE-Bookmark-file-1>\n<META HTTP-EQUIV=\"Content-Type\" CONTENT=\"text/html; charset=UTF-8\">\n<TITLE>Bookmarks</TITLE>\n<H1>Bookmarks</H1>\n<DL><p>\n",
        );
        for folder in &self.folders {
            let fav = folder == FAVORITES_FOLDER;
            let label = if fav { "Favorites Bar" } else { folder.as_str() };
            out.push_str(&format!(
                "    <DT><H3{}>{}</H3>\n    <DL><p>\n",
                if fav {
                    " PERSONAL_TOOLBAR_FOLDER=\"true\""
                } else {
                    ""
                },
                html_escape(label)
            ));
            for b in &self.bookmarks {
                if b.folder == *folder {
                    out.push_str(&format!(
                        "        <DT><A HREF=\"{}\" ADD_DATE=\"{}\">{}</A>\n",
                        html_escape(&b.url),
                        b.added / 1000,
                        html_escape(&b.title)
                    ));
                }
            }
            out.push_str("    </DL><p>\n");
        }
        out.push_str("</DL><p>\n");
        out
    }

    // ---- Reading List ----
    pub fn reading_list(&self) -> &[ReadingItem] {
        &self.reading
    }

    pub fn add_reading(&mut self, title: &str, url: &str, now: i64) -> String {
        if url.is_empty() || url.len() > MAX_URL {
            return String::new();
        }
        if let Some(existing) = self.reading.iter_mut().find(|r| r.url == url) {
            existing.read = false;
            existing.added = now;
            self.dirty = true;
            return existing.id.clone();
        }
        let id = self.new_id(now);
        let t = title.trim();
        self.reading.insert(
            0,
            ReadingItem {
                id: id.clone(),
                title: clip_utf8(if t.is_empty() { url } else { t }, MAX_TITLE).to_string(),
                url: url.to_string(),
                added: now,
                read: false,
            },
        );
        self.dirty = true;
        id
    }

    pub fn remove_reading(&mut self, id: &str) -> bool {
        let Some(pos) = self.reading.iter().position(|r| r.id == id) else {
            return false;
        };
        self.reading.remove(pos);
        self.dirty = true;
        true
    }

    pub fn set_read(&mut self, id: &str, read: bool) -> bool {
        let Some(r) = self.reading.iter_mut().find(|r| r.id == id) else {
            return false;
        };
        if r.read != read {
            r.read = read;
            self.dirty = true;
        }
        true
    }

    pub fn mark_read_by_url(&mut self, url: &str) {
        for r in &mut self.reading {
            if r.url == url && !r.read {
                r.read = true;
                self.dirty = true;
            }
        }
    }

    // ---- In-memory History ----
    pub fn history_size(&self) -> usize {
        self.history.len()
    }

    fn rebuild_history_index(&mut self) {
        self.history_index.clear();
        for (i, e) in self.history.iter().enumerate() {
            self.history_index.insert(e.url.clone(), i);
        }
    }

    fn prune_history(&mut self) {
        self.history.sort_by_key(|a| std::cmp::Reverse(a.last));
        if let Some(newest) = self.history.first().map(|e| e.last) {
            self.history
                .retain(|e| newest - e.last <= HISTORY_RETENTION_MS);
        }
        self.history.truncate(MAX_HISTORY);
        self.rebuild_history_index();
    }

    pub fn record_visit(&mut self, url: &str, title: &str, now: i64) {
        if !Self::is_web_url(url) || url.len() > MAX_URL {
            return;
        }
        if let Some(&idx) = self.history_index.get(url) {
            let e = &mut self.history[idx];
            e.last = now;
            e.visits += 1;
            if !title.is_empty() {
                e.title = clip_utf8(title, MAX_TITLE).to_string();
            }
        } else {
            self.history_index
                .insert(url.to_string(), self.history.len());
            self.history.push(HistoryEntry {
                url: url.to_string(),
                title: clip_utf8(title, MAX_TITLE).to_string(),
                last: now,
                visits: 1,
            });
            if self.history.len() > MAX_HISTORY + MAX_HISTORY / 10 {
                self.prune_history();
            }
        }
    }

    pub fn query_history(&self, query: &str, limit: usize) -> Vec<HistoryEntry> {
        let q = query.trim().to_ascii_lowercase();
        let mut out: Vec<HistoryEntry> = self
            .history
            .iter()
            .filter(|e| {
                q.is_empty()
                    || e.title.to_ascii_lowercase().contains(&q)
                    || e.url.to_ascii_lowercase().contains(&q)
            })
            .cloned()
            .collect();
        out.sort_by_key(|a| std::cmp::Reverse(a.last));
        out.truncate(limit);
        out
    }

    pub fn remove_history(&mut self, url: &str) -> bool {
        let Some(&idx) = self.history_index.get(url) else {
            return false;
        };
        self.history.remove(idx);
        self.rebuild_history_index();
        true
    }

    pub fn clear_history(&mut self, since: i64) {
        if since <= 0 {
            self.history.clear();
        } else {
            self.history.retain(|e| e.last < since);
        }
        self.rebuild_history_index();
    }

    pub fn suggest(&self, query: &str, limit: usize) -> Vec<Suggestion> {
        let q = query.trim().to_ascii_lowercase();
        if q.is_empty() {
            return Vec::new();
        }
        let stripped = |url: &str| {
            let mut u = url.to_ascii_lowercase();
            for prefix in ["https://", "http://"] {
                if let Some(rest) = u.strip_prefix(prefix) {
                    u = rest.to_string();
                    break;
                }
            }
            if let Some(rest) = u.strip_prefix("www.") {
                rest.to_string()
            } else {
                u
            }
        };
        let score = |title: &str, url: &str, weight: f64| -> f64 {
            let u = stripped(url);
            let t = title.to_ascii_lowercase();
            let s = if u.starts_with(&q) {
                3.0
            } else if Self::host_of(url).contains(&q) {
                2.0
            } else if u.contains(&q) || t.contains(&q) {
                1.0
            } else {
                0.0
            };
            s * weight
        };

        let mut scored: Vec<(Suggestion, f64)> = Vec::new();
        let mut seen = HashSet::new();
        for b in &self.bookmarks {
            let s = score(&b.title, &b.url, 1.5);
            if s > 0.0 && seen.insert(b.url.clone()) {
                scored.push((
                    Suggestion {
                        title: b.title.clone(),
                        url: b.url.clone(),
                        bookmark: true,
                    },
                    s + 0.5,
                ));
            }
        }
        for e in &self.history {
            let mut s = score(&e.title, &e.url, 1.0);
            if s <= 0.0 || !seen.insert(e.url.clone()) {
                continue;
            }
            s += (e.visits as f64 / 20.0).min(1.0);
            scored.push((
                Suggestion {
                    title: e.title.clone(),
                    url: e.url.clone(),
                    bookmark: false,
                },
                s,
            ));
        }
        scored.sort_by(|a, b| b.1.total_cmp(&a.1));
        scored.into_iter().take(limit).map(|(s, _)| s).collect()
    }

    // ---- Tab groups ----
    pub fn groups(&self) -> &[TabGroup] {
        &self.groups
    }

    pub fn find_group(&self, id: &str) -> Option<&TabGroup> {
        self.groups.iter().find(|g| g.id == id)
    }

    pub fn save_group(&mut self, name: &str, tabs: &[SavedTab], now: i64) -> String {
        let filtered: Vec<SavedTab> = tabs
            .iter()
            .filter(|t| Self::is_web_url(&t.url))
            .map(|t| SavedTab {
                title: clip_utf8(&t.title, MAX_TITLE).to_string(),
                url: clip_utf8(&t.url, MAX_URL).to_string(),
            })
            .collect();
        if filtered.is_empty() {
            return String::new();
        }
        let id = self.new_id(now);
        let t = name.trim();
        self.groups.push(TabGroup {
            id: id.clone(),
            name: clip_utf8(if t.is_empty() { "标签组" } else { t }, MAX_TITLE).to_string(),
            tabs: filtered,
            updated: now,
        });
        self.dirty = true;
        id
    }

    pub fn rename_group(&mut self, id: &str, name: &str) -> bool {
        if name.trim().is_empty() {
            return false;
        }
        if let Some(g) = self.groups.iter_mut().find(|g| g.id == id) {
            g.name = clip_utf8(name.trim(), MAX_TITLE).to_string();
            self.dirty = true;
            true
        } else {
            false
        }
    }

    pub fn remove_group(&mut self, id: &str) -> bool {
        let Some(pos) = self.groups.iter().position(|g| g.id == id) else {
            return false;
        };
        self.groups.remove(pos);
        self.dirty = true;
        true
    }

    // ---- In-memory Per-Site Settings ----
    pub fn site(&self, host: &str) -> SiteSettings {
        self.sites.get(host).cloned().unwrap_or_default()
    }

    pub fn set_site(&mut self, host: &str, settings: SiteSettings) {
        if host.is_empty() {
            return;
        }
        if settings.is_default() {
            self.sites.remove(host);
        } else {
            self.sites.insert(host.to_string(), settings);
        }
    }

    pub fn clear_site(&mut self, host: &str) {
        self.sites.remove(host);
    }

    pub fn clear_sites(&mut self) {
        self.sites.clear();
    }

    pub fn site_count(&self) -> usize {
        self.sites.len()
    }

    pub fn hosts_with_adblock_off(&self) -> Vec<String> {
        self.sites
            .iter()
            .filter(|(_, s)| !s.adblock)
            .map(|(h, _)| h.clone())
            .collect()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn netscape_import_edge_cases() {
        let cases = [
            ("<A HREF = 'https://space.example/'>Space</A>", 1, "https://space.example/", BOOKMARKS_FOLDER),
            ("<A DATA-HREF=\"https://wrong.example/\" HREF=\"https://right.example/\">Right</A>", 1, "https://right.example/", BOOKMARKS_FOLDER),
            ("<A TITLE=\"href=https://wrong.example/\" HREF=\"https://right.example/\">Right</A>", 1, "https://right.example/", BOOKMARKS_FOLDER),
            ("<A TITLE=\"a > b\" HREF=\"https://right.example/\">Right</A>", 1, "https://right.example/", BOOKMARKS_FOLDER),
            ("<!-- <A HREF=\"https://wrong.example/\">old</A> --><A HREF=\"https://right.example/\">Right</A>", 1, "https://right.example/", BOOKMARKS_FOLDER),
            ("<A HREF=https://tab.example/\tADD_DATE=1>Tab</A>", 1, "https://tab.example/", BOOKMARKS_FOLDER),
            ("<A DISABLED HrEf=\"https://case.example/\">Case</A>", 1, "https://case.example/", BOOKMARKS_FOLDER),
            ("<A DATA-HREF=\"https://wrong.example/\">Not a link</A>", 0, "", BOOKMARKS_FOLDER),
            ("<A HREF=\"https://bad.example/>Bad</A>", 0, "", BOOKMARKS_FOLDER),
            ("<H3 PERSONAL_TOOLBAR_FOLDER=\"true\">Meine Favoriten</H3><DL><A HREF=\"https://bar.example/\">Bar</A></DL>", 1, "https://bar.example/", FAVORITES_FOLDER),
            ("<H3 PERSONAL_TOOLBAR_FOLDER=\"false\">Meine Favoriten</H3><DL><A HREF=\"https://folder.example/\">Folder</A></DL>", 1, "https://folder.example/", "Meine Favoriten"),
        ];
        for (html, count, url, folder) in cases {
            let mut lib = Library::new("/tmp/unused-ulb");
            assert_eq!(lib.import_netscape_html(html, 1), count, "case: {html}");
            if count > 0 {
                assert_eq!(lib.bookmarks()[0].url, url);
                assert_eq!(lib.bookmarks()[0].folder, folder);
            }
        }
    }

    #[test]
    fn library_bookmarks_history_sites_and_privacy() {
        let dir = std::env::temp_dir().join("ulb-core-library-test");
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        fs::write(dir.join("history.json"), "[]").unwrap();
        fs::write(dir.join("session.json"), "[]").unwrap();

        let mut lib = Library::new(&dir);
        lib.load();
        assert_eq!(lib.history_size(), 0);
        assert!(!dir.join("history.json").exists());
        assert!(!dir.join("session.json").exists());

        let a = lib.add_bookmark("GitHub", "https://github.com/", FAVORITES_FOLDER, 1000);
        let b = lib.add_bookmark("", "https://example.com/x", "missing", 1001);
        assert!(lib.add_folder("新闻") && !lib.add_folder("新闻") && !lib.remove_folder(FAVORITES_FOLDER));
        assert!(lib.update_bookmark(&b, "Example", "", "新闻"));
        assert!(lib.rename_folder("新闻", "资讯") && lib.bookmarks_in("资讯").len() == 1);
        assert!(lib.remove_folder("资讯") && lib.bookmarks_in(BOOKMARKS_FOLDER).len() == 1);
        assert!(lib.move_bookmark(&b, &a) && lib.bookmarks()[0].id == b && lib.bookmarks()[0].folder == FAVORITES_FOLDER);

        lib.record_visit("https://news.example.com/a", "A", 10);
        lib.record_visit("https://news.example.com/a", "A2", 20);
        lib.record_visit("https://news.example.com/b", "B", 30);
        lib.record_visit("https://other.org/", "Other", 40);
        lib.record_visit("javascript:alert(1)", "x", 50);
        assert_eq!(lib.history_size(), 3);
        assert_eq!(lib.query_history("a2", 10).len(), 1);
        lib.clear_history(35);
        assert_eq!(lib.history_size(), 2);

        let sug = lib.suggest("git", 5);
        assert_eq!(sug[0].url, "https://github.com/");
        assert!(sug[0].bookmark);

        let r = lib.add_reading("Long read", "https://example.com/read", 5);
        lib.mark_read_by_url("https://example.com/read");
        assert!(lib.reading_list()[0].read);
        assert!(lib.set_read(&r, false) && lib.remove_reading(&r));

        let g = lib.save_group(
            "工作",
            &[
                SavedTab {
                    title: "A".into(),
                    url: "https://a.com/".into(),
                },
                SavedTab {
                    title: "blank".into(),
                    url: "about:blank".into(),
                },
            ],
            7,
        );
        assert_eq!(lib.find_group(&g).unwrap().tabs.len(), 1);

        lib.set_site(
            "news.example.org",
            SiteSettings {
                adblock: false,
                ..Default::default()
            },
        );
        assert_eq!(lib.hosts_with_adblock_off(), vec!["news.example.org"]);
        assert!(lib.save());
        assert!(!lib.is_dirty());

        let mut reloaded = Library::new(&dir);
        reloaded.load();
        assert_eq!(reloaded.bookmarks().len(), 2);
        assert_eq!(reloaded.history_size(), 0);
        assert!(reloaded.site("news.example.org").is_default());

        let exported = reloaded.export_netscape_html();
        let mut copy = Library::new(dir.join("copy"));
        assert_eq!(copy.import_netscape_html(&exported, 1), 2);
        assert_eq!(Library::host_of("https://user:pw@Sub.Example.COM:8080/x?y"), "sub.example.com");
        assert_eq!(Library::host_of("http://[::1]:80/"), "[::1]");

        let _ = fs::remove_dir_all(&dir);
    }
}
