/// Classifies an audio output endpoint as headphones vs speakers from PipeWire /
/// ALSA port form-factor hints and device names.
pub fn is_headphone_endpoint(is_headphone_hint: bool, name: &str, description: &str) -> bool {
    if is_headphone_hint {
        return true;
    }
    let lower = format!("{name}\n{description}").to_lowercase();
    const KEYWORDS: &[&str] = &[
        "headphone",
        "headset",
        "earphone",
        "earbud",
        "airpods",
        "buds",
        "wh-1000",
        "wf-1000",
        "hands-free",
        "耳机",
        "耳麦",
        "头戴",
    ];
    KEYWORDS.iter().any(|kw| lower.contains(kw))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn headphone_detection_matches_hints_and_device_names() {
        assert!(is_headphone_endpoint(true, "Speakers", ""));
        assert!(is_headphone_endpoint(
            false,
            "WH-1000XM5 Stereo",
            "BlueZ Output"
        ));
        assert!(is_headphone_endpoint(false, "USB Audio", "FiiO 耳机"));
        assert!(!is_headphone_endpoint(
            false,
            "HDMI / DisplayPort",
            "Monitor Audio"
        ));
        assert!(!is_headphone_endpoint(false, "MacBook Pro Speakers", ""));
    }
}
