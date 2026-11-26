/// Built-in `https://ulb.internal/` assets, self-test pages, and diagnostic scripts.
pub const INTERNAL_ORIGIN: &str = "https://ulb.internal/";
pub const INTERNAL_HOST: &str = "ulb.internal";

struct AssetEntry {
    path: &'static str,
    mime: &'static str,
    bytes: &'static [u8],
}

const ASSETS: &[AssetEntry] = &[
    AssetEntry { path: "ui.css", mime: "text/css; charset=utf-8", bytes: include_bytes!("../../../ui/ui.css") },
    AssetEntry { path: "ui.js", mime: "application/javascript; charset=utf-8", bytes: include_bytes!("../../../ui/ui.js") },
    AssetEntry { path: "start.html", mime: "text/html; charset=utf-8", bytes: include_bytes!("../../../ui/start.html") },
    AssetEntry { path: "start.css", mime: "text/css; charset=utf-8", bytes: include_bytes!("../../../ui/start.css") },
    AssetEntry { path: "start.js", mime: "application/javascript; charset=utf-8", bytes: include_bytes!("../../../ui/start.js") },
    AssetEntry { path: "history.html", mime: "text/html; charset=utf-8", bytes: include_bytes!("../../../ui/history.html") },
    AssetEntry { path: "history.js", mime: "application/javascript; charset=utf-8", bytes: include_bytes!("../../../ui/history.js") },
    AssetEntry { path: "bookmarks.html", mime: "text/html; charset=utf-8", bytes: include_bytes!("../../../ui/bookmarks.html") },
    AssetEntry { path: "bookmarks.js", mime: "application/javascript; charset=utf-8", bytes: include_bytes!("../../../ui/bookmarks.js") },
    AssetEntry { path: "settings.html", mime: "text/html; charset=utf-8", bytes: include_bytes!("../../../ui/settings.html") },
    AssetEntry { path: "settings.js", mime: "application/javascript; charset=utf-8", bytes: include_bytes!("../../../ui/settings.js") },
    AssetEntry { path: "overview.html", mime: "text/html; charset=utf-8", bytes: include_bytes!("../../../ui/overview.html") },
    AssetEntry { path: "overview.js", mime: "application/javascript; charset=utf-8", bytes: include_bytes!("../../../ui/overview.js") },
    AssetEntry { path: "sidebar.html", mime: "text/html; charset=utf-8", bytes: include_bytes!("../../../ui/sidebar.html") },
    AssetEntry { path: "sidebar.js", mime: "application/javascript; charset=utf-8", bytes: include_bytes!("../../../ui/sidebar.js") },
    AssetEntry { path: "reader.html", mime: "text/html; charset=utf-8", bytes: include_bytes!("../../../ui/reader.html") },
    AssetEntry { path: "reader.css", mime: "text/css; charset=utf-8", bytes: include_bytes!("../../../ui/reader.css") },
    AssetEntry { path: "reader.js", mime: "application/javascript; charset=utf-8", bytes: include_bytes!("../../../ui/reader.js") },
    AssetEntry { path: "reader-extract.js", mime: "application/javascript; charset=utf-8", bytes: include_bytes!("../../../ui/reader-extract.js") },
    AssetEntry { path: "find.js", mime: "application/javascript; charset=utf-8", bytes: include_bytes!("../../../ui/find.js") },
    AssetEntry { path: "vendor/Readability.js", mime: "application/javascript; charset=utf-8", bytes: include_bytes!("../../../ui/vendor/Readability.js") },
    AssetEntry { path: "vendor/Readability-readerable.js", mime: "application/javascript; charset=utf-8", bytes: include_bytes!("../../../ui/vendor/Readability-readerable.js") },
];

pub const SELF_TEST_URL: &str = "https://ulb.internal/selftest.html";

pub const SELF_TEST_HTML: &str = r#"<!doctype html><meta charset="utf-8"><title>UltraLight Self-Test</title>
<pre id="out">running...</pre>
<video id="v" width="320" height="180" muted playsinline style="display:block"></video>
<canvas id="gl" width="16" height="16"></canvas>
<script>
const out = document.getElementById("out");
window.__results = {};
async function run() {
  const r = {};
  r.userAgent = navigator.userAgent;
  r.platform = navigator.platform;
  r.vendor = navigator.vendor;
  r.webdriver = navigator.webdriver;
  r.hardwareConcurrency = navigator.hardwareConcurrency;
  r.deviceMemory = navigator.deviceMemory;
  r.languages = navigator.languages;
  r.scrollbarWidth = (() => {
    const d = document.createElement("div");
    d.style.cssText = "width:100px;height:100px;overflow:scroll;position:absolute;top:-9999px";
    document.body.appendChild(d);
    const w = d.offsetWidth - d.clientWidth;
    d.remove();
    return w;
  })();
  try {
    const gl = document.getElementById("gl").getContext("webgl");
    const dbg = gl && gl.getExtension("WEBGL_debug_renderer_info");
    r.webglVendor = dbg ? gl.getParameter(dbg.UNMASKED_VENDOR_WEBGL) : null;
    r.webglRenderer = dbg ? gl.getParameter(dbg.UNMASKED_RENDERER_WEBGL) : null;
  } catch (e) { r.webglError = String(e); }
  try {
    if (navigator.gpu) {
      const ad = await navigator.gpu.requestAdapter();
      r.webgpu = ad ? { vendor: ad.info && ad.info.vendor, arch: ad.info && ad.info.architecture } : null;
    } else { r.webgpu = "unavailable"; }
  } catch (e) { r.webgpuError = String(e); }
  try {
    r.uach = navigator.userAgentData ? await navigator.userAgentData.getHighEntropyValues(["platform", "platformVersion", "architecture", "bitness", "model", "uaFullVersion", "fullVersionList"]) : null;
  } catch (e) { r.uachError = String(e); }
  try {
    const blob = new Blob([`postMessage({ua:navigator.userAgent,pl:navigator.platform})`], {type:"text/javascript"});
    r.worker = await new Promise((res) => { const w = new Worker(URL.createObjectURL(blob)); w.onmessage = e => res(e.data); w.onerror = e => res({err:e.message}); setTimeout(() => res({timeout:true}), 3000); });
  } catch (e) { r.workerError = String(e); }
  try {
    const mc = MediaSource;
    r.hevc = mc.isTypeSupported('video/mp4; codecs="hvc1.1.6.L93.B0"');
    r.h264 = mc.isTypeSupported('video/mp4; codecs="avc1.42E01E"');
    r.av1  = mc.isTypeSupported('video/mp4; codecs="av01.0.04M.08"');
  } catch (e) { r.mseError = String(e); }
  window.__results = r;
  out.textContent = JSON.stringify(r, null, 2);
}
run();
</script>"#;

