use crate::extension_pkg::{crx_zip_offset, read_extension_manifest};
use crate::util::write_file_atomic;
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::time::{SystemTime, UNIX_EPOCH};

const MAX_BYTES: u64 = 200 * 1024 * 1024;
const MAX_FILES: usize = 20_000;
const MAX_EXTENSIONS: usize = 50;

/// Installed extension metadata tracked in `<data_dir>/extensions/index.json`.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ExtensionEntry {
    pub dir: String,
    pub id: String,
    pub name: String,
    pub version: String,
    pub description: String,
    pub popup: String,
    pub options: String,
    pub enabled: bool,
    pub proxy: bool,
    pub source: String,
    pub error: String,
}

#[derive(Debug, Clone)]
pub struct ExtensionsStore {
    folder: PathBuf,
    entries: Vec<ExtensionEntry>,
    loaded: bool,
}

fn now_ticks() -> u128 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_millis())
        .unwrap_or(0)
}

fn slug(name: &str) -> String {
    let mut out = String::new();
    for ch in name.chars() {
        if ch.is_ascii_alphanumeric() {
            out.push(ch.to_ascii_lowercase());
        } else if !out.is_empty() && !out.ends_with('-') {
            out.push('-');
        }
        if out.len() >= 32 {
            break;
        }
    }
    while out.ends_with('-') {
        out.pop();
    }
    if out.is_empty() {
        "ext".to_string()
    } else {
        out
    }
}

fn check_size(dir: &Path) -> Result<(), String> {
    let mut stack = vec![dir.to_path_buf()];
    let mut total_bytes: u64 = 0;
    let mut total_files: usize = 0;

    while let Some(cur) = stack.pop() {
        let Ok(rd) = fs::read_dir(&cur) else {
            continue;
        };
        for entry in rd.flatten() {
            let Ok(meta) = fs::symlink_metadata(entry.path()) else {
                continue;
            };
            if meta.file_type().is_symlink() {
                continue;
            }
            if meta.is_dir() {
                stack.push(entry.path());
            } else if meta.is_file() {
                total_bytes = total_bytes.saturating_add(meta.len());
                total_files += 1;
                if total_bytes > MAX_BYTES || total_files > MAX_FILES {
                    return Err("扩展太大（超过 200 MB 或 2 万个文件）".to_string());
                }
            }
        }
    }
    Ok(())
}

fn copy_dir_no_symlinks(src: &Path, dst: &Path) -> std::io::Result<()> {
    fs::create_dir_all(dst)?;
    for entry in fs::read_dir(src)? {
        let entry = entry?;
        let file_type = entry.file_type()?;
        if file_type.is_symlink() {
            continue;
        }
        let target = dst.join(entry.file_name());
        if file_type.is_dir() {
            copy_dir_no_symlinks(&entry.path(), &target)?;
        } else if file_type.is_file() {
            fs::copy(entry.path(), &target)?;
        }
    }
    Ok(())
}

fn unpack_zip(zip_path: &Path, dest: &Path) -> bool {
    if let Ok(status) = Command::new("unzip")
        .arg("-qq")
        .arg("-o")
        .arg(zip_path)
        .arg("-d")
        .arg(dest)
        .status()
    {
        if status.success() {
            return true;
        }
    }
    Command::new("bsdtar")
        .arg("-xf")
        .arg(zip_path)
        .arg("-C")
        .arg(dest)
        .status()
        .map(|s| s.success())
        .unwrap_or(false)
}

impl ExtensionsStore {
    pub fn new(data_dir: impl AsRef<Path>) -> Self {
        Self {
            folder: data_dir.as_ref().join("extensions"),
            entries: Vec::new(),
            loaded: false,
        }
    }

    pub fn folder(&self) -> &Path {
        &self.folder
    }

    fn staging_path(&self, kind: &str) -> PathBuf {
        self.folder
            .join(format!(".staging-{kind}-{}", now_ticks()))
    }

