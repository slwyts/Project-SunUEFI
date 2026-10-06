"""Derive the product-staging ClockDxe compatibility image from pinned ROM bytes.

This pure function does not modify the capture or write files. Product staging
may retain the seven CESTA clock acquisitions for firmware lifetime and defer
display CESTA/PLL automatic initialization through their existing native flags.
This is neither display-only ownership nor full ABL display preservation, and
no verified hardware behavior or automatic EBS retirement is claimed.
"""
import hashlib
import struct

ORIGINAL_SHA256 = 'f9e85aa758932b4366c55ec83b58e0176f58fba2944cc2fdd34875a46fdb769e'
DERIVED_SHA256 = '3f459c822c8d8f87ca832b37709c3b56548319bfe60ff01ffd37ddc56d72d967'
PATCH_RVA = 0xc6d0
ORIGINAL_INSTRUCTION = bytes.fromhex('79ffff97')  # BL 0xc4b4
REPLACEMENT_INSTRUCTION = bytes.fromhex('00008052')  # MOV w0,#0
ORIGINAL_CONTEXT = bytes.fromhex('007975f879ffff97e0000035')
PROTOCOL_RVA = 0x28148
PROTOCOL_BYTES = 0x128  # Version and all 36 callbacks, before the next list object.
DISPLAY_CESTA_RVA = 0x3e4b0
DISPLAY_CESTA_FLAG_RVA = 0x3e588
DISPLAY_PLL_RVA = 0x31580
DISPLAY_PLL_FLAGS_RVA = 0x31590
AUTO_INIT_OPCODES = {
    0xc5b4: '09604339', 0xc5b8: '89000036', 0xc5bc: '08ffff97',
    0xc604: 'a84e4039', 0xc608: '28021036',
    0xc62c: 'c7f3ff97', 0xc644: '61f5ff97',
}
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


def _name_matches(data, name_rva, expected):
    encoded = expected.encode('ascii') + b'\0'
    offset = _section_offset(data, name_rva, len(encoded))
    return data[offset:offset + len(encoded)] == encoded


def derive(original):
    """Return (derived_bytes, provenance), for product staging only."""
    if not isinstance(original, bytes) or _sha(original) != ORIGINAL_SHA256:
        raise ValueError('Original ClockDxe SHA256 pin mismatch')
    offset = _section_offset(original, PATCH_RVA, 4)
    if offset != PATCH_RVA or original[offset - 4:offset + 8] != ORIGINAL_CONTEXT:
        raise ValueError('ClockDxe LDR/BL/CBNZ cleanup context changed')
    if _branch_target(struct.unpack_from('<I', original, offset)[0], PATCH_RVA) != 0xc4b4:
        raise ValueError('Cleanup branch no longer targets the audited callee')
    for rva, instruction in AUTO_INIT_OPCODES.items():
        instruction_offset = _section_offset(original, rva, 4)
        if original[instruction_offset:instruction_offset + 4] != bytes.fromhex(instruction):
            raise ValueError('Display automatic-initialization opcode guard changed')
    for rva, target in ((0xc5bc, 0xc1dc), (0xc62c, 0x9548), (0xc644, 0x9bc8)):
        if _branch_target(struct.unpack_from('<I', original, rva)[0], rva) != target:
            raise ValueError('Display automatic-initialization call target changed')
    if struct.unpack_from('<Q', original, 0x28388)[0] != DISPLAY_CESTA_RVA:
        raise ValueError('Display CESTA BSP instance changed')
    cesta_names = struct.unpack_from('<3Q', original, DISPLAY_CESTA_RVA)
    if cesta_names != (0x1657a, 0x16585, 0x1659c) or not all(
            _name_matches(original, rva, name) for rva, name in zip(
                cesta_names, ('disp_cesta', 'qcom,disp_cesta-pakala', '/soc/cesta@af27000'))):
        raise ValueError('Display CESTA descriptor names changed')
    cesta_flag = _section_offset(original, DISPLAY_CESTA_FLAG_RVA, 4)
    if cesta_flag != DISPLAY_CESTA_FLAG_RVA or struct.unpack_from('<I', original, cesta_flag)[0] != 1:
        raise ValueError('Display CESTA automatic-initialization flag changed')
    if (struct.unpack_from('<Q', original, 0x2fe28)[0] != DISPLAY_PLL_RVA or
            struct.unpack_from('<Q', original, DISPLAY_PLL_RVA)[0] != 0x14159 or
            not _name_matches(original, 0x14159, 'disp_cc_pll0')):
        raise ValueError('Display PLL BSP node/name changed')
    pll_flags = _section_offset(original, DISPLAY_PLL_FLAGS_RVA, 4)
    if pll_flags != DISPLAY_PLL_FLAGS_RVA or struct.unpack_from('<I', original, pll_flags)[0] != 0x04004000:
        raise ValueError('Display PLL automatic-initialization flags changed')
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
    changed_data = bytearray(original)
    changed_data[offset:offset + 4] = REPLACEMENT_INSTRUCTION
    changed_data[cesta_flag] = 0  # Existing instance-only TBZ skip route.
    struct.pack_into('<I', changed_data, pll_flags, 0x00004000)  # Preserve bit14; clear only bit26.
    derived = bytes(changed_data)
    changed = [i for i, (old, new) in enumerate(zip(original, derived)) if old != new]
    expected_changes = list(range(offset, offset + 4)) + [pll_flags + 3, cesta_flag]
    if len(derived) != len(original) or changed != expected_changes:
        raise ValueError('Derivation changed bytes outside the six-byte compatibility delta')
    if _sha(derived) != DERIVED_SHA256:
        raise ValueError('Derived ClockDxe SHA256 pin mismatch')
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
        'display_cesta_auto_init_deferred': True, 'display_pll_auto_init_deferred': True,
        'full_display_preservation_claimed': False,
        'display_cesta_auto_init': {'descriptor_rva': DISPLAY_CESTA_RVA,
                                   'flag_rva': DISPLAY_CESTA_FLAG_RVA,
                                   'original_bytes_hex': '01000000', 'derived_bytes_hex': '00000000'},
        'display_pll_auto_init': {'node_rva': DISPLAY_PLL_RVA, 'name': 'disp_cc_pll0',
                                 'flags_rva': DISPLAY_PLL_FLAGS_RVA,
                                 'original_bytes_hex': '00400004', 'derived_bytes_hex': '00400000',
                                 'cleared_bit': 26, 'preserved_bit14': True},
        'software_state_limit': 'Display CESTA tables and PLL initialized/active-config state are deferred, not fabricated. Later explicit display consumers may initialize them; inherited hardware state and UFS/USB behavior require verification.',
        'automatic_exit_release_verified': False, 'register_writes_added': False,
        'protocol_abi_unchanged': True, 'protocol_callback_table_unchanged': True,
        'changed_byte_count': len(changed), 'protocol_version': 0x1000b,
        'protocol_table_rva': PROTOCOL_RVA, 'protocol_table_bytes': PROTOCOL_BYTES,
        'protocol_table_sha256': _sha(before_table), 'callbacks': callbacks,
        'other_cesta_release_call_unchanged_rva': 0x8e48,
        'vote_limit': 'Parent/source/domain votes may remain; live power corners and power cost are not verified.',
    }
    return derived, metadata