pub const VIDEO_DIAGNOSTICS_JS: &str = r#"(function(){
  if (window.__ulbVideoDiagInstalled) return;
  window.__ulbVideoDiagInstalled = true;
  var lastReport = 0;
  function check() {
    var now = Date.now();
    if (now - lastReport < 2000) return;
    lastReport = now;
    var v = document.querySelector('video');
    if (!v || typeof v.getVideoPlaybackQuality !== 'function') return;
    var q = v.getVideoPlaybackQuality();
    if (!q || q.totalVideoFrames < 10) return;
    if (q.droppedVideoFrames > 0) {
      var ratio = q.droppedVideoFrames / q.totalVideoFrames;
      if (ratio > 0.05) {
        console.debug('[ULB-Video] dropped frames:', q.droppedVideoFrames, '/', q.totalVideoFrames);
      }
    }
  }
  setInterval(check, 2500);
})();"#;

/// Returns `true` for URLs belonging to `https://ulb.internal/...`.
pub fn is_internal_url(url: &str) -> bool {
    let lower = url.to_ascii_lowercase();
    lower == "https://ulb.internal" || lower.starts_with(INTERNAL_ORIGIN)
}

/// Constructs an internal page URL for the given relative page path (e.g. `"start.html"`).
pub fn internal_url(page: &str) -> String {
    format!("{}{}", INTERNAL_ORIGIN, page.trim_start_matches('/'))
}

/// Extracts the normalized asset path from a `https://ulb.internal/...` URL.
pub fn page_name(url: &str) -> Option<String> {
    if !is_internal_url(url) {
        return None;
    }
    let after = url
        .get(INTERNAL_ORIGIN.len()..)
        .unwrap_or("");
    let cut = after.find(&['?', '#'][..]).unwrap_or(after.len());
    let path = &after[..cut];
    if path.is_empty() {
        Some("start.html".to_string())
    } else {
        Some(path.to_string())
    }
}

/// Looks up an embedded internal asset or self-test HTML page by path or full URL.
pub fn lookup_asset(path_or_url: &str) -> Option<(&'static [u8], &'static str)> {
    let rel = if path_or_url.starts_with("http://") || path_or_url.starts_with("https://") {
        page_name(path_or_url)?
    } else {
        let cut = path_or_url.find(&['?', '#'][..]).unwrap_or(path_or_url.len());
        let trimmed = path_or_url[..cut].trim_start_matches('/');
        if trimmed.is_empty() {
            "start.html".to_string()
        } else {
            trimmed.to_string()
        }
    };

    if rel == "selftest.html" {
        return Some((SELF_TEST_HTML.as_bytes(), "text/html; charset=utf-8"));
    }

    ASSETS
        .iter()
        .find(|a| a.path == rel)
        .map(|a| (a.bytes, a.mime))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn resolves_all_ui_assets_and_selftest() {
        assert!(is_internal_url("https://ulb.internal/start.html"));
        assert!(!is_internal_url("https://example.com/start.html"));
        assert_eq!(page_name("https://ulb.internal/"), Some("start.html".to_string()));
        assert_eq!(
            page_name("https://ulb.internal/bookmarks.html?tab=reading#top"),
            Some("bookmarks.html".to_string())
        );

        let expected = [
            "ui.css",
            "ui.js",
            "start.html",
            "start.css",
            "start.js",
            "history.html",
            "history.js",
            "bookmarks.html",
            "bookmarks.js",
            "settings.html",
            "settings.js",
            "overview.html",
            "overview.js",
            "sidebar.html",
            "sidebar.js",
            "reader.html",
            "reader.css",
            "reader.js",
            "reader-extract.js",
            "find.js",
            "vendor/Readability.js",
            "vendor/Readability-readerable.js",
            "selftest.html",
        ];
        for path in expected {
            let (bytes, mime) = lookup_asset(&internal_url(path))
                .unwrap_or_else(|| panic!("missing internal asset: {path}"));
            assert!(!bytes.is_empty(), "{path} should not be empty");
            assert!(!mime.is_empty());
        }
        assert!(lookup_asset("https://ulb.internal/nonexistent.js").is_none());
    }
}
