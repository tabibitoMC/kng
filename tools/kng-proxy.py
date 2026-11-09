#!/usr/bin/env python3
"""kng-proxy: puente para que Königreich OS (que solo habla http y HTML sencillo) pueda buscar y leer la web moderna.

Se ejecuta en TU PC (Windows o WSL), no dentro de kng:      python3 tools/kng-proxy.py
Escucha en el puerto 8080. Desde la maquina virtual (red NAT) el PC anfitrion es 10.0.2.2.
  /search?q=texto   busca (DuckDuckGo por defecto; &e=brave, google o bing para cambiar de motor) y devuelve una pagina simple
  /go?u=URL         descarga cualquier http/https y lo reduce a HTML basico (titulos, parrafos, enlaces)
Solo biblioteca estandar de Python. El codigo de kng sigue sin tener TLS: el cifrado lo hace este programa en tu PC."""
import sys, re, html, urllib.request, urllib.parse
from html.parser import HTMLParser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

UA = "Mozilla/5.0 (X11; Linux x86_64) kng-proxy/1.0"
KEEP = {"h1", "h2", "h3", "h4", "p", "br", "hr", "ul", "ol", "li", "b", "strong", "i", "em", "pre", "a"}
BLOCK = {"p", "div", "tr", "section", "article", "header", "footer", "table", "h1", "h2", "h3", "h4", "li", "ul", "ol", "pre"}
SKIP = {"script", "style", "svg", "noscript", "template", "iframe", "form", "select", "button"}
MAXOUT = 90_000
LINKS = {}      # id corto -> url (las direcciones de kng son de 192 caracteres como maximo)
ORDER = []

def short(url):
    for k, v in LINKS.items():
        if v == url: return k
    k = format(len(ORDER) + 1, "x"); LINKS[k] = url; ORDER.append(k)
    if len(ORDER) > 3000: LINKS.pop(ORDER.pop(0), None)
    return k

class Simplify(HTMLParser):
    def __init__(self, base):
        super().__init__(convert_charrefs=True)
        self.base, self.out, self.title, self.in_title, self.skip, self.stack = base, [], "", False, 0, []
    def handle_starttag(self, tag, attrs):
        if tag in SKIP: self.skip += 1; return
        if self.skip: return
        if tag == "title": self.in_title = True; return
        if tag == "a":
            href = dict(attrs).get("href") or ""
            href = unwrap(urllib.parse.urljoin(self.base, href))
            if href.startswith(("http://", "https://")):
                self.out.append('<a href="/g/%s">' % short(href)); self.stack.append("a"); return
            self.stack.append(None); return
        if tag in KEEP: self.out.append("<%s>" % tag)
        elif tag in BLOCK: self.out.append("<br>")
    def handle_endtag(self, tag):
        if tag in SKIP: self.skip = max(0, self.skip - 1); return
        if self.skip: return
        if tag == "title": self.in_title = False; return
        if tag == "a":
            if self.stack and self.stack.pop(): self.out.append("</a>")
            return
        if tag in KEEP and tag not in ("br", "hr"): self.out.append("</%s>" % tag)
        elif tag in BLOCK: self.out.append("<br>")
    def handle_data(self, d):
        if self.in_title: self.title += d; return
        if self.skip: return
        d = re.sub(r"\s+", " ", d)
        if d.strip(): self.out.append(html.escape(d))
        elif d: self.out.append(" ")

ENGINES = {   # nombre -> (titulo, plantilla de URL)
    "ddg":    ("DuckDuckGo", "https://lite.duckduckgo.com/lite/?q=%s"),
    "brave":  ("Brave",      "https://search.brave.com/search?q=%s"),
    "google": ("Google",     "https://www.google.com/search?q=%s&gbv=1&hl=es"),
    "bing":   ("Bing",       "https://www.bing.com/search?q=%s&setlang=es"),
}
DEFAULT_ENGINE = "ddg"

