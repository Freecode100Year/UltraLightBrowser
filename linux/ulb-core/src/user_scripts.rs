use crate::util::write_file_atomic;
use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::fs;
use std::path::{Path, PathBuf};

const MAX_SCRIPTS: usize = 200;
const MAX_SIZE: usize = 2 * 1024 * 1024;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct UserScript {
    pub id: String,
    pub name: String,
    pub ns: String,
    pub version: String,
    pub description: String,
    pub matches: Vec<String>,
    pub includes: Vec<String>,
    pub excludes: Vec<String>,
    pub run_at: String,
    pub noframes: bool,
    pub enabled: bool,
    pub ignored: Vec<String>,
    pub code: String,
}

impl Default for UserScript {
    fn default() -> Self {
        Self {
            id: String::new(),
            name: String::new(),
            ns: String::new(),
            version: String::new(),
            description: String::new(),
            matches: Vec::new(),
            includes: Vec::new(),
            excludes: Vec::new(),
            run_at: "document-idle".to_string(),
            noframes: false,
            enabled: true,
            ignored: Vec::new(),
            code: String::new(),
        }
    }
}

const WRAPPER: &str = r#"(function () {
var C = %CONFIG%;
try {
  if (!/^(https?|file):$/.test(location.protocol) || /\.internal$/i.test(location.hostname)) return;
  if (C.noframes && window.top !== window) return;
  var href = location.href;
  var hit = function (list) { for (var i = 0; i < list.length; i++) { try { if (new RegExp(list[i]).test(href)) return true; } catch (e) {} } return false; };
  if (!hit(C.inc) || hit(C.exc)) return;
} catch (e) { return; }
var P = "__ulbgm_" + C.id + "_";
var store = (function () { try { return window.localStorage; } catch (e) { return null; } })();
var GM_info = { script: C.info, scriptHandler: "UltraLightBrowser", version: C.version };
var GM_getValue = function (k, d) { try { var v = store && store.getItem(P + k); return v === null || v === undefined ? d : JSON.parse(v); } catch (e) { return d; } };
var GM_setValue = function (k, v) { try { if (store) store.setItem(P + k, JSON.stringify(v)); } catch (e) {} };
var GM_deleteValue = function (k) { try { if (store) store.removeItem(P + k); } catch (e) {} };
var GM_listValues = function () { var out = []; try { for (var i = 0; store && i < store.length; i++) { var k = store.key(i); if (k.indexOf(P) === 0) out.push(k.slice(P.length)); } } catch (e) {} return out; };
var GM_addStyle = function (css) { var s = document.createElement("style"); s.textContent = css; (document.head || document.documentElement).appendChild(s); return s; };
var GM_addElement = function (parent, tag, attrs) {
  if (typeof parent === "string") { attrs = tag; tag = parent; parent = document.head || document.documentElement; }
  var n = document.createElement(tag);
  for (var k in attrs || {}) { if (k === "textContent") n.textContent = attrs[k]; else n.setAttribute(k, attrs[k]); }
  parent.appendChild(n); return n;
};
var GM_openInTab = function (url) { return window.open(url, "_blank"); };
var GM_setClipboard = function (text) { try { navigator.clipboard.writeText(String(text)); } catch (e) {} };
var GM_notification = function (d) { try { console.info("[" + C.info.name + "] " + (typeof d === "string" ? d : (d && (d.text || d.title)) || "")); } catch (e) {} };
var GM_registerMenuCommand = function () { return 0; };
var GM_unregisterMenuCommand = function () {};
var GM_log = function () { try { console.log.apply(console, arguments); } catch (e) {} };
var GM_xmlhttpRequest = function (d) {
  var ctrl = new AbortController();
  var timer = d.timeout ? setTimeout(function () { ctrl.abort(); if (d.ontimeout) d.ontimeout({}); }, d.timeout) : 0;
  fetch(d.url, { method: d.method || "GET", headers: d.headers || {}, body: d.data, signal: ctrl.signal, credentials: d.anonymous ? "omit" : "include" })
    .then(function (r) {
      var h = ""; r.headers.forEach(function (v, k) { h += k + ": " + v + "\r\n"; });
      var t = d.responseType;
      return (t === "json" ? r.json() : t === "blob" ? r.blob() : t === "arraybuffer" ? r.arrayBuffer() : r.text()).then(function (b) {
        clearTimeout(timer);
        var res = { status: r.status, statusText: r.statusText, finalUrl: r.url, responseHeaders: h, response: b, responseText: typeof b === "string" ? b : "", readyState: 4 };
        if (d.onload) d.onload(res); if (d.onloadend) d.onloadend(res);
      });
    })
    .catch(function (e) { clearTimeout(timer); var res = { status: 0, error: String(e), readyState: 4 }; if (d.onerror) d.onerror(res); if (d.onloadend) d.onloadend(res); });
  return { abort: function () { ctrl.abort(); } };
};
var unsafeWindow = window;
var GM = {
  info: GM_info,
  getValue: function (k, d) { return Promise.resolve(GM_getValue(k, d)); },
  setValue: function (k, v) { return Promise.resolve(GM_setValue(k, v)); },
  deleteValue: function (k) { return Promise.resolve(GM_deleteValue(k)); },
  listValues: function () { return Promise.resolve(GM_listValues()); },
  addStyle: function (c) { return Promise.resolve(GM_addStyle(c)); },
  addElement: GM_addElement,
  openInTab: GM_openInTab,
  setClipboard: function (t) { return Promise.resolve(GM_setClipboard(t)); },
  notification: GM_notification,
  registerMenuCommand: GM_registerMenuCommand,
  xmlHttpRequest: GM_xmlhttpRequest
};
var run = function () {
  try {
    (function (GM_info, GM_getValue, GM_setValue, GM_deleteValue, GM_listValues, GM_addStyle, GM_addElement, GM_openInTab,
               GM_setClipboard, GM_notification, GM_registerMenuCommand, GM_unregisterMenuCommand, GM_log, GM_xmlhttpRequest,
               unsafeWindow, GM) {
%CODE%
    }).call(window, GM_info, GM_getValue, GM_setValue, GM_deleteValue, GM_listValues, GM_addStyle, GM_addElement, GM_openInTab,
            GM_setClipboard, GM_notification, GM_registerMenuCommand, GM_unregisterMenuCommand, GM_log, GM_xmlhttpRequest,
            unsafeWindow, GM);
  } catch (e) { try { console.error("[UltraLightBrowser 用户脚本] " + C.info.name + ":", e); } catch (x) {} }
};
if (C.runAt === "document-start") run();
else if (C.runAt === "document-body") {
  if (document.body) run();
  else new MutationObserver(function (m, o) { if (document.body) { o.disconnect(); run(); } }).observe(document.documentElement, { childList: true });
} else if (document.readyState === "loading") {
  document.addEventListener("DOMContentLoaded", function () { if (C.runAt === "document-idle") setTimeout(run, 0); else run(); }, { once: true });
} else if (C.runAt === "document-idle") setTimeout(run, 0);
else run();
})();
"#;

