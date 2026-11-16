#include "UserScripts.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string_view>

#ifndef ULB_VERSION
#define ULB_VERSION "dev"
#endif

namespace UltraLight {

using json = nlohmann::json;

namespace {

std::string Trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    return s.substr(i);
}

// Escapes everything that is special in an ECMAScript regex.
std::string RegexEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (std::string_view("\\^$.|?*+()[]{}/").find(c) != std::string_view::npos) out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

// Glob: * is any text, everything else literal.
std::string GlobBody(const std::string& glob) {
    std::string out;
    for (char c : glob) {
        if (c == '*') out += ".*";
        else out += RegexEscape(std::string(1, c));
    }
    return out;
}

bool ValidId(const std::string& id) {
    if (id.empty() || id.size() > 16) return false;
    return std::all_of(id.begin(), id.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)); });
}

// The wrapper around one script. %CONFIG% and %CODE% are filled in; the code
// goes inside a function so a script with a syntax error only breaks itself.
constexpr const char* kWrapper = R"JS((function () {
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
)JS";

} // namespace

std::string UserScriptStore::MatchToRegex(const std::string& raw) {
    const std::string p = Trim(raw);
    if (p == "<all_urls>") return "^(https?|file)://";
    const size_t sep = p.find("://");
    if (sep == std::string::npos) return {};
    const std::string scheme = p.substr(0, sep);
    std::string rest = p.substr(sep + 3);
    std::string schemeRe;
    if (scheme == "*") schemeRe = "https?";
    else if (scheme == "http" || scheme == "https" || scheme == "file") schemeRe = scheme;
    else return {};
    const size_t slash = rest.find('/');
    if (slash == std::string::npos) return {};
    const std::string host = rest.substr(0, slash);
    const std::string path = rest.substr(slash);
    std::string hostRe;
    if (host == "*") {
        hostRe = "[^/]*";
    } else if (host.rfind("*.", 0) == 0) {
        const std::string base = host.substr(2);
        if (base.empty() || base.find('*') != std::string::npos) return {};
        hostRe = "([^/]*\\.)?" + RegexEscape(base);
    } else {
        if (host.find('*') != std::string::npos || (host.empty() && scheme != "file")) return {};
        hostRe = RegexEscape(host);
    }
    // The port is not part of a match pattern: any port matches.
    return "^" + schemeRe + "://" + hostRe + "(:\\d+)?" + GlobBody(path) + "$";
}

std::string UserScriptStore::IncludeToRegex(const std::string& raw) {
    const std::string p = Trim(raw);
    if (p.size() >= 2 && p.front() == '/' && p.back() == '/') return p.substr(1, p.size() - 2);
    if (p.empty()) return {};
    if (p == "*") return "^(https?|file)://";
    return "^" + GlobBody(p) + "$";
}

bool UserScriptStore::ParseMeta(const std::string& source, UserScript& out, std::string& error) {
    const size_t begin = source.find("==UserScript==");
    if (begin == std::string::npos) return false;
    const size_t end = source.find("==/UserScript==", begin);
    if (end == std::string::npos) {
        error = "==UserScript== 没有结束标记";
        return true;
    }
    std::istringstream in(source.substr(begin, end - begin));
    std::string line;
    while (std::getline(in, line)) {
        line = Trim(line);
        if (line.rfind("//", 0) != 0) continue;
        line = Trim(line.substr(2));
        if (line.empty() || line[0] != '@') continue;
        const size_t sp = line.find_first_of(" \t");
        const std::string key = line.substr(0, sp);
        const std::string value = sp == std::string::npos ? std::string() : Trim(line.substr(sp));
        if (key == "@name") { if (out.name.empty()) out.name = value; }
        else if (key.rfind("@name:", 0) == 0) { if (key == "@name:zh-CN" || key == "@name:zh") out.name = value; }
        else if (key == "@namespace") out.ns = value;
        else if (key == "@version") out.version = value;
        else if (key == "@description") { if (out.description.empty()) out.description = value; }
        else if (key == "@description:zh-CN") out.description = value;
        else if (key == "@match") out.matches.push_back(value);
        else if (key == "@include") out.includes.push_back(value);
        else if (key == "@exclude" || key == "@exclude-match") out.excludes.push_back(key == "@exclude-match" ? "match:" + value : value);
        else if (key == "@run-at") out.runAt = value;
        else if (key == "@noframes") out.noframes = true;
        else if (key == "@require" || key == "@resource") {
            if (std::find(out.ignored.begin(), out.ignored.end(), key) == out.ignored.end()) out.ignored.push_back(key);
        }
    }
    if (out.runAt != "document-start" && out.runAt != "document-end" && out.runAt != "document-idle" && out.runAt != "document-body") {
        out.runAt = "document-idle";
    }
    if (out.matches.empty() && out.includes.empty()) error = "脚本没有 @match 或 @include，不知道要在哪些网站运行";
    for (const auto& m : out.matches) if (MatchToRegex(m).empty()) error = "无法识别的 @match：" + m;
    return true;
}

std::string UserScriptStore::NextId() const {
    for (int n = 1;; ++n) {
        const std::string id = "u" + std::to_string(n);
        if (std::none_of(m_scripts.begin(), m_scripts.end(), [&](const UserScript& s) { return s.id == id; })) return id;
    }
}

