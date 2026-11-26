use std::sync::OnceLock;

/// Builds the OS-rewritten User-Agent string, preserving browser tokens.
pub fn build_user_agent(original: &str, profile: &str) -> String {
    if profile != "macos-edge" {
        return original.to_string();
    }
    let Some(start) = original.find('(') else {
        return String::new();
    };
    let Some(rel_end) = original[start..].find(')') else {
        return String::new();
    };
    let end = start + rel_end;
    format!(
        "{}(Macintosh; Intel Mac OS X 10_15_7){}",
        &original[..start],
        &original[end + 1..]
    )
}

/// Builds a macOS Edge User-Agent from a Chromium/CEF UA, appending `Edg/<ver>`
/// when the underlying runtime does not include an Edge token.
pub fn build_macos_edge_user_agent(original: &str, profile: &str) -> String {
    let base = build_user_agent(original, profile);
    if profile != "macos-edge" || base.is_empty() || !user_agent_token(&base, "Edg").is_empty() {
        return base;
    }
    let chrome = user_agent_token(&base, "Chrome");
    if chrome.is_empty() {
        base
    } else {
        format!("{base} Edg/{chrome}")
    }
}

/// Extracts the version string following `<token>/` in a User-Agent string.
pub fn user_agent_token(ua: &str, token: &str) -> String {
    let needle = format!("{token}/");
    let Some(pos) = ua.find(&needle) else {
        return String::new();
    };
    ua[pos + needle.len()..]
        .bytes()
        .take_while(|b| b.is_ascii_digit() || *b == b'.')
        .map(char::from)
        .collect()
}

/// Chromium's GREASE brand list matching `components/embedder_support/user_agent_utils.cc`.
pub fn greased_brand_list(
    seed: i32,
    brand: &str,
    brand_version: &str,
    chromium_version: &str,
) -> Vec<(String, String)> {
    const ORDERS: [[usize; 3]; 6] = [
        [0, 1, 2],
        [0, 2, 1],
        [1, 0, 2],
        [1, 2, 0],
        [2, 0, 1],
        [2, 1, 0],
    ];
    const CHARS: [&str; 11] = [" ", "(", ":", "-", ".", "/", ")", ";", "=", "?", "_"];
    const VERSIONS: [&str; 3] = ["8", "99", "24"];

    let s = seed.max(0) as usize;
    let order = ORDERS[s % 6];
    let mut list = vec![(String::new(), String::new()); 3];
    list[order[0]] = (
        format!("Not{}A{}Brand", CHARS[s % CHARS.len()], CHARS[(s + 1) % CHARS.len()]),
        VERSIONS[s % VERSIONS.len()].to_string(),
    );
    list[order[1]] = ("Chromium".to_string(), chromium_version.to_string());
    list[order[2]] = (brand.to_string(), brand_version.to_string());
    list
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct MacDeviceProfile {
    pub renderer: &'static str,
    pub hardware_concurrency: u32,
}

pub const MAC_DEVICE_PROFILES: [MacDeviceProfile; 4] = [
    MacDeviceProfile {
        renderer: "ANGLE (Apple, ANGLE Metal Renderer: Apple M1, Unspecified Version)",
        hardware_concurrency: 8,
    },
    MacDeviceProfile {
        renderer: "ANGLE (Apple, ANGLE Metal Renderer: Apple M2, Unspecified Version)",
        hardware_concurrency: 8,
    },
    MacDeviceProfile {
        renderer: "ANGLE (Apple, ANGLE Metal Renderer: Apple M3, Unspecified Version)",
        hardware_concurrency: 8,
    },
    MacDeviceProfile {
        renderer: "ANGLE (Apple, ANGLE Metal Renderer: Apple M4, Unspecified Version)",
        hardware_concurrency: 10,
    },
];

pub fn mac_device_profile_for_index(index: usize) -> &'static MacDeviceProfile {
    &MAC_DEVICE_PROFILES[index % MAC_DEVICE_PROFILES.len()]
}

pub fn mac_device_profile_index(rng_succeeded: bool, random_value: u32) -> usize {
    if rng_succeeded {
        (random_value as usize) % MAC_DEVICE_PROFILES.len()
    } else {
        0
    }
}

pub fn session_mac_device_profile_with<F>(mut random_source: F) -> &'static MacDeviceProfile
where
    F: FnMut() -> Option<u32>,
{
    static INDEX: OnceLock<usize> = OnceLock::new();
    let idx = *INDEX.get_or_init(|| match random_source() {
        Some(v) => mac_device_profile_index(true, v),
        None => 0,
    });
    mac_device_profile_for_index(idx)
}

pub const MAC_UA_PLACEHOLDER: &str = "\"__ULB_MAC_UA__\"";

