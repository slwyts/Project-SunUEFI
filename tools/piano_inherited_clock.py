"""Derive the product-staging ClockDxe compatibility image from pinned ROM bytes.

This pure function does not modify the capture or write files. Product staging
may retain the seven CESTA clock acquisitions for firmware lifetime by skipping
the shared initialization cleanup call. This is neither display-only ownership
nor verified hardware behavior, and no automatic EBS retirement is claimed.
"""
import hashlib
import struct

ORIGINAL_SHA256 = 'f9e85aa758932b4366c55ec83b58e0176f58fba2944cc2fdd34875a46fdb769e'
PATCH_RVA = 0xc6d0
ORIGINAL_INSTRUCTION = bytes.fromhex('79ffff97')  # BL 0xc4b4
REPLACEMENT_INSTRUCTION = bytes.fromhex('00008052')  # MOV w0,#0
ORIGINAL_CONTEXT = bytes.fromhex('007975f879ffff97e0000035')
PROTOCOL_RVA = 0x28148
PROTOCOL_BYTES = 0x128  # Version and all 36 callbacks, before the next list object.
RETAINED_CLOCKS = (
    'gcc_camera_ahb_clk', 'cam_cc_drv_xo_clk', 'cam_cc_drv_ahb_clk',
    'gcc_disp_ahb_clk', 'disp_cc_mdss_non_gdsc_ahb_clk',
    'disp_cc_mdss_rscc_ahb_clk', 'disp_cc_mdss_rscc_vsync_clk',
)


def _sha(data):
    return hashlib.sha256(data).hexdigest()


def _section_offset(data, rva, size):
    pe = struct.unpack_from('<I', data, 60)[0]
    if data[pe:pe + 4] != b'PE\0\0' or struct.unpack_from('<H', data, pe + 4)[0] != 0xaa64:
        raise ValueError('Expected the captured ARM64 PE')
    count = struct.unpack_from('<H', data, pe + 6)[0]
    headers = pe + 24 + struct.unpack_from('<H', data, pe + 20)[0]
    for i in range(count):
        _, _, virtual, raw_size, raw_offset = struct.unpack_from('<8s4I', data, headers + i * 40)
        if virtual <= rva and rva - virtual + size <= raw_size:
            return raw_offset + rva - virtual
    raise ValueError('Audited region is outside PE raw sections')


def _branch_target(word, pc):
    if word & 0xfc000000 != 0x94000000:
        raise ValueError('Expected an ARM64 BL at the cleanup site')
    displacement = word & 0x3ffffff
    if displacement & (1 << 25):
        displacement -= 1 << 26
    return pc + displacement * 4


def derive(original):
    """Return (derived_bytes, provenance), for product staging only."""
    if not isinstance(original, bytes) or _sha(original) != ORIGINAL_SHA256:
        raise ValueError('Original ClockDxe SHA256 pin mismatch')
    offset = _section_offset(original, PATCH_RVA, 4)
    if offset != PATCH_RVA or original[offset - 4:offset + 8] != ORIGINAL_CONTEXT:
        raise ValueError('ClockDxe LDR/BL/CBNZ cleanup context changed')
    if _branch_target(struct.unpack_from('<I', original, offset)[0], PATCH_RVA) != 0xc4b4:
        raise ValueError('Cleanup branch no longer targets the audited callee')
    protocol = _section_offset(original, PROTOCOL_RVA, PROTOCOL_BYTES)
    before_table = original[protocol:protocol + PROTOCOL_BYTES]
    if struct.unpack_from('<Q', before_table)[0] != 0x1000b:
        raise ValueError('Clock protocol version changed')
    callback_offsets = {'GetClockID': 8, 'EnableClock': 16, 'DisableClock': 24,
                        'GetClockPowerDomainID': 80, 'EnableClockPowerDomain': 88,
                        'DisableClockPowerDomain': 96}
    expected = (0x156c, 0x15ac, 0x15f0, 0x156c, 0x15ac, 0x15f0)
    callbacks = {name: struct.unpack_from('<Q', before_table, member)[0]
                 for name, member in callback_offsets.items()}
    if tuple(callbacks.values()) != expected:
        raise ValueError('Clock protocol callbacks/typed aliases changed')
    # This call site visits all four BSP instances, not just disp_cesta.
    clocks = []
    for i in range(4):
        instance = struct.unpack_from('<Q', original, 0x28380 + i * 8)[0]
        names = struct.unpack_from('<Q', original, instance + 0xc8)[0]
        count = struct.unpack_from('<I', original, instance + 0xd0)[0]
        for j in range(count):
            name = struct.unpack_from('<Q', original, names + j * 8)[0]
            clocks.append(original[name:original.index(b'\0', name)].decode('ascii'))
    if tuple(clocks) != RETAINED_CLOCKS:
        raise ValueError('CESTA retained-resource scope changed')
    derived = original[:offset] + REPLACEMENT_INSTRUCTION + original[offset + 4:]
    changed = [i for i, (old, new) in enumerate(zip(original, derived)) if old != new]
    if len(derived) != len(original) or changed != list(range(offset, offset + 4)):
        raise ValueError('Derivation changed bytes outside the four-byte instruction')
    if derived[protocol:protocol + PROTOCOL_BYTES] != before_table:
        raise ValueError('Derivation changed the protocol callback table')
    metadata = {
        'status': 'DERIVED_PRODUCT_STAGING_ONLY',
        'scope': 'FIRMWARE_LIFETIME_CESTA_CLOCK_RETENTION',
        'original_sha256': ORIGINAL_SHA256, 'derived_sha256': _sha(derived),
        'bytes': len(derived), 'hardware_verified': False,
        'patch_rva': PATCH_RVA, 'patch_file_offset': offset,
        'original_bytes_hex': ORIGINAL_INSTRUCTION.hex(),
        'replacement_bytes_hex': REPLACEMENT_INSTRUCTION.hex(),
        'original_instruction': 'BL 0xc4b4', 'replacement_instruction': 'MOV w0,#0',
        'semantic_return': 'Zero status for intentionally skipped cleanup; NOP would leave a descriptor pointer in w0.',
        'retained_clocks': clocks, 'retained_resource_count': len(clocks),
        'display_only': False, 'owned_retirement_claimed': False,
        'automatic_exit_release_verified': False, 'register_writes_added': False,
        'protocol_abi_unchanged': True, 'protocol_callback_table_unchanged': True,
        'changed_byte_count': len(changed), 'protocol_version': 0x1000b,
        'protocol_table_rva': PROTOCOL_RVA, 'protocol_table_bytes': PROTOCOL_BYTES,
        'protocol_table_sha256': _sha(before_table), 'callbacks': callbacks,
        'other_cesta_release_call_unchanged_rva': 0x8e48,
        'vote_limit': 'Parent/source/domain votes may remain; live power corners and power cost are not verified.',
    }
    return derived, metadata
