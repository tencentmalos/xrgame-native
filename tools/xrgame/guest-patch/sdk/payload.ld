/* SPDX-License-Identifier: GPL-2.0-or-later */
/* From shadPS4 guest/custom/v1/payload.ld. No CRT, dynamic linker, TLS, unwinder
 * or constructors: RX code first, then an optional RW segment. */
PHDRS { code PT_LOAD FLAGS(5); data PT_LOAD FLAGS(6); }
SECTIONS {
  . = 0;
  .text : { *(.text .text.*) } :code
  .rodata : { *(.rodata .rodata.*) } :code
  .xrg_imports : { KEEP(*(.xrg_imports)) } :code
  . = ALIGN(4096);
  .data : { *(.data .data.*) } :data
  .bss : { *(.bss .bss.* COMMON) } :data
  /DISCARD/ : { *(.comment .note.*) }
}