const MAC_STEALTH_SCRIPT_TEMPLATE: &str = r#"(() => {
  const G = globalThis;
  const fnToString = Function.prototype.toString;
  const masked = new WeakMap();
  const toStringProxy = new Proxy(fnToString, {
    apply(target, self, args) { return Reflect.apply(target, masked.get(self) || self, args); }
  });
  masked.set(toStringProxy, fnToString);
  Object.defineProperty(Function.prototype, "toString", { value: toStringProxy, writable: true, configurable: true, enumerable: false });
  const wrap = (orig, apply) => { const p = new Proxy(orig, { apply }); masked.set(p, orig); return p; };
  const hookMethod = (proto, name, apply) => {
    const d = proto && Object.getOwnPropertyDescriptor(proto, name);
    if (d && typeof d.value === "function") Object.defineProperty(proto, name, { ...d, value: wrap(d.value, apply) });
  };
  const hookGetter = (proto, name, value) => {
    const d = proto && Object.getOwnPropertyDescriptor(proto, name);
    if (d && d.get) Object.defineProperty(proto, name, { ...d, get: wrap(d.get, () => value) });
  };
  const VENDOR = "Google Inc. (Apple)";
  const RENDERER = "__ULB_MAC_GPU_RENDERER__";
  const HARDWARE_CONCURRENCY = __ULB_MAC_HARDWARE_CONCURRENCY__;
  for (const C of [G.WebGLRenderingContext, G.WebGL2RenderingContext]) {
    hookMethod(C && C.prototype, "getParameter", (t, self, args) =>
      args[0] === 0x9245 ? VENDOR : args[0] === 0x9246 ? RENDERER : Reflect.apply(t, self, args));
  }
  if (G.GPUAdapterInfo) {
    const P = G.GPUAdapterInfo.prototype;
    hookGetter(P, "vendor", "apple");
    hookGetter(P, "architecture", "metal-3");
    hookGetter(P, "device", "");
    hookGetter(P, "description", "");
  }
  if (G.Navigator) {
    hookGetter(G.Navigator.prototype, "platform", "MacIntel");
    hookGetter(G.Navigator.prototype, "hardwareConcurrency", HARDWARE_CONCURRENCY);
  }
  if (G.WorkerNavigator) {
    const UA = "__ULB_MAC_UA__";
    const P = G.WorkerNavigator.prototype;
    hookGetter(P, "platform", "MacIntel");
    hookGetter(P, "userAgent", UA);
    hookGetter(P, "appVersion", UA.slice(UA.indexOf("/") + 1));
    hookGetter(P, "hardwareConcurrency", HARDWARE_CONCURRENCY);
  }
  if (G.SharedWorker && G.document) delete G.SharedWorker;
  if (G.SpeechSynthesis) {
    hookMethod(G.SpeechSynthesis.prototype, "getVoices", (t, self, args) =>
      Reflect.apply(t, self, args).filter((v) => !v.localService));
  }
})();"#;

pub fn build_mac_stealth_script(profile: &MacDeviceProfile, mac_ua: Option<&str>) -> String {
    let mut script = MAC_STEALTH_SCRIPT_TEMPLATE
        .replace("__ULB_MAC_GPU_RENDERER__", profile.renderer)
        .replace(
            "__ULB_MAC_HARDWARE_CONCURRENCY__",
            &profile.hardware_concurrency.to_string(),
        );
    if let Some(ua) = mac_ua {
        let quoted = serde_json::to_string(ua).unwrap_or_else(|_| "\"\"".to_string());
        script = script.replace(MAC_UA_PLACEHOLDER, &quoted);
    }
    script
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn user_agent_rewriting_and_grease_brand_list() {
        let original = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0.0.0 Safari/537.36 Edg/130.0.2849.68";
        let mac = build_user_agent(original, "macos-edge");
        assert!(mac.contains("(Macintosh; Intel Mac OS X 10_15_7)"));
        assert_eq!(
            &mac[mac.find(')').unwrap()..],
            &original[original.find(')').unwrap()..]
        );
        assert_eq!(build_user_agent(original, "default"), original);
        assert_eq!(build_user_agent(original, "invalid"), original);
        assert!(build_user_agent("broken", "macos-edge").is_empty());
        assert!(build_user_agent("Mozilla/5.0 (Windows NT 10.0", "macos-edge").is_empty());
        assert_eq!(user_agent_token(original, "Edg"), "130.0.2849.68");
        assert_eq!(user_agent_token(original, "Chrome"), "130.0.0.0");
        assert!(user_agent_token(original, "Firefox").is_empty());

        let linux_cef = "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/156.0.0.0 Safari/537.36";
        let mac_from_cef = build_macos_edge_user_agent(linux_cef, "macos-edge");
        assert_eq!(
            mac_from_cef,
            "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/156.0.0.0 Safari/537.36 Edg/156.0.0.0"
        );

        let g = greased_brand_list(153, "HeadlessChrome", "153", "153");
        assert_eq!(g[0], ("HeadlessChrome".into(), "153".into()));
        assert_eq!(g[1], ("Not_A Brand".into(), "8".into()));
        assert_eq!(g[2], ("Chromium".into(), "153".into()));
    }

    #[test]
    fn mac_device_profile_and_stealth_script() {
        let mut calls = 0u32;
        let first = session_mac_device_profile_with(|| {
            calls += 1;
            Some(2)
        });
        let second = session_mac_device_profile_with(|| {
            calls += 1;
            Some(0)
        });
        assert!(std::ptr::eq(first, second));
        assert!(first.renderer.contains("Apple M3"));
        assert_eq!(calls, 1);

        assert_eq!(mac_device_profile_index(false, 3), 0);
        assert_eq!(mac_device_profile_index(true, 5), 1);
        assert_eq!(mac_device_profile_for_index(4).hardware_concurrency, 8);

        let script = build_mac_stealth_script(&MAC_DEVICE_PROFILES[2], Some("Mozilla/5.0 (Macintosh)"));
        assert!(!script.contains("__ULB_MAC_GPU_RENDERER__"));
        assert!(!script.contains("__ULB_MAC_HARDWARE_CONCURRENCY__"));
        assert!(!script.contains(MAC_UA_PLACEHOLDER));
        assert!(script.contains("Apple M3"));
        assert!(script.contains("const HARDWARE_CONCURRENCY = 8;"));
        assert!(!script.contains("toDataURL"));
        assert!(!script.contains("getImageData"));
        assert!(!script.contains("copyFromChannel"));
        assert!(!script.contains("getChannelData"));
    }
}
