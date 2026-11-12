#!/usr/bin/env python3
"""Decode a bounded PNG screenshot from previously collected UEFI console RAM."""
import argparse
import base64
from pathlib import Path
import re
import struct
import zlib

ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('--test-id', type=int, required=True)
args = ap.parse_args()
directory = Path(__file__).resolve().parent.parent / 'private/analysis' / f'ramlog-test-{args.test_id}'
text = (directory / 'console.txt').read_text(errors='replace')
begin = text.rfind('SUNUEFI_PNG_BEGIN ')
if begin < 0:
    raise SystemExit('No RAM screenshot marker')
end = text.find('SUNUEFI_PNG_END', begin)
if end < 0:
    raise SystemExit('Incomplete RAM screenshot')
segment = text[begin:end]
size = int(re.search(r'bytes=(\d+)', segment).group(1))
if not 8 <= size <= 0x80000:
    raise SystemExit('Invalid screenshot size')
encoded = ''.join(re.findall(r'SUNUEFI_PNG_DATA ([A-Za-z0-9+/=]+)', segment))
png = base64.b64decode(encoded, validate=True)
if len(png) != size or png[:8] != b'\x89PNG\r\n\x1a\n':
    raise SystemExit('Screenshot size/signature mismatch')
offset = 8
while offset < len(png):
    length = struct.unpack_from('>I', png, offset)[0]
    if offset + 12 + length > len(png):
        raise SystemExit('Truncated PNG chunk')
    chunk = png[offset+4:offset+8+length]
    if zlib.crc32(chunk) != struct.unpack_from('>I', png, offset+8+length)[0]:
        raise SystemExit('Screenshot PNG CRC mismatch')
    offset += 12 + length
output = directory / 'uefi-screen.png'
output.write_bytes(png)
print(output)
