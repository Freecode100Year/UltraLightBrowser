use crate::util::write_file_atomic;
use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::collections::BTreeMap;
use std::fs;
use std::path::{Path, PathBuf};

pub const DEFAULT_MAC_PLATFORM_VERSION: &str = "27.2.0";
pub const DEFAULT_HOME_URL: &str = "https://search.brave.com/?utm_source=freedom8964.com";
const MAX_RULES_PER_HOST: usize = 100;

/// Migrates legacy default macOS platform version `"26.2.0"` to `"27.2.0"`.
pub fn migrate_mac_platform_version(version: &str) -> String {
    if version == "26.2.0" {
        DEFAULT_MAC_PLATFORM_VERSION.to_string()
    } else {
        version.to_string()
    }
}

pub fn is_default_home_url(url: &str) -> bool {
    url == DEFAULT_HOME_URL
}

fn valid_mac_platform_version(v: &str) -> bool {
    !v.is_empty()
        && v.len() <= 16
        && v.as_bytes()[0].is_ascii_digit()
        && v.bytes().all(|b| b.is_ascii_digit() || b == b'.')
}

/// Persistent user configuration matching Windows `AppSettings`.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct AppSettings {
    pub start_url: String,
    pub user_agent_profile: String,
    pub mac_platform_version: String,
    pub hardware_acceleration: bool,
    pub video_enhancement: bool,
    pub enable_ad_block: bool,
    pub system_audio_passthrough: bool,
    pub enable_surround_sound: bool,
    pub surround_sound_mode: String,
    pub audio_device_mode: String,
    pub enable_de_esser: bool,
    pub enable_night_mode: bool,
    pub enable_vocal_boost: bool,
    pub audio_volume_boost: f64,
    pub enable_mono_downmix: bool,

    pub startup_page: String,
    pub new_tab_page: String,
    pub search_engine: String,
    pub save_history: bool,
    pub tab_suspend_minutes: i32,
    pub start_show_favorites: bool,
    pub start_show_reading: bool,
    pub start_background: String,
    pub reader_theme: String,
    pub reader_font: String,
    pub reader_font_size: i32,
    pub sidebar_visible: bool,
    pub preload_links: bool,
    pub warp_enabled: bool,
    pub warp_fail_closed: bool,
    pub use_line: bool,
}

impl Default for AppSettings {
    fn default() -> Self {
        Self {
            start_url: DEFAULT_HOME_URL.to_string(),
            user_agent_profile: "default".to_string(),
            mac_platform_version: DEFAULT_MAC_PLATFORM_VERSION.to_string(),
            hardware_acceleration: true,
            video_enhancement: true,
            enable_ad_block: true,
            system_audio_passthrough: true,
            enable_surround_sound: true,
            surround_sound_mode: "standard".to_string(),
            audio_device_mode: "auto".to_string(),
            enable_de_esser: false,
            enable_night_mode: false,
            enable_vocal_boost: false,
            audio_volume_boost: 1.0,
            enable_mono_downmix: false,
            startup_page: "home".to_string(),
            new_tab_page: "home".to_string(),
            search_engine: "google".to_string(),
            save_history: true,
            tab_suspend_minutes: 10,
            start_show_favorites: true,
            start_show_reading: true,
            start_background: "aurora".to_string(),
            reader_theme: "sepia".to_string(),
            reader_font: "serif".to_string(),
            reader_font_size: 19,
            sidebar_visible: false,
            preload_links: true,
            warp_enabled: true,
            warp_fail_closed: false,
            use_line: false,
        }
    }
}

/// Manages `config.json` (settings and per-host cosmetic block rules).
#[derive(Debug, Clone)]
pub struct ConfigStore {
    path: PathBuf,
    pub settings: AppSettings,
    pub block_rules: BTreeMap<String, Vec<String>>,
}

impl ConfigStore {
    pub fn new(app_dir: impl AsRef<Path>) -> Self {
        Self {
            path: app_dir.as_ref().join("config.json"),
            settings: AppSettings::default(),
            block_rules: BTreeMap::new(),
        }
    }

    pub fn app_dir(&self) -> &Path {
        self.path.parent().unwrap_or_else(|| Path::new("."))
    }

