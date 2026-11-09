#!/usr/bin/env python3
"""Arma out/rootfs.cpio (formato cpio newc) para kng: programas de prueba + enlazador dinamico y libc del PC (copias reales).
Uso: python3 tools/mkrootfs.py [salida.cpio]     Necesita gcc con libc estatica (libc6-dev)."""
import os, subprocess, sys, tempfile, shutil
raiz = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(raiz, 'out', 'rootfs.cpio')
os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
tmp = tempfile.mkdtemp()
try:
    for d in ('bin', 'lib64', 'lib/x86_64-linux-gnu'): os.makedirs(os.path.join(tmp, d))
    src = os.path.join(raiz, 'kernel64', 'user', 'hola-glibc.c')
    subprocess.check_call(['gcc', '-O2', '-static-pie', '-o', tmp + '/bin/hola-pie', src])
    subprocess.check_call(['gcc', '-O2', '-static', '-no-pie', '-o', tmp + '/bin/hola-static', src])
    subprocess.check_call(['gcc', '-O2', '-o', tmp + '/bin/hola-dyn', src])
    msrc = os.path.join(raiz, 'kernel64', 'user', 'multi.c')
    subprocess.check_call(['gcc', '-O2', '-pthread', '-static-pie', '-o', tmp + '/bin/multi-pie', msrc])
    subprocess.check_call(['gcc', '-O2', '-pthread', '-static', '-no-pie', '-o', tmp + '/bin/multi-static', msrc])
    subprocess.check_call(['gcc', '-O2', '-pthread', '-o', tmp + '/bin/multi-dyn', msrc])
    ldd = subprocess.check_output(['ldd', tmp + '/bin/hola-dyn'], text=True)
    ld = libc = None
    for l in ldd.splitlines():
        p = l.split()
        if 'ld-linux' in l: ld = p[0] if p[0].startswith('/') else p[2]
        if l.strip().startswith('libc.so.6'): libc = p[2]
    shutil.copyfile(os.path.realpath(ld), tmp + '/lib64/ld-linux-x86-64.so.2')
    shutil.copyfile(os.path.realpath(libc), tmp + '/lib/x86_64-linux-gnu/libc.so.6')
    ents = []
    for dp, dn, fn in os.walk(tmp):
        for n in sorted(dn + fn): ents.append(os.path.relpath(os.path.join(dp, n), tmp))
    buf = bytearray(); ino = 1
    def put(name, mode, data):
        global ino
        nm = name.encode() + b'\0'
        hdr = '070701' + ''.join('%08x' % v for v in (ino, mode, 0, 0, 1, 0, len(data), 0, 0, 0, 0, len(nm), 0))
        buf.extend(hdr.encode() + nm); buf.extend(b'\0' * (-len(buf) % 4)); buf.extend(data); buf.extend(b'\0' * (-len(buf) % 4)); ino += 1
    for e in sorted(ents):
        p = os.path.join(tmp, e)
        if os.path.isdir(p): put(e, 0o040755, b'')
        else: put(e, 0o100755, open(p, 'rb').read())
    put('TRAILER!!!', 0, b'')
    open(out, 'wb').write(buf)
    print('rootfs: %s (%d KB, %d entradas)' % (out, len(buf) // 1024, len(ents)))
finally:
    shutil.rmtree(tmp)
