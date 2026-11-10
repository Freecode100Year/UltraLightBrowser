use std::ffi::c_char;
use std::slice;
use std::str;

const DOMAINS_TEXT: &str = include_str!("../domains.txt");
const KEYWORDS: &[&str] = &[
    "/pagead/js/",
    "/pagead/show_ads.js",
    "/advert.js",
    "/ads.js",
    "/google-analytics.com/analytics.js",
    "/gtag/js?id=",
    "/gtm.js?id=",
    "/hm.js?",
    "/beacon.js",
    "/pixel.gif",
    "/pixel.png",
];
const HOST_PATHS: &[(&str, &str)] = &[
    ("facebook.com", "/tr"),
    ("google.com", "/pagead/"),
    ("youtube.com", "/pagead/"),
    ("youtube.com", "/api/stats/ads"),
    ("youtube.com", "/ptracking"),
    ("t.co", "/i/adsct"),
    ("twitter.com", "/i/adsct"),
    ("x.com", "/i/adsct"),
];
const MULTI_TENANT: &[&str] = &[
    "github.io",
    "gitlab.io",
    "gitee.io",
    "blogspot.com",
    "wordpress.com",
    "appspot.com",
    "pages.dev",
    "workers.dev",
    "r2.dev",
    "trycloudflare.com",
    "vercel.app",
    "netlify.app",
    "web.app",
    "firebaseapp.com",
    "herokuapp.com",
    "azurewebsites.net",
    "azureedge.net",
    "cloudfront.net",
    "fastly.net",
    "fly.dev",
    "render.com",
    "onrender.com",
    "myshopify.com",
    "railway.app",
    "glitch.me",
    "surge.sh",
];

