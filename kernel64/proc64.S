/* proc64.S — entrada/salida a modo usuario (ring 3) y la instruccion SYSCALL del kernel de 64 bits. */
.section .data
.align 8
.global kstk_top
kstk_top:   .quad 0        /* pila del kernel para llamadas al sistema (la pone proc64.c) */
ursp:       .quad 0
ksaved_rsp: .quad 0
kcr3_save:  .quad 0

.section .text
.code64
/* u64 user_enter(entry=rdi, user_rsp=rsi, cr3=rdx): salta a ring 3; "vuelve" cuando el proceso termina (user_leave) */
.global user_enter
user_enter:
    push %rbx
    push %rbp
    push %r12
    push %r13
    push %r14
    push %r15
    mov %rsp, ksaved_rsp(%rip)
    mov %cr3, %rax
    mov %rax, kcr3_save(%rip)
    mov %rdx, %cr3
    pushq $0x1B              /* SS: datos de usuario */
    push %rsi                /* RSP de usuario */
    pushq $0x202             /* RFLAGS: IF */
    pushq $0x23              /* CS: codigo de usuario 64 bits */
    push %rdi                /* RIP */
    xor %eax,%eax
    xor %ebx,%ebx
    xor %ecx,%ecx
    xor %edx,%edx
    xor %esi,%esi
    xor %edi,%edi
    xor %ebp,%ebp
    xor %r8d,%r8d
    xor %r9d,%r9d
    xor %r10d,%r10d
    xor %r11d,%r11d
    xor %r12d,%r12d
    xor %r13d,%r13d
    xor %r14d,%r14d
    xor %r15d,%r15d
    iretq

/* void user_leave(code=rdi): abandona el proceso y regresa de user_enter con rax=code */
.global user_leave
user_leave:
    mov kcr3_save(%rip), %rax
    mov %rax, %cr3
    mov ksaved_rsp(%rip), %rsp
    mov %rdi, %rax
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbp
    pop %rbx
    ret

/* SYSCALL: rax=numero, rdi,rsi,rdx,r10,r8,r9 = argumentos; rcx=RIP de retorno, r11=RFLAGS (convencion de Linux x86-64) */
.global syscall_entry
syscall_entry:
    mov %rsp, ursp(%rip)
    mov kstk_top(%rip), %rsp
    push ursp(%rip)
    push %rcx
    push %r11
    push %rdi
    push %rsi
    push %rdx
    push %r10
    push %r8
    push %r9
    push %rax
    mov %rsp, %rdi
    cld
    call sys_dispatch64
    add $8, %rsp
    pop %r9
    pop %r8
    pop %r10
    pop %rdx
    pop %rsi
    pop %rdi
    pop %r11
    pop %rcx
    pop %rsp
    sysretq
