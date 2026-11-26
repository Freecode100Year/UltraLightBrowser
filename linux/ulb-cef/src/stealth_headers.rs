use crate::ffi::sys;
use sha2::{Digest, Sha256};
use std::path::Path;
use ulb_core::adblock::ResourceKind;
use ulb_core::user_agent::{build_macos_edge_user_agent, greased_brand_list, user_agent_token};

/// Builds rewritten HTTP headers `(name, value)` for `macOS Edge` UA + Client Hints consistency.
pub fn macos_edge_request_headers(
    base_ua: &str,
    profile: &str,
    mac_platform_version: &str,
) -> Vec<(String, String)> {
    if profile != "macos-edge" {
        return Vec::new();
    }
    let ua = build_macos_edge_user_agent(base_ua, profile);
    let chrome = user_agent_token(&ua, "Chrome");
    let edge = user_agent_token(&ua, "Edg");
    let major = edge
        .split('.')
        .next()
        .filter(|s| !s.is_empty())
        .unwrap_or("142");
    let major_int = major.parse::<i32>().unwrap_or(142);

    let brands = greased_brand_list(major_int, "Microsoft Edge", major, major);
    let full_brands = greased_brand_list(
        major_int,
        "Microsoft Edge",
        if edge.is_empty() { "142.0.0.0" } else { &edge },
        if chrome.is_empty() { "142.0.0.0" } else { &chrome },
    );
    let format_list = |items: &[(String, String)]| -> String {
        items
            .iter()
            .map(|(b, v)| format!(r#""{b}";v="{v}""#))
            .collect::<Vec<_>>()
            .join(", ")
    };

    vec![
        ("User-Agent".to_string(), ua.clone()),
        ("Sec-CH-UA".to_string(), format_list(&brands)),
        ("Sec-CH-UA-Mobile".to_string(), "?0".to_string()),
        ("Sec-CH-UA-Platform".to_string(), r#""macOS""#.to_string()),
        (
            "Sec-CH-UA-Platform-Version".to_string(),
            format!(r#""{mac_platform_version}""#),
        ),
        ("Sec-CH-UA-Arch".to_string(), r#""x86""#.to_string()),
        ("Sec-CH-UA-Bitness".to_string(), r#""64""#.to_string()),
        ("Sec-CH-UA-Model".to_string(), r#""""#.to_string()),
        (
            "Sec-CH-UA-Full-Version".to_string(),
            format!(r#""{}""#, if edge.is_empty() { "142.0.0.0" } else { &edge }),
        ),
        (
            "Sec-CH-UA-Full-Version-List".to_string(),
            format_list(&full_brands),
        ),
    ]
}

/// Maps CEF resource types to `ulb_core::adblock::ResourceKind`.
pub fn map_cef_resource_type(rt: sys::cef_resource_type_t) -> ResourceKind {
    match rt {
        sys::cef_resource_type_t_RT_MAIN_FRAME
        | sys::cef_resource_type_t_RT_NAVIGATION_PRELOAD_MAIN_FRAME => ResourceKind::Document,
        sys::cef_resource_type_t_RT_SCRIPT => ResourceKind::Script,
        sys::cef_resource_type_t_RT_IMAGE | sys::cef_resource_type_t_RT_FAVICON => {
            ResourceKind::Image
        }
        _ => ResourceKind::Other,
    }
}

/// Computes Chromium's unpacked extension ID (`a`..`p` hex over SHA-256 of canonical path).
pub fn unpacked_extension_id(dir: &Path) -> String {
    let canonical = std::fs::canonicalize(dir).unwrap_or_else(|_| dir.to_path_buf());
    let digest = Sha256::digest(canonical.to_string_lossy().as_bytes());
    let mut out = String::with_capacity(32);
    for &b in &digest[..16] {
        out.push(char::from(b'a' + (b >> 4)));
        out.push(char::from(b'a' + (b & 0x0f)));
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rewrites_macos_edge_client_hints() {
        let base = "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/156.0.0.0 Safari/537.36";
        let headers = macos_edge_request_headers(base, "macos-edge", "27.2.0");
        assert!(headers.iter().any(|(k, v)| k == "User-Agent" && v.contains("Macintosh") && v.contains("Edg/156.0.0.0")));
        assert!(headers.iter().any(|(k, v)| k == "Sec-CH-UA-Platform" && v == "\"macOS\""));
        assert!(headers.iter().any(|(k, v)| k == "Sec-CH-UA-Platform-Version" && v == "\"27.2.0\""));
    }

    #[test]
    fn generates_32_char_mpdecimal_extension_id() {
        let id = unpacked_extension_id(Path::new("/tmp/sample-ext"));
        assert_eq!(id.len(), 32);
        assert!(id.chars().all(|c| ('a'..='p').contains(&c)));
    }
}
