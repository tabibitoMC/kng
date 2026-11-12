#!/usr/bin/env python3
"""Maneja el kernel de kng en QEMU por el puerto serie: espera el indicador "kng> ", manda un comando, espera a que vuelva el indicador.
Uso: ci_drive.py <archivo-de-comandos> <registro.log> -- <comando de qemu...>
Cada linea del archivo:  [segundos|]comando     (segundos = tiempo maximo de espera, por omision 30). Las lineas con # se ignoran.
Linea especial:  @esperar N   (espera N segundos sin mandar nada)
Si hay un monitor de QEMU (variable QMON = ruta del socket) y la VM se cuelga, se vuelcan los registros y la pila con simbolos (variable KELF = kernel .elf)."""
import os, subprocess, sys, time, threading, os, socket, re, bisect
cmds_file, logf = sys.argv[1], sys.argv[2]
qemu = sys.argv[sys.argv.index('--') + 1:]
hang_vuelcos = []
cmds = [l.rstrip('\n') for l in open(cmds_file) if l.strip() and not l.startswith('#')]
p = subprocess.Popen(qemu, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=0)
buf = bytearray(); last = [time.time()]; lock = threading.Lock(); log = open(logf, 'wb')

def cargar_simbolos():
    try:
        out = subprocess.check_output(['nm', '-n', os.environ['KELF']], text=True)
    except Exception: return [], []
    ad, nm = [], []
    for l in out.splitlines():
        p = l.split()
        if len(p) == 3 and p[1] in 'tTwW': ad.append(int(p[0], 16)); nm.append(p[2])
    return ad, nm
SYM = cargar_simbolos()
def sim(v):
    ad, nm = SYM
    if not ad or v < ad[0] or v > ad[-1] + 0x20000: return None
    i = bisect.bisect_right(ad, v) - 1
    return '%s+0x%x' % (nm[i], v - ad[i])
def monitor(cmd, espera=1.0):
    q = os.environ.get('QMON')
    if not q: return ''
    try:
        s = socket.socket(socket.AF_UNIX); s.settimeout(3); s.connect(q)
        time.sleep(0.3); s.recv(65536)
        s.sendall((cmd + '\n').encode()); time.sleep(espera)
        s.settimeout(1.0); data = b''
        try:
            while True:
                b = s.recv(65536)
                if not b: break
                data += b
        except Exception: pass
        s.close(); return data.decode(errors='replace')
    except Exception as e: return 'monitor: %s' % e
def volcar_cuelgue():
    r = monitor('info registers')
    txt = '\n[ci_drive] ---- VM colgada: registros ----\n' + r + '\n'
    m = re.search(r'RIP=([0-9a-f]+)', r)
    if m: txt += '[ci_drive] RIP = %s\n' % (sim(int(m.group(1), 16)) or hex(int(m.group(1), 16)))
    m = re.search(r'RFL=([0-9a-f]+)', r)
    if m: txt += '[ci_drive] IF (interrupciones) = %d\n' % ((int(m.group(1), 16) >> 9) & 1)
    st = monitor('x/96gx $rsp')
    txt += '[ci_drive] pila (direcciones de codigo del kernel):\n'
    for w in re.findall(r'0x([0-9a-f]{16})', st):
        n = sim(int(w, 16))
        if n: txt += '    %s %s\n' % (w, n)
    log.write(txt.encode()); log.flush()

def reader():
    while True:
        b = p.stdout.read(4096)
        if not b: break
        with lock: buf.extend(b); last[0] = time.time()
        log.write(b); log.flush()
threading.Thread(target=reader, daemon=True).start()
cmd_actual = ['']
PROMPT = os.environ.get('KNG_PROMPT', 'kng> ').encode()   # en modo grafico la terminal muestra '/> '
def listo():
    with lock:
        b = bytes(buf)
        q = time.time() - last[0]
        if b.endswith(PROMPT) and q > 0.7: return True
        if cmd_actual[0].rstrip().endswith('&'):          # programa en segundo plano: basta con que vuelva el indicador
            i = b.find(cmd_actual[0].encode())
            return i >= 0 and b.find(PROMPT, i) >= 0 and q > 0.2
        return False
def espera(t):
    fin = time.time() + t
    while time.time() < fin:
        if p.poll() is not None: return False
        if listo(): return True
        time.sleep(0.2)
    return False
ok = espera(420)   # en modo grafico GRUB tarda en cargar los 143 MB de Debian
if not ok: sys.stderr.write('[ci_drive] no aparecio el indicador inicial\n')
for c in cmds:
    if c.startswith('@esperar'): time.sleep(float(c.split()[1])); continue
    t = 30
    if '|' in c and c.split('|', 1)[0].isdigit(): t, c = int(c.split('|', 1)[0]), c.split('|', 1)[1]
    if p.poll() is not None: break
    with lock: buf.clear(); log.write(('\n[ci_drive %s] > %s\n' % (time.strftime('%H:%M:%S'), c)).encode()); log.flush()
    cmd_actual[0] = c; t0 = time.time()
    try: p.stdin.write(c.encode() + b'\r'); p.stdin.flush()
    except Exception: break
    fin_ok = espera(t)
    with lock: log.write(('\n[ci_drive %s] "%s" tardo %d s\n' % (time.strftime('%H:%M:%S'), c[:40], int(time.time()-t0))).encode()); log.flush()
    if not fin_ok:
        with lock: log.write(('\n[ci_drive] TIEMPO AGOTADO esperando el fin de: %s\n' % c).encode()); log.flush()
        try:                                                  # Ctrl+] : el kernel lista los procesos y lo que espera cada uno
            with lock: log.write(b'\n[ci_drive] ---- procesos (Ctrl+]) ----\n'); log.flush()
            p.stdin.write(b'\x1d'); p.stdin.flush(); time.sleep(2.5)
        except Exception: pass
        if not hang_vuelcos: hang_vuelcos.append(1); volcar_cuelgue()
        # se intenta recuperar el indicador con Ctrl+C
        try: p.stdin.write(b'\x03\r'); p.stdin.flush()
        except Exception: pass
        espera(10)
try: p.terminate(); p.wait(10)
except Exception: p.kill()
