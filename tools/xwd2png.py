#!/usr/bin/env python3
"""Convierte un archivo XWD (el que escribe Xvfb con -fbdir) a PNG, solo con la biblioteca estandar. Uso: xwd2png.py entrada.xwd salida.png"""
import struct, sys, zlib
def main(src, dst):
    d = open(src, 'rb').read()
    h = struct.unpack('>25I', d[:100])
    hsize, ver, fmt, depth, w, hgt, xoff, border, bunit, bbo, bpad, bpp, bpl, vclass, rm, gm, bm, bprgb, cme, ncol = h[:20]
    off = hsize + ncol * 12
    pix = d[off:]
    out = bytearray()
    sh = lambda m: (m & -m).bit_length() - 1
    rs, gs, bs = sh(rm), sh(gm), sh(bm)
    for y in range(hgt):
        row = pix[y * bpl:(y + 1) * bpl]
        out.append(0)
        for x in range(w):
            p = int.from_bytes(row[x * (bpp // 8):(x + 1) * (bpp // 8)], 'little' if border == 0 else 'big')
            out += bytes(((p & rm) >> rs, (p & gm) >> gs, (p & bm) >> bs))
    def chunk(t, c): return struct.pack('>I', len(c)) + t + c + struct.pack('>I', zlib.crc32(t + c) & 0xffffffff)
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, hgt, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(bytes(out), 6)) + chunk(b'IEND', b'')
    open(dst, 'wb').write(png)
    print('PNG', w, 'x', hgt, 'bytes', len(png))
main(sys.argv[1], sys.argv[2])
