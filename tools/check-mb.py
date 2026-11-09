#!/usr/bin/env python3
"""Comprueba que cada ELF cumple Multiboot 1 (lo que GRUB exige): ELF32, cabecera en los primeros 8 KB, suma de control correcta."""
import struct, sys
ok=True
for path in sys.argv[1:]:
    d=open(path,'rb').read()
    if d[:4]!=b'\x7fELF' or d[4]!=1: print(path,'ERROR: no es ELF32 (Multiboot 1 solo carga ELF32)'); ok=False; continue
    entry=struct.unpack_from('<I',d,24)[0]
    for off in range(0,min(len(d)-12,8192),4):
        m,f,c=struct.unpack_from('<III',d,off)
        if m==0x1BADB002 and (m+f+c)&0xFFFFFFFF==0:
            mode=''
            if f&4: mode=' modo de video '+'x'.join(str(v) for v in struct.unpack_from('<III',d,off+36))
            print(f'{path}: OK  cabecera en {off}, flags=0x{f:x}{mode}, entrada=0x{entry:x}'); break
    else: print(path,'ERROR: cabecera Multiboot no encontrada en los primeros 8 KB'); ok=False
sys.exit(0 if ok else 1)
