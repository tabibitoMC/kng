/* sock.c — sockets para programas de usuario (ring 3).
 *
 * TCP completo para varias conexiones a la vez (cliente y servidor): apreton de manos, ventana, retransmision, cierre ordenado,
 * RST. UDP con cola de datagramas. AF_UNIX (stream y dgram, socketpair) y bucle local (127.0.0.1 / la propia IP): en esos casos
 * los datos pasan directo de un socket al otro dentro del kernel. Se engancha a la pila de red (net.c) por sock_ip_hook y
 * sock_tick_hook; la pila vieja (HTTP/DNS/DHCP del propio kernel) sigue funcionando al lado.
 * Codigo propio a partir de las especificaciones (RFC 793, 768). */
#include "kng.h"
#include "net.h"
#include "sock.h"

typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;

enum { E_NOENT=2, E_AGAIN=11, E_NOMEM=12, E_FAULT=14, E_INVAL=22, E_PIPE=32, E_NOTSOCK=88, E_MSGSIZE=90, E_AFNOSUPPORT=97, E_ADDRINUSE=98,
       E_NETUNREACH=101, E_CONNRESET=104, E_NOBUFS=105, E_ISCONN=106, E_NOTCONN=107, E_TIMEDOUT=110, E_CONNREFUSED=111, E_INPROGRESS=115,
       E_OPNOTSUPP=95, E_DESTADDRREQ=89 };

#define NSOCK 512
#define RXSZ  32768
#define TXSZ  32768
#define MSS   1400
#define RTO   100

enum { ST_CLOSED, ST_LISTEN, ST_SYN_SENT, ST_SYN_RCVD, ST_EST, ST_FW1, ST_FW2, ST_CWAIT, ST_CLOSING, ST_LASTACK, ST_TW };
#define TF_FIN 1
#define TF_SYN 2
#define TF_RST 4
#define TF_PSH 8
#define TF_ACK 16
#define SEQ_LT(a,b) ((int32_t)((a)-(b))<0)

typedef struct KSock {
    u8 used, family, type, local;                 /* local: los datos van directo al par dentro del kernel */
    u8 st, shut_rd, shut_wr, bound, ever, connecting, fin_rcvd, fin_sent, fin_acked, reuse;
    int refs, err, parent, peer;
    u32 lip, rip; u16 lport, rport, peer_mss;
    u32 iss, snd_una, snd_nxt, rcv_nxt, snd_wnd, t_rtx, t_tw; int tries, sent;
    u8 *rx; int rx_r, rx_n; u8 *tx; int tx_r, tx_n;
    int aq[16], aq_n, backlog;
    char upath[108]; int ulen;
} KSock;
static KSock S[NSOCK];
static u16 eph=40000;
static u32 iss_ctr;

/* ---------- utilidades ---------- */
static u16 rd16(const u8 *p){ return (u16)((p[0]<<8)|p[1]); }
static u32 rd32(const u8 *p){ return ((u32)p[0]<<24)|((u32)p[1]<<16)|((u32)p[2]<<8)|p[3]; }
static void wr16(u8 *p,u16 v){ p[0]=(u8)(v>>8); p[1]=(u8)v; }
static void wr32(u8 *p,u32 v){ p[0]=(u8)(v>>24); p[1]=(u8)(v>>16); p[2]=(u8)(v>>8); p[3]=(u8)v; }
static u32 csum_add(u32 s,const u8 *p,int n){ for(;n>1;n-=2,p+=2) s+=rd16(p); if(n) s+=(u32)p[0]<<8; return s; }
static u16 csum_fin(u32 s){ while(s>>16) s=(s&0xFFFF)+(s>>16); return (u16)~s; }
static u32 pseudo(u32 src,u32 dst,u8 proto,int len){ return (src>>16)+(src&0xFFFF)+(dst>>16)+(dst&0xFFFF)+proto+(u32)len; }

#define OK(s) ((s)>=0 && (s)<NSOCK && S[s].used)
static int is_loop(u32 ip){ return (ip>>24)==127 || (ip && ip==net_my_ip()); }

/* ---------- anillos de bytes ---------- */
static void ring_put(u8 *buf,int size,int r,int n,const u8 *d,int len){
    int w=(r+n)%size; int c=size-w; if(c>len) c=len;
    memcpy(buf+w,d,(size_t)c); if(len>c) memcpy(buf,d+c,(size_t)(len-c));
}
static void ring_get(const u8 *buf,int size,int r,u8 *o,int len){
    int c=size-r; if(c>len) c=len;
    memcpy(o,buf+r,(size_t)c); if(len>c) memcpy(o+c,buf,(size_t)(len-c));
}
static void rx_put(KSock *k,const u8 *d,int n){ ring_put(k->rx,RXSZ,k->rx_r,k->rx_n,d,n); k->rx_n+=n; }
static int  rx_take(KSock *k,u8 *o,int n,int peek){
    if(n>k->rx_n) n=k->rx_n;
    ring_get(k->rx,RXSZ,k->rx_r,o,n);
    if(!peek){ k->rx_r=(k->rx_r+n)%RXSZ; k->rx_n-=n; if(!k->rx_n) k->rx_r=0; }
    return n;
}
static void dg_put(KSock *k,u32 ip,u16 port,const u8 *d,int n){        /* registro: [len:2][puerto:2][ip:4][datos] */
    if(RXSZ-k->rx_n<8+n) return;                                         /* cola llena: se descarta (UDP) */
    u8 h[8]; wr16(h,(u16)n); wr16(h+2,port); wr32(h+4,ip);
    rx_put(k,h,8); rx_put(k,d,n);
}

/* ---------- tabla ---------- */
static KSock *sk_new(int family,int type){
    for(int i=0;i<NSOCK;i++) if(!S[i].used){
        KSock *k=&S[i]; memset(k,0,sizeof *k); k->used=1; k->family=(u8)family; k->type=(u8)type; k->parent=-1; k->peer=-1;
        k->rx=kmalloc(RXSZ); if(!k->rx){ k->used=0; return 0; }
        if(type==SK_STREAM){ k->tx=kmalloc(TXSZ); if(!k->tx){ kfree(k->rx); k->rx=0; k->used=0; return 0; } }
        return k; }
    return 0;
}
static int idx(KSock *k){ return (int)(k-S); }
static void sk_free(KSock *k){ if(k->rx) kfree(k->rx); if(k->tx) kfree(k->tx); k->rx=k->tx=0; k->used=0; }
static int port_used(u16 p){ for(int i=0;i<NSOCK;i++) if(S[i].used && S[i].family==SK_AF_INET && S[i].lport==p) return 1; return 0; }
static u16 new_port(void){ for(int t=0;t<9000;t++){ if(++eph>=49000) eph=40000; if(!port_used(eph)) return eph; } return 0; }

int  sock_peer(int s){ return OK(s)?S[s].peer:-1; }
int  sock_type(int s){ return OK(s)?S[s].type:-E_NOTSOCK; }
int  sock_family(int s){ return OK(s)?S[s].family:-E_NOTSOCK; }
void sock_set_reuse(int s,int v){ if(OK(s)) S[s].reuse=(u8)v; }
void sock_ref(int s){ if(OK(s)) S[s].refs++; }
int  sock_take_err(int s){ if(!OK(s)) return E_NOTSOCK; int e=S[s].err; S[s].err=0; return e; }

int sock_create(int family,int type){
    if(family!=SK_AF_INET && family!=SK_AF_UNIX) return -E_AFNOSUPPORT;
    if(type!=SK_STREAM && type!=SK_DGRAM) return -E_INVAL;
    KSock *k=sk_new(family,type); if(!k) return -E_NOMEM;
    k->refs=1; if(family==SK_AF_UNIX) k->local=1;
    return idx(k);
}
int sock_pair(int type,int *a,int *b){
    if(type!=SK_STREAM && type!=SK_DGRAM) return -E_INVAL;
    KSock *x=sk_new(SK_AF_UNIX,type); if(!x) return -E_NOMEM;
    KSock *y=sk_new(SK_AF_UNIX,type); if(!y){ sk_free(x); return -E_NOMEM; }
    x->refs=y->refs=1; x->local=y->local=1; x->st=y->st=ST_EST; x->ever=y->ever=1; x->peer=idx(y); y->peer=idx(x);
    *a=idx(x); *b=idx(y); return 0;
}

/* ---------- TCP ---------- */
static int tcp_xmit(KSock *k,u8 fl,u32 seq,const u8 *d,int n,int mss_opt){
    static u8 b[1500]; int hl=mss_opt?24:20;
    wr16(b,k->lport); wr16(b+2,k->rport); wr32(b+4,seq); wr32(b+8,(fl&TF_ACK)?k->rcv_nxt:0);
    b[12]=(u8)((hl/4)<<4); b[13]=fl; int w=RXSZ-k->rx_n; if(w>65535) w=65535; wr16(b+14,(u16)w); wr16(b+16,0); wr16(b+18,0);
    if(mss_opt){ b[20]=2; b[21]=4; wr16(b+22,MSS); }
    if(n) memcpy(b+hl,d,(size_t)n);
    u32 sum=pseudo(net_my_ip(),k->rip,6,hl+n); sum=csum_add(sum,b,hl+n); wr16(b+16,csum_fin(sum));
    return net_ip_send(6,k->rip,b,hl+n);
}
static void tcp_output(KSock *k){
    if(k->st!=ST_EST && k->st!=ST_CWAIT && k->st!=ST_FW1 && k->st!=ST_CLOSING && k->st!=ST_LASTACK) return;
    u32 now=kticks(); u32 lim=k->snd_wnd; if(lim>16384) lim=16384;
    int mss=k->peer_mss?k->peer_mss:536; if(mss>MSS) mss=MSS;
    while(k->sent<k->tx_n && k->sent<(int)lim){
        int n=k->tx_n-k->sent, cap=(int)lim-k->sent; if(n>cap) n=cap; if(n>mss) n=mss;
        static u8 tmp[1500]; ring_get(k->tx,TXSZ,(k->tx_r+k->sent)%TXSZ,tmp,n);
        if(!k->sent && !(k->fin_sent&&!k->fin_acked)) k->t_rtx=now;
        tcp_xmit(k,TF_ACK|TF_PSH,k->snd_una+(u32)k->sent,tmp,n,0);
        k->sent+=n; k->snd_nxt=k->snd_una+(u32)k->sent;
    }
    if(k->shut_wr && !k->fin_sent && k->sent==k->tx_n){
        if(!k->sent) k->t_rtx=now;
        tcp_xmit(k,TF_FIN|TF_ACK,k->snd_una+(u32)k->sent,0,0,0); k->fin_sent=1; k->snd_nxt=k->snd_una+(u32)k->sent+1;
        if(k->st==ST_EST) k->st=ST_FW1; else if(k->st==ST_CWAIT) k->st=ST_LASTACK;
    }
}
static void tcp_acked(KSock *k,u32 ack,u32 wnd){
    k->snd_wnd=wnd;
    u32 outst=(u32)k->sent+(k->fin_sent?1u:0u), acked=ack-k->snd_una;
    if(!acked || acked>outst) return;
    int dack=(int)acked;
    if(k->fin_sent && acked==outst){ k->fin_acked=1; dack--; }
    k->tx_r=(k->tx_r+dack)%TXSZ; k->tx_n-=dack; k->sent-=dack; k->snd_una+=acked; k->snd_nxt=k->snd_una+(u32)k->sent+(k->fin_sent&&!k->fin_acked?1u:0u);
    k->t_rtx=kticks(); k->tries=0;
    if(!k->tx_n) k->tx_r=0;
}
static void tcp_parse_mss(KSock *k,const u8 *p,int hl){
    k->peer_mss=0;
    for(int i=20;i<hl;){ u8 o=p[i]; if(o==0) break; if(o==1){ i++; continue; } if(i+1>=hl) break; int l=p[i+1]; if(l<2||i+l>hl) break; if(o==2&&l==4) k->peer_mss=rd16(p+i+2); i+=l; }
}
static KSock *tcp_find(u32 src,u16 sport,u16 dport){
    for(int i=0;i<NSOCK;i++){ KSock *k=&S[i]; if(k->used && k->family==SK_AF_INET && k->type==SK_STREAM && !k->local && k->st!=ST_LISTEN && k->st!=ST_CLOSED && k->lport==dport && k->rport==sport && k->rip==src) return k; }
    for(int i=0;i<NSOCK;i++){ KSock *k=&S[i]; if(k->used && k->family==SK_AF_INET && k->type==SK_STREAM && k->st==ST_LISTEN && k->lport==dport) return k; }
    return 0;
}
static int tcp_in(u32 src,u32 dst,const u8 *p,int n){
    if(n<20) return 0;
    u16 sport=rd16(p), dport=rd16(p+2); u32 seq=rd32(p+4), ack=rd32(p+8); int fl=p[13], hl=(p[12]>>4)*4, wnd=rd16(p+14);
    if(hl<20||hl>n) return 0;
    KSock *k=tcp_find(src,sport,dport);
    if(!k) return 0;
    u32 sum=pseudo(src,dst,6,n); sum=csum_add(sum,p,n); if(csum_fin(sum)!=0) return 1;
    const u8 *data=p+hl; int dl=n-hl;
    u32 now=kticks();
    if(k->st==ST_LISTEN){
        if(!(fl&TF_SYN) || (fl&(TF_ACK|TF_RST))) return 1;
        int pend=0; for(int i=0;i<NSOCK;i++) if(S[i].used && S[i].parent==idx(k)) pend++;
        if(pend>=k->backlog+4 || pend>=24) return 1;
        KSock *c=sk_new(SK_AF_INET,SK_STREAM); if(!c) return 1;
        c->parent=idx(k); c->st=ST_SYN_RCVD; c->lip=dst; c->rip=src; c->lport=dport; c->rport=sport; c->bound=1;
        c->iss=now*2654435761u+(iss_ctr+=64013); c->snd_una=c->iss; c->snd_nxt=c->iss+1; c->rcv_nxt=seq+1; c->snd_wnd=(u32)wnd; c->t_rtx=now;
        tcp_parse_mss(c,p,hl);
        tcp_xmit(c,TF_SYN|TF_ACK,c->iss,0,0,1);
        return 1;
    }
    if(fl&TF_RST){
        if(k->st==ST_SYN_SENT){ if(!(fl&TF_ACK)||ack!=k->iss+1) return 1; k->err=E_CONNREFUSED; }
        else { if(SEQ_LT(seq,k->rcv_nxt)||SEQ_LT(k->rcv_nxt+(u32)RXSZ,seq)) return 1; k->err=E_CONNRESET; }
        k->st=ST_CLOSED; k->connecting=0; return 1;
    }
    if(k->st==ST_SYN_SENT){
        if((fl&(TF_SYN|TF_ACK))==(TF_SYN|TF_ACK) && ack==k->iss+1){
            k->rcv_nxt=seq+1; k->snd_una=ack; k->snd_nxt=ack; k->snd_wnd=(u32)wnd; tcp_parse_mss(k,p,hl);
            k->st=ST_EST; k->ever=1; k->connecting=0; k->tries=0; tcp_xmit(k,TF_ACK,k->snd_nxt,0,0,0);
        }
        return 1;
    }
    if(k->st==ST_SYN_RCVD){
        if((fl&TF_ACK) && ack==k->iss+1){
            k->snd_una=ack; k->snd_nxt=ack; k->snd_wnd=(u32)wnd; k->st=ST_EST; k->ever=1; k->tries=0;
            KSock *L=&S[k->parent]; if(k->parent>=0 && L->used && L->aq_n<16) L->aq[L->aq_n++]=idx(k);
        } else return 1;
    }
    int need_ack=0, fin=(fl&TF_FIN)!=0, odl=dl;
    if(fl&TF_ACK){ tcp_acked(k,ack,(u32)wnd);
        if(k->fin_acked){ if(k->st==ST_FW1) k->st=ST_FW2; else if(k->st==ST_CLOSING){ k->st=ST_TW; k->t_tw=now; } else if(k->st==ST_LASTACK){ k->st=ST_CLOSED; return 1; } } }
    if(dl>0 || fin){
        if(SEQ_LT(seq,k->rcv_nxt)){ u32 skip=k->rcv_nxt-seq; if(skip>=(u32)odl+(fin?1u:0u)){ need_ack=1; dl=0; fin=0; } else { if(skip>(u32)dl) skip=(u32)dl; data+=skip; dl-=(int)skip; seq+=skip; } }
        if(dl>0||fin){
            if(seq!=k->rcv_nxt){ need_ack=1; dl=0; fin=0; }
            else {
                if(dl>0 && (k->st==ST_EST||k->st==ST_FW1||k->st==ST_FW2)){
                    int room=RXSZ-k->rx_n, take=dl<room?dl:room;
                    if(take>0 && !k->shut_rd){ rx_put(k,data,take); }
                    k->rcv_nxt+=(u32)take; need_ack=1; if(take<dl) fin=0;
                }
                if(fin){ k->rcv_nxt++; k->fin_rcvd=1; need_ack=1;
                    if(k->st==ST_EST) k->st=ST_CWAIT; else if(k->st==ST_FW1) k->st=ST_CLOSING; else if(k->st==ST_FW2){ k->st=ST_TW; k->t_tw=now; } }
            }
        }
    }
    if(need_ack) tcp_xmit(k,TF_ACK,k->snd_nxt,0,0,0);
    tcp_output(k);
    return 1;
}

