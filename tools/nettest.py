#!/usr/bin/env python3
"""nettest.py — prueba de la pila de red real (kernel/net.c) contra una red simulada INDEPENDIENTE escrita en Python:
ARP, DHCP, DNS, ICMP y un servidor TCP/HTTP con perdidas de paquetes. Verifica ademas todas las sumas de comprobacion."""
import os, struct, subprocess, select, sys, random, time
HERE=os.path.dirname(os.path.abspath(__file__))
exe="/tmp/kng_nettest"
cmd=["gcc","-g","-O1","-fsanitize=address,undefined","-fno-sanitize-recover=undefined","-Wall","-Wno-unused-function","-o",exe,
     os.path.join(HERE,"nettest_main.c")]
r=subprocess.run(cmd,capture_output=True,text=True)
if r.returncode: print(r.stderr); sys.exit("fallo la compilacion")
if r.stderr.strip(): print(r.stderr)

M=0xFFFFFFFF
def ip4(s): return bytes(int(x) for x in s.split('.'))
GW_IP=ip4("192.168.7.1"); CL_IP=ip4("192.168.7.50"); MASK=ip4("255.255.255.0")
GW_MAC=bytes.fromhex("525500070001")
SERVERS={"example.test":"93.184.216.34","silent.test":"93.184.216.41"}
IP_EX=ip4("93.184.216.34"); IP_SILENT=ip4("93.184.216.41")
def cks(b):
    if len(b)%2: b+=b'\0'
    s=sum(struct.unpack("!%dH"%(len(b)//2),b))
    while s>>16: s=(s&0xFFFF)+(s>>16)
    return (~s)&0xFFFF
errors=[]; log=[]
def err(m): errors.append(m); print("ERROR:",m)
def ip_pkt(src,dst,proto,pl,ident=[1]):
    ident[0]+=1
    h=struct.pack("!BBHHHBBH4s4s",0x45,0,20+len(pl),ident[0],0x4000,64,proto,0,src,dst)
    h=h[:10]+struct.pack("!H",cks(h))+h[12:]
    return h+pl
def eth(dst,src,t,pl): return dst+src+struct.pack("!H",t)+pl
def udp(src,dst,sp,dp,pl):
    u=struct.pack("!HHHH",sp,dp,8+len(pl),0)+pl
    c=cks(src+dst+struct.pack("!BBH",0,17,len(u))+u) or 0xFFFF
    return u[:6]+struct.pack("!H",c)+u[8:]
def tcp(src,dst,sp,dp,seq,ack,fl,pl=b'',mss=None):
    o=b'\x02\x04'+struct.pack("!H",mss) if mss else b''
    hl=20+len(o)
    t=struct.pack("!HHIIBBHHH",sp,dp,seq&M,ack&M,(hl//4)<<4,fl,65535,0,0)+o+pl
    c=cks(src+dst+struct.pack("!BBH",0,6,len(t))+t)
    return t[:16]+struct.pack("!H",c)+t[18:]

state={"cli_mac":None,"bound":False,"echo_sent":False,"arp_sent":False,"echo_ok":False,"arp_ok":False}
requests=[]; dns_q=[]; conns={}; out_frames=[]
def send(frame): out_frames.append(frame)

BIG=b''.join(b"line %05d: "%i+b"x"*40+b"\n" for i in range(700))
LOSS=b''.join(b"lossy %06d "%i+b"y"*33+b"\n" for i in range(1300))
def hdr(status,extra,body=None,cl=True):
    h=b"HTTP/1.0 "+status+b"\r\n"+extra
    if body is not None and cl: h+=b"Content-Length: %d\r\n"%len(body)
    return h+b"\r\n"
HOME=b"<title>Hola</title><h1>Hola kng</h1><p>Prueba de red</p>"
def respond(path):
    """devuelve (bytes, expected_body, loss)"""
    if path==b"/":
        return hdr(b"200 OK",b"Content-Type: text/html; charset=utf-8\r\n",HOME)+HOME,HOME,False
    if path==b"/big": return hdr(b"200 OK",b"Content-Type: text/plain\r\n",BIG)+BIG,BIG,False
    if path==b"/loss": return hdr(b"200 OK",b"Content-Type: text/plain\r\n",LOSS)+LOSS,LOSS,True
    if path==b"/chunk":
        parts=[b"Hola ",b"trozo dos "*2,bytes(range(65,91))*100]
        body=b''.join(parts); ch=b''.join(b"%x\r\n"%len(p)+p+b"\r\n" for p in parts)+b"0\r\n\r\n"
        return b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Type: text/html\r\n\r\n"+ch,body,False
    if path==b"/redir": return hdr(b"302 Found",b"Location: http://example.test/\r\n",b"x")+b"x",HOME,False
    if path==b"/rel": return hdr(b"301 Moved",b"Location: big\r\n",b"x")+b"x",BIG,False
    if path==b"/nolen": b=b"<p>sin longitud</p>"; return hdr(b"200 OK",b"Content-Type: text/html\r\n",b,cl=False)+b,b,False
    if path==b"/404": b=b"<h1>No existe</h1>"; return hdr(b"404 Not Found",b"Content-Type: text/html\r\n",b)+b,b,False
    if path==b"/img": b=b"\x89PNG\r\n"+bytes(100); return hdr(b"200 OK",b"Content-Type: image/png\r\n",b)+b,b,False
    b=b"<p>ruta "+path+b"</p>"; return hdr(b"200 OK",b"Content-Type: text/html\r\n",b)+b,b,False
EXPECT={}
class Conn:
    def __init__(s,cip,cport,sip,sport,cseq):
        s.cip,s.cport,s.sip,s.sport=cip,cport,sip,sport
        s.isn=random.getrandbits(32); s.rcv=(cseq+1)&M; s.req=b''; s.resp=None; s.una=0; s.nxt=0; s.fin=False; s.dup=0; s.drop=False; s.loss=False; s.sent_fin=False; s.retx_una=-1
    def seg(s,seq,fl,pl=b'',mss=None):
        send(eth(state["cli_mac"],GW_MAC,0x0800,ip_pkt(s.sip,s.cip,6,tcp(s.sip,s.cip,s.sport,s.cport,seq,s.rcv,fl,pl,mss))))
    def more(s):
        while s.resp is not None and s.nxt<len(s.resp) and s.nxt-s.una<14600:
            ch=s.resp[s.nxt:s.nxt+1460]
            if s.loss and not s.drop and s.nxt==2*1460: s.drop=True
            else: s.seg(s.isn+1+s.nxt,0x18,ch)
            s.nxt+=len(ch)
        if s.resp is not None and s.nxt>=len(s.resp) and not s.sent_fin:
            s.sent_fin=True; s.seg(s.isn+1+len(s.resp),0x11)
def tcp_in(src,dst,t):
    sp,dp,seq,ack,off,fl=struct.unpack("!HHIIBB",t[:14]); hl=(off>>4)*4; data=t[hl:]
    if cks(src+dst+struct.pack("!BBH",0,6,len(t))+t)!=0: err("checksum TCP invalido del cliente")
    key=(src,sp,dst,dp); c=conns.get(key)
    if fl&2:
        if dst==IP_SILENT: return                              # no responde (prueba de tiempo agotado)
        if dp!=80:                                             # sin servicio: RST
            send(eth(state["cli_mac"],GW_MAC,0x0800,ip_pkt(dst,src,6,tcp(dst,src,dp,sp,0,seq+1,0x14)))); return
        if hl<24 or t[20:22]!=b'\x02\x04': err("el SYN no trae opcion MSS")
        c=conns[key]=Conn(src,sp,dst,dp,seq); c.seg(c.isn,0x12,mss=1460); return
    if not c: return
    if fl&1 and c.rcv==seq: c.rcv=(c.rcv+len(data)+1)&M; c.seg(c.isn+1+(c.nxt if c.resp is not None else 0)+(1 if c.sent_fin else 0),0x10); return
    if data and seq==c.rcv:
        c.rcv=(c.rcv+len(data))&M; c.req+=data; c.seg(c.isn+1,0x10)
        if c.resp is None and b"\r\n\r\n" in c.req:
            requests.append(c.req)
            line=c.req.split(b"\r\n")[0].split(b" ")
            path=line[1].replace(b"%20",b" ") if len(line)>1 else b"/"
            c.resp,body,c.loss=respond(line[1] if len(line)>1 else b"/")
            c.more()
        return
    if fl&0x10 and c.resp is not None:
        o=(ack-(c.isn+1))&M
        if o>len(c.resp): return
        if o>c.una: c.una=o; c.dup=0; c.more()
        elif o==c.una and c.una<c.nxt:
            c.dup+=1
            if c.dup==2:
                c.dup=0
                if c.retx_una!=c.una: c.retx_una=c.una; c.nxt=c.una; c.more()    # retransmite la ventana una sola vez por hueco
def dhcp_in(f):
    p=f[14+20+8:]
    if p[0]!=1: return
    xid=p[4:8]; chaddr=p[28:34]
    opts=p[240:]; typ=None; i=0; req55=False
    while i<len(opts) and opts[i]!=255:
        if opts[i]==0: i+=1; continue
        if opts[i]==53: typ=opts[i+2]
        if opts[i]==55: req55=True
        i+=2+opts[i+1]
    if not req55: err("DHCP sin opcion 55")
    if typ not in (1,3): return
    rep=0x02 if typ==1 else 0x05
    b=struct.pack("!BBBBIHH4s4s4s4s",2,1,6,0,0,0,0x8000,b'\0'*4,CL_IP,GW_IP,b'\0'*4)+bytes(0)
    b=bytes([2,1,6,0])+xid+struct.pack("!HH",0,0x8000)+b'\0'*4+CL_IP+GW_IP+b'\0'*4+chaddr+b'\0'*10+b'\0'*192+bytes([0x63,0x82,0x53,0x63])
    b+=bytes([53,1,rep,54,4])+GW_IP+bytes([51,4])+struct.pack("!I",3600)+bytes([1,4])+MASK+bytes([3,4])+GW_IP+bytes([6,4])+GW_IP+bytes([255])
    state["cli_mac"]=chaddr
    send(eth(b'\xff'*6,GW_MAC,0x0800,ip_pkt(GW_IP,b'\xff'*4,17,udp(GW_IP,b'\xff'*4,67,68,b))))
    if typ==3: state["bound"]=True
def dns_in(src,dst,u):
    sp,dp,ln,c=struct.unpack("!HHHH",u[:8]); p=u[8:]
    qid=p[:2]; i=12; labels=[]
    while p[i]:
        l=p[i]; labels.append(p[i+1:i+1+l].decode()); i+=l+1
    q=p[12:i+5]; name=".".join(labels); dns_q.append(name)
    if SERVERS.get(name) and True:
        ans=b'\xc0\x0c'+struct.pack("!HHIH",1,1,60,4)+ip4(SERVERS[name]); fl=0x8180; an=1
    else: ans=b''; fl=0x8183; an=0
    r=qid+struct.pack("!HHHHH",fl,1,an,0,0)+q+ans
    send(eth(state["cli_mac"],GW_MAC,0x0800,ip_pkt(dst,src,17,udp(dst,src,53,sp,r))))
def handle(f):
    if len(f)<14: return
    t=struct.unpack("!H",f[12:14])[0]
    if t==0x0806:
        op=struct.unpack("!H",f[20:22])[0]; sha=f[22:28]; spa=f[28:32]; tpa=f[38:42]
        if op==1 and tpa==GW_IP:
            send(eth(sha,GW_MAC,0x0806,struct.pack("!HHBBH",1,0x0800,6,4,2)+GW_MAC+GW_IP+sha+spa))
        elif op==2 and spa==CL_IP: state["arp_ok"]=True
        return
    if t!=0x0800: return
    ip=f[14:]
    if cks(ip[:20])!=0: err("checksum IP invalido del cliente"); return
    src=ip[12:16]; dst=ip[16:20]; proto=ip[9]; pl=ip[20:struct.unpack("!H",ip[2:4])[0]]
    if proto!=17 or struct.unpack("!H",pl[2:4])[0]!=67:      # salvo DHCP, todo debe ir al MAC de la puerta de enlace
        if f[:6]!=GW_MAC: err("trama con MAC destino incorrecto %s"%f[:6].hex())
    if proto==17:
        dp=struct.unpack("!H",pl[2:4])[0]
        c=struct.unpack("!H",pl[6:8])[0]
        if c and cks(src+dst+struct.pack("!BBH",0,17,len(pl))+pl)!=0: err("checksum UDP invalido")
        if dp==67: dhcp_in(f)
        elif dp==53: dns_in(src,dst,pl)
    elif proto==1:
        if cks(pl)!=0: err("checksum ICMP invalido")
        if pl[0]==8:
            r=bytes([0,0,0,0])+pl[4:]; r=r[:2]+struct.pack("!H",cks(r))+r[4:]
            send(eth(state["cli_mac"],GW_MAC,0x0800,ip_pkt(dst,src,1,r)))
        elif pl[0]==0 and src==CL_IP and pl[8:]==b'hola-kng': state["echo_ok"]=True
    elif proto==6: tcp_in(src,dst,pl)

r1,w1=os.pipe(); r2,w2=os.pipe(); r3,w3=os.pipe()
env=dict(os.environ,FDIN=str(r1),FDOUT=str(w2),FDSYNC=str(r3),ASAN_OPTIONS="detect_leaks=0")
proc=subprocess.Popen([exe],pass_fds=(r1,w2,r3),env=env,stderr=subprocess.PIPE)
os.close(r1); os.close(w2); os.close(r3)
import threading
errtxt=[]
def _rd():
    acc=[]
    for ln in iter(proc.stderr.readline,b''):
        t=ln.decode(errors="replace"); acc.append(t)
        if os.environ.get("NETLIVE"): print("C|",t,end="",flush=True)
    errtxt.append("".join(acc))
th=threading.Thread(target=_rd); th.start()
buf=b''; t0=time.time()
while proc.poll() is None and time.time()-t0<240:
    rl,_,_=select.select([r2],[],[],0.02)
    if rl:
        d=os.read(r2,65536)
        if not d: break
        buf+=d
        while len(buf)>=2:
            l=struct.unpack("<H",buf[:2])[0]
            if len(buf)<2+l: break
            handle(buf[2:2+l]); buf=buf[2+l:]
            while out_frames:
                fr=out_frames.pop(0); os.write(w1,struct.pack("<H",len(fr))+fr)
            os.write(w3,b'.')      # sincronia: esta trama ya fue atendida
        if state["bound"] and not state["echo_sent"]:
            state["echo_sent"]=True
            e=bytes([8,0,0,0,0,1,0,1])+b"hola-kng"; e=e[:2]+struct.pack("!H",cks(e))+e[4:]
            os.write(w1,struct.pack("<H",len(eth(state["cli_mac"],GW_MAC,0x0800,ip_pkt(GW_IP,CL_IP,1,e)))) + eth(state["cli_mac"],GW_MAC,0x0800,ip_pkt(GW_IP,CL_IP,1,e)))
            a=struct.pack("!HHBBH",1,0x0800,6,4,1)+GW_MAC+GW_IP+b'\0'*6+CL_IP
            fr=eth(b'\xff'*6,GW_MAC,0x0806,a); os.write(w1,struct.pack("<H",len(fr))+fr)
if proc.poll() is None: proc.kill(); err("el programa no termino")
th.join(); rc=proc.returncode
out=errtxt[0] if errtxt else ""
print(out)
def fnv(b):
    h=2166136261
    for x in b: h=((h^x)*16777619)&M
    return h
def get(url):
    for l in out.splitlines():
        if l.startswith("FETCH "+url+" "): return l
    return None
def check(cond,msg):
    if not cond: err(msg)
    else: print("ok  ",msg)
check(rc==0,"el programa termina sin errores (codigo %s, ASan/UBSan limpio)"%rc)
check("STATE 2" in out,"DHCP asigna direccion y la red queda lista")
check("IP 192.168.7.50" in out,"IP recibida por DHCP (no la de reserva)")
check("mascara 255.255.255.0  puerta 192.168.7.1  DNS 192.168.7.1" in out,"mascara, puerta y DNS de DHCP")
check(state["echo_ok"],"el cliente responde ping (ICMP echo)")
check("respuesta de 93.184.216.34" in out and "4 enviados, 4 recibidos" in out,"ping a un nombre (DNS + ICMP)")
check("example.test -> 93.184.216.34" in out,"comando dns")
check("no se pudo resolver nx.test" in out,"NXDOMAIN se informa")
def body_of(path): return respond(path)[1]
for url,path in [("http://example.test/",b"/"),("http://example.test/big",b"/big"),("http://example.test/chunk",b"/chunk"),
                 ("http://example.test/loss",b"/loss"),("http://example.test/nolen",b"/nolen"),("http://example.test/404",b"/404")]:
    l=get(url); b=body_of(path)
    check(l is not None and "OK" in l and "len=%d "%len(b) in l and "fnv=%08x"%fnv(b) in l,"GET %s: contenido identico (%d bytes)"%(url,len(b)))
l=get("http://example.test/redir"); check(l and "fnv=%08x"%fnv(HOME) in l and "final=http://example.test/" in l,"redireccion 302 absoluta")
l=get("http://example.test/rel"); check(l and "fnv=%08x"%fnv(BIG) in l and "final=http://example.test/big" in l,"redireccion 301 relativa")
l=get("http://example.test/"); check("html=1" in l and "status=200" in l,"tipo html y estado 200")
l=get("http://example.test/big"); check("html=0" in l,"text/plain no es html")
l=get("http://example.test/404"); check("status=404" in l,"estado 404 con cuerpo")
l=get("http://example.test/img"); check(l and "ERR Tipo de contenido no soportado: image/png" in l,"contenido no textual rechazado")
l=get("http://example.test:81/"); check(l and "ERR conexion rechazada" in l,"RST -> conexion rechazada")
l=get("https://example.test/"); check(l and "ERR HTTPS no esta soportado" in l,"https rechazado con mensaje claro")
l=get("http://nx.test/"); check(l and "ERR No se pudo resolver el nombre: nx.test" in l,"fallo de DNS en HTTP")
l=get("http://silent.test/"); check(l and "ERR tiempo agotado conectando" in l,"servidor mudo -> tiempo agotado")
l=get("http://93.184.216.34/"); check(l and "OK" in l and "fnv=%08x"%fnv(HOME) in l,"IP literal sin DNS")
l=get("example.test/"); check(l and "OK" in l,"direccion sin esquema")
l=get("http://example.test/a b?x=1"); check(l and "OK" in l,"ruta con espacio")
check(any(r.startswith(b"GET /a%20b?x=1 HTTP/1.0\r\nHost: example.test\r\n") for r in requests),"peticion con ruta codificada y Host")
check(any(r.startswith(b"GET /big HTTP/1.0\r\nHost: example.test\r\n") for r in requests),"peticion GET /big (tras redireccion relativa)")
check(any(b"Host: 93.184.216.34\r\n" in r for r in requests),"Host con IP literal")
check(all(b"Connection: close\r\n" in r and b"User-Agent: KonigreichOS" in r for r in requests),"cabeceras Connection y User-Agent")
check("HTTP 200" in out and "Prueba de red" in out,"comando wget muestra el texto")
check(state["arp_ok"],"el cliente responde ARP")
check("example.test" in dns_q and "nx.test" in dns_q,"el cliente hizo consultas DNS")
print("\nRESULTADO:", "NET TEST OK" if not errors else "FALLOS: %d"%len(errors))
sys.exit(1 if errors else 0)
