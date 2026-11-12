"""tools/pack_f8964.py: links point at offline files, outside links are left alone, every page gets the notice."""
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

PAGE = """<!doctype html><html lang="{lang}" data-locale="{loc}"><head>
<link rel="alternate" hreflang="zh-Hans" href="https://freedom8964.com/" data-locale="zh">
<link rel="alternate" hreflang="en" href="https://freedom8964.com/en/" data-locale="en">
<link rel="stylesheet" href="{up}assets/style.css"><script src="/freedom8964.com/assets/lang.js"></script></head>
<body><a href="/freedom8964.com/timeline">t</a><a href="about#x">a</a><a href="./">home</a>
<a href="https://zh.wikipedia.org/wiki/x">wiki</a><a href="undefined">broken</a></body></html>"""

with tempfile.TemporaryDirectory() as d:
    d = Path(d)
    site = d / "site"
    for sub, loc, lang, up in (("", "zh", "zh-Hans", ""), ("en/", "en", "en", "../"), ("zh-hant/", "zh-hant", "zh-Hant", "../")):
        (site / sub).mkdir(parents=True, exist_ok=True)
        for page in ("index.html", "timeline.html", "about.html"):
            (site / sub / page).write_text(PAGE.format(lang=lang, loc=loc, up=up), encoding="utf-8")
    (site / "assets").mkdir()
    (site / "assets/style.css").write_text("body{}")
    (site / "assets/lang.js").write_text("//")
    out = d / "f.pack"
    subprocess.run([sys.executable, "tools/pack_f8964.py", str(out), "--from-dir", str(site), "--date", "2026-06-04"], check=True)

    data = out.read_bytes()
    assert data[:8] == b"ULBPACK1"
    files, i = {}, 8
    while i < len(data):
        (n,) = struct.unpack_from("<I", data, i); i += 4
        name = data[i:i + n].decode(); i += n
        (s,) = struct.unpack_from("<I", data, i); i += 4
        files[name] = data[i:i + s]; i += s
    assert "assets/ulb-offline.css" in files, files.keys()

    zh = files["index.html"].decode()
    assert 'href="/timeline.html"' in zh, zh
    assert 'href="/about.html#x"' in zh
    assert 'href="/index.html">home' in zh
    assert 'src="/assets/lang.js"' in zh and 'href="/assets/style.css"' in zh
    assert 'hreflang="en" href="/en/index.html"' in zh          # language switch stays offline
    assert 'href="https://zh.wikipedia.org/wiki/x"' in zh       # outside links untouched
    assert 'href="undefined"' in zh                             # broken site links are not "fixed" into files
    assert "离线快照（2026-06-04）" in zh and 'href="https://freedom8964.com/"' in zh
    assert '<link rel="stylesheet" href="/assets/ulb-offline.css"></head>' in zh

    en = files["en/timeline.html"].decode()
    assert "Offline snapshot (2026-06-04)" in en and 'href="https://freedom8964.com/en/timeline"' in en
    assert 'href="/en/about.html#x"' in en and 'href="/en/index.html">home' in en
    assert "離線快照" in files["zh-hant/about.html"].decode()
    for html in (v.decode() for k, v in files.items() if k.endswith(".html")):
        assert "/freedom8964.com/" not in html.replace("https://freedom8964.com/", "")

print("freedom8964.com snapshot packer OK:", len(files), "files")