/* ---------- UDP ---------- */
static int udp_in(u32 src,u32 dst,const u8 *p,int n){
    if(n<8) return 0;
    u16 sp=rd16(p), dp=rd16(p+2); int ul=rd16(p+4); if(ul<8||ul>n) return 0;
    KSock *best=0;
    for(int i=0;i<NSOCK;i++){ KSock *k=&S[i]; if(!k->used||k->family!=SK_AF_INET||k->type!=SK_DGRAM||k->lport!=dp) continue;
        if(k->rport){ if(k->rport==sp&&k->rip==src){ best=k; break; } else continue; }
        if(!best) best=k; }
    if(!best) return 0;
    if(rd16(p+6)){ u32 sum=pseudo(src,dst,17,ul); sum=csum_add(sum,p,ul); if(csum_fin(sum)!=0) return 1; }
    dg_put(best,src,sp,p+8,ul-8);
    return 1;
}
static int ip_hook(u8 proto,u32 src,u32 dst,const u8 *p,int n){ return proto==6?tcp_in(src,dst,p,n):proto==17?udp_in(src,dst,p,n):0; }

/* ---------- temporizadores ---------- */
static void unlink_from_parent(KSock *c){
    if(c->parent<0) return;
    KSock *L=&S[c->parent];
    if(L->used) for(int i=0;i<L->aq_n;i++) if(L->aq[i]==idx(c)){ L->aq[i]=L->aq[--L->aq_n]; break; }
    c->parent=-1;
}
static void tick(void){
    u32 now=kticks();
    for(int i=0;i<NSOCK;i++){ KSock *k=&S[i];
        if(!k->used||k->local||k->family!=SK_AF_INET||k->type!=SK_STREAM||k->st==ST_LISTEN) continue;
        if(k->st==ST_CLOSED){ if(k->refs<=0){ unlink_from_parent(k); sk_free(k); } else if(k->parent>=0){ unlink_from_parent(k); sk_free(k); } continue; }
        if(k->st==ST_TW){ if((int)(now-k->t_tw)>=300){ k->st=ST_CLOSED; } continue; }
        if(k->refs<=0 && k->parent<0 && (int)(now-k->t_tw)>6000){ k->st=ST_CLOSED; continue; }      /* huerfano que nunca termina de cerrarse */
        if(k->st==ST_SYN_SENT||k->st==ST_SYN_RCVD){
            if((int)(now-k->t_rtx)>=RTO*(1+k->tries)){
                if(++k->tries>5){ k->err=E_TIMEDOUT; k->st=ST_CLOSED; k->connecting=0; continue; }
                if(k->st==ST_SYN_SENT) tcp_xmit(k,TF_SYN,k->iss,0,0,1); else tcp_xmit(k,TF_SYN|TF_ACK,k->iss,0,0,1);
                k->t_rtx=now; }
            continue; }
        int outst=k->sent>0 || (k->fin_sent&&!k->fin_acked);
        if(outst && (int)(now-k->t_rtx)>=RTO*(1+k->tries)){
            if(++k->tries>8){ k->err=E_TIMEDOUT; k->st=ST_CLOSED; continue; }
            if(k->fin_sent && !k->fin_acked) k->fin_sent=0;
            k->sent=0; k->snd_nxt=k->snd_una; k->t_rtx=now; tcp_output(k);
        } else if(k->tx_n>k->sent || (k->shut_wr&&!k->fin_sent)) tcp_output(k);
    }
}

