/* Reviewed BootObjects ABI; the product wrapper uses this same source. */
/* SPDX-License-Identifier: BSD-2-Clause-Patent */
_Head:
  adr x1, _Payload
  b _Start
  .quad FD_BASE
  .quad FD_SIZE
  .quad 0
  .quad 0
  .quad 0
  .quad 0
  .ascii "ARM\x64"
  .long 0
_Start:
  ldr x7, =0xA7FFF000
  ldr x8, =0x534E554546494448
  stp x8, x0, [x7]
  mrs x9, CurrentEL
  str x9, [x7, #16]
  ldr x8, =0x314A424F544F4F42
  str x8, [x7, #24]
  mov w8, #1
  str w8, [x7, #32]
  mov w8, #144
  str w8, [x7, #36]
  stp xzr, xzr, [x7, #40]
  stp xzr, xzr, [x7, #56]
  stp xzr, xzr, [x7, #72]
  stp xzr, xzr, [x7, #88]
  stp xzr, xzr, [x7, #104]
  stp xzr, xzr, [x7, #120]
  str xzr, [x7, #136]
  mov x10, sp
  str x10, [x7, #96]
  adr x10, _Head
  str x10, [x7, #56]
  adr x11, _Payload
  sub x11, x11, x10
  str x11, [x7, #64]
  str x1, [x7, #72]
  ldr x10, =FD_BASE
  str x10, [x7, #80]
  ldr x10, =FD_SIZE
  str x10, [x7, #88]
  adr x10, _Start
  str x10, [x7, #136]
  mov x11, #12
  cmp x9, #4
  b.ne .Lmetadata_done
  // Only the already-established EL1 path reads EL1 system registers.
  mrs x10, cntvct_el0
  str x10, [x7, #40]
  mrs x10, cntfrq_el0
  str x10, [x7, #48]
  mrs x10, vbar_el1
  str x10, [x7, #104]
  mrs x10, sctlr_el1
  str x10, [x7, #112]
  mov x11, #62
.Lmetadata_done:
  str x11, [x7, #120]
  mov x4, x1
  ldr x5, =FD_BASE
  cmp x4, x5
  b.eq .Lcopy_done
  orr x11, x11, #64
  str x11, [x7, #120]
  ldr x6, =FD_SIZE
.Lcopy:
  ldp x2, x3, [x4], #16
  stp x2, x3, [x5], #16
  subs x6, x6, #16
  b.ne .Lcopy
.Lcopy_done:
  ldr x11, [x7, #120]
  orr x11, x11, #1
  str x11, [x7, #120]
  // Same CRC32 layout as PianoColdHandoffCrc; CRC/reserved words remain zero
  // until this loop completes. No permission or randomness is claimed.
  mov x12, x7
  mov x13, #144
  mov w10, #-1
  mov w16, #0x8320
  movk w16, #0xedb8, lsl #16
.Lcrc_byte:
  ldrb w14, [x12], #1
  eor w10, w10, w14
  mov w15, #8
.Lcrc_bit:
  tst w10, #1
  lsr w10, w10, #1
  b.eq .Lcrc_next
  eor w10, w10, w16
.Lcrc_next:
  subs w15, w15, #1
  b.ne .Lcrc_bit
  subs x13, x13, #1
  b.ne .Lcrc_byte
  mvn w10, w10
  str w10, [x7, #128]
  dsb sy
  ldr x5, =FD_BASE
  br x5
.text
.ltorg
.align 4
_Payload:
