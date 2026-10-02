"""Every built-in UI file must be embedded by resource.rc and extracted by InternalPages.cpp."""
import re
from pathlib import Path

rc = Path('resources/resource.rc').read_text(encoding='utf-8')
cpp = Path('src/InternalPages.cpp').read_text(encoding='utf-8')
rc_entries = {int(i): p.replace('\\\\', '/').replace('../ui/', '') for i, p in re.findall(r'^(\d{4})\s+RCDATA\s+"([^"]+)"', rc, re.M)}
cpp_entries = {int(i): n for i, n in re.findall(r'\{(\d{4}), "([^"]+)"\}', cpp)}
assert rc_entries == cpp_entries, (rc_entries, cpp_entries)
on_disk = {str(p.relative_to('ui')).replace('\\', '/') for p in Path('ui').rglob('*') if p.is_file() and p.suffix in ('.html', '.js', '.css')}
assert set(cpp_entries.values()) == on_disk, set(cpp_entries.values()) ^ on_disk
for name in cpp_entries.values():
    if name.endswith('.html'):
        html = Path('ui', name).read_text(encoding='utf-8')
        assert 'Content-Security-Policy' in html, name
        assert '<script>' not in html and 'onclick=' not in html, name
print('UI resource table OK:', len(cpp_entries), 'files')
