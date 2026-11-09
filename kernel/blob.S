/* Programas de usuario incrustados en el kernel (ELF32) */
.section .rodata
.balign 16
.global blob_utest_start, blob_utest_end
blob_utest_start: .incbin "user/utest.elf"
blob_utest_end:
.balign 16
.global blob_crash_start, blob_crash_end
blob_crash_start: .incbin "user/crash.elf"
blob_crash_end:
.balign 16
.global blob_spin_start, blob_spin_end
blob_spin_start: .incbin "user/spin.elf"
blob_spin_end:
.balign 16
.global blob_ush_start, blob_ush_end
blob_ush_start: .incbin "user/ush.elf"
blob_ush_end:
.balign 16
.global blob_forktest_start, blob_forktest_end
blob_forktest_start: .incbin "user/forktest.elf"
blob_forktest_end:
.section .note.GNU-stack,"",@progbits
