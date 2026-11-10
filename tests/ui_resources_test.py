"""Every built-in UI file must be embedded by resource.rc and extracted by InternalPages.cpp."""
import re
from pathlib import Path

rc = Path('resources/resource.rc').read_text(encoding='utf-8')
cpp = Path('src/InternalPages.cpp').read_text(encoding='utf-8')
rc_entries = {int(i): p.replace('\\\\', '/').replace('../ui/', '') for i, p in re.findall(r'^(\d{4})\s+RCDATA\s+"([^"]+)"', rc, re.M)}
rc_entries = {i: p for i, p in rc_entries.items() if i < 4000}  # 4001: WARP helper binary, not a UI file
cpp_entries = {int(i): n for i, n in re.findall(r'\{(\d{4}), "([^"]+)"\}', cpp)}
assert rc_entries == cpp_entries, (rc_entries, cpp_entries)
on_disk = {str(p.relative_to('ui')).replace('\\', '/') for p in Path('ui').rglob('*') if p.is_file() and p.suffix in ('.html', '.js', '.css')}
assert set(cpp_entries.values()) == on_disk, set(cpp_entries.values()) ^ on_disk
self_test = Path('src/SelfTestPage.hpp').read_text(encoding='utf-8')
assert 'navigator.hardwareConcurrency' in self_test
assert 'w[6]' in self_test and 's[6]' in self_test
assert 'OffscreenCanvas' in self_test and 'w[7] === renderer' in self_test
assert 'Canvas' in self_test and '音频' in self_test and '原生' in self_test
assert '不同会话可能重复' in self_test
for name in cpp_entries.values():
    if name.endswith('.html'):
        html = Path('ui', name).read_text(encoding='utf-8')
        assert 'Content-Security-Policy' in html, name
        assert '<script>' not in html and 'onclick=' not in html, name
print('UI resource table OK:', len(cpp_entries), 'files')