/* ---------- API ---------- */
static int memeq(const void *a,const void *b,int n){ const u8 *x=a,*y=b; for(int i=0;i<n;i++) if(x[i]!=y[i]) return 0; return 1; }
static int path_eq(KSock *k,const char *p,int l){ return k->family==SK_AF_UNIX && k->bound && k->ulen==l && memeq(k->upath,p,l); }
static KSock *unix_find(const char *p,int l,int type){ for(int i=0;i<NSOCK;i++) if(S[i].used && S[i].type==type && path_eq(&S[i],p,l)) return &S[i]; return 0; }
static KSock *inet_listener(u16 port){ for(int i=0;i<NSOCK;i++){ KSock *k=&S[i]; if(k->used&&k->family==SK_AF_INET&&k->type==SK_STREAM&&k->st==ST_LISTEN&&k->lport==port) return k; } return 0; }

int sock_bind(int s,u32 ip,u16 port,const char *upath,int plen){
    if(!OK(s)) return -E_NOTSOCK;
    KSock *k=&S[s];
    if(k->bound) return -E_INVAL;
    if(k->family==SK_AF_UNIX){
        if(plen<=0||plen>(int)sizeof k->upath) return -E_INVAL;
        if(unix_find(upath,plen,k->type)) return -E_ADDRINUSE;
        memcpy(k->upath,upath,(size_t)plen); k->ulen=plen; k->bound=1; return 0;
    }
    if(!port) port=new_port();
    else for(int i=0;i<NSOCK;i++){ KSock *o=&S[i]; if(i!=s&&o->used&&o->family==SK_AF_INET&&o->type==k->type&&o->lport==port&&o->st!=ST_CLOSED&&o->st!=ST_TW&&!(o->reuse&&k->reuse)) return -E_ADDRINUSE; }
    k->lport=port; k->lip=ip; k->bound=1; return 0;
}
int sock_listen(int s,int backlog){
    if(!OK(s)) return -E_NOTSOCK;
    KSock *k=&S[s];
    if(k->type!=SK_STREAM) return -E_OPNOTSUPP;
    if(k->st!=ST_CLOSED) return -E_INVAL;
    if(!k->bound){ if(k->family==SK_AF_INET){ k->lport=new_port(); k->bound=1; } else return -E_INVAL; }
    k->backlog=backlog<1?1:backlog>16?16:backlog; k->st=ST_LISTEN; k->aq_n=0; return 0;
}
int sock_connecting(int s){ return OK(s)?S[s].connecting:0; }

