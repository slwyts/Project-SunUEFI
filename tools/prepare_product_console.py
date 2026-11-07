#!/usr/bin/env python3
"""Prepare the product's scaled text console and compact Chinese HII font."""
import hashlib
import json
from pathlib import Path
import re

SOURCE = Path('uefi/components/product-support/Library/ConsoleScale')
CONSOLE = Path('upstream/Mu-Silicium/Mu_Basecore/MdeModulePkg/Universal/Console/GraphicsConsoleDxe')
PKG = Path('upstream/Mu-Silicium/Mu_Basecore/MdeModulePkg')
STRINGS = (
    'Application/UiApp/FrontPageStrings.uni',
    'Library/DeviceManagerUiLib/DeviceManagerStrings.uni',
    'Library/BootManagerUiLib/BootManagerStrings.uni',
    'Library/BootMaintenanceManagerUiLib/BootMaintenanceManagerStrings.uni',
    'Universal/DisplayEngineDxe/FormDisplayStr.uni',
)
BOOT_TEXT = (
    '未能启动系统正在读取启动镜像正在保存设置并关闭存储正在交接系统'
    '存储驱动未能安全退出仍有文件或驱动没有关闭为保护数据已停止启动'
    '未能加载启动镜像请检查启动镜像或重新安装引导'
    '长按电源重启在选择画面按音量上进入 Android'
    '显示初始化失败引导文件不可用输入初始化失败存储初始化失败'
    'USB 初始化失败启动菜单退出失败系统交接失败请稍候设置未能保存'
    '系统资源未能安全退出Linux 内核或设备树不可用无法读取本机内存布局'
    '设备树内存或保留区不匹配系统交接检查失败无法退出固件服务'
)


def wide_controls(text):
    result, wide = [], False
    for ch in text:
        state = ord(ch) >= 0x2e80
        if state != wide:
            result.append('\ufff1' if state else '\ufff0')
            wide = state
        result.append(ch)
    if wide:
        result.append('\ufff0')
    return ''.join(result)


def output_file(path, data, apply):
    if apply:
        if not path.is_file() or path.read_bytes() != data:
            path.write_bytes(data)
    elif not path.is_file() or path.read_bytes() != data:
        raise ValueError('Product console file not prepared: ' + str(path))


def translations(root, apply):
    mapping = json.loads((root / SOURCE / 'setup-zh.json').read_text())
    chars, files = set('简体中文'), []
    for relative in STRINGS:
        path = root / PKG / relative
        raw = path.read_bytes()
        ending = '\r\n' if b'\r\n' in raw else '\n'
        text = raw.decode('utf-8-sig').replace('\r\n', '\n')
        # Repeated preparation derives from the fixed English definitions.
        text = re.sub(r'^#langdef\s+zh-CN.*\n', '', text, flags=re.M)
        text = re.sub(r'^\s*#language\s+zh-CN\s+"[^"]*"\s*\n', '', text, flags=re.M)
        at = text.index('#langdef')
        text = text[:at] + '#langdef zh-CN "简体中文"\n' + text[at:]
        pattern = r'(#string\s+\w+\s+#language\s+en-US\s+"([^"]*)")'
        def replace(match):
            english = match[2]
            value = mapping.get(english, english)
            chars.update(ch for ch in value if 0x2e80 <= ord(ch) < 0xfff0)
            return match[1] + '\n    #language zh-CN "' + wide_controls(value) + '"'
        text = re.sub(pattern, replace, text)
        output_file(path, text.replace('\n', ending).encode('utf-8'), apply)
        files.append(str(path.relative_to(root)))
    return chars, files


def font_header(root, chars, apply):
    import freetype
    fonts = (Path('/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc'),
             Path('/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc'))
    font = next((p for p in fonts if p.is_file()), None)
    if font is None:
        raise ValueError('Install fonts-noto-cjk / noto-fonts-cjk for Setup Chinese glyphs')
    face = freetype.Face(str(font), index=2)
    face.set_pixel_sizes(16, 16)
    rows = ['// Generated from Noto Sans CJK SC; retain its SIL OFL notice.',
            'STATIC CONST EFI_WIDE_GLYPH mPianoWideGlyphs[]={']
    for ch in sorted(chars):
        if not face.get_char_index(ord(ch)):
            raise ValueError('Missing Setup font glyph: ' + ch)
        face.load_char(ch, freetype.FT_LOAD_RENDER | freetype.FT_LOAD_TARGET_MONO |
                       freetype.FT_LOAD_MONOCHROME)
        slot, bitmap = face.glyph, face.glyph.bitmap
        pixels = [[0] * 16 for _ in range(19)]
        x0 = (16 - bitmap.width) // 2
        y0 = 16 - slot.bitmap_top
        for y in range(bitmap.rows):
            for x in range(bitmap.width):
                if 0 <= x0 + x < 16 and 0 <= y0 + y < 19:
                    byte = bitmap.buffer[y * abs(bitmap.pitch) + x // 8]
                    pixels[y0+y][x0+x] = bool(byte & (0x80 >> (x % 8)))
        cols = [[sum((0x80 >> x) if pixels[y][offset+x] else 0 for x in range(8))
                 for y in range(19)] for offset in (0, 8)]
        rows.append('{0x%04x,EFI_GLYPH_WIDE,{%s},{%s},{0,0,0}},' %
                    (ord(ch), ','.join(map(str, cols[0])), ','.join(map(str, cols[1]))))
    rows.append('};\n')
    target = root / CONSOLE / 'PianoChineseGlyphs.h'
    data = '\n'.join(rows).encode('utf-8')
    output_file(target, data, apply)
    return {'glyphs': len(chars), 'font_sha256': hashlib.sha256(font.read_bytes()).hexdigest(),
            'generated_sha256': hashlib.sha256(data).hexdigest()}


def prepare(root, apply=False):
    chars, files = translations(root, apply)
    chars.update(ch for ch in BOOT_TEXT if 0x2e80 <= ord(ch) < 0xfff0)
    output_file(root / CONSOLE / 'PianoConsoleScale.h',
                (root / SOURCE / 'PianoConsoleScale.h').read_bytes(), apply)
    return {'scale': 4, 'physical': [3200, 2136], 'logical': [800, 534],
            'translated_files': files, 'font': font_header(root, chars, apply)}