    /// Loads `index.json` and sweeps leftover `.staging-*` / `.trash-*` directories.
    pub fn load_index(&mut self) {
        if self.loaded {
            return;
        }
        self.loaded = true;
        self.entries.clear();

        if let Ok(rd) = fs::read_dir(&self.folder) {
            for item in rd.flatten() {
                if let Some(name) = item.file_name().to_str() {
                    if name.starts_with(".staging-") || name.starts_with(".trash-") {
                        let _ = fs::remove_dir_all(item.path());
                    }
                }
            }
        }

        let Ok(raw) = fs::read(self.folder.join("index.json")) else {
            return;
        };
        let Ok(root) = serde_json::from_slice::<Value>(&raw) else {
            return;
        };
        let Some(arr) = root.get("extensions").and_then(|v| v.as_array()) else {
            return;
        };

        for j in arr {
            let Some(obj) = j.as_object() else { continue };
            let dir = obj
                .get("dir")
                .and_then(|v| v.as_str())
                .unwrap_or_default()
                .to_string();
            if dir.is_empty() || dir.contains(['/', '\\', ':', '.']) {
                continue;
            }
            let manifest = read_extension_manifest(&self.folder.join(&dir));
            if !manifest.error.is_empty() {
                continue;
            }
            self.entries.push(ExtensionEntry {
                dir,
                id: String::new(),
                name: manifest.name,
                version: manifest.version,
                description: manifest.description,
                popup: manifest.popup,
                options: manifest.options,
                enabled: obj.get("enabled").and_then(|v| v.as_bool()).unwrap_or(true),
                proxy: manifest.proxy,
                source: obj
                    .get("source")
                    .and_then(|v| v.as_str())
                    .unwrap_or_default()
                    .to_string(),
                error: String::new(),
            });
        }
    }

    pub fn save_index(&self) -> bool {
        let list: Vec<Value> = self
            .entries
            .iter()
            .map(|e| {
                let mut obj = json!({
                    "dir": e.dir,
                    "enabled": e.enabled,
                });
                if !e.source.is_empty() {
                    obj["source"] = Value::String(e.source.clone());
                }
                obj
            })
            .collect();
        let Ok(bytes) = serde_json::to_vec_pretty(&json!({ "extensions": list })) else {
            return false;
        };
        write_file_atomic(&self.folder.join("index.json"), &bytes).is_ok()
    }

    pub fn list(&mut self) -> &[ExtensionEntry] {
        self.load_index();
        &self.entries
    }

    /// Installs an unpacked extension directory. Returns the extension name on success.
    pub fn install_folder(&mut self, source_dir: &Path) -> Result<String, String> {
        self.load_index();
        let manifest = read_extension_manifest(source_dir);
        if !manifest.error.is_empty() {
            return Err(manifest.error);
        }
        check_size(source_dir)?;
        let staged = self.staging_path("dir");
        if let Err(err) = copy_dir_no_symlinks(source_dir, &staged) {
            let _ = fs::remove_dir_all(&staged);
            return Err(format!("复制扩展文件失败: {err}"));
        }
        self.finalize_install(&staged, "")
    }

    /// Installs a `.crx` or `.zip` extension archive. Returns the extension name on success.
    pub fn install_package(&mut self, file: &Path, source: &str) -> Result<String, String> {
        self.load_index();
        let meta = fs::metadata(file).map_err(|_| "无法读取扩展文件".to_string())?;
        if meta.len() > MAX_BYTES {
            return Err("扩展文件超过 200 MB".to_string());
        }
        let bytes = fs::read(file).map_err(|_| "无法读取扩展文件".to_string())?;
        let Some(offset) = crx_zip_offset(&bytes) else {
            return Err("不是 .crx 或 .zip 扩展包".to_string());
        };
        let _ = fs::create_dir_all(&self.folder);
        let zip_path = self.staging_path("zip").with_extension("zip");
        fs::write(&zip_path, &bytes[offset..]).map_err(|_| "无法写入临时文件".to_string())?;
        let staged = self.staging_path("pkg");
        let _ = fs::create_dir_all(&staged);
        let unpacked = unpack_zip(&zip_path, &staged);
        let _ = fs::remove_file(&zip_path);
        if !unpacked {
            let _ = fs::remove_dir_all(&staged);
            return Err("解压扩展包失败".to_string());
        }

        let mut root = staged.clone();
        if !staged.join("manifest.json").exists() {
            if let Ok(rd) = fs::read_dir(&staged) {
                let dirs: Vec<PathBuf> = rd
                    .flatten()
                    .map(|e| e.path())
                    .filter(|p| p.is_dir())
                    .collect();
                if dirs.len() == 1 && dirs[0].join("manifest.json").exists() {
                    root = dirs[0].clone();
                }
            }
        }

        if let Err(e) = check_size(&root) {
            let _ = fs::remove_dir_all(&staged);
            return Err(e);
        }
        if root == staged {
            self.finalize_install(&staged, source)
        } else {
            let inner = self.staging_path("dir");
            let renamed = fs::rename(&root, &inner);
            let _ = fs::remove_dir_all(&staged);
            if renamed.is_err() {
                return Err("整理扩展文件失败".to_string());
            }
            self.finalize_install(&inner, source)
        }
    }

