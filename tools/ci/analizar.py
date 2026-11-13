#!/usr/bin/env python3
"""analizar.py BINARIO DIRECCIONES: para cada direccion (separadas por coma) muestra las instrucciones previas
(si es una direccion de retorno, la llamada) y las cadenas de texto que esas instrucciones referencian."""
import sys, struct, subprocess, re
b_path = sys.argv[1]; addrs = [a for a in sys.argv[2].split(',') if a]
data = open(b_path, 'rb').read()
phoff = struct.unpack_from('<Q', data, 32)[0]; nph = struct.unpack_from('<H', data, 56)[0]
segs = [struct.unpack_from('<IIQQQQQQ', data, phoff + i * 56) for i in range(nph)]
def cadena(a):
    for t, f, off, va, pa, fs, ms, al in segs:
        if t == 1 and va <= a < va + fs:
            s = data[off + a - va: off + a - va + 100].split(b'\0')[0]
            if len(s) > 3 and all(32 <= c < 127 for c in s): return s.decode()
    return None
for spec in addrs:
    if spec.startswith('s'):
        print('cadena %s: %s' % (spec[1:], cadena(int(spec[1:], 16)))); continue
    a = int(spec.split(':')[0], 16); win = int(spec.split(':')[1], 16) if ':' in spec else 0x80
    out = subprocess.run(['objdump', '-d', '--no-show-raw-insn', '--start-address=%#x' % (a - win), '--stop-address=%#x' % (a + 0x10), b_path], capture_output=True, text=True).stdout
    lines = [l for l in out.splitlines() if re.match(r'^ +[0-9a-f]+:', l)]
    print('=== %#x' % a)
    for l in lines[-(win // 3):]:
        m = re.search(r'# ([0-9a-f]+)', l); c = cadena(int(m.group(1), 16)) if m else None
        print(l + ('   "%s"' % c if c else ''))
