#!/usr/bin/env python3
"""Offline snapshot of freedom8964.com for the start page entry "六四事件（离线版）".

    python tools/pack_f8964.py resources/f8964.pack            # download the site and pack it
    python tools/pack_f8964.py OUT --from-dir DIR [--date D]   # pack a local copy (tests)

Downloads the built site from its GitHub Pages mirror, makes every internal
link point to a file (https://f8964.internal/timeline.html), adds a notice
with the snapshot date and a link to the live site on every page, and writes
one pack file that resources/resource.rc embeds in the exe. The browser
extracts it next to its built-in pages and serves it on its own virtual host,
which — unlike the built-in pages — cannot talk to the browser.

Pack format: b"ULBPACK1", then per file: u32 name length, UTF-8 name,
u32 data length, data (little endian). Only standard library.
"""
import argparse
import datetime as dt
import html
import re
import struct
import sys
import urllib.error
import urllib.parse
import urllib.request
from collections import deque
from pathlib import Path

MIRROR = "https://freecode100year.github.io/freedom8964.com/"
LIVE = "https://freedom8964.com/"
ROOTS = ["", "zh-hant/", "en/"]
NAME_OK = re.compile(r"^[A-Za-z0-9._-]+(/[A-Za-z0-9._-]+)*$")
ATTR = re.compile(r'(\s(?:href|src)=")([^"]*)(")')

NOTICE = {
    "zh": ("离线快照（{date}）· 内容可能不是最新", "查看在线最新版 →",
           "在线版请在开启 WARP 或「我的线路」时打开；直接连接时，网络运营商能看到你访问了这个网站。页面中的外部资料链接同样需要联网。"),
    "zh-hant": ("離線快照（{date}）· 內容可能不是最新", "查看線上最新版 →",
                "線上版請在開啟 WARP 或「我的線路」時開啟；直接連線時，網路業者能看到你造訪了這個網站。頁面中的外部資料連結同樣需要連網。"),
    "en": ("Offline snapshot ({date}) · may be out of date", "Latest version online →",
           "Open the online site with WARP or “My line” turned on; on a direct connection your network provider can see that you visited it. Links to outside sources also need a connection."),
}
CSS = """.ulb-offline{background:#2b2620;color:#f4ede1;font:14px/1.6 system-ui,-apple-system,"Segoe UI","Microsoft YaHei",sans-serif;padding:10px 16px;border-bottom:3px solid #b8860b}
.ulb-offline b{font-weight:600}.ulb-offline a{color:#f2c46d;margin-left:.8em;text-decoration:underline}
.ulb-offline small{display:block;opacity:.8;font-size:12.5px;margin-top:2px}
"""


def file_for(path: str) -> str:
    """Site path → file name: "" → index.html, "zh-hant/" → zh-hant/index.html, "timeline" → timeline.html."""
    path = urllib.parse.unquote(path.split("#")[0].split("?")[0]).lstrip("/")
    if path == "" or path.endswith("/"):
        return path + "index.html"
    last = path.rsplit("/", 1)[-1]
    return path if "." in last else path + ".html"


def site_path(url: str):
    """Path inside the site for links to the mirror or to the live domain; None for anything else."""
    for base in (MIRROR, LIVE, "https://www.freedom8964.com/", "/freedom8964.com/"):
        if url.startswith(base):
            return url[len(base):]
    return None


def live_url(name: str) -> str:
    """index.html → https://freedom8964.com/, zh-hant/timeline.html → .../zh-hant/timeline"""
    p = name[:-5] if name.endswith(".html") else name
    if p == "index" or p.endswith("/index"):
        p = p[: -len("index")]
    return LIVE + p