fn domains() -> Vec<&'static str> {
    DOMAINS_TEXT.lines().filter(|s| !s.is_empty()).collect()
}
fn under(host: &str, domain: &str) -> bool {
    host == domain || host.strip_suffix(domain).is_some_and(|p| p.ends_with('.'))
}
fn web_scheme(uri: &str) -> bool {
    uri.split_once("://").is_some_and(|(s, _)| {
        matches!(
            s.to_ascii_lowercase().as_str(),
            "http" | "https" | "ws" | "wss"
        )
    })
}
fn lower_path(uri: &str) -> String {
    let Some((_, rest)) = uri.split_once("://") else {
        return "/".into();
    };
    let authority_end = rest.find(['/', '?', '#']).unwrap_or(rest.len());
    let tail = &rest[authority_end..];
    if tail.is_empty() || tail.starts_with('#') {
        return "/".into();
    }
    let end = tail.find('#').unwrap_or(tail.len());
    let mut p = tail[..end].to_ascii_lowercase();
    if p.starts_with('?') {
        p.insert(0, '/');
    }
    p
}
fn extract_host(uri: &str) -> String {
    let Some((_, rest)) = uri.split_once("://") else {
        return String::new();
    };
    let end = rest.find(['/', '?', '#']).unwrap_or(rest.len());
    let authority = &rest[..end];
    let authority = authority.rsplit_once('@').map_or(authority, |(_, h)| h);
    let mut host = if authority.starts_with('[') {
        authority.find(']').map_or(authority, |i| &authority[..=i])
    } else {
        authority.split(':').next().unwrap_or("")
    }
    .to_ascii_lowercase();
    while host.ends_with('.') {
        host.pop();
    }
    host
}
fn base_domain(host: &str) -> String {
    if host.is_empty() || host.starts_with('[') {
        return host.to_string();
    }
    if host.bytes().all(|c| c == b'.' || c.is_ascii_digit()) {
        return host.to_string();
    }
    let p: Vec<_> = host.split('.').collect();
    if p.len() <= 2 {
        return host.to_string();
    }
    let tld = p[p.len() - 1];
    let sld = p[p.len() - 2];
    let public = if tld.len() == 2 {
        matches!(
            sld,
            "com" | "net" | "org" | "gov" | "gob" | "edu" | "co" | "ne" | "ac" | "go" | "or" | "re"
        )
    } else {
        false
    };
    let take = if public || MULTI_TENANT.contains(&format!("{sld}.{tld}").as_str()) {
        3
    } else {
        2
    };
    p[p.len() - take..].join(".")
}
fn third_party(req: &str, top: &str) -> bool {
    req.is_empty() || top.is_empty() || (req != top && base_domain(req) != base_domain(top))
}
fn match_host(host: &str) -> Option<&'static str> {
    let ds = domains();
    ds.into_iter().find(|d| under(host, d))
}
fn match_host_path(host: &str, path: &str) -> bool {
    HOST_PATHS.iter().any(|(domain, prefix)| {
        if !under(host, domain) || !path.starts_with(prefix) {
            return false;
        }
        prefix.ends_with('/')
            || path.len() == prefix.len()
            || path
                .as_bytes()
                .get(prefix.len())
                .is_some_and(|c| *c == b'/' || *c == b'?')
    })
}
fn surrogate(host: &str, path: &str) -> i32 {
    if under(host, "google-analytics.com") && path.starts_with("/analytics.js") {
        1
    } else if under(host, "googletagmanager.com")
        && (path.starts_with("/gtag/js") || path.starts_with("/gtm.js"))
    {
        2
    } else if (under(host, "googletagservices.com") || under(host, "doubleclick.net"))
        && path.starts_with("/tag/js/gpt.js")
    {
        3
    } else if under(host, "googlesyndication.com") && path.starts_with("/pagead/js/adsbygoogle.js")
    {
        4
    } else {
        0
    }
}
fn evaluate(uri: &str, top: &str, kind: i32) -> i32 {
    if !web_scheme(uri) {
        return 0;
    }
    let host = extract_host(uri);
    if host.is_empty() {
        return 0;
    }
    if (kind == 1 || kind == 0) && !top.is_empty() && !third_party(&host, top) {
        return 0;
    }
    let path = lower_path(uri);
    let blocked = match_host(&host).is_some_and(|d| top.is_empty() || !under(top, d))
        || match_host_path(&host, &path)
        || KEYWORDS.iter().any(|k| path.contains(k));
    if !blocked {
        return 0;
    }
    1 | if kind == 2 {
        surrogate(&host, &path) << 8
    } else {
        0
    }
}
fn request_filter_patterns() -> Vec<String> {
    let ds = domains();
    let mut out = Vec::new();
    for d in &ds {
        let mut covered = false;
        for i in d.match_indices('.').map(|(i, _)| i) {
            let parent = &d[i + 1..];
            if parent.contains('.') && ds.contains(&parent) {
                covered = true;
                break;
            }
        }
        if !covered {
            out.push(format!("*://*{d}/*"));
        }
    }
    for (h, p) in HOST_PATHS {
        out.push(format!("*://*{h}{p}*"));
    }
    for k in KEYWORDS {
        out.push(format!("*{k}*"));
    }
    out.sort();
    out
}
#[cfg(test)]
fn glob_match(p: &str, s: &str) -> bool {
    let (mut pi, mut si, mut star, mut mark) = (0, 0, None, 0);
    let pb = p.as_bytes();
    let sb = s.as_bytes();
    while si < sb.len() {
        if pi < pb.len() && pb[pi] == sb[si] {
            pi += 1;
            si += 1;
        } else if pi < pb.len() && pb[pi] == b'*' {
            star = Some(pi);
            pi += 1;
            mark = si;
        } else if let Some(st) = star {
            pi = st + 1;
            mark += 1;
            si = mark;
        } else {
            return false;
        }
    }
    while pi < pb.len() && pb[pi] == b'*' {
        pi += 1;
    }
    pi == pb.len()
}

