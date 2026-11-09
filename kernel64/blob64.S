/* blob64.S — programas ELF64 incluidos en el kernel (se ejecutan con: ejecutar /bin/<nombre>) */
.section .rodata
.align 16
.global blob_hola_start, blob_hola_end, blob_glibc_start, blob_glibc_end
blob_hola_start:   .incbin "user/hola.elf"
blob_hola_end:
.align 16
blob_glibc_start:  .incbin "user/hola-glibc.elf"
blob_glibc_end:
