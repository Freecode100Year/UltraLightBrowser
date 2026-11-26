fn main() {
    let lib_dir = std::env::var("CEF_LIB_DIR").unwrap_or_else(|_| "/usr/lib/cef".to_string());
    println!("cargo::rerun-if-env-changed=CEF_LIB_DIR");
    println!("cargo::rustc-link-arg=-Wl,-rpath,{lib_dir}");
}