unsafe fn read_utf8<'a>(ptr: *const c_char, len: usize) -> &'a str {
    if ptr.is_null() {
        return "";
    }
    str::from_utf8(slice::from_raw_parts(ptr as *const u8, len)).unwrap_or("")
}
unsafe fn copy_out(s: &str, out: *mut c_char, cap: usize) -> usize {
    let bytes = s.as_bytes();
    if !out.is_null() && cap > 0 {
        let n = bytes.len().min(cap);
        std::ptr::copy_nonoverlapping(bytes.as_ptr(), out as *mut u8, n);
    }
    bytes.len()
}
#[no_mangle]
pub extern "C" fn ulb_domain_count() -> usize {
    domains().len()
}
#[no_mangle]
pub unsafe extern "C" fn ulb_domain_at(i: usize, out: *mut c_char, cap: usize) -> usize {
    domains().get(i).map_or(0, |s| copy_out(s, out, cap))
}
#[no_mangle]
pub extern "C" fn ulb_pattern_count() -> usize {
    request_filter_patterns().len()
}
#[no_mangle]
pub unsafe extern "C" fn ulb_pattern_at(i: usize, out: *mut c_char, cap: usize) -> usize {
    request_filter_patterns()
        .get(i)
        .map_or(0, |s| copy_out(s, out, cap))
}
#[no_mangle]
pub unsafe extern "C" fn ulb_extract_host(
    uri: *const c_char,
    len: usize,
    out: *mut c_char,
    cap: usize,
) -> usize {
    copy_out(&extract_host(read_utf8(uri, len)), out, cap)
}
#[no_mangle]
pub unsafe extern "C" fn ulb_base_domain(
    host: *const c_char,
    len: usize,
    out: *mut c_char,
    cap: usize,
) -> usize {
    copy_out(&base_domain(read_utf8(host, len)), out, cap)
}
#[no_mangle]
pub unsafe extern "C" fn ulb_match_host(
    host: *const c_char,
    len: usize,
    out: *mut c_char,
    cap: usize,
) -> usize {
    match_host(read_utf8(host, len)).map_or(0, |s| copy_out(s, out, cap))
}
#[no_mangle]
pub unsafe extern "C" fn ulb_evaluate(
    uri: *const c_char,
    uri_len: usize,
    top: *const c_char,
    top_len: usize,
    kind: i32,
) -> i32 {
    evaluate(read_utf8(uri, uri_len), read_utf8(top, top_len), kind)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn host_parsing_and_base_domain_are_stable() {
        assert_eq!(
            extract_host("https://User:Pw@WWW.DoubleClick.NET.:443/x?y#z"),
            "www.doubleclick.net"
        );
        assert_eq!(extract_host("https://[2001:db8::1]:8080/"), "[2001:db8::1]");
        assert_eq!(extract_host("about:blank"), "");
        assert_eq!(extract_host("custom://Sub.Example/path"), "sub.example");
        assert_eq!(base_domain("news.bbc.co.uk"), "bbc.co.uk");
        assert_eq!(base_domain("x.user.github.io"), "user.github.io");
        assert_eq!(base_domain("192.168.1.20"), "192.168.1.20");
    }
    #[test]
    fn rule_decisions_preserve_first_party_and_surrogate_behavior() {
        assert_eq!(
            evaluate("https://hm.baidu.com/hm.gif?x", "www.example.cn", 1),
            1
        );
        assert_eq!(
            evaluate(
                "https://www.example.com/img/pixel.gif",
                "www.example.com",
                1
            ),
            0
        );
        assert_eq!(
            evaluate(
                "https://analytics.tiktok.com/i18n/pixel/events.js",
                "www.tiktok.com",
                3
            ),
            1
        );
        assert_eq!(
            evaluate("https://www.google-analytics.com/analytics.js", "a.com", 2),
            1 | (1 << 8)
        );
        assert_eq!(
            evaluate("https://www.google-analytics.com/g/collect", "a.com", 2),
            1
        );
        assert_eq!(evaluate("data:text/javascript,1", "www.example.com", 3), 0);
    }
    #[test]
    fn filter_patterns_route_decisions_without_routing_normal_pages() {
        let patterns = request_filter_patterns();
        assert!(patterns.iter().any(|p| p == "*://*doubleclick.net/*"));
        assert!(patterns
            .iter()
            .any(|p| p == "*://*youtube.com/api/stats/ads*"));
        assert!(!patterns
            .iter()
            .any(|p| glob_match(p, "https://www.example.com/index.html")));
        assert!(patterns.len() < domains().len() + 30);
    }
}