int sock_connect(int s,u32 ip,u16 port,const char *upath,int plen){
    if(!OK(s)) return -E_NOTSOCK;
    KSock *k=&S[s];
    if(k->type==SK_DGRAM){                                         /* "conectar" un datagrama solo fija el destino */
        if(k->family==SK_AF_UNIX){ KSock *t=unix_find(upath,plen,SK_DGRAM); if(!t) return -E_CONNREFUSED; k->peer=idx(t); k->ever=1; return 0; }
        if(!k->bound){ k->lport=new_port(); k->bound=1; }
        k->rip=ip; k->rport=port; k->ever=1; return 0;
    }
    if(k->st==ST_EST||k->st==ST_CWAIT) return -E_ISCONN;
    if(k->st==ST_SYN_SENT) return -E_INPROGRESS;
    if(k->st!=ST_CLOSED) return -E_INVAL;
    if(k->family==SK_AF_UNIX||is_loop(ip)){                          /* conexion directa dentro del kernel */
        KSock *L=k->family==SK_AF_UNIX?unix_find(upath,plen,SK_STREAM):inet_listener(port);
        if(!L||L->st!=ST_LISTEN) return -E_CONNREFUSED;
        if(L->aq_n>=L->backlog) return -E_CONNREFUSED;
        KSock *c=sk_new(k->family,SK_STREAM); if(!c) return -E_NOMEM;
        if(k->family==SK_AF_INET){ if(!k->bound){ k->lport=new_port(); k->bound=1; } k->rip=ip; k->rport=port; k->lip=ip; c->lport=port; c->rport=k->lport; c->rip=ip; c->lip=ip; c->bound=1; }
        else { memcpy(c->upath,L->upath,(size_t)L->ulen); c->ulen=L->ulen; c->bound=1; }
        c->local=1; k->local=1; c->st=k->st=ST_EST; c->ever=k->ever=1; c->peer=idx(k); k->peer=idx(c);
        c->parent=idx(L); L->aq[L->aq_n++]=idx(c);
        return 0;
    }
    if(!net_my_ip()) return -E_NETUNREACH;
    if(!k->bound){ k->lport=new_port(); k->bound=1; }
    k->rip=ip; k->rport=port; k->lip=net_my_ip();
    k->iss=kticks()*2654435761u+(iss_ctr+=64013); k->snd_una=k->iss; k->snd_nxt=k->iss+1; k->t_rtx=kticks(); k->tries=0;
    k->tx_n=k->tx_r=k->sent=0; k->rx_n=k->rx_r=0; k->fin_rcvd=k->fin_sent=k->fin_acked=k->shut_wr=k->shut_rd=0; k->err=0;
    k->st=ST_SYN_SENT; k->connecting=1;
    tcp_xmit(k,TF_SYN,k->iss,0,0,1);
    return -E_INPROGRESS;
}
int sock_accept(int s,u32 *ip,u16 *port){
    if(!OK(s)) return -E_NOTSOCK;
    KSock *k=&S[s];
    if(k->st!=ST_LISTEN) return -E_INVAL;
    if(!k->aq_n) return -E_AGAIN;
    int c=k->aq[0]; for(int i=1;i<k->aq_n;i++) k->aq[i-1]=k->aq[i]; k->aq_n--;
    KSock *a=&S[c]; a->parent=-1; a->refs=1;
    if(ip) *ip=a->local&&a->family==SK_AF_INET?a->lip:a->rip;
    if(port) *port=a->rport;
    return c;
}
static int dg_deliver_local(KSock *from,u16 port,const u8 *d,int n){
    for(int i=0;i<NSOCK;i++){ KSock *t=&S[i]; if(t->used&&t->family==SK_AF_INET&&t->type==SK_DGRAM&&t->lport==port&&t!=from){ dg_put(t,(u32)0x7F000001,from->lport,d,n); return 0; } }
    return -E_CONNREFUSED;
}
int sock_send(int s,const void *dv,int n,u32 ip,u16 port,const char *upath,int plen,int have_dst){
    if(!OK(s)) return -E_NOTSOCK;
    KSock *k=&S[s]; const u8 *d=dv;
    if(n<0) return -E_INVAL;
    if(k->type==SK_STREAM){
        if(k->local){
            if(k->shut_wr) return -E_PIPE;
            if(k->peer<0||!S[k->peer].used) return -E_PIPE;
            KSock *B=&S[k->peer]; if(B->shut_rd) return -E_PIPE;
            int fr=RXSZ-B->rx_n; if(fr<=0) return n?-E_AGAIN:0;
            int t=n<fr?n:fr; rx_put(B,d,t); return t;
        }
        if(k->st==ST_SYN_SENT||k->st==ST_SYN_RCVD) return -E_AGAIN;
        if(k->st!=ST_EST&&k->st!=ST_CWAIT){ if(k->err){ int e=k->err; k->err=0; return -e; } return k->ever?-E_PIPE:-E_NOTCONN; }
        if(k->shut_wr) return -E_PIPE;
        int fr=TXSZ-k->tx_n; if(fr<=0) return n?-E_AGAIN:0;
        int t=n<fr?n:fr; ring_put(k->tx,TXSZ,k->tx_r,k->tx_n,d,t); k->tx_n+=t; tcp_output(k); return t;
    }
    /* datagramas */
    if(n>1400) return -E_MSGSIZE;
    if(k->family==SK_AF_UNIX){
        KSock *t=0;
        if(have_dst) t=unix_find(upath,plen,SK_DGRAM); else if(k->peer>=0&&S[k->peer].used) t=&S[k->peer];
        if(!t) return have_dst?-E_CONNREFUSED:-E_DESTADDRREQ;
        dg_put(t,0,0,d,n); return n;
    }
    if(!k->bound){ k->lport=new_port(); k->bound=1; }
    if(!have_dst){ if(!k->rport) return -E_DESTADDRREQ; ip=k->rip; port=k->rport; }
    if(is_loop(ip)||ip==0){ int r=dg_deliver_local(k,port,d,n); return r<0?r:n; }
    if(!net_my_ip()) return -E_NETUNREACH;
    int r=net_udp_send(ip,k->lport,port,d,n); return r<0?-E_NOBUFS:n;
}
int sock_recv(int s,void *dv,int n,int peek,u32 *ip,u16 *port){
    if(!OK(s)) return -E_NOTSOCK;
    KSock *k=&S[s]; u8 *d=dv;
    if(k->type==SK_STREAM){
        if(k->st==ST_LISTEN) return -E_INVAL;
        if(k->rx_n>0){
            int before=RXSZ-k->rx_n; int t=rx_take(k,d,n,peek);
            if(!peek && !k->local && before<2*MSS && (k->st==ST_EST||k->st==ST_FW1||k->st==ST_FW2)) tcp_xmit(k,TF_ACK,k->snd_nxt,0,0,0);   /* aviso de ventana abierta */
            return t; }
        if(k->err){ int e=k->err; k->err=0; return -e; }
        if(k->fin_rcvd||k->shut_rd) return 0;
        if(k->st==ST_CLOSED) return k->ever?0:-E_NOTCONN;
        return -E_AGAIN;
    }
    if(k->rx_n<8){ if(k->err){ int e=k->err; k->err=0; return -e; } return -E_AGAIN; }
    u8 h[8]; ring_get(k->rx,RXSZ,k->rx_r,h,8); int len=rd16(h);
    int t=len<n?len:n;
    if(!peek){ k->rx_r=(k->rx_r+8)%RXSZ; k->rx_n-=8; rx_take(k,d,t,0); if(len>t){ k->rx_r=(k->rx_r+(len-t))%RXSZ; k->rx_n-=len-t; } if(!k->rx_n) k->rx_r=0; }
    else { u8 tmp[8]; (void)tmp; int r2=(k->rx_r+8)%RXSZ; ring_get(k->rx,RXSZ,r2,d,t); }
    if(ip) *ip=rd32(h+4);
    if(port) *port=rd16(h+2);
    return t;
}
int sock_events(int s){
    if(!OK(s)) return SK_POLLERR|SK_POLLHUP;
    KSock *k=&S[s]; int ev=0;
    if(k->type==SK_DGRAM){ if(k->rx_n>=8) ev|=SK_POLLIN; if(k->err) ev|=SK_POLLERR; return ev|SK_POLLOUT; }
    if(k->st==ST_LISTEN) return k->aq_n?SK_POLLIN:0;
    if(k->connecting) return 0;
    if(k->err) ev|=SK_POLLERR|SK_POLLOUT|SK_POLLHUP;
    if(k->rx_n>0||k->fin_rcvd||k->shut_rd) ev|=SK_POLLIN;
    if(k->fin_rcvd) ev|=SK_POLLRDHUP;
    if(k->local){
        if(k->st==ST_EST){ if(k->peer>=0&&S[k->peer].used){ if(S[k->peer].rx_n<RXSZ) ev|=SK_POLLOUT; } else ev|=SK_POLLOUT|SK_POLLHUP; }
        else if(!k->ever) ev|=SK_POLLOUT|SK_POLLHUP;
        return ev;
    }
    if((k->st==ST_EST||k->st==ST_CWAIT)&&!k->shut_wr&&k->tx_n<TXSZ) ev|=SK_POLLOUT;
    if(k->st==ST_CLOSED){ if(k->ever) ev|=SK_POLLIN|SK_POLLHUP; else ev|=SK_POLLOUT|SK_POLLHUP; }
    if(k->st==ST_TW||k->st==ST_CLOSING||k->st==ST_LASTACK) ev|=SK_POLLHUP;
    return ev;
}
int sock_shutdown(int s,int how){
    if(!OK(s)) return -E_NOTSOCK;
    KSock *k=&S[s];
    if(k->type!=SK_STREAM) return 0;
    if(k->st==ST_CLOSED&&!k->local) return -E_NOTCONN;
    if(how==0||how==2) k->shut_rd=1;
    if(how==1||how==2){ k->shut_wr=1;
        if(k->local){ if(k->peer>=0&&S[k->peer].used) S[k->peer].fin_rcvd=1; }
        else tcp_output(k); }
    return 0;
}
void sock_unref(int s){
    if(!OK(s)) return;
    KSock *k=&S[s];
    if(--k->refs>0) return;
    k->refs=0;
    if(k->st==ST_LISTEN){
        for(int i=0;i<k->aq_n;i++){ KSock *c=&S[k->aq[i]]; if(c->used){ c->parent=-1; c->refs=0; if(c->local){ if(c->peer>=0&&S[c->peer].used){ S[c->peer].fin_rcvd=1; S[c->peer].peer=-1; } sk_free(c); } else { c->st=ST_CLOSED; } } }
        for(int i=0;i<NSOCK;i++) if(S[i].used&&S[i].parent==s){ S[i].parent=-1; S[i].refs=0; S[i].st=ST_CLOSED; }
        sk_free(k); return; }
    if(k->type==SK_DGRAM||k->local){
        if(k->local&&k->peer>=0&&S[k->peer].used){ S[k->peer].fin_rcvd=1; S[k->peer].peer=-1; }
        sk_free(k); return; }
    switch(k->st){
    case ST_CLOSED: case ST_SYN_SENT: sk_free(k); return;
    case ST_TW: k->t_tw=kticks(); return;
    default: break; }
    if(k->rx_n>0 && (k->st==ST_EST||k->st==ST_CWAIT)){ tcp_xmit(k,TF_RST|TF_ACK,k->snd_nxt,0,0,0); k->st=ST_CLOSED; return; }   /* cerrar con datos sin leer: se reinicia */
    k->shut_wr=1; k->shut_rd=1; k->t_tw=kticks(); tcp_output(k);
}
int sock_names(int s,int peer,u32 *ip,u16 *port,char *upath,int *plen){
    if(!OK(s)) return -E_NOTSOCK;
    KSock *k=&S[s];
    if(k->family==SK_AF_UNIX){
        KSock *o=k; if(peer){ if(k->peer<0||!S[k->peer].used) return -E_NOTCONN; o=&S[k->peer]; }
        *plen=o->bound?o->ulen:0; if(*plen) memcpy(upath,o->upath,(size_t)*plen); return SK_AF_UNIX; }
    if(peer){ if(!k->ever||(k->st==ST_CLOSED&&!k->rport)) return -E_NOTCONN; *ip=k->rip; *port=k->rport; }
    else { *ip=k->lip?k->lip:(k->st==ST_LISTEN||!k->ever?0:net_my_ip()); *port=k->lport; }
    return SK_AF_INET;
}
void sock_init(void){ memset(S,0,sizeof S); sock_ip_hook=ip_hook; sock_tick_hook=tick; }
