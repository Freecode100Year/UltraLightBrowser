use crate::config::{migrate_mac_platform_version, AppSettings, ConfigStore};
use crate::extension_pkg::recommended_extensions;
use crate::extensions_store::ExtensionsStore;
use crate::internal_pages::{internal_url, is_internal_url, page_name};
use crate::library::{Library, SavedTab, BOOKMARKS_FOLDER, FAVORITES_FOLDER};
use crate::line_store::{LineEntry, LineStore};
use crate::user_scripts::UserScriptStore;
use crate::util::{domain_of, now_epoch_ms};
use crate::warp_host::WarpStatus;
use serde_json::{json, Value};

/// Lightweight snapshot of one open browser tab passed to UI bridge queries (`getTabs`, `saveGroup`, `getArticle`).
#[derive(Debug, Clone)]
pub struct TabView {
    pub id: u64,
    pub title: String,
    pub url: String,
    pub loading: bool,
    pub audio: bool,
    pub muted: bool,
    pub active: bool,
    pub reader_source: String,
}

/// Active reader article state passed when the current tab is `reader.html`.
#[derive(Debug, Clone, Default)]
pub struct ReaderView {
    pub title: String,
    pub byline: String,
    pub site_name: String,
    pub url: String,
    pub html: String,
}

/// Host side effects emitted by a UI RPC command for the shell/runtime to execute.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum SideEffect {
    SaveConfig,
    SaveLibrary,
    SaveLines,
    SyncAdblock,
    RestartWarp,
    RescanWarp,
    ProbeLines(Vec<LineEntry>),
    FetchSubscription { id: String, url: String },
    DownloadRecommendedExtension(String),
    ReloadExtensionsRuntime,
    OpenExtensionPage { dir: String, options: bool },
    OpenUrl { url: String, new_tab: bool, activate: bool },
    NewPrivateWindow,
    ActivateTab(u64),
    CloseTab(u64),
    NewTab(bool),
    CloseOverview,
    ExitReader,
}

/// Result of dispatching one JSON RPC message from an internal `ulb.internal` page.
#[derive(Debug, Clone)]
pub struct RpcOutcome {
    pub response_json: String,
    pub side_effects: Vec<SideEffect>,
}

/// Mutable references to browser stores and current UI context for RPC dispatch.
pub struct BridgeContext<'a> {
    pub config: &'a mut ConfigStore,
    pub library: &'a mut Library,
    pub lines: &'a mut LineStore,
    pub extensions: &'a mut ExtensionsStore,
    pub scripts: &'a mut UserScriptStore,
    pub warp: &'a WarpStatus,
    pub tabs: &'a [TabView],
    pub reader: Option<&'a ReaderView>,
    pub private_mode: bool,
    pub active_tab_id: u64,
    pub total_blocked_ads: u64,
}

fn str_arg(args: &Value, key: &str) -> String {
    args.get(key)
        .and_then(|v| v.as_str())
        .unwrap_or_default()
        .to_string()
}

fn bool_arg(args: &Value, key: &str, default: bool) -> bool {
    args.get(key).and_then(|v| v.as_bool()).unwrap_or(default)
}

fn ok_response(id: &Value, result: Value, side_effects: Vec<SideEffect>) -> RpcOutcome {
    RpcOutcome {
        response_json: json!({ "id": id, "result": result }).to_string(),
        side_effects,
    }
}

fn err_response(id: &Value, msg: &str) -> RpcOutcome {
    RpcOutcome {
        response_json: json!({ "id": id, "error": msg }).to_string(),
        side_effects: Vec::new(),
    }
}

fn lines_snapshot(ctx: &BridgeContext<'_>) -> Value {
    let lines: Vec<Value> = ctx
        .lines
        .lines()
        .iter()
        .map(|l| {
            json!({
                "id": l.id,
                "name": l.name,
                "proto": LineStore::protocol_of(&l.link),
                "server": LineStore::server_of(&l.link),
                "sub": l.sub,
            })
        })
        .collect();
    let subs: Vec<Value> = ctx
        .lines
        .subs()
        .iter()
        .map(|s| {
            json!({
                "id": s.id,
                "name": s.name,
                "url": s.url,
                "updated": s.updated,
                "count": s.count,
            })
        })
        .collect();
    json!({
        "selected": ctx.lines.selected(),
        "lines": lines,
        "subs": subs,
    })
}

