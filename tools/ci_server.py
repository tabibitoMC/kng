#!/usr/bin/env python3
"""Servidor de prueba para el CI: HTTP (TCP) y eco UDP en el PC anfitrion. kng lo ve como 10.0.2.2.
Uso: python3 tools/ci_server.py [puerto_tcp] [puerto_udp]"""
import socket, sys, threading
tp = int(sys.argv[1]) if len(sys.argv) > 1 else 18080
up = int(sys.argv[2]) if len(sys.argv) > 2 else 18081
BIG = bytes((i * 7 + 13) & 255 for i in range(400000))
def http(c):
    try:
        data = b''
        while b'\r\n\r\n' not in data:
            d = c.recv(4096)
            if not d: break
            data += d
        if data.startswith(b'GET /big'):
            body = BIG
        else:
            body = b'KNG-HTTP-OK saludos desde el anfitrion\n'
        c.sendall(b'HTTP/1.0 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: %d\r\nConnection: close\r\n\r\n' % len(body) + body)
    finally:
        c.close()
def tcp_loop():
    s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); s.bind(('0.0.0.0', tp)); s.listen(16)
    while True:
        c, a = s.accept(); print('tcp desde', a, flush=True); threading.Thread(target=http, args=(c,), daemon=True).start()
def udp_loop():
    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); u.bind(('0.0.0.0', up))
    while True:
        d, a = u.recvfrom(2048); print('udp desde', a, d, flush=True); u.sendto(b'eco:' + d + b'\n'[:0] + b'!', a)
threading.Thread(target=udp_loop, daemon=True).start()
print('servidor de prueba listo tcp=%d udp=%d' % (tp, up), flush=True)
tcp_loop()
