#!/usr/bin/env python3
"""Maneja el kernel de kng en QEMU por el puerto serie: espera el indicador "kng> ", manda un comando, espera a que vuelva el indicador.
Uso: ci_drive.py <archivo-de-comandos> <registro.log> -- <comando de qemu...>
Cada linea del archivo:  [segundos|]comando     (segundos = tiempo maximo de espera, por omision 30). Las lineas con # se ignoran.
Linea especial:  @esperar N   (espera N segundos sin mandar nada)"""
import subprocess, sys, time, threading, os
cmds_file, logf = sys.argv[1], sys.argv[2]
qemu = sys.argv[sys.argv.index('--') + 1:]
cmds = [l.rstrip('\n') for l in open(cmds_file) if l.strip() and not l.startswith('#')]
p = subprocess.Popen(qemu, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=0)
buf = bytearray(); last = [time.time()]; lock = threading.Lock(); log = open(logf, 'wb')
def reader():
    while True:
        b = p.stdout.read(4096)
        if not b: break
        with lock: buf.extend(b); last[0] = time.time()
        log.write(b); log.flush()
threading.Thread(target=reader, daemon=True).start()
def listo():
    with lock: return bytes(buf[-5:]) == b'kng> ' and time.time() - last[0] > 0.7
def espera(t):
    fin = time.time() + t
    while time.time() < fin:
        if p.poll() is not None: return False
        if listo(): return True
        time.sleep(0.2)
    return False
ok = espera(180)
if not ok: sys.stderr.write('[ci_drive] no aparecio el indicador inicial\n')
for c in cmds:
    if c.startswith('@esperar'): time.sleep(float(c.split()[1])); continue
    t = 30
    if '|' in c and c.split('|', 1)[0].isdigit(): t, c = int(c.split('|', 1)[0]), c.split('|', 1)[1]
    if p.poll() is not None: break
    with lock: buf.clear()
    try: p.stdin.write(c.encode() + b'\r'); p.stdin.flush()
    except Exception: break
    if not espera(t):
        with lock: log.write(('\n[ci_drive] TIEMPO AGOTADO esperando el fin de: %s\n' % c).encode()); log.flush()
        # se intenta recuperar el indicador con Ctrl+C
        try: p.stdin.write(b'\x03\r'); p.stdin.flush()
        except Exception: pass
        espera(10)
try: p.terminate(); p.wait(10)
except Exception: p.kill()