fn settings_snapshot(s: &AppSettings) -> Value {
    json!({
        "startupPage": s.startup_page,
        "newTabPage": s.new_tab_page,
        "startUrl": s.start_url,
        "searchEngine": s.search_engine,
        "userAgentProfile": s.user_agent_profile,
        "macPlatformVersion": s.mac_platform_version,
        "hardwareAcceleration": s.hardware_acceleration,
        "videoEnhancement": s.video_enhancement,
        "enableAdBlock": s.enable_ad_block,
        "saveHistory": s.save_history,
        "tabSuspendMinutes": s.tab_suspend_minutes,
        "preloadLinks": s.preload_links,
        "warpEnabled": s.warp_enabled,
        "warpFailClosed": s.warp_fail_closed,
        "useLine": s.use_line,
        "systemAudioPassthrough": s.system_audio_passthrough,
        "enableSurroundSound": s.enable_surround_sound,
        "surroundSoundMode": s.surround_sound_mode,
        "audioDeviceMode": s.audio_device_mode,
        "enableDeEsser": s.enable_de_esser,
        "enableNightMode": s.enable_night_mode,
        "enableVocalBoost": s.enable_vocal_boost,
        "audioVolumeBoost": s.audio_volume_boost,
        "enableMonoDownmix": s.enable_mono_downmix,
        "startShowFavorites": s.start_show_favorites,
        "startShowReading": s.start_show_reading,
        "startBackground": s.start_background,
        "readerTheme": s.reader_theme,
        "readerFont": s.reader_font,
        "readerFontSize": s.reader_font_size,
    })
}