std::string UserScriptStore::Install(const std::string& source, const std::string& fallbackName, const std::string& defaultMatch) {
    if (source.size() > kMaxSize) throw std::runtime_error("脚本超过 2 MB");
    if (Trim(source).empty()) throw std::runtime_error("脚本是空的");
    UserScript s;
    std::string error;
    if (!ParseMeta(source, s, error)) {
        if (MatchToRegex(defaultMatch).empty()) throw std::runtime_error("请填写有效的网址规则，例如 https://example.com/*");
        s.matches.push_back(Trim(defaultMatch));
        s.name = fallbackName;
    } else if (!error.empty()) {
        throw std::runtime_error(error);
    }
    if (Trim(s.name).empty()) s.name = Trim(fallbackName).empty() ? "未命名脚本" : fallbackName;
    s.code = source;
    for (auto& existing : m_scripts) {
        if (existing.name == s.name && existing.ns == s.ns) {  // same script: update in place
            s.id = existing.id;
            s.enabled = existing.enabled;
            existing = std::move(s);
            return existing.id;
        }
    }
    if (m_scripts.size() >= kMaxScripts) throw std::runtime_error("脚本太多了（最多 200 个）");
    s.id = NextId();
    m_scripts.push_back(std::move(s));
    return m_scripts.back().id;
}

bool UserScriptStore::Remove(const std::string& id) {
    const auto it = std::find_if(m_scripts.begin(), m_scripts.end(), [&](const UserScript& s) { return s.id == id; });
    if (it == m_scripts.end()) return false;
    m_scripts.erase(it);
    return true;
}

bool UserScriptStore::SetEnabled(const std::string& id, bool enabled) {
    for (auto& s : m_scripts) {
        if (s.id == id) {
            s.enabled = enabled;
            return true;
        }
    }
    return false;
}

std::vector<std::string> UserScriptStore::InjectionScripts() const {
    std::vector<std::string> out;
    for (const auto& s : m_scripts) {
        if (!s.enabled) continue;
        json inc = json::array(), exc = json::array();
        for (const auto& m : s.matches) if (auto r = MatchToRegex(m); !r.empty()) inc.push_back(r);
        for (const auto& i : s.includes) if (auto r = IncludeToRegex(i); !r.empty()) inc.push_back(r);
        for (const auto& e : s.excludes) {
            const std::string r = e.rfind("match:", 0) == 0 ? MatchToRegex(e.substr(6)) : IncludeToRegex(e);
            if (!r.empty()) exc.push_back(r);
        }
        if (inc.empty()) continue;
        const json config = {{"id", s.id}, {"inc", inc}, {"exc", exc}, {"runAt", s.runAt}, {"noframes", s.noframes},
                             {"version", ULB_VERSION},
                             {"info", {{"name", s.name}, {"namespace", s.ns}, {"version", s.version}, {"description", s.description}}}};
        // Cut at the markers rather than search-and-replace: the name or the code
        // could contain the marker text.
        const std::string_view w = kWrapper;
        const size_t cfgAt = w.find("%CONFIG%"), codeAt = w.find("%CODE%");
        std::string js;
        js.reserve(w.size() + s.code.size() + 512);
        js.append(w.substr(0, cfgAt));
        js.append(config.dump(-1, ' ', true, json::error_handler_t::replace));  // ASCII only: safe anywhere in JS
        js.append(w.substr(cfgAt + 8, codeAt - cfgAt - 8));
        js.append(s.code);
        js.append(w.substr(codeAt + 6));
        out.push_back(std::move(js));
    }
    return out;
}

void UserScriptStore::Load() {
    m_scripts.clear();
    std::ifstream in(m_dir / "index.json", std::ios::binary);
    if (!in.is_open()) return;
    const json root = json::parse(in, nullptr, false);
    if (!root.is_object() || !root.contains("scripts") || !root["scripts"].is_array()) return;
    for (const auto& j : root["scripts"]) {
        if (!j.is_object()) continue;
        const std::string id = j.value("id", "");
        if (!ValidId(id)) continue;
        std::ifstream code(m_dir / (id + ".user.js"), std::ios::binary);
        if (!code.is_open()) continue;
        std::string source((std::istreambuf_iterator<char>(code)), std::istreambuf_iterator<char>());
        UserScript s;
        std::string error;
        if (!ParseMeta(source, s, error)) {
            // Plain JavaScript: its sites were saved with it.
            s.matches = j.value("matches", std::vector<std::string>{});
        }
        if (j.contains("name") && j["name"].is_string()) s.name = j["name"].get<std::string>();
        s.id = id;
        s.enabled = j.value("enabled", true);
        s.code = std::move(source);
        m_scripts.push_back(std::move(s));
        if (m_scripts.size() >= kMaxScripts) break;
    }
}

bool UserScriptStore::Save() const {
    std::error_code ec;
    std::filesystem::create_directories(m_dir, ec);
    json scripts = json::array();
    for (const auto& s : m_scripts) {
        std::ofstream code(m_dir / (s.id + ".user.js"), std::ios::binary | std::ios::trunc);
        code << s.code;
        if (!code) return false;
        UserScript probe;
        std::string error;
        json entry = {{"id", s.id}, {"name", s.name}, {"enabled", s.enabled}};
        if (!ParseMeta(s.code, probe, error)) entry["matches"] = s.matches;
        scripts.push_back(std::move(entry));
    }
    // Code files of removed scripts.
    for (const auto& entry : std::filesystem::directory_iterator(m_dir, ec)) {
        const std::string file = entry.path().filename().string();
        const size_t dot = file.find(".user.js");
        if (dot == std::string::npos || dot + 8 != file.size()) continue;
        const std::string id = file.substr(0, dot);
        if (std::none_of(m_scripts.begin(), m_scripts.end(), [&](const UserScript& s) { return s.id == id; })) {
            std::filesystem::remove(entry.path(), ec);
        }
    }
    const std::filesystem::path tmp = m_dir / "index.json.tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << json{{"scripts", scripts}}.dump(1, ' ', false, json::error_handler_t::replace);
        if (!out) return false;
    }
    std::filesystem::rename(tmp, m_dir / "index.json", ec);
    return !ec;
}

} // namespace UltraLight