fn regex_escape(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    for ch in s.chars() {
        if "\\^$.|?*+()[]{}/".contains(ch) {
            out.push('\\');
        }
        out.push(ch);
    }
    out
}

fn glob_body(glob: &str) -> String {
    let mut out = String::new();
    for ch in glob.chars() {
        if ch == '*' {
            out.push_str(".*");
        } else {
            out.push_str(&regex_escape(&ch.to_string()));
        }
    }
    out
}

fn valid_id(id: &str) -> bool {
    !id.is_empty() && id.len() <= 16 && id.bytes().all(|b| b.is_ascii_alphanumeric())
}

pub fn match_to_regex(raw: &str) -> String {
    let p = raw.trim();
    if p == "<all_urls>" {
        return "^(https?|file)://".to_string();
    }
    let Some((scheme, rest)) = p.split_once("://") else {
        return String::new();
    };
    let scheme_re = match scheme {
        "*" => "https?",
        "http" | "https" | "file" => scheme,
        _ => return String::new(),
    };
    let Some(slash) = rest.find('/') else {
        return String::new();
    };
    let host = &rest[..slash];
    let path = &rest[slash..];
    let host_re = if host == "*" {
        "[^/]*".to_string()
    } else if let Some(base) = host.strip_prefix("*.") {
        if base.is_empty() || base.contains('*') {
            return String::new();
        }
        format!("([^/]*\\.)?{}", regex_escape(base))
    } else {
        if host.contains('*') || (host.is_empty() && scheme != "file") {
            return String::new();
        }
        regex_escape(host)
    };
    format!("^{scheme_re}://{host_re}(:\\d+)?{}$", glob_body(path))
}