fn apply_setting(s: &mut AppSettings, key: &str, v: &Value, fx: &mut Vec<SideEffect>) -> bool {
    let as_bool = |v: &Value| v.as_bool();
    let as_str = |v: &Value| v.as_str().map(|x| x.to_string());

    match key {
        "startupPage" => {
            if let Some(val) = as_str(v).filter(|x| matches!(x.as_str(), "home" | "restore" | "blank")) {
                s.startup_page = val;
            }
        }
        "newTabPage" => {
            if let Some(val) = as_str(v).filter(|x| matches!(x.as_str(), "home" | "blank")) {
                s.new_tab_page = val;
            }
        }
        "startUrl" => {
            if let Some(val) = as_str(v) {
                s.start_url = val;
            }
        }
        "searchEngine" => {
            if let Some(val) = as_str(v).filter(|x| matches!(x.as_str(), "google" | "bing" | "duckduckgo" | "baidu")) {
                s.search_engine = val;
            }
        }
        "userAgentProfile" => {
            if let Some(val) = as_str(v).filter(|x| matches!(x.as_str(), "default" | "chrome" | "edge" | "safari")) {
                s.user_agent_profile = val;
            }
        }
        "macPlatformVersion" => {
            if let Some(val) = as_str(v).filter(|x| !x.is_empty() && x.len() <= 16) {
                s.mac_platform_version = migrate_mac_platform_version(&val);
            }
        }
        "hardwareAcceleration" => if let Some(b) = as_bool(v) { s.hardware_acceleration = b; },
        "videoEnhancement" => if let Some(b) = as_bool(v) { s.video_enhancement = b; },
        "enableAdBlock" => {
            if let Some(b) = as_bool(v) {
                s.enable_ad_block = b;
                fx.push(SideEffect::SyncAdblock);
            }
        }
        "saveHistory" => if let Some(b) = as_bool(v) { s.save_history = b; },
        "tabSuspendMinutes" => {
            if let Some(n) = v.as_i64() {
                s.tab_suspend_minutes = n.clamp(0, 1440) as i32;
            }
        }
        "preloadLinks" => if let Some(b) = as_bool(v) { s.preload_links = b; },
        "warpEnabled" => {
            if let Some(b) = as_bool(v) {
                s.warp_enabled = b;
                fx.push(SideEffect::RestartWarp);
            }
        }
        "warpFailClosed" => {
            if let Some(b) = as_bool(v) {
                s.warp_fail_closed = b;
                fx.push(SideEffect::RestartWarp);
            }
        }
        "useLine" => {
            if let Some(b) = as_bool(v) {
                s.use_line = b;
                fx.push(SideEffect::RestartWarp);
            }
        }
        "systemAudioPassthrough" => if let Some(b) = as_bool(v) { s.system_audio_passthrough = b; },
        "enableSurroundSound" => if let Some(b) = as_bool(v) { s.enable_surround_sound = b; },
        "surroundSoundMode" => {
            if let Some(val) = as_str(v).filter(|x| matches!(x.as_str(), "dialogue" | "light" | "standard" | "cinema")) {
                s.surround_sound_mode = val;
            }
        }
        "audioDeviceMode" => {
            if let Some(val) = as_str(v).filter(|x| matches!(x.as_str(), "auto" | "headphones" | "speakers")) {
                s.audio_device_mode = val;
            }
        }
        "enableDeEsser" => if let Some(b) = as_bool(v) { s.enable_de_esser = b; },
        "enableNightMode" => if let Some(b) = as_bool(v) { s.enable_night_mode = b; },
        "enableVocalBoost" => if let Some(b) = as_bool(v) { s.enable_vocal_boost = b; },
        "audioVolumeBoost" => {
            if let Some(f) = v.as_f64() {
                s.audio_volume_boost = f.clamp(1.0, 2.5);
            }
        }
        "enableMonoDownmix" => if let Some(b) = as_bool(v) { s.enable_mono_downmix = b; },
        "startShowFavorites" => if let Some(b) = as_bool(v) { s.start_show_favorites = b; },
        "startShowReading" => if let Some(b) = as_bool(v) { s.start_show_reading = b; },
        "startBackground" => {
            if let Some(val) = as_str(v).filter(|x| matches!(x.as_str(), "aurora" | "graphite" | "indigo" | "warm")) {
                s.start_background = val;
            }
        }
        "readerTheme" => {
            if let Some(val) = as_str(v).filter(|x| matches!(x.as_str(), "white" | "sepia" | "gray" | "night")) {
                s.reader_theme = val;
            }
        }
        "readerFont" => {
            if let Some(val) = as_str(v).filter(|x| matches!(x.as_str(), "sans" | "serif" | "mono")) {
                s.reader_font = val;
            }
        }
        "readerFontSize" => {
            if let Some(n) = v.as_i64() {
                s.reader_font_size = n.clamp(14, 28) as i32;
            }
        }
        _ => return false,
    }
    true
}

