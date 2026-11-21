"""Describe Piano's GENI console and DWC3 debug ports using the Pakala tables."""
import struct


_GAS = struct.Struct('<BBBBQ')
_PORT = struct.Struct('<BHBHHHHHHHHH')
_WRITE = struct.Struct('<BBHIII')


def _dt_strings(node, name):
    return node[name].rstrip(b'\0').split(b'\0')


def _uart(board):
    alias, settings = _dt_strings(board['/chosen'], 'stdout-path')[0].split(b':', 1)
    uart = board[_dt_strings(board['/aliases'], alias.decode())[0].decode()]
    kind, spi, flags = struct.unpack('>III', uart['interrupts'])
    if (b'qcom,geni-debug-uart' not in _dt_strings(uart, 'compatible') or
            settings != b'115200n8' or kind != 0 or flags & 15 != 4):
        raise ValueError('Expected the Piano GENI console with its level-high GIC SPI')
    base, length = struct.unpack('>II', uart['reg'])
    return base, length, spi + 32


def _usb(board):
    (path, wrapper), = ((p, n) for p, n in board.items()
                        if b'qcom,dwc-usb3-msm' in n.get('compatible', b'').split(b'\0')
                        and n.get('status', b'okay\0') in (b'ok\0', b'okay\0'))
    names = _dt_strings(wrapper, 'reg-names')
    base, length = dict(zip(names, struct.iter_unpack('>II', wrapper['reg']), strict=True))[b'core_base']
    core, = (n for p, n in board.items() if p.rpartition('/')[0] == path
             and b'snps,dwc3' in n.get('compatible', b'').split(b'\0'))
    core_base, core_bytes = struct.unpack('>QQ', core['reg'])
    if core_base != base or length < 0xf8814 or not 0xc708 <= core_bytes <= length:
        raise ValueError('Piano DWC3 debug registers are outside the board MMIO windows')
    return base, length


def _ports(table):
    offset, count = struct.unpack_from('<II', table, 36)
    for _ in range(count):
        fields = _PORT.unpack_from(table, offset)
        length, name_size, name_pos = fields[1], fields[3], fields[4]
        if not (_PORT.size <= name_pos < name_pos + name_size <= length <= len(table) - offset):
            raise ValueError('Invalid Pakala DBG2 device length or namespace')
        yield fields, bytes(table[offset + name_pos:offset + name_pos + name_size])
        offset += length
    if offset != len(table):
        raise ValueError('Invalid Pakala DBG2 device count')


def _info(source, path, ranges, oem=b''):
    count = len(ranges)
    size_pos = _PORT.size + count * _GAS.size
    name_pos = size_pos + count * 4
    vendor_pos = name_pos + len(path)
    # Microsoft DBG2 device revision 0. No fictitious namespace object for UART.
    header = _PORT.pack(0, vendor_pos + len(oem), count, len(path), name_pos, len(oem),
                        vendor_pos if oem else 0, source[7], source[8], 0, _PORT.size, size_pos)
    registers = b''.join(_GAS.pack(0, 32, 0, 3, base) for base, _ in ranges)
    lengths = b''.join(struct.pack('<I', size) for _, size in ranges)
    return header + registers + lengths + path + oem


def _synopsys(super_speed, oem_size):
    # Pakala ACPI/src/DBG2.dsl's original hex dump distinguishes the two URS0
    # modes. The compiled OEM fields contain the start of the table instead.
    # WDK's Synopsys USB-function miniport reads these as masked 32-bit writes.
    if oem_size != 148:
        raise ValueError('Unexpected Pakala Synopsys OEM record size')
    if super_speed:
        control = (0xc704, 0xffffe1ff)  # DCTL: clear U1/U2 requests/enables
        session_phase = 1
    else:
        control = (0xc700, 0xfffffff8)  # DCFG: select USB high speed
        session_phase = 2
    data = bytearray(oem_size)
    struct.pack_into('<HH4sI', data, 0, 5, 0, b'2XIF', 2)
    _WRITE.pack_into(data, 12, 0, 2, 0, *control, 0)
    # Preserve other session bits, as the Piano USB driver does. The reference
    # board's additional whole-register write at 0xf88b4 is not used by Piano.
    _WRITE.pack_into(data, 28, 0, session_phase, 0, 0xf8810, 0xffffffff, (1 << 20) | (1 << 28))
    data[-4:] = b'CBSU'  # Reference port-zero USB connector trailer (little endian).
    return data


def prepare(table, board):
    """Return one corrected debug table, or the unmodified bytes for other tables."""
    sig = table[:4]
    if sig not in (b'SPCR', b'DBG2'):
        return bytes(table)
    uart, uart_size, uart_irq = _uart(board)
    result = bytearray(table)
    if sig == b'SPCR':
        # Android's clock framework measured 7,372,800 Hz on both the GENI
        # source and its a9c000 UART branch. Idle gating does not change the
        # configured rate. This is Microsoft's 0x13 subtype, not a 16550 UART.
        if len(result) != 80 or (result[8], result[36], result[52], result[58]) != (2, 0x13, 8, 7):
            raise ValueError('Unexpected Pakala serial port layout')
        _GAS.pack_into(result, 40, 0, 32, 0, 3, uart)
        struct.pack_into('<I', result, 54, uart_irq)
    else:
        ref = list(_ports(result))
        serial, = (f for f, path in ref if f[7:9] == (0x8000, 0x13) and path in (b'\\_SB.UARD\0', b'.\0'))
        hs, ss = (f for f, path in ref if f[7:9] == (0x8003, 0x5143) and path == b'\\_SB.URS0\0')
        base, size = _usb(board)
        ranges = ((base, size), (base, 0x1000))
        rows = (_info(serial, b'.\0', ((uart, uart_size),)),
                _info(hs, b'\\_SB.URS0\0', ranges, _synopsys(False, hs[5])),
                _info(ss, b'\\_SB.URS0\0', ranges, _synopsys(True, ss[5])))
        result = result[:36] + struct.pack('<II', 44, len(rows)) + b''.join(rows)
        result[8] = 0
    struct.pack_into('<I', result, 4, len(result))
    result[9] = 0
    result[9] = -sum(result) & 255
    return bytes(result)
