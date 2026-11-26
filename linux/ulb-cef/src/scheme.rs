use crate::ffi::{read_cef_str, take_userfree_str, CefObject, CefStr, sys};
use std::ffi::c_void;
use std::os::raw::c_int;
use std::path::PathBuf;
use std::sync::{Arc, Mutex};
use ulb_core::internal_pages::{lookup_asset, page_name};

pub type RpcCallback = Arc<Mutex<Box<dyn FnMut(&str) -> String + Send>>>;

/// JS shim injected ahead of `ui.js` inside `https://ulb.internal/*.html` pages so existing UI pages
/// seamlessly communicate over `POST https://ulb.internal/__rpc`.
const BRIDGE_JS: &str = r#"(function(){
  if (window.chrome && window.chrome.webview) return;
  const listeners = new Set();
  window.chrome = window.chrome || {};
  window.chrome.webview = {
    addEventListener(type, fn) { if (type === "message" && typeof fn === "function") listeners.add(fn); },
    removeEventListener(type, fn) { if (type === "message") listeners.delete(fn); },
    postMessage(msg) {
      fetch("https://ulb.internal/__rpc", {
        method: "POST",
        headers: { "content-type": "application/json" },
        body: JSON.stringify(msg)
      }).then(r => r.json()).then(resp => {
        const ev = { data: resp };
        listeners.forEach(fn => { try { fn(ev); } catch (e) {} });
      }).catch(err => {
        if (msg && msg.id) {
          const ev = { data: { id: msg.id, error: String(err) } };
          listeners.forEach(fn => { try { fn(ev); } catch (e) {} });
        }
      });
    }
  };
})();"#;

struct ResourceData {
    status: c_int,
    mime: String,
    body: Mutex<ResourceCursor>,
}

struct ResourceCursor {
    bytes: Vec<u8>,
    offset: usize,
}

/// Allocates a static in-memory `cef_resource_handler_t` returning `status`, `mime`, and `bytes`.
pub fn make_static_resource_handler(
    status: c_int,
    mime: &str,
    bytes: Vec<u8>,
) -> *mut sys::cef_resource_handler_t {
    CefObject::new(
        |h: &mut sys::cef_resource_handler_t| {
            h.open = Some(res_open);
            h.get_response_headers = Some(res_headers);
            h.read = Some(res_read);
        },
        ResourceData {
            status,
            mime: mime.to_string(),
            body: Mutex::new(ResourceCursor { bytes, offset: 0 }),
        },
    )
}

unsafe extern "C" fn res_open(
    _self_: *mut sys::cef_resource_handler_t,
    _request: *mut sys::cef_request_t,
    handle_request: *mut c_int,
    _callback: *mut sys::cef_callback_t,
) -> c_int {
    if !handle_request.is_null() {
        *handle_request = 1;
    }
    1
}

unsafe extern "C" fn res_headers(
    self_: *mut sys::cef_resource_handler_t,
    response: *mut sys::cef_response_t,
    response_length: *mut i64,
    _redirect_url: *mut sys::cef_string_t,
) {
    let data: &ResourceData = CefObject::state_from(self_);
    if !response.is_null() {
        if let Some(set_status) = (*response).set_status {
            set_status(response, data.status);
        }
        let mime_only = data.mime.split(';').next().unwrap_or("text/plain").trim();
        let cs = CefStr::from_str(mime_only);
        if let Some(set_mime) = (*response).set_mime_type {
            set_mime(response, cs.as_ptr());
        }
        let cc_k = CefStr::from_str("Cache-Control");
        let cc_v = CefStr::from_str("no-store");
        if let Some(set_hdr) = (*response).set_header_by_name {
            set_hdr(response, cc_k.as_ptr(), cc_v.as_ptr(), 1);
        }
    }
    if !response_length.is_null() {
        *response_length = data.body.lock().unwrap().bytes.len() as i64;
    }
}

unsafe extern "C" fn res_read(
    self_: *mut sys::cef_resource_handler_t,
    data_out: *mut c_void,
    bytes_to_read: c_int,
    bytes_read: *mut c_int,
    _callback: *mut sys::cef_resource_read_callback_t,
) -> c_int {
    let data: &ResourceData = CefObject::state_from(self_);
    let mut cur = data.body.lock().unwrap();
    let avail = cur.bytes.len().saturating_sub(cur.offset);
    if avail == 0 || bytes_to_read <= 0 || data_out.is_null() {
        if !bytes_read.is_null() {
            *bytes_read = 0;
        }
        return 0;
    }
    let n = avail.min(bytes_to_read as usize);
    std::ptr::copy_nonoverlapping(
        cur.bytes.as_ptr().add(cur.offset),
        data_out.cast::<u8>(),
        n,
    );
    cur.offset += n;
    if !bytes_read.is_null() {
        *bytes_read = n as c_int;
    }
    1
}

