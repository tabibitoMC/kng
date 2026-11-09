#!/usr/bin/env python3
"""Recibe archivos que kng sube con `curl -T archivo http://10.0.2.2:PUERTO/nombre` (PUT) y los guarda en ./subidas/. Solo para el CI."""
import http.server, os, sys
port = int(sys.argv[1]) if len(sys.argv) > 1 else 18091
os.makedirs('subidas', exist_ok=True)
class H(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.0'
    def do_PUT(self):
        n = int(self.headers.get('Content-Length', '0'))
        name = os.path.basename(self.path) or 'sin-nombre'
        got = 0
        with open(os.path.join('subidas', name), 'wb') as f:
            while got < n:
                d = self.rfile.read(min(65536, n - got))
                if not d: break
                f.write(d); got += len(d)
        self.send_response(201); self.send_header('Content-Length', '0'); self.end_headers()
        print('subido', name, got, 'de', n, flush=True)
    do_POST = do_PUT
    def log_message(self, *a): pass
http.server.ThreadingHTTPServer(('0.0.0.0', port), H).serve_forever()
