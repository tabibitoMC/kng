ENTRY(_start)
SECTIONS {
  . = 1M;
  .multiboot : { KEEP(*(.multiboot)) }
  .text   : ALIGN(4K) { *(.text*) }
  .rodata : ALIGN(4K) { *(.rodata*) }
  .data   : ALIGN(4K) { *(.data*) }
  .bss    : ALIGN(4K) { *(COMMON) *(.bss*) }
  _kend = .;
  ASSERT(_kend <= 0x400000, "la imagen del kernel debe terminar antes de 4 MB (ahi empieza el espacio de usuario)")
  /DISCARD/ : { *(.note*) *(.comment) *(.eh_frame*) }
}
