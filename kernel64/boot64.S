/* kng-kernel64: arranque Multiboot 1 (entra en modo protegido de 32 bits) -> modo largo (64 bits) + stubs de interrupciones.
 * Se enlaza como ELF64 y luego se convierte a ELF32 (objcopy) porque Multiboot 1 solo carga ELF32. */
.set MAGIC,    0x1BADB002
#ifndef NOVIDEO
.set FLAGS,    0x00000007
#else
.set FLAGS,    0x00000003
#endif
.set CHECKSUM, -(MAGIC + FLAGS)

.section .multiboot, "a"
.align 4
.long MAGIC, FLAGS, CHECKSUM
#ifndef NOVIDEO
.long 0, 0, 0, 0, 0
.long 0, 1024, 768, 32
#endif

.section .bss
.align 4096
pml4:  .skip 4096
pdpt:  .skip 4096
pd:    .skip 4096*16
.global pd, pml4
.align 16
stack_bottom: .skip 65536
stack_top:
mb_magic: .skip 8
mb_info:  .skip 8

.section .rodata
.align 8
gdt64:
    .quad 0
    .quad 0x00AF9A000000FFFF        /* 0x08: codigo 64 bits, ring 0 */
    .quad 0x00CF92000000FFFF        /* 0x10: datos, ring 0 */
gdt64_ptr:
    .word gdt64_ptr - gdt64 - 1
    .long gdt64
msg_nolong: .asciz "kng64: este procesador no soporta 64 bits (modo largo). Use el kernel de 32 bits."

.section .text
.code32
.global _start
_start:
    cli
    mov %eax, mb_magic
    mov %ebx, mb_info
    movl $0, mb_magic+4
    movl $0, mb_info+4
    mov $stack_top, %esp
    /* CPUID disponible? (bit 21 de EFLAGS) */
    pushfl
    pop %eax
    mov %eax, %ecx
    xor $0x200000, %eax
    push %eax
    popfl
    pushfl
    pop %eax
    push %ecx
    popfl
    cmp %ecx, %eax
    je .nolong
    mov $0x80000000, %eax
    cpuid
    cmp $0x80000001, %eax
    jb .nolong
    mov $0x80000001, %eax
    cpuid
    test $(1<<29), %edx              /* LM */
    jz .nolong
    /* tablas: PML4[0] -> PDPT; PDPT[0..15] -> 16 PD de 512 entradas de 2 MB = 16 GB en identidad */
    mov $pdpt, %eax
    or $3, %eax
    mov %eax, pml4
    xor %ecx, %ecx
1:  mov %ecx, %eax
    shl $12, %eax
    add $pd, %eax
    or $3, %eax
    mov %eax, pdpt(,%ecx,8)
    inc %ecx
    cmp $16, %ecx
    jne 1b
    xor %ecx, %ecx
2:  mov %ecx, %edx
    shl $21, %edx
    or $0x83, %edx                    /* P | RW | PS */
    cmp $1024, %ecx                   /* a partir de 2 GB: sin cache (MMIO de tarjetas PCI y framebuffer); kmain64 vuelve a poner cache a la RAM real */
    jb 3f
    or $0x18, %edx
3:  mov %edx, pd(,%ecx,8)
    movl $0, pd+4(,%ecx,8)
    inc %ecx
    cmp $8192, %ecx
    jne 2b
    mov %cr4, %eax
    or $0x20, %eax                    /* PAE */
    mov %eax, %cr4
    mov $pml4, %eax
    mov %eax, %cr3
    mov $0xC0000080, %ecx             /* EFER */
    rdmsr
    or $0x100, %eax                   /* LME */
    wrmsr
    mov %cr0, %eax
    or $0x80000000, %eax              /* PG */
    mov %eax, %cr0
    lgdt gdt64_ptr
    ljmp $0x08, $long_entry
.nolong:
    mov $msg_nolong, %esi
    mov $0xB8000, %edi
    mov $0x4F, %ah
4:  lodsb
    test %al, %al
    jz 5f
    mov %ax, (%edi)
    add $2, %edi
    jmp 4b
5:  cli
    hlt
    jmp 5b

.code64
long_entry:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss
    mov $stack_top, %esp
    xor %ebp, %ebp
    mov mb_magic, %edi
    mov mb_info, %esi
    call kmain64
6:  cli
    hlt
    jmp 6b

/* ---- interrupciones ---- */
.macro ISR_NOERR n
isr\n:
    pushq $0
    pushq $\n
    jmp isr_common64
.endm
.macro ISR_ERR n
isr\n:
    pushq $\n
    jmp isr_common64
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

isr_common64:
    push %rax
    push %rcx
    push %rdx
    push %rbx
    push %rbp
    push %rsi
    push %rdi
    push %r8
    push %r9
    push %r10
    push %r11
    push %r12
    push %r13
    push %r14
    push %r15
    testb $3, 144(%rsp)              /* venia de ring 3? entonces lo atiende proc64.S (cambia de CR3 y de pila) */
    jnz enter_from_user
    mov %rsp, %rdi
    cld
    call isr_handler64
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rdi
    pop %rsi
    pop %rbp
    pop %rbx
    pop %rdx
    pop %rcx
    pop %rax
    add $16, %rsp
    iretq

.section .data
.align 8
.global isr_table64
isr_table64:
.irp n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47
.quad isr\n
.endr
