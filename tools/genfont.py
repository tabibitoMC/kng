#!/usr/bin/env python3
"""Genera kernel/font.h: fuente de mapa de bits con antialiasing (8x16 y 12x24, Latin-1) a partir de DejaVu Sans Mono
(licencia libre Bitstream Vera/DejaVu). El resultado ya viene incluido; solo hace falta ejecutarlo para cambiar la fuente."""
import sys
from PIL import Image, ImageDraw, ImageFont
PATH = sys.argv[1] if len(sys.argv) > 1 else '/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf'
def make(size, W, H, base):
    f = ImageFont.truetype(PATH, size); out = []
    for code in range(32, 256):
        im = Image.new('L', (W, H), 0)
        ImageDraw.Draw(im).text((0, base), chr(code), font=f, fill=255, anchor='ls')
        out.append(list(im.getdata()))
    return out
def emit(name, data):
    s = f'static const unsigned char {name}[224][{len(data[0])}] = {{\n'
    for g in data: s += '{' + ','.join(map(str, g)) + '},\n'
    return s + '};\n'
with open('kernel/font.h', 'w') as fh:
    fh.write('/* Generado por tools/genfont.py (DejaVu Sans Mono). Alfa 0-255, filas de arriba abajo. Codigos 32..255. */\n')
    fh.write(emit('font_s', make(13, 8, 16, 12)))
    fh.write(emit('font_b', make(20, 12, 24, 19)))
