/* net.c — pila de red de kng (compartida por los kernels de 32 y 64 bits).
 * PCI + controladores e1000 y RTL8139, ARP, IPv4, ICMP, UDP, DHCP, DNS, TCP (cliente, una conexion) y HTTP/1.0.
 * Codigo propio escrito a partir de las especificaciones (RFC 791/792/793/826/1035/2131, manuales de Intel y Realtek).
 * Memoria DMA: arreglos estaticos en la imagen del kernel (identidad: direccion virtual = fisica, por debajo de 4 GB). */
#include "net.h"

typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;

/* ---------- E/S de puertos (en el simulador del host no hacen nada) ---------- */
#ifdef HOSTNET
static inline void io_o8(u16 p,u8 v){ (void)p;(void)v; }
static inline u8   io_i8(u16 p){ (void)p; return 0; }
static inline void io_o16(u16 p,u16 v){ (void)p;(void)v; }
static inline u16  io_i16(u16 p){ (void)p; return 0; }
static inline void io_o32(u16 p,u32 v){ (void)p;(void)v; }
static inline u32  io_i32(u16 p){ (void)p; return 0; }
#else
static inline void io_o8(u16 p,u8 v){ __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p)); }
static inline u8   io_i8(u16 p){ u8 v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline void io_o16(u16 p,u16 v){ __asm__ volatile("outw %0,%1"::"a"(v),"Nd"(p)); }
static inline u16  io_i16(u16 p){ u16 v; __asm__ volatile("inw %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline void io_o32(u16 p,u32 v){ __asm__ volatile("outl %0,%1"::"a"(v),"Nd"(p)); }
static inline u32  io_i32(u16 p){ u32 v; __asm__ volatile("inl %1,%0":"=a"(v):"Nd"(p)); return v; }
#endif

#ifdef HOSTNET
static void msleep(int ms){ (void)ms; }
#else
static void msleep(int ms){ u32 t=kticks(); int n=(ms+9)/10+1; while((int)(kticks()-t)<n) __asm__ volatile("pause"); }
#endif

/* ---------- utilidades ---------- */
static u16 rd16(const u8 *p){ return (u16)((p[0]<<8)|p[1]); }
static u32 rd32(const u8 *p){ return ((u32)p[0]<<24)|((u32)p[1]<<16)|((u32)p[2]<<8)|p[3]; }
static void wr16(u8 *p,u16 v){ p[0]=(u8)(v>>8); p[1]=(u8)v; }
static void wr32(u8 *p,u32 v){ p[0]=(u8)(v>>24); p[1]=(u8)(v>>16); p[2]=(u8)(v>>8); p[3]=(u8)v; }
static u32 csum_add(u32 s,const u8 *p,int n){ for(;n>1;n-=2,p+=2) s+=rd16(p); if(n) s+=(u32)p[0]<<8; return s; }
static u16 csum_fin(u32 s){ while(s>>16) s=(s&0xFFFF)+(s>>16); return (u16)~s; }
static u32 pseudo(u32 src,u32 dst,u8 proto,int len){ return (src>>16)+(src&0xFFFF)+(dst>>16)+(dst&0xFFFF)+proto+(u32)len; }
static int lc(int c){ return (c>='A'&&c<='Z')?c+32:c; }
static int istr_prefix(const char *s,const char *p){ while(*p){ if(lc((u8)*s)!=lc((u8)*p)) return 0; s++; p++; } return 1; }
static const char *istr_find(const char *h,int n,const char *nd){      /* busca nd en h[0..n) sin distinguir mayusculas */
    int l=k_strlen(nd);
    for(int i=0;i+l<=n;i++) if(istr_prefix(h+i,nd)) return h+i;
    return 0;
}
static int mcmp(const void *a,const void *b,int n){ const u8 *x=a,*y=b; for(int i=0;i<n;i++) if(x[i]!=y[i]) return x[i]-y[i]; return 0; }
static void portstr(char *o,u16 port){ char t[8]; int n=0,k=0; u32 v=port; while(v){ t[n++]=(char)('0'+v%10); v/=10; } o[k++]=':'; while(n) o[k++]=t[--n]; o[k]=0; }
static void mmove_fwd(u8 *d,const u8 *s,int n){ for(int i=0;i<n;i++) d[i]=s[i]; }   /* d<=s */
static char ipbuf[4][16]; static int ipbi;
static const char *ipstr(u32 ip){
    char *o=ipbuf[ipbi++&3]; int k=0;
    for(int i=3;i>=0;i--){ u32 v=(ip>>(i*8))&255; char t[4]; int n=0; if(!v) t[n++]='0'; while(v){ t[n++]=(char)('0'+v%10); v/=10; } while(n) o[k++]=t[--n]; if(i) o[k++]='.'; }
    o[k]=0; return o;
}
static int parse_ip(const char *s,u32 *out){
    u32 ip=0; int parts=0;
    for(;;){
        if(*s<'0'||*s>'9') return 0;
        u32 v=0; int d=0; while(*s>='0'&&*s<='9'){ v=v*10+(u32)(*s-'0'); s++; if(++d>3||v>255) return 0; }
        ip=(ip<<8)|v; parts++;
        if(parts==4) break;
        if(*s!='.') return 0;
        s++;
    }
    if(*s) return 0;
    *out=ip; return 1;
}
static void scat(char *d,int max,const char *s){ int i=k_strlen(d); while(*s && i<max-1) d[i++]=*s++; d[i]=0; }

/* ---------- PCI ---------- */
static u32 pci_rd(int b,int d,int f,int r){ io_o32(0xCF8,0x80000000u|((u32)b<<16)|((u32)d<<11)|((u32)f<<8)|(u32)(r&0xFC)); return io_i32(0xCFC); }
static void pci_wr(int b,int d,int f,int r,u32 v){ io_o32(0xCF8,0x80000000u|((u32)b<<16)|((u32)d<<11)|((u32)f<<8)|(u32)(r&0xFC)); io_o32(0xCFC,v); }

/* ---------- interfaz de tarjeta ----------
 * recv: >0 longitud de la trama, 0 no hay mas, <0 trama descartada (hay que seguir leyendo). */
struct nicdrv { const char *name; int (*send)(const u8*,int); int (*recv)(u8*,int); int (*link)(void); };
static const struct nicdrv *nic;
static u8 mac[6];
static char nic_desc[64];
static char pci_seen[80];                       /* tarjetas de red PCI vistas (ven:dev), para el diagnostico */

/* ===== Intel e1000 (8254x) — descriptores heredados ===== */
#define E_NRX 32
#define E_NTX 8
struct edesc { uint64_t addr; u16 length; u8 f2; u8 cmd; u8 status; u8 errors; u16 special; };
static struct edesc e_rxd[E_NRX] __attribute__((aligned(128)));
static struct edesc e_txd[E_NTX] __attribute__((aligned(128)));
static u8 e_rxb[E_NRX][2048] __attribute__((aligned(16)));
static u8 e_txb[E_NTX][2048] __attribute__((aligned(16)));
static volatile u8 *e_mmio; static int e_rx_cur, e_tx_cur;
uint32_t net_mmio_addr(void){ return (uint32_t)(uintptr_t)e_mmio; }
static u32 er(u32 r){ return *(volatile u32*)(e_mmio+r); }
static void ew(u32 r,u32 v){ *(volatile u32*)(e_mmio+r)=v; }
static int e_send(const u8 *f,int n){
    volatile struct edesc *d=&e_txd[e_tx_cur];
    int t=200000; while(!(d->status&1) && --t) __asm__ volatile("" ::: "memory");
    if(!t) return -1;
    if(n>2000) return -1;
    memcpy(e_txb[e_tx_cur],f,(size_t)n);
    if(n<60){ memset(e_txb[e_tx_cur]+n,0,(size_t)(60-n)); n=60; }
    d->addr=(uint64_t)(uintptr_t)e_txb[e_tx_cur]; d->length=(u16)n; d->cmd=0x0B; d->status=0;   /* EOP|IFCS|RS */
    e_tx_cur=(e_tx_cur+1)%E_NTX;
    __asm__ volatile("" ::: "memory");
    ew(0x3818,(u32)e_tx_cur);
    return 0;
}
static int e_recv(u8 *buf,int max){
    volatile struct edesc *d=&e_rxd[e_rx_cur];
    if(!(d->status&1)) return 0;
    int n=d->length, bad=d->errors!=0 || n>max || n<14;
    if(!bad) memcpy(buf,e_rxb[e_rx_cur],(size_t)n);
    d->status=0; ew(0x2818,(u32)e_rx_cur); e_rx_cur=(e_rx_cur+1)%E_NRX;
    return bad?-1:n;
}
static int e_link(void){ return (er(0x8)&2)!=0; }
static const struct nicdrv e_drv={"Intel e1000",e_send,e_recv,e_link};
static const u16 e_ids[]={0x1000,0x1001,0x1004,0x1008,0x1009,0x100C,0x100D,0x100E,0x100F,0x1010,0x1011,0x1012,0x1013,
    0x1015,0x1016,0x1017,0x1018,0x1019,0x101A,0x101D,0x1026,0x1027,0x1028,0x1075,0x1076,0x1077,0x1078,0x1079,0x107A,0x107B,0x107C,0x10B5,0x10D3};
static int e_init(u32 bar0){
    e_mmio=(volatile u8*)(uintptr_t)bar0;
    ew(0x0D8,0xFFFFFFFFu);                         /* IMC: sin interrupciones (se atiende por sondeo) */
    ew(0x000,er(0x000)|(1u<<26));                  /* CTRL.RST */
    msleep(20);
    ew(0x0D8,0xFFFFFFFFu); (void)er(0x0C0);
    u32 ctrl=er(0x000); ctrl|=(1u<<6)|(1u<<5); ctrl&=~((1u<<3)|(1u<<31)|(1u<<30)); ew(0x000,ctrl);   /* SLU, ASDE; sin LRST/PHY_RST/VME */
    u32 ral=er(0x5400), rah=er(0x5404);
    if(!ral && !(rah&0xFFFF)){                      /* sin MAC precargada: leer la EEPROM */
        for(int i=0;i<3;i++){
            ew(0x014,1u|((u32)i<<8)); int t=100000; u32 v=0;
            while(t--){ v=er(0x014); if(v&0x10) break; }
            u16 w=(u16)(v>>16); mac[i*2]=(u8)w; mac[i*2+1]=(u8)(w>>8);
        }
    } else { mac[0]=(u8)ral; mac[1]=(u8)(ral>>8); mac[2]=(u8)(ral>>16); mac[3]=(u8)(ral>>24); mac[4]=(u8)rah; mac[5]=(u8)(rah>>8); }
    for(int i=0;i<128;i++) ew(0x5200+(u32)i*4,0);   /* MTA */
    for(int i=0;i<E_NRX;i++){ e_rxd[i].addr=(uint64_t)(uintptr_t)e_rxb[i]; e_rxd[i].status=0; e_rxd[i].errors=0; e_rxd[i].length=0; }
    ew(0x2800,(u32)(uintptr_t)e_rxd); ew(0x2804,0); ew(0x2808,E_NRX*16); ew(0x2810,0); ew(0x2818,E_NRX-1);
    e_rx_cur=0;
    ew(0x100,(1u<<1)|(1u<<15)|(1u<<26));           /* RCTL: EN | BAM | SECRC, buffer de 2048 */
    for(int i=0;i<E_NTX;i++){ e_txd[i].status=1; e_txd[i].cmd=0; e_txd[i].length=0; }
    ew(0x3800,(u32)(uintptr_t)e_txd); ew(0x3804,0); ew(0x3808,E_NTX*16); ew(0x3810,0); ew(0x3818,0);
    e_tx_cur=0;
    ew(0x410,0x0060200Au);                          /* TIPG */
    ew(0x400,(1u<<1)|(1u<<3)|(0x0Fu<<4)|(0x40u<<12));   /* TCTL: EN | PSP | CT | COLD */
    return 0;
}

/* ===== Realtek RTL8139 ===== */
static u16 r_io; static u32 r_rx_off; static int r_tx_cur;
static u8 r_rxb[8192+16+1536+16] __attribute__((aligned(16)));
static u8 r_txb[4][2048] __attribute__((aligned(16)));
static int r_send(const u8 *f,int n){
    if(n>1792) return -1;
    int t=100000; while(!(io_i32((u16)(r_io+0x10+4*r_tx_cur))&0x2000) && --t) __asm__ volatile("" ::: "memory");
    memcpy(r_txb[r_tx_cur],f,(size_t)n);
    if(n<60){ memset(r_txb[r_tx_cur]+n,0,(size_t)(60-n)); n=60; }
    io_o32((u16)(r_io+0x20+4*r_tx_cur),(u32)(uintptr_t)r_txb[r_tx_cur]);
    io_o32((u16)(r_io+0x10+4*r_tx_cur),(u32)n&0x1FFF);
    r_tx_cur=(r_tx_cur+1)&3;
    return 0;
}
static int r_recv(u8 *buf,int max){
    if(io_i8((u16)(r_io+0x37))&1) return 0;           /* CR.BUFE: buffer vacio */
    u8 *p=r_rxb+r_rx_off; u16 st=(u16)(p[0]|(p[1]<<8)), len=(u16)(p[2]|(p[3]<<8));
    if(!(st&1) || len<18 || len>1522){                /* paquete invalido: reiniciar el receptor */
        io_o8((u16)(r_io+0x37),0x04); io_o8((u16)(r_io+0x37),0x0C);
        io_o32((u16)(r_io+0x30),(u32)(uintptr_t)r_rxb); r_rx_off=0; io_o16((u16)(r_io+0x38),(u16)(0u-16u)); return 0;
    }
    int n=len-4, bad=n>max; if(!bad) memcpy(buf,p+4,(size_t)n);
    r_rx_off=(r_rx_off+len+4+3)&~3u; if(r_rx_off>=8192) r_rx_off-=8192;
    io_o16((u16)(r_io+0x38),(u16)(r_rx_off-16));
    return bad?-1:n;
}
static int r_link(void){ return !(io_i8((u16)(r_io+0x58))&4); }
static const struct nicdrv r_drv={"Realtek RTL8139",r_send,r_recv,r_link};
static int r_init(u16 io){
    r_io=io;
    io_o8((u16)(io+0x52),0);                           /* CONFIG1: encender */
    io_o8((u16)(io+0x37),0x10); int t=100000; while((io_i8((u16)(io+0x37))&0x10) && --t);
    for(int i=0;i<6;i++) mac[i]=io_i8((u16)(io+i));
    memset(r_rxb,0,sizeof r_rxb); r_rx_off=0; r_tx_cur=0;
    io_o32((u16)(io+0x30),(u32)(uintptr_t)r_rxb);
    io_o16((u16)(io+0x3C),0);                          /* IMR: sin interrupciones */
    io_o32((u16)(io+0x44),0x0Eu|0x80u|(7u<<8));        /* RCR: AB|AM|APM, WRAP, DMA sin limite, buffer de 8 KB */
    io_o32((u16)(io+0x40),0x03000700u);                /* TCR */
    io_o8((u16)(io+0x37),0x0C);                        /* RE | TE */
    io_o16((u16)(io+0x38),(u16)(0u-16u));
    return 0;
}

#ifdef HOSTNET
extern int host_send(const u8*,int); extern int host_recv(u8*,int);
static int host_link(void){ return 1; }
static const struct nicdrv h_drv={"simulada (host)",host_send,host_recv,host_link};
#endif

#include "net_pcnet_virtio.h"

/* ---------- configuracion IP ---------- */
static u32 my_ip, mask, gw, dns_ip;
static int net_st;               /* 0 sin NIC, 1 DHCP, 2 lista */
static int ip_static;            /* 1 si la configuracion es la de reserva (DHCP fallo) */
static u16 ipid;

/* ---------- ARP ---------- */
struct arpe { u32 ip; u8 mac[6]; int ok; };
static struct arpe arpt[8]; static int arp_next;
static u8 pend[1600]; static int pend_len, pend_try; static u32 pend_ip, pend_t;
static u8 ob[1600];

static int arp_find(u32 ip,u8 *m){ for(int i=0;i<8;i++) if(arpt[i].ok && arpt[i].ip==ip){ memcpy(m,arpt[i].mac,6); return 1; } return 0; }
static void arp_learn(u32 ip,const u8 *m){
    for(int i=0;i<8;i++) if(arpt[i].ok && arpt[i].ip==ip){ memcpy(arpt[i].mac,m,6); return; }
    arpt[arp_next].ip=ip; memcpy(arpt[arp_next].mac,m,6); arpt[arp_next].ok=1; arp_next=(arp_next+1)&7;
}
static void arp_send(int op,const u8 *dmac,const u8 *tha,u32 tpa){
    u8 f[60]; memset(f,0,sizeof f);
    memcpy(f,dmac,6); memcpy(f+6,mac,6); wr16(f+12,0x0806);
    u8 *a=f+14; wr16(a,1); wr16(a+2,0x0800); a[4]=6; a[5]=4; wr16(a+6,(u16)op);
    memcpy(a+8,mac,6); wr32(a+14,my_ip); memcpy(a+18,tha,6); wr32(a+24,tpa);
    nic->send(f,60);
}
static void arp_request(u32 ip){ static const u8 bc[6]={255,255,255,255,255,255}, z[6]={0}; arp_send(1,bc,z,ip); }
static void arp_in(const u8 *p,int n){
    if(n<28 || rd16(p)!=1 || rd16(p+2)!=0x0800 || p[4]!=6 || p[5]!=4) return;
    int op=rd16(p+6); u32 spa=rd32(p+14), tpa=rd32(p+24);
    if(spa) arp_learn(spa,p+8);
    if(op==1 && my_ip && tpa==my_ip) arp_send(2,p+8,p+8,spa);
    if(pend_len && spa==pend_ip){ memcpy(pend,p+8,6); memcpy(pend+6,mac,6); nic->send(pend,pend_len); pend_len=0; }
}

/* ---------- IPv4 ---------- */
static int ip_out(u8 proto,u32 dst,const u8 *pl,int plen){
    if(!nic || plen>1480) return -1;
    u8 *ip=ob+14;
    ip[0]=0x45; ip[1]=0; wr16(ip+2,(u16)(20+plen)); wr16(ip+4,ipid++); wr16(ip+6,0x4000); ip[8]=64; ip[9]=proto; wr16(ip+10,0);
    wr32(ip+12,my_ip); wr32(ip+16,dst);
    wr16(ip+10,csum_fin(csum_add(0,ip,20)));
    memcpy(ip+20,pl,(size_t)plen);
    wr16(ob+12,0x0800);
    int fl=14+20+plen; u8 dm[6];
    if(dst==0xFFFFFFFFu || (mask && dst==((my_ip&mask)|~mask))) memset(dm,0xFF,6);
    else {
        u32 nh=(mask && ((dst&mask)==(my_ip&mask)))?dst:(gw?gw:dst);
        if(!arp_find(nh,dm)){
            arp_request(nh);
            memcpy(pend,ob,(size_t)fl); pend_len=fl; pend_ip=nh; pend_t=kticks(); pend_try=1;
            return 1;
        }
    }
    memcpy(ob,dm,6); memcpy(ob+6,mac,6);
    return nic->send(ob,fl);
}

/* ---------- UDP ---------- */
static int udp_out(u32 dst,u16 sp,u16 dp,const u8 *d,int n){
    static u8 s[1500];
    if(n>1400) return -1;
    wr16(s,sp); wr16(s+2,dp); wr16(s+4,(u16)(8+n)); wr16(s+6,0); memcpy(s+8,d,(size_t)n);
    u32 sum=pseudo(my_ip,dst,17,8+n); sum=csum_add(sum,s,8+n);
    u16 c=csum_fin(sum); if(!c) c=0xFFFF; wr16(s+6,c);
    return ip_out(17,dst,s,8+n);
}

/* ---------- DHCP ---------- */
static int dh_state;             /* 0 inactivo, 1 DISCOVER enviado, 2 REQUEST enviado, 3 asignada, 4 reserva */
static u32 dh_xid, dh_t, dh_offer, dh_server; static int dh_tries;
static void dhcp_send(int type){
    u8 b[320]; memset(b,0,sizeof b);
    b[0]=1; b[1]=1; b[2]=6; wr32(b+4,dh_xid); wr16(b+10,0x8000);
    memcpy(b+28,mac,6); b[236]=0x63; b[237]=0x82; b[238]=0x53; b[239]=0x63;
    u8 *o=b+240;
    *o++=53; *o++=1; *o++=(u8)type;
    if(type==3){ *o++=50; *o++=4; wr32(o,dh_offer); o+=4; *o++=54; *o++=4; wr32(o,dh_server); o+=4; }
    *o++=55; *o++=4; *o++=1; *o++=3; *o++=6; *o++=28;
    *o++=255;
    udp_out(0xFFFFFFFFu,68,67,b,(int)(o-b)<300?300:(int)(o-b));
    dh_t=kticks();
}
static void dhcp_in(const u8 *p,int n){
    if(n<241 || p[0]!=2 || rd32(p+4)!=dh_xid || mcmp(p+28,mac,6)) return;
    if(rd32(p+236)!=0x63825363u) return;
    int type=0; u32 m=0,r=0,d=0,sid=0;
    for(int i=240;i<n;){
        int c=p[i]; if(c==255) break; if(c==0){ i++; continue; }
        if(i+1>=n) break;
        int l=p[i+1]; if(i+2+l>n) break;
        const u8 *v=p+i+2;
        if(c==53&&l>=1) type=v[0];
        else if(c==1&&l>=4) m=rd32(v);
        else if(c==3&&l>=4) r=rd32(v);
        else if(c==6&&l>=4) d=rd32(v);
        else if(c==54&&l>=4) sid=rd32(v);
        i+=2+l;
    }
    if(type==2 && dh_state==1){ dh_offer=rd32(p+16); dh_server=sid; dh_state=2; dh_tries=0; dhcp_send(3); }
    else if(type==5 && dh_state==2){
        my_ip=rd32(p+16); mask=m?m:0xFFFFFF00u; gw=r; dns_ip=d?d:r; dh_state=3; net_st=2; ip_static=0;
    } else if(type==6){ dh_state=1; dh_tries=0; dh_xid+=7; dhcp_send(1); }
}
static void dhcp_tick(void){
    if(dh_state!=1 && dh_state!=2) return;
    if((int)(kticks()-dh_t)<200) return;
    if(++dh_tries>=5){                                  /* sin servidor DHCP: configuracion de reserva (red NAT tipica de QEMU) */
        my_ip=(10u<<24)|(0<<16)|(2<<8)|15; mask=0xFFFFFF00u; gw=(10u<<24)|(2<<8)|2; dns_ip=(10u<<24)|(2<<8)|3;
        dh_state=4; net_st=2; ip_static=1; return;
    }
    if(dh_state==2) { dh_state=1; }
    dhcp_send(1);
}

/* ---------- DNS ---------- */
#define DNS_LPORT 53535
static char dq_name[64]; static int dq_state; static u32 dq_ip, dq_t; static u16 dq_id; static int dq_tries;
static void dns_send(void){
    u8 b[300]; int k=12; memset(b,0,12);
    wr16(b,dq_id); wr16(b+2,0x0100); wr16(b+4,1);
    const char *s=dq_name;
    while(*s){
        const char *e=s; while(*e && *e!='.') e++;
        int l=(int)(e-s); if(l<=0||l>63||k+l+6>(int)sizeof b){ dq_state=3; return; }
        b[k++]=(u8)l; for(int i=0;i<l;i++) b[k++]=(u8)s[i];
        s=*e?e+1:e;
    }
    b[k++]=0; wr16(b+k,1); wr16(b+k+2,1); k+=4;
    udp_out(dns_ip,DNS_LPORT,53,b,k);
    dq_t=kticks();
}
static int dns_skip_name(const u8 *p,int n,int i){
    while(i<n){
        int c=p[i];
        if(c==0) return i+1;
        if((c&0xC0)==0xC0) return i+2;
        i+=c+1;
    }
    return -1;
}
static void dns_in(const u8 *p,int n){
    if(dq_state!=1 || n<12 || rd16(p)!=dq_id || !(p[2]&0x80)) return;
    if((p[3]&15)!=0){ dq_state=3; return; }
    int qd=rd16(p+4), an=rd16(p+6), i=12;
    for(int q=0;q<qd;q++){ i=dns_skip_name(p,n,i); if(i<0||i+4>n){ dq_state=3; return; } i+=4; }
    for(int a=0;a<an;a++){
        i=dns_skip_name(p,n,i); if(i<0||i+10>n) break;
        int type=rd16(p+i), rdl=rd16(p+i+8); i+=10;
        if(i+rdl>n) break;
        if(type==1 && rdl==4){ dq_ip=rd32(p+i); dq_state=2; return; }
        i+=rdl;
    }
    dq_state=3;
}
/* 1 resuelto (en *ip), 0 pendiente, -1 error. Una consulta a la vez. */
static int dns_resolve(const char *name,u32 *ip){
    if(parse_ip(name,ip)) return 1;
    if(!net_st || !dns_ip) return -1;
    if(dq_state==0){
        k_strcpy(dq_name,name,sizeof dq_name); dq_state=1; dq_tries=1; dq_id=(u16)(kticks()*31u+7u); dns_send();
        return dq_state==3?(dq_state=0,-1):0;
    }
    if(k_strcmp(dq_name,name)) return 0;                /* ocupado con otro nombre */
    if(dq_state==2){ *ip=dq_ip; dq_state=0; return 1; }
    if(dq_state==3){ dq_state=0; return -1; }
    return 0;
}
static void dns_tick(void){
    if(dq_state!=1 || (int)(kticks()-dq_t)<100) return;
    if(++dq_tries>4){ dq_state=3; return; }
    dns_send();
}

/* ---------- ICMP / ping ---------- */
static struct { int act; u32 ip; char name[64]; int sent, recvd; u32 t_send, next; u16 seq; int got; } pg;
static void icmp_in(u32 src,const u8 *p,int n){
    if(n<8) return;
    if(csum_fin(csum_add(0,p,n))!=0) return;
    if(p[0]==8){                                           /* echo request -> echo reply */
        static u8 r[1500]; if(n>1400) return;
        memcpy(r,p,(size_t)n); r[0]=0; wr16(r+2,0); wr16(r+2,csum_fin(csum_add(0,r,n)));
        ip_out(1,src,r,n);
    } else if(p[0]==0 && pg.act==3 && rd16(p+4)==0x4B4E && rd16(p+6)==pg.seq && src==pg.ip && !pg.got){
        pg.got=1; pg.recvd++;
        u32 ms=(kticks()-pg.t_send)*10;
        if(ms<10) kprintf("respuesta de %s: tiempo<10 ms\n",ipstr(src)); else kprintf("respuesta de %s: tiempo=%u ms\n",ipstr(src),ms);
    }
}
static void ping_send(void){
    u8 b[40]; memset(b,0,sizeof b); b[0]=8; wr16(b+4,0x4B4E); pg.seq++; wr16(b+6,pg.seq);
    for(int i=8;i<40;i++) b[i]=(u8)('a'+(i%23));
    wr16(b+2,csum_fin(csum_add(0,b,40)));
    pg.t_send=kticks(); pg.got=0; pg.sent++;
    ip_out(1,pg.ip,b,40);
}
static void ping_step(void){
    if(pg.act==1||pg.act==2){
        u32 ip; int r=dns_resolve(pg.name,&ip);
        if(r<0){ kprintf("ping: no se pudo resolver %s\n",pg.name); pg.act=0; return; }
        if(r>0){ pg.ip=ip; pg.act=3; pg.next=kticks(); kprintf("PING %s (%s)\n",pg.name,ipstr(ip)); }
        return;
    }
    if(pg.act==3 && (int)(kticks()-pg.next)>=0){
        if(pg.sent>0 && !pg.got) kprintf("tiempo agotado\n");
        if(pg.sent>=4){ kprintf("%d enviados, %d recibidos\n",pg.sent,pg.recvd); pg.act=0; return; }
        ping_send(); pg.next=kticks()+100;
    }
}

/* ---------- TCP (cliente, una conexion) ---------- */
#define TF_FIN 1
#define TF_SYN 2
#define TF_RST 4
#define TF_PSH 8
#define TF_ACK 16
#define SEQ_LT(a,b) ((int32_t)((a)-(b))<0)
enum { TC_CLOSED=0, TC_SYN, TC_EST, TC_LAST, TC_DONE, TC_ERR };
static struct { int st; u32 rip; u16 rport,lport; u32 iss,snd_una,snd_nxt,rcv_nxt; u32 t,last_rx; int tries; char err[64]; } tc;
static u8 req[1200]; static int reqlen; static u16 lport_ctr;

static int tcp_seg(u8 flags,u32 seq,u32 ack,const u8 *d,int n,int mss){
    static u8 s[1500]; int hl=mss?24:20;
    wr16(s,tc.lport); wr16(s+2,tc.rport); wr32(s+4,seq); wr32(s+8,(flags&TF_ACK)?ack:0);
    s[12]=(u8)((hl/4)<<4); s[13]=flags; wr16(s+14,16384); wr16(s+16,0); wr16(s+18,0);
    if(mss){ s[20]=2; s[21]=4; wr16(s+22,1460); }
    if(n) memcpy(s+hl,d,(size_t)n);
    u32 sum=pseudo(my_ip,tc.rip,6,hl+n); sum=csum_add(sum,s,hl+n); wr16(s+16,csum_fin(sum));
    return ip_out(6,tc.rip,s,hl+n);
}
static void tc_fail(const char *m){ k_strcpy(tc.err,m,sizeof tc.err); tc.st=TC_ERR; }
static void tcp_open(u32 ip,u16 port){
    memset(&tc,0,sizeof tc);
    tc.rip=ip; tc.rport=port; tc.lport=(u16)(49152+(lport_ctr++%4000));
    tc.iss=kticks()*7919u+12345u; tc.snd_una=tc.iss; tc.snd_nxt=tc.iss+1; tc.st=TC_SYN; tc.t=kticks();
    tcp_seg(TF_SYN,tc.iss,0,0,0,1);
}

/* HTTP: estado del trabajo */
#define HTTP_MAX (512*1024)
static struct {
    int st;                 /* 0 inactivo, 1 DNS, 2 transferencia, 3 listo, 4 error */
    char host[64], path[256], url[300]; u16 port; u32 ip; char err[96];
    u8 *buf; int len; int hdr_end, hdr_done; int clen, chunked, end, trunc;
    int status, html, redirects; u32 t0; char loc[300]; char ctype[48];
    int body_off, body_len;
} hj;

static void hj_free(void){ if(hj.buf){ kfree(hj.buf); hj.buf=0; } hj.len=0; }
static void hj_err(const char *m){ k_strcpy(hj.err,m,sizeof hj.err); hj.st=4; hj_free(); }

static void http_data(const u8 *d,int n){
    if(!hj.buf) return;
    int room=HTTP_MAX-hj.len;
    if(n>room){ n=room; hj.trunc=1; }
    if(n>0){ memcpy(hj.buf+hj.len,d,(size_t)n); hj.len+=n; }
}
static void hj_parse_head(void){            /* cuando ya llego "\r\n\r\n" */
    const char *h=(const char*)hj.buf; int n=hj.hdr_end;
    hj.clen=-1; hj.chunked=0;
    const char *c=istr_find(h,n,"\r\ncontent-length:");
    if(c){ c+=17; while(*c==' ') c++; int v=0; while(*c>='0'&&*c<='9'){ v=v*10+(*c-'0'); c++; } hj.clen=v; }
    const char *t=istr_find(h,n,"\r\ntransfer-encoding:");
    if(t){ const char *e=t+20; int l=0; while(e+l<h+n && e[l]!='\r') l++; if(istr_find(e,l,"chunked")) hj.chunked=1; }
}
static int http_complete(void){              /* ¿ya tenemos todo el cuerpo (Content-Length)? */
    if(!hj.buf) return 0;
    if(!hj.hdr_done){
        for(int i=0;i+3<hj.len;i++) if(hj.buf[i]=='\r'&&hj.buf[i+1]=='\n'&&hj.buf[i+2]=='\r'&&hj.buf[i+3]=='\n'){ hj.hdr_end=i+4; hj.hdr_done=1; hj_parse_head(); break; }
    }
    if(hj.hdr_done && !hj.chunked && hj.clen>=0 && hj.len-hj.hdr_end>=hj.clen) return 1;
    return 0;
}

static void tcp_in(u32 src,const u8 *p,int n){
    if(n<20 || tc.st==TC_CLOSED || tc.st==TC_DONE || tc.st==TC_ERR) return;
    if(src!=tc.rip || rd16(p)!=tc.rport || rd16(p+2)!=tc.lport) return;
    u32 sum=pseudo(src,my_ip,6,n); sum=csum_add(sum,p,n); if(csum_fin(sum)!=0) return;
    u32 seq=rd32(p+4), ack=rd32(p+8); int fl=p[13], hl=(p[12]>>4)*4;
    if(hl<20||hl>n) return;
    const u8 *data=p+hl; int dl=n-hl;
    if(fl&TF_RST){ if(tc.st==TC_SYN) tc_fail("conexion rechazada"); else tc_fail("conexion reiniciada por el servidor"); return; }
    if(tc.st==TC_SYN){
        if((fl&(TF_SYN|TF_ACK))==(TF_SYN|TF_ACK) && ack==tc.iss+1){
            tc.rcv_nxt=seq+1; tc.snd_una=ack; tc.st=TC_EST; tc.last_rx=kticks(); tc.tries=0;
            tcp_seg(TF_ACK,tc.snd_nxt,tc.rcv_nxt,0,0,0);
            tcp_seg(TF_PSH|TF_ACK,tc.snd_nxt,tc.rcv_nxt,req,reqlen,0);
            tc.snd_nxt+=(u32)reqlen; tc.t=kticks();
        }
        return;
    }
    if(tc.st==TC_EST){
        if((fl&TF_ACK) && SEQ_LT(tc.snd_una,ack) && !SEQ_LT(tc.snd_nxt,ack)) tc.snd_una=ack;
        int need_ack=0, fin=0;
        if(dl>0){
            if(seq==tc.rcv_nxt){ http_data(data,dl); tc.rcv_nxt+=(u32)dl; need_ack=1; tc.last_rx=kticks(); }
            else { tcp_seg(TF_ACK,tc.snd_nxt,tc.rcv_nxt,0,0,0); return; }      /* fuera de orden o repetido: ACK duplicado */
        }
        if((fl&TF_FIN) && seq+(u32)dl==tc.rcv_nxt){ tc.rcv_nxt++; fin=1; }
        if(fin || http_complete()){
            hj.end=1;
            tcp_seg(TF_FIN|TF_ACK,tc.snd_nxt,tc.rcv_nxt,0,0,0); tc.snd_nxt++; tc.st=TC_LAST; tc.t=kticks();
        } else if(need_ack) tcp_seg(TF_ACK,tc.snd_nxt,tc.rcv_nxt,0,0,0);
        return;
    }
    if(tc.st==TC_LAST){
        if((fl&TF_FIN) && seq==tc.rcv_nxt){ tc.rcv_nxt++; tcp_seg(TF_ACK,tc.snd_nxt,tc.rcv_nxt,0,0,0); }
        if((fl&TF_ACK) && ack==tc.snd_nxt) tc.st=TC_DONE;
    }
}
static void tcp_tick(void){
    u32 now=kticks();
    if(tc.st==TC_SYN && (int)(now-tc.t)>=100){
        if(++tc.tries>4){ tc_fail("tiempo agotado conectando con el servidor"); return; }
        tcp_seg(TF_SYN,tc.iss,0,0,0,1); tc.t=now;
    } else if(tc.st==TC_EST){
        if(tc.snd_una!=tc.snd_nxt && (int)(now-tc.t)>=100){
            if(++tc.tries>6){ tc_fail("tiempo agotado: el servidor no responde"); return; }
            tcp_seg(TF_PSH|TF_ACK,tc.iss+1,tc.rcv_nxt,req,reqlen,0); tc.t=now;
        }
        if((int)(now-tc.last_rx)>2000) tc_fail("tiempo agotado esperando datos");
    }
}

/* ---------- HTTP ---------- */
/* 0 ok; -1 https; -2 direccion invalida */
static int url_split(const char *url,char *host,int hmax,u16 *port,char *path,int pmax){
    int https=0;
    if(istr_prefix(url,"http://")) url+=7;
    else if(istr_prefix(url,"https://")){ https=1; url+=8; }
    int i=0; while(url[i] && url[i]!='/' && url[i]!=':' && url[i]!='?' && url[i]!='#' && i<hmax-1){ host[i]=url[i]; i++; }
    host[i]=0; if(!i) return -2;
    url+=i;
    *port=https?443:80;
    if(*url==':'){ url++; int v=0; while(*url>='0'&&*url<='9'){ v=v*10+(*url-'0'); url++; if(v>65535) return -2; } if(!v) return -2; *port=(u16)v; }
    int k=0;
    if(*url=='?'||!*url||*url=='#'){ path[k++]='/'; }
    while(*url && *url!='#' && k<pmax-4){ if(*url==' '){ path[k++]='%'; path[k++]='2'; path[k++]='0'; } else path[k++]=*url; url++; }
    path[k]=0;
    return https?-1:0;
}
static void http_begin_request(void){
    req[0]=0; reqlen=0;
    char r[1200]; r[0]=0;
    scat(r,sizeof r,"GET "); scat(r,sizeof r,hj.path); scat(r,sizeof r," HTTP/1.0\r\nHost: "); scat(r,sizeof r,hj.host);
    if(hj.port!=80){ char pb[10]; portstr(pb,hj.port); scat(r,sizeof r,pb); }
    scat(r,sizeof r,"\r\nUser-Agent: KonigreichOS/0.6 (kng)\r\nAccept: text/html, text/plain;q=0.9, */*;q=0.1\r\nConnection: close\r\n\r\n");
    reqlen=k_strlen(r); memcpy(req,r,(size_t)reqlen);
}
static int http_start(const char *url){
    hj_free();
    char host[64], path[256]; u16 port;
    int r=url_split(url,host,sizeof host,&port,path,sizeof path);
    if(r==-1){ k_strcpy(hj.err,"HTTPS no esta soportado todavia (kng no tiene TLS). Usa una direccion http://",sizeof hj.err); if(net_st==0 && pci_seen[0]) scat(hj.err,sizeof hj.err,pci_seen); hj.st=4; return -1; }
    if(r<0){ k_strcpy(hj.err,"Direccion invalida",sizeof hj.err); hj.st=4; return -1; }
    if(net_st!=2){ k_strcpy(hj.err,net_st==0?(pci_seen[0]?"Sin driver para tu tarjeta de red. Vista en PCI (ven:dev):":"La VM no tiene tarjeta de red PCI. Agrega una (Intel e1000, PCnet o virtio)"):"La red aun se esta configurando (DHCP). Intenta de nuevo",sizeof hj.err); hj.st=4; return -1; }
    k_strcpy(hj.host,host,sizeof hj.host); k_strcpy(hj.path,path,sizeof hj.path); hj.port=port;
    hj.buf=(u8*)kmalloc(HTTP_MAX+1);
    if(!hj.buf){ k_strcpy(hj.err,"Sin memoria",sizeof hj.err); hj.st=4; return -1; }
    hj.len=0; hj.hdr_done=0; hj.hdr_end=0; hj.clen=-1; hj.chunked=0; hj.end=0; hj.trunc=0; hj.status=0; hj.html=0;
    hj.url[0]=0; scat(hj.url,sizeof hj.url,"http://"); scat(hj.url,sizeof hj.url,host);
    if(port!=80){ char pb[10]; portstr(pb,port); scat(hj.url,sizeof hj.url,pb); }
    scat(hj.url,sizeof hj.url,path);
    hj.st=1; hj.t0=kticks(); hj.err[0]=0; hj.ctype[0]=0; hj.loc[0]=0;
    return 0;
}
int http_get(const char *url){ hj.redirects=0; hj.st=0; return http_start(url); }
void http_cancel(void){ hj_free(); hj.st=0; tc.st=TC_CLOSED; }
int http_poll(void){ return hj.st==3?1:(hj.st==4?-1:0); }
int http_status(void){ return hj.status; }
const char *http_error(void){ return hj.err; }
const char *http_final_url(void){ return hj.url; }
int http_is_html(void){ return hj.html; }
char *http_take(int *len){
    if(hj.st!=3 || !hj.buf) return 0;
    char *b=(char*)hj.buf; *len=hj.body_len; hj.buf=0; hj.len=0; hj.st=0;
    return b;
}
static void http_join(const char *loc){          /* resuelve Location contra la URL actual y la guarda en hj.loc */
    char o[300]; o[0]=0;
    if(istr_prefix(loc,"http://")||istr_prefix(loc,"https://")) k_strcpy(o,loc,sizeof o);
    else if(loc[0]=='/'&&loc[1]=='/'){ scat(o,sizeof o,"http:"); scat(o,sizeof o,loc); }
    else if(loc[0]=='/'){ scat(o,sizeof o,"http://"); scat(o,sizeof o,hj.host);
        if(hj.port!=80){ char pb[10]; portstr(pb,hj.port); scat(o,sizeof o,pb); }
        scat(o,sizeof o,loc); }
    else { k_strcpy(o,hj.url,sizeof o); int l=k_strlen(o); while(l>7 && o[l-1]!='/') l--; o[l]=0; scat(o,sizeof o,loc); }
    k_strcpy(hj.loc,o,sizeof hj.loc);
}
static void http_finish(void){
    if(!hj.hdr_done){
        for(int i=0;i+3<hj.len;i++) if(hj.buf[i]=='\r'&&hj.buf[i+1]=='\n'&&hj.buf[i+2]=='\r'&&hj.buf[i+3]=='\n'){ hj.hdr_end=i+4; hj.hdr_done=1; hj_parse_head(); break; }
        if(!hj.hdr_done){ hj_err("Respuesta HTTP invalida"); return; }
    }
    const char *h=(const char*)hj.buf;
    if(!istr_prefix(h,"HTTP/1.")){ hj_err("El servidor no respondio HTTP"); return; }
    int st=0; { const char *c=h+8; while(*c==' ') c++; while(*c>='0'&&*c<='9'){ st=st*10+(*c-'0'); c++; } }
    hj.status=st;
    /* Location */
    hj.loc[0]=0;
    { const char *l=istr_find(h,hj.hdr_end,"\r\nlocation:");
      if(l){ l+=11; while(*l==' ') l++; int k=0; char tmp[300]; while(l<h+hj.hdr_end && *l!='\r' && k<299) tmp[k++]=*l++; tmp[k]=0; http_join(tmp); } }
    /* tipo */
    hj.ctype[0]=0;
    { const char *l=istr_find(h,hj.hdr_end,"\r\ncontent-type:");
      if(l){ l+=15; while(*l==' ') l++; int k=0; while(l<h+hj.hdr_end && *l!='\r' && *l!=';' && k<47) hj.ctype[k++]=(char)lc((u8)*l++); hj.ctype[k]=0; } }
    if((st==301||st==302||st==303||st==307||st==308) && hj.loc[0]){
        if(++hj.redirects>5){ hj_err("Demasiadas redirecciones"); return; }
        char nu[300]; k_strcpy(nu,hj.loc,sizeof nu);
        if(http_start(nu)!=0) { /* hj.err ya fijado */ }
        return;
    }
    /* cuerpo (con o sin chunked) hacia el inicio del buffer */
    u8 *src=hj.buf+hj.hdr_end; int avail=hj.len-hj.hdr_end, out=0;
    if(hj.chunked){
        int i=0;
        while(i<avail){
            int sz=0, any=0;
            while(i<avail){ int c=src[i]; int v=c>='0'&&c<='9'?c-'0':(c>='a'&&c<='f'?c-'a'+10:(c>='A'&&c<='F'?c-'A'+10:-1)); if(v<0) break; sz=sz*16+v; any=1; i++; if(sz>HTTP_MAX) break; }
            while(i<avail && src[i]!='\n') i++;
            i++;
            if(!any||sz==0) break;
            if(i+sz>avail) sz=avail-i;
            mmove_fwd(hj.buf+out,src+i,sz); out+=sz; i+=sz;
            if(i<avail && src[i]=='\r') i++;
            if(i<avail && src[i]=='\n') i++;
        }
    } else { mmove_fwd(hj.buf,src,avail); out=avail; if(hj.clen>=0 && out>hj.clen) out=hj.clen; }
    hj.buf[out]=0; hj.body_len=out; hj.len=out;
    int is_text=istr_prefix(hj.ctype,"text/")||hj.ctype[0]==0||istr_find(hj.ctype,k_strlen(hj.ctype),"xml")||istr_find(hj.ctype,k_strlen(hj.ctype),"json");
    if(!is_text){ char m[96]="Tipo de contenido no soportado: "; scat(m,sizeof m,hj.ctype); hj_err(m); return; }
    hj.html=istr_find(hj.ctype,k_strlen(hj.ctype),"html")!=0;
    if(!hj.html && hj.ctype[0]==0){ for(int i=0;i<out&&i<64;i++){ if(hj.buf[i]=='<'){ hj.html=1; break; } if(hj.buf[i]>' ') break; } }
    if(st>=400 && out==0){ char m[96]="El servidor respondio con error HTTP "; char nb[8]; int q=0; char t[8]; int n=0,v=st; while(v){ t[n++]=(char)('0'+v%10); v/=10; } while(n) nb[q++]=t[--n]; nb[q]=0; scat(m,sizeof m,nb); hj_err(m); return; }
    hj.st=3;
}
static void http_step(void){
    if(hj.st==1){
        u32 ip; int r=dns_resolve(hj.host,&ip);
        if(r<0){ char m[96]="No se pudo resolver el nombre: "; scat(m,sizeof m,hj.host); hj_err(m); return; }
        if(r>0){ hj.ip=ip; http_begin_request(); tcp_open(ip,hj.port); hj.st=2; }
    } else if(hj.st==2){
        if(tc.st==TC_ERR){ hj_err(tc.err); return; }
        if((tc.st==TC_LAST||tc.st==TC_DONE) && hj.end){ http_finish(); return; }
    }
    if((hj.st==1||hj.st==2) && (int)(kticks()-hj.t0)>3000) hj_err("Tiempo agotado");
}

/* ---------- recepcion ---------- */
static u8 rxf[2048];
int (*sock_ip_hook)(u8 proto,u32 src,u32 dst,const u8 *p,int n);
static void ip_in(const u8 *p,int n){
    if(n<20 || (p[0]>>4)!=4) return;
    int ihl=(p[0]&15)*4, tl=rd16(p+2);
    if(ihl<20||tl<ihl||tl>n) return;
    if(csum_fin(csum_add(0,p,ihl))!=0) return;
    if(rd16(p+6)&0x3FFF) return;                       /* fragmentos: no soportados */
    u32 src=rd32(p+12), dst=rd32(p+16);
    if(my_ip && dst!=my_ip && dst!=0xFFFFFFFFu && !(mask && dst==((my_ip&mask)|~mask))) return;
    const u8 *q=p+ihl; int qn=tl-ihl;
    if(sock_ip_hook && (p[9]==6||p[9]==17) && sock_ip_hook(p[9],src,dst,q,qn)) return;   /* sockets de programas de usuario (sock.c) */
    switch(p[9]){
    case 1: icmp_in(src,q,qn); break;
    case 6: tcp_in(src,q,qn); break;
    case 17: if(qn>=8){ int ul=rd16(q+4); if(ul<8||ul>qn) break; u16 dp=rd16(q+2);
                 if(dp==68) dhcp_in(q+8,ul-8); else if(dp==DNS_LPORT) dns_in(q+8,ul-8); } break;
    }
}
static void frame_in(const u8 *f,int n){
    if(n<14) return;
    if(!(f[0]&1) && mcmp(f,mac,6)) return;
    int t=rd16(f+12);
    if(t==0x0806) arp_in(f+14,n-14); else if(t==0x0800) ip_in(f+14,n-14);
}

/* ---------- API principal ---------- */
static void wget_step(void);
static void dns_job(void);
int net_state(void){ return net_st; }
static void net_start(void){
    my_ip=mask=gw=dns_ip=0; net_st=1; dh_state=1; dh_tries=0; dh_xid=kticks()*2654435761u+0x1234u;
    arp_next=0; memset(arpt,0,sizeof arpt); pend_len=0;
    dhcp_send(1);
}
int net_init(void){
#ifdef HOSTNET
    nic=&h_drv; mac[0]=0x52; mac[1]=0x54; mac[2]=0; mac[3]=0x12; mac[4]=0x34; mac[5]=0x56;
    k_strcpy(nic_desc,"simulada (host)",sizeof nic_desc); net_start(); return 0;
#else
    for(int b=0;b<32;b++) for(int d=0;d<32;d++) for(int f=0;f<8;f++){
        u32 id=pci_rd(b,d,f,0);
        if((id&0xFFFF)==0xFFFF){ if(f==0) break; continue; }
        u16 ven=(u16)id, dev=(u16)(id>>16);
        int is_e=0; if(ven==0x8086) for(unsigned i=0;i<sizeof e_ids/sizeof e_ids[0];i++) if(e_ids[i]==dev) is_e=1;
        int is_r=(ven==0x10EC && dev==0x8139);
        int is_p=(ven==0x1022 && (dev==0x2000||dev==0x2001));
        int is_v=(ven==0x1AF4 && dev==0x1000);
        if((pci_rd(b,d,f,8)>>24)==2){ char h[24]; static const char *hx="0123456789abcdef"; int k=0;                 /* anota toda tarjeta de red PCI para el diagnostico */
            for(int sh=12;sh>=0;sh-=4) h[k++]=hx[(ven>>sh)&15];
            h[k++]=':';
            for(int sh=12;sh>=0;sh-=4) h[k++]=hx[(dev>>sh)&15];
            h[k++]=' '; h[k]=0;
            if(k_strlen(pci_seen)+k<(int)sizeof pci_seen-1) scat(pci_seen,sizeof pci_seen,h); }
        if(is_p||is_v){
            u32 cmd=pci_rd(b,d,f,4); pci_wr(b,d,f,4,cmd|7);
            u32 bar0=pci_rd(b,d,f,0x10); if(!(bar0&1)) continue;
            if(is_p){ if(p_init((u16)(bar0&~3u))) continue; nic=&p_drv; }
            else { if(v_init((u16)(bar0&~3u))) continue; nic=&v_drv; }
            nic_desc[0]=0; scat(nic_desc,sizeof nic_desc,nic->name); net_start(); return 0;
        }
        if(is_e||is_r){
            u32 cmd=pci_rd(b,d,f,4); pci_wr(b,d,f,4,cmd|7);            /* E/S, memoria y bus maestro */
            u32 bar0=pci_rd(b,d,f,0x10);
            if(is_e && (bar0&1)) continue;
            if(is_r && !(bar0&1)) continue;
            if(is_e){ if(e_init(bar0&~0xFu)) continue; nic=&e_drv; }
            else { if(r_init((u16)(bar0&~3u))) continue; nic=&r_drv; }
            nic_desc[0]=0; scat(nic_desc,sizeof nic_desc,nic->name);
            net_start(); return 0;
        }
        if(f==0 && !(pci_rd(b,d,0,0x0C)&0x800000)) break;               /* dispositivo de una sola funcion */
    }
    return -1;
#endif
}
void (*sock_tick_hook)(void);
void net_poll(void){
    if(!nic) return;
    int cnt=0;
    for(;;){
        int n=nic->recv(rxf,sizeof rxf);
        if(n==0) break;
        if(n>0) frame_in(rxf,n);
        if(++cnt>=64) break;
    }
    u32 now=kticks();
    if(pend_len && (int)(now-pend_t)>=100){
        if(++pend_try>3) pend_len=0; else { arp_request(pend_ip); pend_t=now; }
    }
    dhcp_tick(); dns_tick(); tcp_tick(); http_step(); ping_step(); dns_job(); wget_step();
    if(sock_tick_hook) sock_tick_hook();
}

/* ---------- informacion y comandos ---------- */
static void macstr(char *o){ static const char *hx="0123456789abcdef"; int k=0; for(int i=0;i<6;i++){ o[k++]=hx[mac[i]>>4]; o[k++]=hx[mac[i]&15]; if(i<5) o[k++]=':'; } o[k]=0; }
void net_info(char *out,int max){
    out[0]=0;
    if(!nic){ k_strcpy(out,"Red: sin tarjeta compatible (e1000, RTL8139, PCnet, virtio)",max);
        if(pci_seen[0]){ scat(out,max,". Vista en PCI: "); scat(out,max,pci_seen); } else scat(out,max,". No hay ninguna tarjeta de red PCI en la VM.");
        return; }
    scat(out,max,"Red: "); scat(out,max,nic_desc);
    if(net_st==2){ scat(out,max,"  IP "); scat(out,max,ipstr(my_ip)); if(ip_static) scat(out,max," (reserva)"); }
    else scat(out,max,"  configurando (DHCP)...");
}
static int wg_act;
static struct { int act; char name[64]; } dn;
static void dns_job(void){
    if(!dn.act) return;
    u32 ip; int r=dns_resolve(dn.name,&ip);
    if(r>0){ kprintf("%s -> %s\n",dn.name,ipstr(ip)); dn.act=0; }
    else if(r<0){ kprintf("dns: no se pudo resolver %s\n",dn.name); dn.act=0; }
}
static void wget_step(void){
    if(!wg_act) return;
    int r=http_poll();
    if(!r) return;
    wg_act=0;
    if(r<0){ kprintf("wget: %s\n",http_error()); return; }
    int n=0; int st=http_status(); char *b=http_take(&n);
    kprintf("HTTP %d  %d bytes  %s\n",st,n,http_final_url());
    if(b){ int shown=0, intag=0; for(int i=0;i<n&&shown<600;i++){ char c=b[i];
            if(http_is_html()){ if(c=='<') intag=1; if(intag){ if(c=='>') intag=0; continue; } }
            if(c=='\r') continue;
            if(c>=32||c=='\n'){ char t[2]={c,0}; kprintf("%s",t); shown++; } }
        kprintf("\n"); kfree(b); }
}
int net_cmd(char *l){
    while(*l==' ') l++;
    if(!k_strcmp(l,"red")||!k_strcmp(l,"ip")||!k_strcmp(l,"ifconfig")){
        if(!nic){ kprintf("Sin tarjeta de red compatible (soportadas: Intel e1000, Realtek RTL8139).\n"); return 1; }
        char m[20]; macstr(m);
        kprintf("Tarjeta: %s  MAC %s  enlace %s\n",nic_desc,m,nic->link&&nic->link()?"activo":"sin senal");
        if(net_st==2){ kprintf("IP %s  mascara %s  puerta %s  DNS %s%s\n",ipstr(my_ip),ipstr(mask),ipstr(gw),ipstr(dns_ip),ip_static?"  (configuracion de reserva: no hubo DHCP)":""); }
        else kprintf("Esperando respuesta DHCP...\n");
        return 1;
    }
    if(k_startswith(l,"ping")){
        char *a=l+4; while(*a==' ') a++;
        if(!*a){ kprintf("uso: ping <direccion o nombre>\n"); return 1; }
        if(net_st!=2){ kprintf("ping: la red no esta lista (use 'red')\n"); return 1; }
        if(pg.act){ kprintf("ping: ya hay uno en curso\n"); return 1; }
        memset(&pg,0,sizeof pg); k_strcpy(pg.name,a,sizeof pg.name); pg.act=1; return 1;
    }
    if(k_startswith(l,"dns ")){
        char *a=l+4; while(*a==' ') a++;
        if(net_st!=2){ kprintf("dns: la red no esta lista\n"); return 1; }
        if(dn.act){ kprintf("dns: ya hay una consulta en curso\n"); return 1; }
        k_strcpy(dn.name,a,sizeof dn.name); dn.act=1; return 1;
    }
    if(k_startswith(l,"wget ")){
        char *a=l+5; while(*a==' ') a++;
        if(wg_act){ kprintf("wget: ya hay una descarga en curso\n"); return 1; }
        if(http_get(a)<0){ kprintf("wget: %s\n",http_error()); return 1; }
        wg_act=1; kprintf("descargando %s ...\n",a); return 1;
    }
    return 0;
}

/* ---------- servicios para la capa de sockets (sock.c) ---------- */
int net_ip_send(uint8_t proto,uint32_t dst,const uint8_t *pl,int plen){ return ip_out(proto,dst,pl,plen); }
int net_udp_send(uint32_t dst,uint16_t sp,uint16_t dp,const uint8_t *d,int n){ return udp_out(dst,sp,dp,d,n); }
uint32_t net_my_ip(void){ return my_ip; }
uint32_t net_dns_addr(void){ return dns_ip; }
uint32_t net_gw_addr(void){ return gw; }
