"""Prepare the Piano tables from mu_wide_platforms for EDK2's ACPI table driver."""
import struct


def architected_timer_irqs(table, board):
    """Set the architected timer PPIs; leave the donor's unused MMIO frames disabled."""
    arch, = (n for n in board.values() if b'arm,armv8-timer' in n.get('compatible', b'').split(b'\0'))
    irqs = list(struct.iter_unpack('>III', arch['interrupts']))
    if len(irqs) != 4:
        raise ValueError('Expected four architected timer PPIs')
    for index, (kind, number, flags) in enumerate(irqs):
        sense = flags & 15
        if kind != 1 or sense not in (1, 2, 4, 8):
            raise ValueError('Unsupported architected timer interrupt')
        acpi = int(sense in (1, 2)) | (int(sense in (2, 8)) << 1) | (int('always-on' in arch) << 2)
        struct.pack_into('<II', table, 48 + index * 8, number + 16, acpi)


def prepare(source, output, board):
    # The community MCFG's seven ECAM segments do not describe Piano's single
    # DesignWare host with an iATU configuration window. DSDT has no PCI root.
    # IORT is generated separately from this board's XBL DMA routes.
    names = ('APIC', 'CSRT', 'DBG2', 'FACP', 'FACS', 'GTDT', 'PPTT', 'SPCR')
    output.mkdir(exist_ok=True)
    for name in names:
        data = bytearray((source / (name + '.aml')).read_bytes())
        if data[:4] != name.encode() or struct.unpack_from('<I', data, 4)[0] != len(data):
            raise ValueError(f'Invalid {name} input')
        if name == 'GTDT':
            architected_timer_irqs(data, board)
        if name != 'FACS':
            data[9] = 0
            data[9] = -sum(data) & 255
        (output / (name + '.bin')).write_bytes(data)
