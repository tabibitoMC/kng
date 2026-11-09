#!/usr/bin/env python3
"""Compila el simulador del escritorio, lo ejecuta y convierte las capturas PPM a PNG (en OUTDIR, por defecto /tmp/kngshots)."""
import os, subprocess, glob, sys
from PIL import Image
here = os.path.dirname(os.path.abspath(__file__)); out = os.environ.get('OUTDIR', '/tmp/kngshots'); os.makedirs(out, exist_ok=True)
subprocess.check_call(['gcc', '-O1', '-g', '-w', '-fsanitize=address,undefined', '-o', '/tmp/hostgui', os.path.join(here, 'hostgui.c')])
env = dict(os.environ, OUTDIR=out, ASAN_OPTIONS='detect_leaks=0')
subprocess.check_call(['/tmp/hostgui'], env=env)
for p in sorted(glob.glob(out + '/*.ppm')):
    Image.open(p).save(p[:-4] + '.png'); os.remove(p); print(p[:-4] + '.png')
