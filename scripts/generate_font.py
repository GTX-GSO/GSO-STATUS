"""Regenerate private preview font subsets (Python + Pillow required only for this step)."""
from PIL import Image, ImageFont, ImageDraw
from pathlib import Path
import argparse

out = Path(__file__).resolve().parent.parent / 'simulator'
parser=argparse.ArgumentParser();parser.add_argument('--sizes',nargs='+',default=['16','20','countdown']);args=parser.parse_args()
source = '\n'.join(path.read_text(encoding='utf-8') for path in [out/'ui.c',*sorted(out.glob('*.inc')),*sorted((out.parent/'main').glob('*.c')),*sorted((out.parent/'main').glob('*.inc'))])
chars = sorted(set(chr(i) for i in range(32,127)) | {c for c in source if ord(c)>127})
for name,size in (("16",16),("20",20),("countdown",20)):
    if name not in args.sizes: continue
    active_chars = chars
    if name == "16":
        extra = list(range(0x4E00,0xA000)) + list(range(0x3000,0x3040)) + list(range(0xFF00,0xFFF0))
        active_chars = sorted(set(chars).union(map(chr, extra)))
    if name == "20":
        # Body text is 20 px: cover every GB2312 level-1 hanzi (3755 common characters, e.g. device names coming from
        # Home Assistant) so dynamic Chinese never falls back to the smaller 16 px face.
        level1 = []
        for hi in range(0xB0,0xD8):
            for lo in range(0xA1,0xFF):
                try: level1.append(bytes([hi,lo]).decode('gb2312'))
                except UnicodeDecodeError: pass
        extra = level1 + [chr(i) for i in list(range(0x3000,0x3040)) + list(range(0xFF00,0xFFF0))]
        active_chars = sorted(set(chars).union(extra))
    font = ImageFont.truetype('C:/Windows/Fonts/simhei.ttf',size)
    bits, desc = [], ['{0}']
    for c in active_chars:
        glyph_font = ImageFont.truetype("C:/Windows/Fonts/simhei.ttf",16) if name=="countdown" and c=="天" else font
        left,top,right,bottom = glyph_font.getbbox(c,anchor='ls')
        width,height = right-left,bottom-top
        image = Image.new('L',(max(1,width),max(1,height)))
        ImageDraw.Draw(image).text((-left,-top),c,font=glyph_font,fill=255,anchor='ls')
        values = [int(p)//17 for p in image.get_flattened_data()] if width*height else []
        if len(values)%2: values.append(0)
        data = [values[i]*16+values[i+1] for i in range(0,len(values),2)]
        desc.append('{.bitmap_index=%d,.adv_w=%d,.box_w=%d,.box_h=%d,.ofs_x=%d,.ofs_y=%d}' %
                    (len(bits),round(glyph_font.getlength(c)*16),width,height,left,-bottom))
        bits += data
    code = '#include "lvgl.h"\n'
    code += 'static const uint8_t bitmap[]={'+','.join(map(str,bits))+'};\n'
    code += 'static const lv_font_fmt_txt_glyph_dsc_t glyphs[]={'+','.join(desc)+'};\n'
    code += 'static const uint16_t unicode_list[]={'+','.join(str(ord(c)-32) for c in active_chars)+'};\n'
    code += 'static const lv_font_fmt_txt_cmap_t cmaps[]={{.range_start=32,.range_length=%d,.glyph_id_start=1,.unicode_list=unicode_list,.list_length=%d,.type=LV_FONT_FMT_TXT_CMAP_SPARSE_TINY}};\n' % (ord(active_chars[-1])-31,len(active_chars))
    code += 'static const lv_font_fmt_txt_dsc_t dsc={.glyph_bitmap=bitmap,.glyph_dsc=glyphs,.cmaps=cmaps,.cmap_num=1,.bpp=4,.bitmap_format=0};\n'
    if name!='16':code+='extern const lv_font_t desk_font_16;\n'
    fallback='' if name=='16' else ',.fallback=&desk_font_16'
    code += 'const lv_font_t desk_font_%s={.get_glyph_dsc=lv_font_get_glyph_dsc_fmt_txt,.get_glyph_bitmap=lv_font_get_bitmap_fmt_txt,.line_height=%d,.base_line=4,.dsc=&dsc%s};\n' % (name,size+4,fallback)
    (out/f'font_{name}.c').write_text(code,encoding='utf-8')
print('Generated preview font subsets:',len(chars),'characters')