    fn finalize_install(&mut self, staged: &Path, source: &str) -> Result<String, String> {
        let manifest = read_extension_manifest(staged);
        if !manifest.error.is_empty() {
            let _ = fs::remove_dir_all(staged);
            return Err(manifest.error);
        }

        let existing_dirs: Vec<String> = self
            .entries
            .iter()
            .filter(|e| e.name == manifest.name || (!source.is_empty() && e.source == source))
            .map(|e| e.dir.clone())
            .collect();
        for dir in existing_dirs {
            self.remove(&dir);
        }

        if self.entries.len() >= MAX_EXTENSIONS {
            let _ = fs::remove_dir_all(staged);
            return Err("扩展太多了（最多 50 个）".to_string());
        }

        let base = slug(&manifest.name);
        let mut dir = base.clone();
        let mut n = 2;
        while self.folder.join(&dir).exists() {
            dir = format!("{base}-{n}");
            n += 1;
        }
        if fs::rename(staged, self.folder.join(&dir)).is_err() {
            let _ = fs::remove_dir_all(staged);
            return Err("无法保存扩展".to_string());
        }

        self.entries.push(ExtensionEntry {
            dir,
            id: String::new(),
            name: manifest.name.clone(),
            version: manifest.version,
            description: manifest.description,
            popup: manifest.popup,
            options: manifest.options,
            enabled: true,
            proxy: manifest.proxy,
            source: source.to_string(),
            error: String::new(),
        });
        self.save_index();
        Ok(manifest.name)
    }

    pub fn set_enabled(&mut self, dir: &str, enabled: bool) -> bool {
        self.load_index();
        let Some(entry) = self.entries.iter_mut().find(|e| e.dir == dir) else {
            return false;
        };
        entry.enabled = enabled;
        self.save_index();
        true
    }

    pub fn remove(&mut self, dir: &str) -> bool {
        self.load_index();
        let Some(pos) = self.entries.iter().position(|e| e.dir == dir) else {
            return false;
        };
        self.entries.remove(pos);
        self.save_index();
        let target = self.folder.join(dir);
        let trash = self.folder.join(format!(".trash-{dir}-{}", now_ticks()));
        if fs::rename(&target, &trash).is_ok() {
            let _ = fs::remove_dir_all(&trash);
        } else {
            let _ = fs::remove_dir_all(&target);
        }
        true
    }

    pub fn mark_runtime_status(&mut self, dir: &str, runtime_id: &str, error: &str) {
        self.load_index();
        if let Some(entry) = self.entries.iter_mut().find(|e| e.dir == dir) {
            entry.id = runtime_id.to_string();
            entry.error = error.to_string();
        }
    }

    pub fn page_url(&mut self, dir: &str, options: bool) -> Option<String> {
        self.load_index();
        let entry = self.entries.iter().find(|e| e.dir == dir)?;
        if entry.id.is_empty() {
            return None;
        }
        let page = if options {
            &entry.options
        } else {
            &entry.popup
        };
        if page.is_empty() {
            return None;
        }
        Some(format!("chrome-extension://{}/{page}", entry.id))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;

    fn temp_dir(tag: &str) -> PathBuf {
        let p = std::env::temp_dir().join(format!(
            "ulb-ext-test-{tag}-{}-{}",
            std::process::id(),
            now_ticks()
        ));
        let _ = fs::remove_dir_all(&p);
        fs::create_dir_all(&p).unwrap();
        p
    }

    #[test]
    fn install_folder_enable_disable_and_remove() {
        let root = temp_dir("folder");
        let src = root.join("src-ext");
        fs::create_dir_all(&src).unwrap();
        let mut f = fs::File::create(src.join("manifest.json")).unwrap();
        writeln!(
            f,
            r#"{{"manifest_version":3,"name":"Sample Ext","version":"1.2.3","action":{{"default_popup":"popup.html"}}}}"#
        )
        .unwrap();

        let mut store = ExtensionsStore::new(&root);
        let name = store.install_folder(&src).unwrap();
        assert_eq!(name, "Sample Ext");
        assert_eq!(store.list().len(), 1);
        assert_eq!(store.list()[0].dir, "sample-ext");
        assert!(store.set_enabled("sample-ext", false));
        assert!(!store.list()[0].enabled);

        store.mark_runtime_status("sample-ext", "abcdef123456", "");
        assert_eq!(
            store.page_url("sample-ext", false).as_deref(),
            Some("chrome-extension://abcdef123456/popup.html")
        );

        let mut reloaded = ExtensionsStore::new(&root);
        assert_eq!(reloaded.list().len(), 1);
        assert!(!reloaded.list()[0].enabled);

        assert!(reloaded.remove("sample-ext"));
        assert!(reloaded.list().is_empty());
        let _ = fs::remove_dir_all(&root);
    }
}