/// Handles one JSON RPC request from an internal `ulb.internal` page (`{id, cmd, args}`).
pub fn dispatch_rpc(ctx: &mut BridgeContext<'_>, raw_json: &str) -> Option<RpcOutcome> {
    let msg: Value = serde_json::from_str(raw_json).ok()?;
    let id = msg.get("id")?.clone();
    let cmd = msg.get("cmd").and_then(|v| v.as_str())?;
    let args = msg.get("args").cloned().unwrap_or_else(|| json!({}));

    let mut fx = Vec::new();
    let result = match cmd {
        "getStart" => {
            let favs: Vec<Value> = ctx
                .library
                .bookmarks_in(FAVORITES_FOLDER)
                .into_iter()
                .map(|b| json!({ "id": b.id, "title": b.title, "url": b.url }))
                .collect();
            let reading: Vec<Value> = ctx
                .library
                .reading_list()
                .iter()
                .take(8)
                .map(|r| json!({ "id": r.id, "title": r.title, "url": r.url, "read": r.read, "added": r.added }))
                .collect();
            json!({
                "favorites": favs,
                "reading": reading,
                "blocked": ctx.total_blocked_ads,
                "private": ctx.private_mode,
                "showFavorites": ctx.config.settings.start_show_favorites,
                "showReading": ctx.config.settings.start_show_reading,
                "background": ctx.config.settings.start_background,
                "warp": {
                    "enabled": ctx.config.settings.warp_enabled,
                    "up": ctx.warp.up,
                    "endpoint": ctx.warp.endpoint,
                    "reason": ctx.warp.reason,
                    "error": ctx.warp.error,
                    "mode": ctx.warp.mode,
                    "probing": ctx.warp.probing,
                    "useLine": ctx.config.settings.use_line,
                    "lineId": ctx.lines.selected(),
                    "lineName": ctx.lines.find(ctx.lines.selected()).map(|l| l.name.clone()).unwrap_or_default(),
                }
            })
        }
        "getHistory" => {
            let q = str_arg(&args, "query");
            let items: Vec<Value> = ctx
                .library
                .query_history(&q, 500)
                .into_iter()
                .map(|h| json!({ "url": h.url, "title": h.title, "last": h.last, "visits": h.visits }))
                .collect();
            json!({ "items": items })
        }
        "deleteHistory" => {
            ctx.library.remove_history(&str_arg(&args, "url"));
            fx.push(SideEffect::SaveLibrary);
            json!({ "ok": true })
        }
        "clearHistory" => {
            let minutes = args.get("minutes").and_then(|v| v.as_i64()).unwrap_or(0);
            let since = if minutes > 0 {
                now_epoch_ms().saturating_sub(minutes * 60_000)
            } else {
                0
            };
            ctx.library.clear_history(since);
            fx.push(SideEffect::SaveLibrary);
            json!({ "ok": true })
        }
        "getBookmarks" => {
            let folders: Vec<Value> = ctx
                .library
                .folders()
                .iter()
                .map(|f| Value::String(f.clone()))
                .collect();
            let items: Vec<Value> = ctx
                .library
                .bookmarks()
                .iter()
                .map(|b| json!({ "id": b.id, "title": b.title, "url": b.url, "folder": b.folder, "added": b.added }))
                .collect();
            let reading: Vec<Value> = ctx
                .library
                .reading_list()
                .iter()
                .map(|r| json!({ "id": r.id, "title": r.title, "url": r.url, "added": r.added, "read": r.read }))
                .collect();
            json!({ "folders": folders, "items": items, "reading": reading })
        }
        "addBookmark" => {
            let folder = str_arg(&args, "folder");
            let folder = if folder.is_empty() { BOOKMARKS_FOLDER } else { &folder };
            let id_added = ctx.library.add_bookmark(
                &str_arg(&args, "title"),
                &str_arg(&args, "url"),
                folder,
                now_epoch_ms(),
            );
            fx.push(SideEffect::SaveLibrary);
            json!({ "id": id_added })
        }
        "updateBookmark" => {
            ctx.library.update_bookmark(
                &str_arg(&args, "id"),
                &str_arg(&args, "title"),
                &str_arg(&args, "url"),
                &str_arg(&args, "folder"),
            );
            fx.push(SideEffect::SaveLibrary);
            json!({ "ok": true })
        }
        "removeBookmark" => {
            ctx.library.remove_bookmark(&str_arg(&args, "id"));
            fx.push(SideEffect::SaveLibrary);
            json!({ "ok": true })
        }
        "moveBookmark" => {
            ctx.library
                .move_bookmark(&str_arg(&args, "id"), &str_arg(&args, "beforeId"));
            fx.push(SideEffect::SaveLibrary);
            json!({ "ok": true })
        }
        "addFolder" => {
            let ok = ctx.library.add_folder(&str_arg(&args, "name"));
            if ok {
                fx.push(SideEffect::SaveLibrary);
            }
            json!({ "ok": ok })
        }
        "renameFolder" => {
            let ok = ctx
                .library
                .rename_folder(&str_arg(&args, "from"), &str_arg(&args, "to"));
            if ok {
                fx.push(SideEffect::SaveLibrary);
            }
            json!({ "ok": ok })
        }
        "removeFolder" => {
            ctx.library.remove_folder(&str_arg(&args, "name"));
            fx.push(SideEffect::SaveLibrary);
            json!({ "ok": true })
        }
        "removeReading" => {
            ctx.library.remove_reading(&str_arg(&args, "id"));
            fx.push(SideEffect::SaveLibrary);
            json!({ "ok": true })
        }
        "markRead" => {
            ctx.library
                .set_read(&str_arg(&args, "id"), bool_arg(&args, "read", true));
            fx.push(SideEffect::SaveLibrary);
            json!({ "ok": true })
        }
        "warpRescan" => {
            fx.push(SideEffect::RescanWarp);
            json!({ "ok": true })
        }
        "getScripts" => {
            ctx.scripts.load();
            let list: Vec<Value> = ctx
                .scripts
                .scripts()
                .iter()
                .map(|s| {
                    json!({
                        "id": s.id,
                        "name": s.name,
                        "namespace": s.ns,
                        "version": s.version,
                        "description": s.description,
                        "runAt": s.run_at,
                        "enabled": s.enabled,
                        "matches": s.matches.len() + s.includes.len(),
                        "ignored": s.ignored,
                    })
                })
                .collect();
            json!({ "scripts": list })
        }
        "installScript" => {
            match ctx.scripts.install(
                &str_arg(&args, "code"),
                &str_arg(&args, "name"),
                &str_arg(&args, "match"),
            ) {
                Ok(id_installed) => {
                    let name = ctx
                        .scripts
                        .scripts()
                        .iter()
                        .find(|s| s.id == id_installed)
                        .map(|s| s.name.clone())
                        .unwrap_or_default();
                    json!({ "ok": true, "name": name, "id": id_installed })
                }
                Err(e) => return Some(err_response(&id, &e)),
            }
        }
        "setScriptEnabled" => {
            let ok = ctx
                .scripts
                .set_enabled(&str_arg(&args, "id"), bool_arg(&args, "enabled", true));
            json!({ "ok": ok })
        }
        "removeScript" => {
            let ok = ctx.scripts.remove(&str_arg(&args, "id"));
            json!({ "ok": ok })
        }
        "getExtensions" => {
            let list: Vec<Value> = ctx
                .extensions
                .list()
                .iter()
                .map(|e| {
                    json!({
                        "dir": e.dir,
                        "id": e.id,
                        "name": e.name,
                        "version": e.version,
                        "description": e.description,
                        "enabled": e.enabled,
                        "proxy": e.proxy,
                        "hasPopup": !e.popup.is_empty(),
                        "hasOptions": !e.options.is_empty(),
                        "error": e.error,
                    })
                })
                .collect();
            json!({ "extensions": list })
        }
        "getRecommendedExtensions" => {
            let installed: Vec<String> = ctx
                .extensions
                .list()
                .iter()
                .filter(|e| !e.source.is_empty())
                .map(|e| e.source.clone())
                .collect();
            let list: Vec<Value> = recommended_extensions()
                .iter()
                .map(|r| {
                    json!({
                        "key": r.key,
                        "name": r.name,
                        "summary": r.summary,
                        "note": r.note,
                        "homepage": r.homepage,
                        "proxy": r.proxy,
                        "installed": installed.iter().any(|s| s == r.key),
                    })
                })
                .collect();
            json!({ "items": list })
        }
        "downloadRecommendedExtension" => {
            fx.push(SideEffect::DownloadRecommendedExtension(str_arg(&args, "key")));
            json!({ "started": true })
        }
        "setExtensionEnabled" => {
            let ok = ctx
                .extensions
                .set_enabled(&str_arg(&args, "dir"), bool_arg(&args, "enabled", true));
            if ok {
                fx.push(SideEffect::ReloadExtensionsRuntime);
            }
            json!({ "ok": ok })
        }
        "removeExtension" => {
            let ok = ctx.extensions.remove(&str_arg(&args, "dir"));
            if ok {
                fx.push(SideEffect::ReloadExtensionsRuntime);
            }
            json!({ "ok": ok })
        }
        "openExtensionPage" => {
            fx.push(SideEffect::OpenExtensionPage {
                dir: str_arg(&args, "dir"),
                options: bool_arg(&args, "options", false),
            });
            json!({ "ok": true })
        }
        "getLines" => lines_snapshot(ctx),
        "addLines" => {
            let raw = str_arg(&args, "links");
            let parsed: Vec<&str> = raw
                .lines()
                .map(|s| s.trim())
                .filter(|s| !s.is_empty())
                .collect();
            let n = ctx.lines.add(&parsed, "");
            if n > 0 {
                fx.push(SideEffect::SaveLines);
                if ctx.config.settings.warp_enabled && ctx.config.settings.use_line {
                    fx.push(SideEffect::RestartWarp);
                }
            }
            let mut snap = lines_snapshot(ctx);
            snap["added"] = json!(n);
            snap
        }
        "removeLine" => {
            let lid = str_arg(&args, "id");
            let was_selected = ctx.lines.selected() == lid;
            ctx.lines.remove(&lid);
            fx.push(SideEffect::SaveLines);
            if was_selected && ctx.config.settings.warp_enabled && ctx.config.settings.use_line {
                fx.push(SideEffect::RestartWarp);
            }
            lines_snapshot(ctx)
        }
        "renameLine" => {
            ctx.lines.rename(&str_arg(&args, "id"), &str_arg(&args, "name"));
            fx.push(SideEffect::SaveLines);
            lines_snapshot(ctx)
        }
        "selectLine" => {
            let lid = str_arg(&args, "id");
            ctx.lines.select(&lid);
            ctx.config.settings.use_line = !lid.is_empty();
            fx.push(SideEffect::SaveLines);
            fx.push(SideEffect::SaveConfig);
            if ctx.config.settings.warp_enabled {
                fx.push(SideEffect::RestartWarp);
            }
            lines_snapshot(ctx)
        }
        "removeSub" => {
            let before = ctx.lines.selected().to_string();
            ctx.lines.remove_sub(&str_arg(&args, "id"));
            fx.push(SideEffect::SaveLines);
            if before != ctx.lines.selected() && ctx.config.settings.warp_enabled && ctx.config.settings.use_line {
                fx.push(SideEffect::RestartWarp);
            }
            lines_snapshot(ctx)
        }
        "probeLines" => {
            let lines = ctx.lines.lines().to_vec();
            if lines.is_empty() {
                return Some(err_response(&id, "还没有添加自选线路"));
            }
            fx.push(SideEffect::ProbeLines(lines));
            json!({ "started": true })
        }
        "addSub" => {
            let url = str_arg(&args, "url");
            let name = str_arg(&args, "name");
            if !(url.starts_with("http://") || url.starts_with("https://")) {
                return Some(err_response(&id, "需要 http:// 或 https:// 订阅地址"));
            }
            let sub_id = ctx.lines.add_sub(&name, &url);
            fx.push(SideEffect::SaveLines);
            fx.push(SideEffect::FetchSubscription { id: sub_id, url });
            lines_snapshot(ctx)
        }
        "updateSub" => {
            let sub_id = str_arg(&args, "id");
            let Some(sub) = ctx.lines.subs().iter().find(|s| s.id == sub_id).cloned() else {
                return Some(err_response(&id, "找不到此订阅"));
            };
            fx.push(SideEffect::FetchSubscription {
                id: sub.id,
                url: sub.url,
            });
            json!({ "started": true })
        }
        "getSettings" => settings_snapshot(&ctx.config.settings),
        "setSetting" => {
            let key = str_arg(&args, "key");
            let val = args.get("value").cloned().unwrap_or(Value::Null);
            if apply_setting(&mut ctx.config.settings, &key, &val, &mut fx) {
                fx.push(SideEffect::SaveConfig);
                json!({ "ok": true })
            } else {
                json!({ "ok": false })
            }
        }
        "clearSiteSettings" => {
            ctx.library.clear_sites();
            fx.push(SideEffect::SaveLibrary);
            fx.push(SideEffect::SyncAdblock);
            json!({ "ok": true })
        }
        "getTabs" => {
            let list: Vec<Value> = ctx
                .tabs
                .iter()
                .map(|t| {
                    let is_start = is_internal_url(&t.url)
                        && page_name(&t.url).as_deref() == Some("start.html");
                    let url = if t.reader_source.is_empty() {
                        &t.url
                    } else {
                        &t.reader_source
                    };
                    let dom = if is_start { String::new() } else { domain_of(url) };
                    json!({
                        "id": t.id,
                        "title": if t.title.is_empty() { "新标签页" } else { &t.title },
                        "url": if is_start { "" } else { url },
                        "domain": dom,
                        "active": t.id == ctx.active_tab_id,
                        "loading": t.loading,
                        "audio": t.audio,
                        "muted": t.muted,
                    })
                })
                .collect();
            json!({ "tabs": list, "private": ctx.private_mode })
        }
        "activateTab" => {
            fx.push(SideEffect::ActivateTab(args.get("id").and_then(|v| v.as_u64()).unwrap_or(0)));
            json!({ "ok": true })
        }
        "closeTab" => {
            fx.push(SideEffect::CloseTab(args.get("id").and_then(|v| v.as_u64()).unwrap_or(0)));
            json!({ "ok": true })
        }
        "newTab" => {
            fx.push(SideEffect::NewTab(true));
            json!({ "ok": true })
        }
        "closeOverview" => {
            fx.push(SideEffect::CloseOverview);
            json!({ "ok": true })
        }
        "getSidebar" => {
            let groups: Vec<Value> = ctx
                .library
                .groups()
                .iter()
                .map(|g| json!({ "id": g.id, "name": g.name, "count": g.tabs.len() }))
                .collect();
            let favorites: Vec<Value> = ctx
                .library
                .bookmarks_in(FAVORITES_FOLDER)
                .into_iter()
                .map(|b| json!({ "id": b.id, "title": b.title, "url": b.url }))
                .collect();
            let bookmarks: Vec<Value> = ctx
                .library
                .bookmarks_in(BOOKMARKS_FOLDER)
                .into_iter()
                .map(|b| json!({ "id": b.id, "title": b.title, "url": b.url }))
                .collect();
            let reading: Vec<Value> = ctx
                .library
                .reading_list()
                .iter()
                .map(|r| json!({ "id": r.id, "title": r.title, "url": r.url, "read": r.read }))
                .collect();
            json!({
                "groups": groups,
                "favorites": favorites,
                "bookmarks": bookmarks,
                "reading": reading,
            })
        }
        "saveGroup" => {
            let tabs_saved: Vec<SavedTab> = ctx
                .tabs
                .iter()
                .filter(|t| !is_internal_url(&t.url) && !t.url.is_empty())
                .map(|t| SavedTab {
                    title: t.title.clone(),
                    url: t.url.clone(),
                })
                .collect();
            if tabs_saved.is_empty() {
                return Some(err_response(&id, "当前没有可保存的网页标签页"));
            }
            let name = str_arg(&args, "name");
            let gid = ctx.library.save_group(&name, &tabs_saved, now_epoch_ms());
            fx.push(SideEffect::SaveLibrary);
            json!({ "id": gid })
        }
        "openGroup" => {
            let gid = str_arg(&args, "id");
            let Some(group) = ctx.library.find_group(&gid) else {
                return Some(err_response(&id, "找不到标签页组"));
            };
            for (idx, t) in group.tabs.iter().enumerate() {
                fx.push(SideEffect::OpenUrl {
                    url: t.url.clone(),
                    new_tab: true,
                    activate: idx == 0,
                });
            }
            json!({ "ok": true })
        }
        "deleteGroup" => {
            ctx.library.remove_group(&str_arg(&args, "id"));
            fx.push(SideEffect::SaveLibrary);
            json!({ "ok": true })
        }
        "open" => {
            let url = str_arg(&args, "url");
            if !url.is_empty() {
                fx.push(SideEffect::OpenUrl {
                    url,
                    new_tab: bool_arg(&args, "newTab", false),
                    activate: true,
                });
            }
            json!({ "ok": true })
        }
        "newPrivateWindow" => {
            fx.push(SideEffect::NewPrivateWindow);
            json!({ "ok": true })
        }
        "getArticle" => {
            let article = ctx.reader.cloned().unwrap_or_default();
            json!({
                "title": article.title,
                "byline": article.byline,
                "siteName": article.site_name,
                "url": article.url,
                "domain": domain_of(&article.url),
                "html": article.html,
                "theme": ctx.config.settings.reader_theme,
                "font": ctx.config.settings.reader_font,
                "fontSize": ctx.config.settings.reader_font_size,
            })
        }
        "setReaderPrefs" => {
            if let Some(theme) = args.get("theme").and_then(|v| v.as_str()) {
                let _ = apply_setting(&mut ctx.config.settings, "readerTheme", &json!(theme), &mut fx);
            }
            if let Some(font) = args.get("font").and_then(|v| v.as_str()) {
                let _ = apply_setting(&mut ctx.config.settings, "readerFont", &json!(font), &mut fx);
            }
            if let Some(size) = args.get("fontSize").and_then(|v| v.as_i64()) {
                let _ = apply_setting(&mut ctx.config.settings, "readerFontSize", &json!(size), &mut fx);
            }
            fx.push(SideEffect::SaveConfig);
            json!({ "ok": true })
        }
        "exitReader" => {
            fx.push(SideEffect::ExitReader);
            json!({ "ok": true })
        }
        _ => return Some(err_response(&id, &format!("未知命令: {cmd}"))),
    };

    let _ = internal_url("start.html");
    Some(ok_response(&id, result, fx))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::path::PathBuf;

    fn temp_root(tag: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("ulb-bridge-{tag}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        dir
    }

    #[test]
    fn settings_bookmarks_lines_and_tabs_rpc_roundtrip() {
        let dir = temp_root("rpc");
        let mut config = ConfigStore::new(dir.join("config.json"));
        let mut library = Library::new(dir.join("library.json"));
        let mut lines = LineStore::new();
        let mut extensions = ExtensionsStore::new(&dir);
        let mut scripts = UserScriptStore::new(dir.join("scripts"), "2.10.1");
        let warp = WarpStatus::default();
        let tabs = vec![TabView {
            id: 42,
            title: "Example".to_string(),
            url: "https://example.com/news".to_string(),
            loading: false,
            audio: true,
            muted: false,
            active: true,
            reader_source: String::new(),
        }];

        let mut ctx = BridgeContext {
            config: &mut config,
            library: &mut library,
            lines: &mut lines,
            extensions: &mut extensions,
            scripts: &mut scripts,
            warp: &warp,
            tabs: &tabs,
            reader: None,
            private_mode: false,
            active_tab_id: 42,
            total_blocked_ads: 128,
        };

        let out = dispatch_rpc(
            &mut ctx,
            r#"{"id":1,"cmd":"setSetting","args":{"key":"warpFailClosed","value":true}}"#,
        )
        .unwrap();
        assert!(out.side_effects.contains(&SideEffect::RestartWarp));
        assert!(out.side_effects.contains(&SideEffect::SaveConfig));
        assert!(ctx.config.settings.warp_fail_closed);

        let bm = dispatch_rpc(
            &mut ctx,
            r#"{"id":2,"cmd":"addBookmark","args":{"title":"Arch","url":"https://archlinux.org"}}"#,
        )
        .unwrap();
        let v: Value = serde_json::from_str(&bm.response_json).unwrap();
        assert!(!v["result"]["id"].as_str().unwrap().is_empty());

        let tabs_res = dispatch_rpc(&mut ctx, r#"{"id":3,"cmd":"getTabs","args":{}}"#).unwrap();
        let tv: Value = serde_json::from_str(&tabs_res.response_json).unwrap();
        assert_eq!(tv["result"]["tabs"][0]["domain"], "example.com");

        let start = dispatch_rpc(&mut ctx, r#"{"id":4,"cmd":"getStart","args":{}}"#).unwrap();
        let sv: Value = serde_json::from_str(&start.response_json).unwrap();
        assert_eq!(sv["result"]["blocked"], 128);

        let _ = std::fs::remove_dir_all(&dir);
    }
}
