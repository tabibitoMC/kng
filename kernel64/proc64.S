/* proc64.S — entrada y salida de modo usuario (ring 3), cambio de tarea y SYSCALL del kernel de 64 bits.
 *
 * Toda entrada desde ring 3 (llamada al sistema, interrupcion o excepcion) construye el MISMO marco "struct regs"
 * (r15..rax, int_no, err, rip, cs, rflags, rsp, ss) en una pila estatica pequena (dentro de la imagen, visible con la CR3 del
 * usuario), cambia a la CR3 del kernel, copia el marco a la pila propia de la tarea y llama a kernel_entry().
 * Toda salida a ring 3 sale por iretq desde ret_to_user_frame. */
.section .bss
.align 16
.skip 8192
.global kstack_top
kstack_top:

.section .data
.align 8
.global kcr3, ucr3, cur_ktop
kcr3:       .quad 0        /* CR3 del kernel (identidad completa) */
ucr3:       .quad 0        /* CR3 del proceso que va a ring 3 */
cur_ktop:   .quad 0        /* tope de la pila de kernel de la tarea en curso */
ursp:       .quad 0

.section .text
.code64
.global syscall_entry
syscall_entry:             /* rax=numero, rdi,rsi,rdx,r10,r8,r9 = argumentos; rcx=RIP, r11=RFLAGS (ABI Linux x86-64) */
    mov %rsp, ursp(%rip)
    lea kstack_top(%rip), %rsp
    pushq $0x1B
    push ursp(%rip)
    push %r11
    pushq $0x23
    push %rcx
    pushq $0
    pushq $0x80
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
    /* sigue en enter_from_user */

.global enter_from_user
enter_from_user:          /* rsp -> marco en la pila estatica */
    mov kcr3(%rip), %rax
    mov %rax, %cr3
    mov cur_ktop(%rip), %rdi
    sub $176, %rdi
    mov %rsp, %rsi
    mov %rdi, %rax
    mov $22, %ecx
    cld
    rep movsq
    mov %rax, %rsp
    mov %rsp, %rdi
    call kernel_entry
    /* sigue en ret_to_user_frame */

.global ret_to_user_frame
ret_to_user_frame:        /* rsp -> marco de la tarea (en su pila de kernel) */
    cli
    lea kstack_top(%rip), %rcx
    mov 168(%rsp), %r11
    mov %r11, -8(%rcx)       /* ss */
    mov 160(%rsp), %r11
    mov %r11, -16(%rcx)      /* rsp */
    mov 152(%rsp), %r11
    mov %r11, -24(%rcx)      /* rflags */
    mov 144(%rsp), %r11
    mov %r11, -32(%rcx)      /* cs */
    mov 136(%rsp), %r11
    mov %r11, -40(%rcx)      /* rip */
    mov 104(%rsp), %r11
    mov %r11, -48(%rcx)      /* rcx final */
    mov 32(%rsp), %r11
    mov %r11, -56(%rcx)      /* r11 final */
    mov 0(%rsp), %r15
    mov 8(%rsp), %r14
    mov 16(%rsp), %r13
    mov 24(%rsp), %r12
    mov 40(%rsp), %r10
    mov 48(%rsp), %r9
    mov 56(%rsp), %r8
    mov 64(%rsp), %rdi
    mov 72(%rsp), %rsi
    mov 80(%rsp), %rbp
    mov 88(%rsp), %rbx
    mov 96(%rsp), %rdx
    mov 112(%rsp), %rax
    lea -56(%rcx), %r11
    mov ucr3(%rip), %rcx
    mov %rcx, %cr3
    mov %r11, %rsp
    pop %r11
    pop %rcx
    iretq

/* void switch_to(u64 *prev_ksp, u64 next_ksp) */
.global switch_to
switch_to:
    push %rbp
    push %rbx
    push %r12
    push %r13
    push %r14
    push %r15
    mov %rsp, (%rdi)
    mov %rsi, %rsp
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbx
    pop %rbp
    ret

.global task_start        /* primera vez que corre una tarea nueva: rsp -> su marco */
task_start:
    jmp ret_to_user_frame