def unwrap(u):   # enlaces de redireccion de los buscadores (DuckDuckGo /l/?uddg=, Google /url?q=, Bing /ck/a?u=a1...)
    p = urllib.parse.urlparse(u)
    if p.path == "/url" and "google." in p.netloc:
        q = urllib.parse.parse_qs(p.query).get("q")
        if q: return q[0]
    if p.netloc.endswith("bing.com") and p.path.startswith("/ck/"):
        v = (urllib.parse.parse_qs(p.query).get("u") or [""])[0]
        if v.startswith("a1"):
            import base64
            try: return base64.urlsafe_b64decode(v[2:] + "=" * (-len(v[2:]) % 4)).decode()
            except Exception: pass
    if p.netloc.endswith("duckduckgo.com") and p.path.startswith("/l/"):
        q = urllib.parse.parse_qs(p.query).get("uddg")
        if q: return q[0]
    return u

def fetch(url):
    req = urllib.request.Request(url, headers={"User-Agent": UA, "Accept-Language": "es,en;q=0.8"})
    with urllib.request.urlopen(req, timeout=15) as r:
        raw = r.read(2_000_000); ctype = r.headers.get("Content-Type", ""); final = r.geturl()
        enc = r.headers.get_content_charset() or "utf-8"
    return raw.decode(enc, "replace"), ctype, final

def page(title, body, q=None, eng=None):
    top = '<p><a href="kng://inicio">Inicio</a> | <b>kng-proxy</b>'
    if q:
        top += ' | <a href="kng://buscar?q=%s">Archivos de kng</a></p><p>Buscar con: ' % urllib.parse.quote_plus(q)
        top += " | ".join(("<b>%s</b>" if k == eng else '<a href="/search?q=%%s&e=%s">%%s</a>' % k) % ((n,) if k == eng else (urllib.parse.quote_plus(q), n)) for k, (n, _) in ENGINES.items())
    return ("<title>%s</title>%s</p><hr>%s" % (html.escape(title or "Pagina"), top, body))[:MAXOUT]

def render(url, q=None, eng=None):
    text, ctype, final = fetch(url)
    if "html" not in ctype.lower() and "<html" not in text[:500].lower():
        return page(final, "<pre>%s</pre>" % html.escape(text[:60000]), q, eng)
    s = Simplify(final); s.feed(text)
    body = "".join(s.out)
    body = re.sub(r"(<br>\s*){3,}", "<br><br>", body)
    return page(s.title.strip() or final, "<h2>%s</h2>%s" % (html.escape(s.title.strip()), body) if s.title.strip() else body, q, eng)

class H(BaseHTTPRequestHandler):
    def log_message(self, f, *a): sys.stderr.write("[kng-proxy] " + (f % a) + "\n")
    def do_GET(self):
        u = urllib.parse.urlparse(self.path); qs = urllib.parse.parse_qs(u.query); q = None
        try:
            if u.path == "/search":
                global DEFAULT_ENGINE
                q = (qs.get("q") or [""])[0]
                eng = (qs.get("e") or [DEFAULT_ENGINE])[0]
                if eng not in ENGINES: eng = DEFAULT_ENGINE
                DEFAULT_ENGINE = eng                                  # el ultimo motor elegido queda como predeterminado
                body = render(ENGINES[eng][1] % urllib.parse.quote_plus(q), q, eng)
            elif u.path == "/go":
                body = render((qs.get("u") or [""])[0])
            elif u.path.startswith("/g/"):
                url = LINKS.get(u.path[3:])
                body = render(url) if url else page("Enlace vencido", "<p>Ese enlace ya no esta en memoria. Vuelve a buscar.</p>")
            else:
                body = page("kng-proxy", "<h1>kng-proxy</h1><p>Usa /search?q=palabras o /go?u=http://sitio</p>")
            code = 200
        except Exception as e:
            code, body = 502, page("Error", "<h1>No se pudo cargar</h1><p>%s</p>" % html.escape(str(e)))
        data = body.encode("utf-8", "replace")
        self.send_response(code); self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(data))); self.end_headers(); self.wfile.write(data)

if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
    print("kng-proxy escuchando en el puerto %d (desde la VM: http://10.0.2.2:%d/search?q=hola)" % (port, port))
    ThreadingHTTPServer(("0.0.0.0", port), H).serve_forever()
