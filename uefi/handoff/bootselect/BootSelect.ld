/* SPDX-License-Identifier: BSD-2-Clause-Patent */
ENTRY(_start)
SECTIONS {
  . = 0;
  .text : { KEEP(*(.text.entry)) *(.text*) }
  .rodata : ALIGN(16) { *(.rodata*) }
  .data : ALIGN(16) { *(.data*) }
  . = ALIGN(16);
  .stack (NOLOAD) : {
    __stack_start = .;
    . += 8192;
    __stack_end = .;
  }
  __image_end = .;
  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
