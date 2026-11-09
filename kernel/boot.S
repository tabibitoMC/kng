/* kng-kernel: arranque Multiboot + stubs de interrupciones (i386) */
.set MAGIC,    0x1BADB002
#ifndef NOVIDEO
.set FLAGS,    0x00000007            /* alinear + info de memoria + pedir modo de video */
#else
.set FLAGS,    0x00000003
#endif
.set CHECKSUM, -(MAGIC + FLAGS)

.section .multiboot, "a"
.align 4
.long MAGIC, FLAGS, CHECKSUM
#ifndef NOVIDEO
.long 0, 0, 0, 0, 0                  /* campos de direccion (sin uso con ELF) */
.long 0, 1024, 768, 32               /* modo lineal 1024x768, 32 bpp */
#endif

.section .bss
.align 16
stack_bottom: .skip 16384
stack_top:

.section .text
.global _start
_start:
    cli
    mov $stack_top, %esp
    push %ebx
    push %eax
    call kmain
1:  cli
    hlt
    jmp 1b

.macro ISR_NOERR n
isr\n:
    push $0
    push $\n
    jmp isr_common
.endm
.macro ISR_ERR n
isr\n:
    push $\n
    jmp isr_common
.endm

.irp n,0,1,2,3,4,5,6,7,9,15,16,18,19,20,21,22,23,24,25,26,27,28,29,30,31
ISR_NOERR \n
.endr
.irp n,8,10,11,12,13,14,17
ISR_ERR \n
.endr
.irp n,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47
ISR_NOERR \n
.endr

.global isr128
ISR_NOERR 128

isr_common:
    pusha
    push %esp
    call isr_handler
    mov %eax, %esp          /* isr_handler devuelve la pila de la tarea a retomar */
    testb $3, 44(%esp)      /* RPL del CS que se va a restaurar */
    jz 1f
    mov $0x23, %ax          /* volvemos a ring 3: segmentos de datos de usuario */
    jmp 2f
1:  mov $0x10, %ax
2:  mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    popa
    add $8, %esp
    iret

.section .data
.global isr_table
isr_table:
.irp n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47
.long isr\n
.endr
