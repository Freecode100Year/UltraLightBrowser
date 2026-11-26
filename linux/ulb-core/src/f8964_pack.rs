use std::fs;
use std::path::Path;

fn safe_relative_name(name: &str) -> bool {
    if name.is_empty() || name.len() > 200 || name.starts_with('/') || name.ends_with('/') {
        return false;
    }
    for part in name.split('/') {
        if part.is_empty() || part == "." || part == ".." {
            return false;
        }
    }
    name.bytes()
        .all(|b| b.is_ascii_alphanumeric() || matches!(b, b'.' | b'_' | b'-' | b'/'))
}

fn read_u32_le(data: &[u8], cursor: &mut usize) -> Option<usize> {
    let end = cursor.checked_add(4)?;
    let slice: [u8; 4] = data.get(*cursor..end)?.try_into().ok()?;
    *cursor = end;
    Some(u32::from_le_bytes(slice) as usize)
}

/// Unpacks an `ULBPACK1` archive into `dest`, validating every relative entry name.
pub fn extract_pack(data: &[u8], dest: &Path) -> std::io::Result<usize> {
    if data.get(..8) != Some(b"ULBPACK1") {
        return Err(std::io::Error::new(
            std::io::ErrorKind::InvalidData,
            "invalid ULBPACK1 header",
        ));
    }
    let _ = fs::remove_dir_all(dest);
    fs::create_dir_all(dest)?;

    let mut i = 8;
    let mut count = 0;
    while i < data.len() {
        let Some(nlen) = read_u32_le(data, &mut i) else {
            break;
        };
        let Some(name_bytes) = i.checked_add(nlen).and_then(|end| data.get(i..end)) else {
            break;
        };
        i += nlen;
        let Some(dlen) = read_u32_le(data, &mut i) else {
            break;
        };
        let Some(payload) = i.checked_add(dlen).and_then(|end| data.get(i..end)) else {
            break;
        };
        i += dlen;

        if let Ok(name) = std::str::from_utf8(name_bytes) {
            if safe_relative_name(name) {
                let target = dest.join(name);
                if let Some(parent) = target.parent() {
                    fs::create_dir_all(parent)?;
                }
                fs::write(target, payload)?;
                count += 1;
            }
        }
    }
    Ok(count)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn pack_entry(out: &mut Vec<u8>, name: &str, data: &[u8]) {
        out.extend_from_slice(&(name.len() as u32).to_le_bytes());
        out.extend_from_slice(name.as_bytes());
        out.extend_from_slice(&(data.len() as u32).to_le_bytes());
        out.extend_from_slice(data);
    }

    #[test]
    fn extract_pack_validates_names_and_writes_files() {
        let dir = std::env::temp_dir().join("ulb-core-pack-test");
        let _ = fs::remove_dir_all(&dir);

        let mut blob = b"ULBPACK1".to_vec();
        pack_entry(&mut blob, "index.html", b"<h1>ok</h1>");
        pack_entry(&mut blob, "zh-hant/index.html", b"<h1>hant</h1>");
        pack_entry(&mut blob, "../escape.txt", b"bad");
        pack_entry(&mut blob, "/abs.txt", b"bad");

        let count = extract_pack(&blob, &dir).unwrap();
        assert_eq!(count, 2);
        assert_eq!(fs::read(dir.join("index.html")).unwrap(), b"<h1>ok</h1>");
        assert_eq!(
            fs::read(dir.join("zh-hant/index.html")).unwrap(),
            b"<h1>hant</h1>"
        );
        assert!(!dir.parent().unwrap().join("escape.txt").exists());
        let _ = fs::remove_dir_all(&dir);
    }
}
