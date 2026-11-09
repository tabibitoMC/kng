#!/usr/bin/env python3
"""Prueba en el host (sin QEMU) del PMM, los espacios de direcciones de usuario y el cargador ELF.
Extrae ese codigo de kernel.c, lo compila con ASan/UBSan y lo prueba con los ELF reales de user/.
Uso: make -C kernel hosttest"""
import subprocess, sys, os
os.chdir(os.path.dirname(os.path.abspath(__file__)))
s = open('kernel.c', encoding='utf-8').read()
a = s.index('#define PMM_MAX_PAGES'); b = s.index('static void *page_alloc_z(void)'); b = s.index('\n', b) + 1
c = s.index('/*HT-BEGIN*/'); d = s.index('/*HT-END*/')
head = """#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#define PG_P 1u
#define PG_RW 2u
#define PG_PS 0x80u
static uint32_t page_dir[1024] __attribute__((aligned(4096)));
static uint32_t pmm_next,pmm_end,pages_alloc;
"""
open('/tmp/kng_hosttest.c', 'w').write(head + s[a:b] + s[c:d] + open('hosttest_main.c').read())
subprocess.check_call(['gcc', '-O1', '-w', '-fsanitize=address,undefined', '-o', '/tmp/kng_hosttest', '/tmp/kng_hosttest.c'])
env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0')
sys.exit(subprocess.call(['/tmp/kng_hosttest'], env=env))
