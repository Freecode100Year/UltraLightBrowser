use std::env;
use std::fs;
use std::path::{Path, PathBuf};

fn read_api_version_last(include_root: &Path) -> u32 {
    let header = include_root.join("include").join("cef_api_versions.h");
    let contents = fs::read_to_string(&header)
        .unwrap_or_else(|e| panic!("failed to read {}: {e}", header.display()));
    contents
        .lines()
        .find_map(|line| {
            line.strip_prefix("#define CEF_API_VERSION_LAST ")?
                .trim()
                .strip_prefix("CEF_API_VERSION_")?
                .parse::<u32>()
                .ok()
        })
        .unwrap_or_else(|| panic!("missing CEF_API_VERSION_LAST in {}", header.display()))
}

fn collect_capi_headers(include_root: &Path) -> Vec<PathBuf> {
    let mut headers = Vec::new();
    let capi = include_root.join("include").join("capi");
    for dir in [capi.clone(), capi.join("views")] {
        if let Ok(rd) = fs::read_dir(&dir) {
            for entry in rd.flatten() {
                let path = entry.path();
                if path.extension().and_then(|s| s.to_str()) == Some("h") {
                    headers.push(path);
                }
            }
        }
    }
    headers.sort();
    headers
}

fn main() {
    let include_root = PathBuf::from(
        env::var("CEF_INCLUDE_DIR").unwrap_or_else(|_| "/usr/include/cef".to_string()),
    );
    let lib_dir =
        PathBuf::from(env::var("CEF_LIB_DIR").unwrap_or_else(|_| "/usr/lib/cef".to_string()));

    println!("cargo::rerun-if-env-changed=CEF_INCLUDE_DIR");
    println!("cargo::rerun-if-env-changed=CEF_LIB_DIR");
    println!(
        "cargo::rerun-if-changed={}",
        include_root.join("include/cef_version.h").display()
    );
    println!(
        "cargo::rerun-if-changed={}",
        include_root.join("include/cef_api_versions.h").display()
    );

    assert!(
        lib_dir.join("libcef.so").exists(),
        "system CEF library not found at {}/libcef.so (install Arch package `cef`)",
        lib_dir.display()
    );

    let api_version = read_api_version_last(&include_root);
    println!("cargo::rustc-env=ULB_CEF_API_VERSION={api_version}");
    println!("cargo::rustc-env=ULB_CEF_LIB_DIR={}", lib_dir.display());
    println!("cargo::rustc-link-search=native={}", lib_dir.display());
    println!("cargo::rustc-link-lib=dylib=cef");
    println!("cargo::rustc-link-arg=-Wl,-rpath,{}", lib_dir.display());

    let out_dir = PathBuf::from(env::var("OUT_DIR").expect("OUT_DIR"));
    let umbrella_path = out_dir.join("cef_umbrella.h");

    let headers = collect_capi_headers(&include_root);
    let mut umbrella = String::new();
    umbrella.push_str("#define CEF_API_VERSION ");
    umbrella.push_str(&api_version.to_string());
    umbrella.push('\n');
    umbrella.push_str("#include \"include/cef_version.h\"\n");
    umbrella.push_str("#include \"include/cef_api_hash.h\"\n");
    for h in &headers {
        if let Ok(rel) = h.strip_prefix(&include_root) {
            umbrella.push_str(&format!("#include \"{}\"\n", rel.display()));
        }
    }
    fs::write(&umbrella_path, &umbrella).expect("write umbrella header");

    let bindings = bindgen::Builder::default()
        .header(umbrella_path.to_string_lossy())
        .clang_arg(format!("-I{}", include_root.display()))
        .clang_arg(format!("-DCEF_API_VERSION={api_version}"))
        .parse_callbacks(Box::new(bindgen::CargoCallbacks::new()))
        .derive_default(true)
        .layout_tests(false)
        .allowlist_function("cef_.*")
        .allowlist_type("cef_.*")
        .allowlist_var("CEF_.*")
        .allowlist_var("cef_.*")
        .allowlist_type(".*_type_t")
        .generate()
        .expect("generate CEF C API bindings");

    bindings
        .write_to_file(out_dir.join("cef_bindings.rs"))
        .expect("write cef_bindings.rs");
}
