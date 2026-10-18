"""Check actual generated maps cover the physical Wi-Fi page text."""
from pathlib import Path
import re
root=Path(__file__).resolve().parents[1]
def glyphs(name):
 source=(root/'simulator'/name).read_text(encoding='utf-8')
 return {int(n)+32 for n in re.search(r'unicode_list\[\]=\{([^}]+)',source).group(1).split(',')}
page=(root/'simulator/wifi_ui.inc').read_text(encoding='utf-8')
required={ord(c) for literal in re.findall(r'"([^"\n]*)"',page) for c in literal if ord(c)>127}
assert not required-glyphs('font_20.c'),'Wi-Fi Chinese labels missing from 20px font'
assert set(range(0x4e00,0xa000))<=glyphs('font_16.c'),'Dynamic Chinese fallback incomplete'
assert '.fallback=&desk_font_16' in (root/'simulator/font_20.c').read_text(), '20px fallback missing'
print('PASS: Wi-Fi static Chinese glyphs and complete 16px CJK fallback')
