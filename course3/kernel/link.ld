/* Chapter 01: a bare-metal RV32 image that starts at 0x80000000. */
ENTRY(_start)
SECTIONS
{
  . = 0x80000000;
  .text   : { *(.text.start) *(.text .text.*) }
  .rodata : { *(.rodata .rodata.*) *(.srodata .srodata.*) }
  .data   : { *(.data .data.*) *(.sdata .sdata.*) }
  .bss    : { *(.bss .bss.*) *(COMMON) }
  _end = .;
}