pub fn include_to_regex(raw: &str) -> String {
    let p = raw.trim();
    if p.len() >= 2 && p.starts_with('/') && p.ends_with('/') {
        return p[1..p.len() - 1].to_string();
    }
    if p.is_empty() {
        return String::new();
    }
    if p == "*" {
        return "^(https?|file)://".to_string();
    }
    format!("^{}$", glob_body(p))
}

pub fn parse_meta(source: &str) -> Option<Result<UserScript, String>> {
    let begin = source.find("==UserScript==")?;
    let Some(rel_end) = source[begin..].find("==/UserScript==") else {
        return Some(Err("==UserScript== 没有结束标记".to_string()));
    };
    let block = &source[begin..begin + rel_end];
    let mut out = UserScript::default();
    for raw_line in block.lines() {
        let line = raw_line.trim();
        let Some(rest) = line.strip_prefix("//") else {
            continue;
        };
        let rest = rest.trim();
        if !rest.starts_with('@') {
            continue;
        }
        let (key, value) = match rest.find([' ', '\t']) {
            Some(sp) => (&rest[..sp], rest[sp..].trim()),
            None => (rest, ""),
        };
        match key {
            "@name" if out.name.is_empty() => out.name = value.to_string(),
            "@name:zh-CN" | "@name:zh" => out.name = value.to_string(),
            "@namespace" => out.ns = value.to_string(),
            "@version" => out.version = value.to_string(),
            "@description" if out.description.is_empty() => out.description = value.to_string(),
            "@description:zh-CN" => out.description = value.to_string(),
            "@match" => out.matches.push(value.to_string()),
            "@include" => out.includes.push(value.to_string()),
            "@exclude" => out.excludes.push(value.to_string()),
            "@exclude-match" => out.excludes.push(format!("match:{value}")),
            "@run-at" => out.run_at = value.to_string(),
            "@noframes" => out.noframes = true,
            "@require" | "@resource" if !out.ignored.iter().any(|k| k == key) => {
                out.ignored.push(key.to_string());
            }
            _ => {}
        }
    }
    if !matches!(
        out.run_at.as_str(),
        "document-start" | "document-end" | "document-idle" | "document-body"
    ) {
        out.run_at = "document-idle".to_string();
    }
    if out.matches.is_empty() && out.includes.is_empty() {
        return Some(Err("脚本没有 @match 或 @include，不知道要在哪些网站运行".to_string()));
    }
    for m in &out.matches {
        if match_to_regex(m).is_empty() {
            return Some(Err(format!("无法识别的 @match：{m}")));
        }
    }
    Some(Ok(out))
}

fn ascii_safe_json(v: &Value) -> String {
    let raw = serde_json::to_string(v).unwrap_or_else(|_| "{}".to_string());
    let mut out = String::with_capacity(raw.len());
    for ch in raw.chars() {
        if (ch as u32) < 0x80 {
            out.push(ch);
        } else {
            for unit in ch.encode_utf16(&mut [0; 2]) {
                out.push_str(&format!("\\u{unit:04x}"));
            }
        }
    }
    out
}

#[derive(Serialize, Deserialize)]
struct IndexEntry {
    id: String,
    name: String,
    enabled: bool,
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    matches: Vec<String>,
}

pub struct UserScriptStore {
    dir: PathBuf,
    scripts: Vec<UserScript>,
    app_version: String,
}

impl UserScriptStore {
    pub fn new(dir: impl AsRef<Path>, app_version: impl Into<String>) -> Self {
        Self {
            dir: dir.as_ref().to_path_buf(),
            scripts: Vec::new(),
            app_version: app_version.into(),
        }
    }

    pub fn scripts(&self) -> &[UserScript] {
        &self.scripts
    }

    fn next_id(&self) -> String {
        for n in 1.. {
            let id = format!("u{n}");
            if self.scripts.iter().all(|s| s.id != id) {
                return id;
            }
        }
        unreachable!()
    }

