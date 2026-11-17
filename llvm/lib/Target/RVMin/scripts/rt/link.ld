/* Chapter 12: a freestanding RV32I layout for QEMU user mode. */
ENTRY(_start)
SECTIONS
{
  . = 0x10000;
  .text   : { *(.text.start) *(.text .text.*) }
  /* QEMU >= 8 maps each segment with its own permissions.  Keep text in its
     own page so a read-only .rodata segment cannot clear the execute bit on
     the page that holds _start. */
  .rodata : ALIGN(0x1000) { *(.rodata .rodata.*) *(.srodata .srodata.*) }
  .data   : ALIGN(0x1000) { *(.data .data.*) *(.sdata .sdata.*) }
  .bss    : ALIGN(0x1000) { *(.bss .bss.*) *(COMMON) }
}