def rewrite(name: str, text: str, date: str, known: set) -> str:
    page_url = urllib.parse.urljoin(MIRROR, name)

    def fix(m):
        val = html.unescape(m.group(2))
        if val.startswith(("#", "mailto:", "tel:", "data:", "javascript:")) or val == "":
            return m.group(0)
        absolute = urllib.parse.urljoin(page_url, val)
        p = site_path(absolute) if absolute.startswith(("http:", "https:")) else None
        if p is None or file_for(p) not in known:
            return m.group(0)  # outside the site, or a broken link on the site: leave it
        frag = "#" + val.split("#", 1)[1] if "#" in val else ""
        return m.group(1) + "/" + file_for(p) + frag + m.group(3)

    text = ATTR.sub(fix, text)
    locale = re.search(r'<html[^>]*\sdata-locale="([^"]+)"', text)
    loc = locale.group(1) if locale and locale.group(1) in NOTICE else ("en" if name.startswith("en/") else "zh-hant" if name.startswith("zh-hant/") else "zh")
    head, link, note = NOTICE[loc]
    banner = (f'<div class="ulb-offline" role="note"><b>{html.escape(head.format(date=date))}</b>'
              f'<a href="{html.escape(live_url(name))}">{html.escape(link)}</a><small>{html.escape(note)}</small></div>')
    text = re.sub(r"(<body[^>]*>)", lambda m: m.group(1) + banner, text, count=1)
    text = text.replace("</head>", '<link rel="stylesheet" href="/assets/ulb-offline.css"></head>', 1)
    return text


def fetch(url: str) -> bytes:
    req = urllib.request.Request(url, headers={"User-Agent": "UltraLightBrowser-build"})
    with urllib.request.urlopen(req, timeout=60) as r:
        return r.read()


def crawl() -> dict:
    """Every page reachable from the three language roots, plus their scripts, styles and images."""
    files, queue, missing = {}, deque(ROOTS), set()
    while queue:
        p = queue.popleft()
        name = file_for(p)
        if name in files or name in missing:
            continue
        try:
            data = fetch(urllib.parse.urljoin(MIRROR, p))
        except urllib.error.HTTPError as e:
            if p in ROOTS:
                raise
            missing.add(name)  # a broken link on the site itself; the link is left as it is
            print(f"  skipped {p!r}: HTTP {e.code}", file=sys.stderr)
            continue
        files[name] = data
        if name.endswith(".html"):
            page_url = urllib.parse.urljoin(MIRROR, name)
            for m in ATTR.finditer(data.decode("utf-8", "replace")):
                val = html.unescape(m.group(2))
                if val.startswith(("#", "mailto:", "data:")):
                    continue
                sp = site_path(urllib.parse.urljoin(page_url, val))
                if sp is not None and file_for(sp) not in files:
                    queue.append(sp)
        if len(files) > 500:
            sys.exit("too many files — did the site layout change?")
    return files


def from_dir(d: Path) -> dict:
    return {str(f.relative_to(d)).replace("\\", "/"): f.read_bytes() for f in d.rglob("*") if f.is_file() and not f.name.startswith(".")}


def pack(files: dict, date: str) -> bytes:
    out = bytearray(b"ULBPACK1")
    files = dict(files)
    files["assets/ulb-offline.css"] = CSS.encode()
    for name in sorted(files):
        if not NAME_OK.match(name) or ".." in name.split("/"):
            sys.exit(f"bad file name: {name!r}")
        data = files[name]
        if name.endswith(".html"):
            data = rewrite(name, data.decode("utf-8"), date, set(files)).encode("utf-8")
        n = name.encode()
        out += struct.pack("<I", len(n)) + n + struct.pack("<I", len(data)) + data
    return bytes(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--from-dir", type=Path)
    ap.add_argument("--date", default=dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%d"))
    a = ap.parse_args()
    files = from_dir(a.from_dir) if a.from_dir else crawl()
    for must in ("index.html", "zh-hant/index.html", "en/index.html"):
        if must not in files:
            sys.exit(f"missing {must}")
    data = pack(files, a.date)
    Path(a.out).parent.mkdir(parents=True, exist_ok=True)
    Path(a.out).write_bytes(data)
    print(f"{a.out}: {len(files) + 1} files, {len(data) // 1024} KB, snapshot {a.date}")


if __name__ == "__main__":
    main()