    pub fn install(
        &mut self,
        source: &str,
        fallback_name: &str,
        default_match: &str,
    ) -> Result<String, String> {
        if source.len() > MAX_SIZE {
            return Err("脚本超过 2 MB".to_string());
        }
        if source.trim().is_empty() {
            return Err("脚本是空的".to_string());
        }
        let mut s = match parse_meta(source) {
            Some(Ok(parsed)) => parsed,
            Some(Err(e)) => return Err(e),
            None => {
                if match_to_regex(default_match).is_empty() {
                    return Err("请填写有效的网址规则，例如 https://example.com/*".to_string());
                }
                UserScript {
                    matches: vec![default_match.trim().to_string()],
                    name: fallback_name.to_string(),
                    ..Default::default()
                }
            }
        };
        if s.name.trim().is_empty() {
            s.name = if fallback_name.trim().is_empty() {
                "未命名脚本".to_string()
            } else {
                fallback_name.to_string()
            };
        }
        s.code = source.to_string();

        if let Some(existing) = self
            .scripts
            .iter_mut()
            .find(|e| e.name == s.name && e.ns == s.ns)
        {
            s.id = existing.id.clone();
            s.enabled = existing.enabled;
            *existing = s;
            return Ok(existing.id.clone());
        }
        if self.scripts.len() >= MAX_SCRIPTS {
            return Err("脚本太多了（最多 200 个）".to_string());
        }
        s.id = self.next_id();
        let id = s.id.clone();
        self.scripts.push(s);
        Ok(id)
    }

    pub fn remove(&mut self, id: &str) -> bool {
        let Some(pos) = self.scripts.iter().position(|s| s.id == id) else {
            return false;
        };
        self.scripts.remove(pos);
        true
    }

    pub fn set_enabled(&mut self, id: &str, enabled: bool) -> bool {
        if let Some(s) = self.scripts.iter_mut().find(|s| s.id == id) {
            s.enabled = enabled;
            true
        } else {
            false
        }
    }

    pub fn injection_scripts(&self) -> Vec<String> {
        let mut out = Vec::new();
        for s in &self.scripts {
            if !s.enabled {
                continue;
            }
            let mut inc = Vec::new();
            for m in &s.matches {
                let r = match_to_regex(m);
                if !r.is_empty() {
                    inc.push(r);
                }
            }
            for i in &s.includes {
                let r = include_to_regex(i);
                if !r.is_empty() {
                    inc.push(r);
                }
            }
            if inc.is_empty() {
                continue;
            }
            let mut exc = Vec::new();
            for e in &s.excludes {
                let r = if let Some(pat) = e.strip_prefix("match:") {
                    match_to_regex(pat)
                } else {
                    include_to_regex(e)
                };
                if !r.is_empty() {
                    exc.push(r);
                }
            }
            let cfg = serde_json::json!({
                "id": s.id,
                "inc": inc,
                "exc": exc,
                "runAt": s.run_at,
                "noframes": s.noframes,
                "version": self.app_version,
                "info": {
                    "name": s.name,
                    "namespace": s.ns,
                    "version": s.version,
                    "description": s.description,
                }
            });
            let cfg_at = WRAPPER.find("%CONFIG%").unwrap();
            let code_at = WRAPPER.find("%CODE%").unwrap();
            let mut js = String::with_capacity(WRAPPER.len() + s.code.len() + 512);
            js.push_str(&WRAPPER[..cfg_at]);
            js.push_str(&ascii_safe_json(&cfg));
            js.push_str(&WRAPPER[cfg_at + 8..code_at]);
            js.push_str(&s.code);
            js.push_str(&WRAPPER[code_at + 6..]);
            out.push(js);
        }
        out
    }

    pub fn load(&mut self) {
        self.scripts.clear();
        let Ok(raw) = fs::read_to_string(self.dir.join("index.json")) else {
            return;
        };
        let Ok(Value::Object(root)) = serde_json::from_str::<Value>(&raw) else {
            return;
        };
        let Some(Value::Array(list)) = root.get("scripts") else {
            return;
        };
        for item in list {
            let Some(id) = item.get("id").and_then(Value::as_str) else {
                continue;
            };
            if !valid_id(id) {
                continue;
            }
            let Ok(source) = fs::read_to_string(self.dir.join(format!("{id}.user.js"))) else {
                continue;
            };
            let mut s = match parse_meta(&source) {
                Some(Ok(parsed)) => parsed,
                Some(Err(_)) => UserScript::default(),
                None => {
                    let matches = item
                        .get("matches")
                        .and_then(Value::as_array)
                        .map(|arr| {
                            arr.iter()
                                .filter_map(Value::as_str)
                                .map(str::to_string)
                                .collect()
                        })
                        .unwrap_or_default();
                    UserScript {
                        matches,
                        ..Default::default()
                    }
                }
            };
            if let Some(name) = item.get("name").and_then(Value::as_str) {
                s.name = name.to_string();
            }
            s.id = id.to_string();
            s.enabled = item.get("enabled").and_then(Value::as_bool).unwrap_or(true);
            s.code = source;
            self.scripts.push(s);
            if self.scripts.len() >= MAX_SCRIPTS {
                break;
            }
        }
    }

