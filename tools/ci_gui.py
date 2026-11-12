#!/usr/bin/env python3
"""Prueba el escritorio grafico de kng en QEMU como lo haria una persona: mueve el raton, hace clics, escribe y saca capturas.
Uso: ci_gui.py guion.txt -- qemu-system-x86_64 ... (con -monitor unix:/tmp/gmon.sock,server,nowait y -serial stdio)
Ordenes del guion: esperar S | raton X Y | clic | doble | der | arrastrar X Y | tecla NOMBRE | escribir TEXTO | captura NOMBRE"""
import os, socket, subprocess, sys, threading, time, zlib, struct
guion, qemu = sys.argv[1], sys.argv[sys.argv.index('--') + 1:]
MON = '/tmp/gmon.sock'
p = subprocess.Popen(qemu, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
serie = bytearray()
def lector():
    while True:
        b = p.stdout.read1(4096) if hasattr(p.stdout, 'read1') else p.stdout.read(1)
        if not b: break
        serie.extend(b); sys.stdout.write(b.decode('latin-1')); sys.stdout.flush()
threading.Thread(target=lector, daemon=True).start()
fin = time.time() + 240
while time.time() < fin and b'Iniciando escritorio' not in serie: time.sleep(0.5)
time.sleep(4)
s = None
for _ in range(50):
    try: s = socket.socket(socket.AF_UNIX); s.connect(MON); break
    except OSError: time.sleep(0.2)
s.settimeout(5)
def mon(c):
    s.sendall((c + '\n').encode()); time.sleep(0.08)
    try: s.recv(65536)
    except Exception: pass
mon('info status')
pos = [0, 0]
def ir(x, y):                                     # el raton es relativo: primero a la esquina, luego al destino (pasos cortos y con pausa:
    for _ in range(12): mon('mouse_move -120 -120'); time.sleep(0.12)   #  la cola PS/2 de QEMU guarda pocos paquetes y el escritorio los lee al ritmo de sus cuadros)
    while x > 0 or y > 0:
        dx = min(100, x); dy = min(100, y)
        mon('mouse_move %d %d' % (dx, dy)); x -= dx; y -= dy; time.sleep(0.12)
    time.sleep(0.6)
def boton(b): mon('mouse_button %d' % b); time.sleep(0.5)
def clic(b=1): boton(b); boton(0)
TECLAS = {' ': 'spc', '.': 'dot', '-': 'minus', '_': 'shift-minus', '/': 'slash', ',': 'comma'}
def escribir(t):
    for ch in t:
        k = TECLAS.get(ch) or (('shift-' + ch.lower()) if ch.isupper() else ch)
        mon('sendkey ' + k); time.sleep(0.06)
def ppm_png(src, dst):
    d = open(src, 'rb').read(); parts = d.split(b'\n', 3)
    w, h = map(int, parts[1].split()); px = parts[3]
    raw = b''.join(b'\x00' + px[y * w * 3:(y + 1) * w * 3] for y in range(h))
    ch = lambda t, c: struct.pack('>I', len(c)) + t + c + struct.pack('>I', zlib.crc32(t + c) & 0xffffffff)
    open(dst, 'wb').write(b'\x89PNG\r\n\x1a\n' + ch(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + ch(b'IDAT', zlib.compress(raw, 6)) + ch(b'IEND', b''))
os.makedirs('capturas', exist_ok=True)
cur = [0, 0]
for linea in open(guion):
    linea = linea.split('#', 1)[0].strip() if not linea.startswith('escribir') else linea.rstrip('\n')
    if not linea: continue
    print('\n[gui] ' + linea, flush=True)
    o, _, a = linea.partition(' ')
    if o == 'esperar': time.sleep(float(a))
    elif o == 'raton': x, y = map(int, a.split()); ir(x, y); cur = [x, y]
    elif o == 'clic': clic(1)
    elif o == 'doble': mon('mouse_button 1'); time.sleep(0.07); mon('mouse_button 0'); time.sleep(0.07); mon('mouse_button 1'); time.sleep(0.07); mon('mouse_button 0'); time.sleep(0.6)
    elif o == 'der': clic(2)
    elif o == 'arrastrar':
        x, y = map(int, a.split()); boton(1)
        for i in range(1, 17): mon('mouse_move %d %d' % ((x - cur[0]) // 16, (y - cur[1]) // 16)); time.sleep(0.15)
        time.sleep(0.2); boton(0); cur = [x, y]
    elif o == 'tecla': mon('sendkey ' + a); time.sleep(0.1)
    elif o == 'escribir': escribir(a)
    elif o == 'captura':
        time.sleep(0.6); mon('screendump /tmp/c.ppm'); time.sleep(1.0)
        try: ppm_png('/tmp/c.ppm', 'capturas/%s.png' % a); print('[gui] captura %s.png' % a, flush=True)
        except Exception as e: print('[gui] captura fallo:', e, flush=True)
mon('quit'); time.sleep(1); p.kill()
