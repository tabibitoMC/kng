ENTRY(_start)
SECTIONS {
  . = 0x40000000;
  .text   : { *(.text*) }
  .rodata : { *(.rodata*) }
  .data   : { *(.data*) }
  .bss    : { *(COMMON) *(.bss*) }
  /DISCARD/ : { *(.note*) *(.comment) *(.eh_frame*) }
}