    pub fn load(&mut self) {
        self.settings = AppSettings::default();
        self.block_rules.clear();
        let Ok(raw) = fs::read_to_string(&self.path) else {
            return;
        };
        let Ok(root) = serde_json::from_str::<Value>(&raw) else {
            return;
        };

        if let Some(s) = root.get("settings").and_then(Value::as_object) {
            let get_str = |k: &str| s.get(k).and_then(Value::as_str);
            let get_bool = |k: &str| s.get(k).and_then(Value::as_bool);
            let read_choice = |k: &str, target: &mut String, allowed: &[&str]| {
                if let Some(v) = get_str(k) {
                    if allowed.contains(&v) {
                        *target = v.to_string();
                    }
                }
            };
            let read_bool = |k: &str, target: &mut bool| {
                if let Some(v) = get_bool(k) {
                    *target = v;
                }
            };
            let read_int = |k: &str, target: &mut i32, lo: i32, hi: i32| {
                if let Some(v) = s.get(k).and_then(Value::as_i64) {
                    if let Ok(iv) = i32::try_from(v) {
                        if (lo..=hi).contains(&iv) {
                            *target = iv;
                        }
                    }
                }
            };

            if let Some(u) = get_str("startUrl") {
                self.settings.start_url = u.to_string();
            }
            if let Some(p) = get_str("userAgentProfile") {
                self.settings.user_agent_profile = if p == "macos-edge" {
                    "macos-edge".to_string()
                } else {
                    "default".to_string()
                };
            }
            if let Some(v) = get_str("macPlatformVersion") {
                if valid_mac_platform_version(v) {
                    self.settings.mac_platform_version = migrate_mac_platform_version(v);
                }
            }

            read_bool("hardwareAcceleration", &mut self.settings.hardware_acceleration);
            read_bool("videoEnhancement", &mut self.settings.video_enhancement);
            read_bool("enableAdBlock", &mut self.settings.enable_ad_block);
            read_bool(
                "systemAudioPassthrough",
                &mut self.settings.system_audio_passthrough,
            );
            read_bool("enableSurroundSound", &mut self.settings.enable_surround_sound);
            read_choice(
                "surroundSoundMode",
                &mut self.settings.surround_sound_mode,
                &["light", "standard", "cinema"],
            );
            read_choice(
                "audioDeviceMode",
                &mut self.settings.audio_device_mode,
                &["auto", "headphones", "speakers"],
            );
            read_bool("enableDeEsser", &mut self.settings.enable_de_esser);
            read_bool("enableNightMode", &mut self.settings.enable_night_mode);
            read_bool("enableVocalBoost", &mut self.settings.enable_vocal_boost);
            if let Some(v) = s.get("audioVolumeBoost").and_then(Value::as_f64) {
                self.settings.audio_volume_boost = v;
            }
            read_bool("enableMonoDownmix", &mut self.settings.enable_mono_downmix);

            read_choice("startupPage", &mut self.settings.startup_page, &["start", "home"]);
            read_choice(
                "newTabPage",
                &mut self.settings.new_tab_page,
                &["start", "blank", "home"],
            );
            read_choice(
                "searchEngine",
                &mut self.settings.search_engine,
                &["google", "brave", "bing", "duckduckgo", "startpage", "baidu"],
            );
            read_bool("saveHistory", &mut self.settings.save_history);
            read_int("tabSuspendMinutes", &mut self.settings.tab_suspend_minutes, 0, 240);
            read_bool("startShowFavorites", &mut self.settings.start_show_favorites);
            read_bool("startShowReading", &mut self.settings.start_show_reading);
            read_choice(
                "startBackground",
                &mut self.settings.start_background,
                &["aurora", "ocean", "sunset", "plain"],
            );
            read_choice(
                "readerTheme",
                &mut self.settings.reader_theme,
                &["light", "sepia", "gray", "dark"],
            );
            read_choice("readerFont", &mut self.settings.reader_font, &["serif", "sans"]);
            read_int("readerFontSize", &mut self.settings.reader_font_size, 12, 40);
            read_bool("sidebarVisible", &mut self.settings.sidebar_visible);
            read_bool("preloadLinks", &mut self.settings.preload_links);
            read_bool("warpEnabled", &mut self.settings.warp_enabled);
            read_bool("warpFailClosed", &mut self.settings.warp_fail_closed);
            read_bool("useLine", &mut self.settings.use_line);

            if !s.contains_key("homeVersion") {
                if self.settings.start_url == "https://www.google.com" {
                    self.settings.start_url = DEFAULT_HOME_URL.to_string();
                }
                if self.settings.startup_page == "start" {
                    self.settings.startup_page = "home".to_string();
                }
                if self.settings.new_tab_page == "start" {
                    self.settings.new_tab_page = "home".to_string();
                }
            }
        }

        if let Some(rules) = root.get("blockRules").and_then(Value::as_object) {
            for (host, list) in rules {
                if let Some(arr) = list.as_array() {
                    let sels: Vec<String> = arr
                        .iter()
                        .filter_map(Value::as_str)
                        .map(str::to_string)
                        .collect();
                    if !sels.is_empty() {
                        self.block_rules.insert(host.clone(), sels);
                    }
                }
            }
        }
    }

