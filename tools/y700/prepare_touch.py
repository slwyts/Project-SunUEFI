#!/usr/bin/env python3
"""Bind the captured Y700 SPI touch wiring and original RAM firmware."""
from pathlib import Path
import hashlib
import json
import shutil
import struct
from compose_piano_dtb import read_fdt

FW_SHA256 = '31382287ce3c5842f6df299589233459056043878deb1d27f7ef6b7f104ce271'


def prepare(root, app):
    root, app = Path(root), Path(app)
    tree = read_fdt((root/'vendor/y700/dtb/y700-live.dtb').read_bytes())['tree']
    reg = struct.pack('>II', 0xa90000, 0x4000)
    nodes = [n for n, p in tree.items() if p.get('reg') == reg and
             p.get('compatible') == b'qcom,spi-geni\0']
    if len(nodes) != 1:
        raise ValueError('Y700 SPI touch controller changed')
    node = nodes[0]
    touch = tree[node+'/novatek@0']
    expected = {'compatible': b'novatek,NVT-ts-spi\0',
                'reg': struct.pack('>I', 0),
                'spi-max-frequency': struct.pack('>I', 10000000),
                'novatek,irq-gpio': struct.pack('>III', 0x26, 162, 0x2001)}
    if any(touch.get(k) != v for k, v in expected.items()):
        raise ValueError('Y700 Novatek wiring changed')
    refs = struct.unpack('>III', tree[node]['pinctrl-0'])+(struct.unpack('>I', touch['pinctrl-0'])[0],)
    for ref, pin, line in zip(refs, (49, 48, 50, 51), (1, 0, 2, 3)):
        matches = [n for n, p in tree.items() if p.get('phandle') == struct.pack('>I', ref)]
        if len(matches) != 1:
            raise ValueError('Y700 touch pin reference changed')
        mux = tree[matches[0]+'/mux']
        if mux.get('pins') != f'gpio{pin}\0'.encode() or mux.get('function') != f'qup1_se4_l{line}\0'.encode():
            raise ValueError('Y700 SPI pin mux changed')
    firmware = (root/'vendor/y700/touch/novatek_ts_csot_3k_fw.bin').read_bytes()
    if len(firmware) != 253952 or hashlib.sha256(firmware).hexdigest() != FW_SHA256:
        raise ValueError('Y700 touch firmware differs from the original device capture')
    header = ('// Board wiring and original firmware are verified by prepare_touch.py.\n#pragma once\n'
              '#define Y700_TOUCH_FRAME_BYTES 108U\n'
              '#define Y700_TOUCH_EVENT 0x130900U\n'
              f'#define Y700_TOUCH_NODE "{node}"\n'
              'STATIC CONST UINT8 mY700TouchFirmware[]={\n')
    header += ''.join(','.join(f'0x{b:02x}' for b in firmware[i:i+24])+',\n'
                      for i in range(0, len(firmware), 24))+'};\n'
    (app/'Y700TouchConfig.h').write_text(header)
    names = ('Y700Touch.c', 'Y700Touch.h', 'Y700TouchDecode.h', 'Y700TouchFirmware.inc')
    for name in names:
        shutil.copyfile(root/'uefi/platforms/y700Pkg'/name, app/name)
    inf = app/'ProductCore.inf'
    inf.write_text(inf.read_text().replace('[Sources]\n', '[Sources]\n  Y700Touch.c\n  Y700Touch.h\n'))
    result = dict(transport='mode-0 GPIO SPI; no host DMA', pins=dict(miso=48, mosi=49, clk=50, cs=51, irq=162),
                  firmware_sha256=FW_SHA256, firmware_bytes=len(firmware),
                  frame_bytes=108, raw_max=[19040,30400], hardware_verified=False)
    return result
