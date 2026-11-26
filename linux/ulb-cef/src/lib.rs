pub mod app;
pub mod browser;
pub mod ffi;
pub mod scheme;
pub mod stealth_headers;

pub use app::{shared_rpc_stub, try_execute_subprocess, CefConfig, CefRuntime};
pub use browser::{BrowserEvent, BrowserPolicy, CefTabView};
pub use scheme::RpcCallback;

#[cfg(test)]
mod tests {
    use super::ffi::sys;
    use std::ffi::CStr;

    #[test]
    fn links_system_cef_and_matches_api_hash() {
        let ver: i32 = env!("ULB_CEF_API_VERSION").parse().unwrap();
        let ptr = unsafe { sys::cef_api_hash(ver, 0) };
        assert!(!ptr.is_null());
        let hash = unsafe { CStr::from_ptr(ptr) }.to_str().unwrap();
        assert_eq!(hash.len(), 40, "expected 40-char SHA1 CEF API hash, got {hash}");
    }
}