    pub fn save(&self) -> std::io::Result<()> {
        let mut settings_val = serde_json::to_value(&self.settings)
            .map_err(std::io::Error::other)?;
        if let Some(obj) = settings_val.as_object_mut() {
            obj.insert("homeVersion".to_string(), Value::from(2));
        }
        let root = serde_json::json!({
            "settings": settings_val,
            "blockRules": self.block_rules,
        });
        let text = serde_json::to_string_pretty(&root).map_err(std::io::Error::other)?;
        write_file_atomic(&self.path, text.as_bytes())
    }

    pub fn block_css_for_host(&self, host: &str) -> String {
        let Some(list) = self.block_rules.get(host) else {
            return String::new();
        };
        let mut out = String::new();
        for sel in list {
            if !sel.is_empty() {
                out.push_str(sel);
                out.push_str(" { display: none !important; }\n");
            }
        }
        out
    }

    pub fn add_block_rule(&mut self, host: &str, selector: &str) -> bool {
        if host.is_empty() || selector.is_empty() {
            return false;
        }
        let list = self.block_rules.entry(host.to_string()).or_default();
        if list.iter().any(|s| s == selector) || list.len() >= MAX_RULES_PER_HOST {
            return false;
        }
        list.push(selector.to_string());
        true
    }

    pub fn clear_block_rules_for_host(&mut self, host: &str) {
        self.block_rules.remove(host);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn settings_defaults_migration_and_rules_round_trip() {
        let dir = std::env::temp_dir().join("ulb-core-config-test");
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();

        let mut cfg = ConfigStore::new(&dir);
        assert_eq!(cfg.settings.mac_platform_version, "27.2.0");
        assert_eq!(migrate_mac_platform_version("26.2.0"), "27.2.0");
        assert_eq!(migrate_mac_platform_version("27.2.0"), "27.2.0");
        assert_eq!(migrate_mac_platform_version("26.1.0"), "26.1.0");

        // Legacy config migration
        fs::write(
            dir.join("config.json"),
            r#"{"settings":{"startUrl":"https://www.google.com","startupPage":"start","newTabPage":"start","macPlatformVersion":"26.2.0","tabSuspendMinutes":999}}"#,
        )
        .unwrap();
        cfg.load();
        assert_eq!(cfg.settings.start_url, DEFAULT_HOME_URL);
        assert_eq!(cfg.settings.startup_page, "home");
        assert_eq!(cfg.settings.new_tab_page, "home");
        assert_eq!(cfg.settings.mac_platform_version, "27.2.0");
        assert_eq!(cfg.settings.tab_suspend_minutes, 10); // out-of-range rejected

        assert!(cfg.add_block_rule("example.com", ".banner"));
        assert!(!cfg.add_block_rule("example.com", ".banner"));
        assert_eq!(
            cfg.block_css_for_host("example.com"),
            ".banner { display: none !important; }\n"
        );
        cfg.save().unwrap();

        let mut reloaded = ConfigStore::new(&dir);
        reloaded.load();
        assert_eq!(reloaded.settings, cfg.settings);
        assert_eq!(reloaded.block_rules, cfg.block_rules);
        let _ = fs::remove_dir_all(&dir);
    }
}
