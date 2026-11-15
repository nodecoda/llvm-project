/* Chapter 12: a freestanding RV32I layout for QEMU user mode. */
ENTRY(_start)
SECTIONS
{
  . = 0x10000;
  .text   : { *(.text.start) *(.text .text.*) }
  .rodata : { *(.rodata .rodata.*) *(.srodata .srodata.*) }
  .data   : { *(.data .data.*) *(.sdata .sdata.*) }
  .bss    : { *(.bss .bss.*) *(COMMON) }
}