    pub fn save(&self) -> std::io::Result<()> {
        fs::create_dir_all(&self.dir)?;
        let mut entries = Vec::with_capacity(self.scripts.len());
        for s in &self.scripts {
            fs::write(self.dir.join(format!("{}.user.js", s.id)), s.code.as_bytes())?;
            let plain_js = parse_meta(&s.code).is_none();
            entries.push(IndexEntry {
                id: s.id.clone(),
                name: s.name.clone(),
                enabled: s.enabled,
                matches: if plain_js {
                    s.matches.clone()
                } else {
                    Vec::new()
                },
            });
        }
        if let Ok(read_dir) = fs::read_dir(&self.dir) {
            for ent in read_dir.flatten() {
                let fname = ent.file_name();
                let name = fname.to_string_lossy();
                if let Some(id) = name.strip_suffix(".user.js") {
                    if self.scripts.iter().all(|s| s.id != id) {
                        let _ = fs::remove_file(ent.path());
                    }
                }
            }
        }
        let body = serde_json::to_string_pretty(&serde_json::json!({ "scripts": entries }))
            .map_err(std::io::Error::other)?;
        write_file_atomic(&self.dir.join("index.json"), body.as_bytes())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use regex::Regex;

    fn re_matches(pattern: &str, url: &str) -> bool {
        !pattern.is_empty() && Regex::new(pattern).unwrap().is_match(url)
    }

    #[test]
    fn user_script_rules_meta_and_persistence() {
        assert!(re_matches(&match_to_regex("*://*.example.com/*"), "https://example.com/"));
        assert!(re_matches(&match_to_regex("*://*.example.com/*"), "http://a.b.example.com/x?y"));
        assert!(!re_matches(&match_to_regex("*://*.example.com/*"), "https://badexample.com/"));
        assert!(!re_matches(&match_to_regex("*://*.example.com/*"), "https://example.com.evil.net/"));
        assert!(re_matches(&match_to_regex("https://example.com/path/*"), "https://example.com:8443/path/a"));
        assert!(!re_matches(&match_to_regex("https://example.com/path/*"), "http://example.com/path/a"));
        assert!(re_matches(&match_to_regex("<all_urls>"), "http://x/"));
        assert!(match_to_regex("ftp://x/*").is_empty());
        assert!(match_to_regex("https://ex*ample.com/*").is_empty());
        assert!(!re_matches(&include_to_regex("http://a.test/?q=1"), "http://a.test/xq=1"));

        let meta = "// ==UserScript==\n// @name Hello\n// @name:zh-CN 你好\n// @namespace https://example.com/ns\n// @version 1.2\n// @match *://*.example.com/*\n// @exclude *://example.com/private/*\n// @run-at document-start\n// @noframes\n// @require https://cdn.example.com/lib.js\n// ==/UserScript==\nconsole.log('hi');";
        let s = parse_meta(meta).unwrap().unwrap();
        assert_eq!(s.name, "你好");
        assert_eq!(s.run_at, "document-start");
        assert!(s.noframes);
        assert_eq!(s.ignored, vec!["@require"]);
        assert!(parse_meta("alert(1)").is_none());
        assert!(parse_meta("// ==UserScript==\n// @name x\n// ==/UserScript==\n")
            .unwrap()
            .is_err());

        let dir = std::env::temp_dir().join("ulb-core-userscripts-test");
        let _ = fs::remove_dir_all(&dir);
        let mut store = UserScriptStore::new(&dir, "2.10.1");
        let a = store.install(meta, "file.user.js", "").unwrap();
        let b = store
            .install("document.title = 'x';", "plain.js", "https://plain.example/*")
            .unwrap();
        assert!(store.set_enabled(&a, false));
        let v2 = meta.replace("1.2", "1.3");
        assert_eq!(store.install(&v2, "", "").unwrap(), a);
        assert_eq!(store.scripts()[0].version, "1.3");
        assert!(!store.scripts()[0].enabled);
        assert!(store.install("x()", "bad.js", "not a rule").is_err());
        assert_eq!(store.injection_scripts().len(), 1);
        store.save().unwrap();

        let mut again = UserScriptStore::new(&dir, "2.10.1");
        again.load();
        assert_eq!(again.scripts().len(), 2);
        assert_eq!(again.injection_scripts(), store.injection_scripts());
        assert!(again.remove(&b));
        again.save().unwrap();
        assert!(!dir.join(format!("{b}.user.js")).exists());
        let _ = fs::remove_dir_all(&dir);
    }
}
