#!/usr/bin/env python3
"""Host-only SHA-256 payload packaging for the separately built ARM64 EFI app."""
import hashlib
import json
from pathlib import Path
import struct

root = Path(__file__).resolve().parent.parent
out = root / 'artifacts/simpleinit'
app = (out / 'SimpleInit.efi').read_bytes()
if app[:2] != b'MZ':
    raise SystemExit('Expected PE/COFF EFI application')
pe = struct.unpack_from('<I', app, 60)[0]
if app[pe:pe + 4] != b'PE\0\0' or struct.unpack_from('<H', app, pe + 4)[0] != 0xaa64:
    raise SystemExit('Expected ARM64 EFI application')
digest = hashlib.sha256(app).digest()
header = struct.pack('<16sIIQ32s', b'SUNUEFI-APPv1\0', 1, 64, len(app), digest)
(out / 'app-payload.bin').write_bytes(header + app)
result = {'status': 'HOST_BUILD_ONLY', 'app_bytes': len(app), 'app_sha256': digest.hex(),
          'payload_bytes': len(header) + len(app), 'source_commit': '3d66a6e78d519dd050fbebde4db6c5ac933f9aa4',
          'locale': 'zh_CN.UTF-8', 'persistent_config': 'Firmware uses emulated variables; storage absent in initial GUI profile'}
(out / 'manifest.json').write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps(result, indent=2))