/// Shared state for `https://ulb.internal/` and `https://f8964.internal/` scheme factories.
#[derive(Clone)]
pub struct InternalSchemeState {
    pub rpc: RpcCallback,
    pub memorial_dir: PathBuf,
}

pub fn create_internal_scheme_factory(
    state: InternalSchemeState,
) -> *mut sys::cef_scheme_handler_factory_t {
    CefObject::new(
        |f: &mut sys::cef_scheme_handler_factory_t| {
            f.create = Some(factory_create);
        },
        state,
    )
}

unsafe fn read_post_body(request: *mut sys::cef_request_t) -> String {
    let Some(get_post) = (*request).get_post_data else {
        return String::new();
    };
    let post = get_post(request);
    if post.is_null() {
        return String::new();
    }
    let mut result = String::new();
    let mut count = (*post).get_element_count.map(|f| f(post)).unwrap_or(0);
    if count > 0 {
        if let Some(get_els) = (*post).get_elements {
            let mut elements: Vec<*mut sys::cef_post_data_element_t> =
                vec![std::ptr::null_mut(); count];
            get_els(post, &mut count, elements.as_mut_ptr());
            for el in elements.into_iter().take(count) {
                if el.is_null() {
                    continue;
                }
                let size = (*el).get_bytes_count.map(|f| f(el)).unwrap_or(0);
                if size > 0 && size <= 4 * 1024 * 1024 {
                    let mut buf = vec![0u8; size];
                    if let Some(get_b) = (*el).get_bytes {
                        let read = get_b(el, size, buf.as_mut_ptr().cast::<c_void>());
                        buf.truncate(read);
                        result.push_str(&String::from_utf8_lossy(&buf));
                    }
                }
                crate::ffi::cef_release(el);
            }
        }
    }
    crate::ffi::cef_release(post);
    result
}

unsafe extern "C" fn factory_create(
    self_: *mut sys::cef_scheme_handler_factory_t,
    _browser: *mut sys::cef_browser_t,
    _frame: *mut sys::cef_frame_t,
    _scheme_name: *const sys::cef_string_t,
    request: *mut sys::cef_request_t,
) -> *mut sys::cef_resource_handler_t {
    if request.is_null() {
        return std::ptr::null_mut();
    }
    let state: &InternalSchemeState = CefObject::state_from(self_);
    let url = (*request)
        .get_url
        .map(|f| take_userfree_str(f(request)))
        .unwrap_or_default();

    if url.starts_with("https://f8964.internal") {
        let rel = url
            .split("://f8964.internal/")
            .nth(1)
            .unwrap_or("index.html")
            .split(['?', '#'])
            .next()
            .unwrap_or("index.html");
        let clean = if rel.is_empty() { "index.html" } else { rel };
        if !clean.contains("..") {
            let candidate = state.memorial_dir.join(clean);
            if let Ok(bytes) = std::fs::read(&candidate) {
                let mime = if clean.ends_with(".css") {
                    "text/css"
                } else if clean.ends_with(".js") {
                    "application/javascript"
                } else if clean.ends_with(".png") {
                    "image/png"
                } else if clean.ends_with(".jpg") || clean.ends_with(".jpeg") {
                    "image/jpeg"
                } else if clean.ends_with(".svg") {
                    "image/svg+xml"
                } else {
                    "text/html"
                };
                return make_static_resource_handler(200, mime, bytes);
            }
        }
        let fallback = "<!doctype html><meta charset=utf-8><body style='background:#151619;color:#ddd;font:15px/1.6 sans-serif;padding:48px'><h2>离线档案包未随此开发树打包</h2><p>将 <code>f8964.pack</code> 置于程序目录或数据目录后即可离线阅读完整资料。</p></body>";
        return make_static_resource_handler(200, "text/html", fallback.as_bytes().to_vec());
    }

    let path = page_name(&url).unwrap_or_default();
    if path == "__rpc" {
        let req_json = read_post_body(request);
        let resp = state.rpc.lock().unwrap()(req_json.as_str());
        return make_static_resource_handler(200, "application/json", resp.into_bytes());
    }
    if path == "__bridge.js" {
        return make_static_resource_handler(
            200,
            "application/javascript",
            BRIDGE_JS.as_bytes().to_vec(),
        );
    }

    if let Some((bytes, mime)) = lookup_asset(&url) {
        if mime.starts_with("text/html") {
            let html = String::from_utf8_lossy(bytes);
            let patched = html.replacen(
                "<script src=\"ui.js\">",
                "<script src=\"__bridge.js\"></script><script src=\"ui.js\">",
                1,
            );
            return make_static_resource_handler(200, mime, patched.into_bytes());
        }
        return make_static_resource_handler(200, mime, bytes.to_vec());
    }

    let _ = read_cef_str(std::ptr::null());
    make_static_resource_handler(404, "text/plain", b"Not Found".to_vec())
}
