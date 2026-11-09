/* net_pcnet_virtio.h — controladores de red extra (se incluye desde net.c):
 *   - AMD PCnet (Am79C970A / 79C973): la tarjeta por omision de VirtualBox, modo de 32 bits, por sondeo.
 *   - virtio-net clasico (E/S): QEMU/KVM y la tarjeta "paravirtualizada" de VirtualBox. */

/* ===== AMD PCnet ===== */
#define P_NRX 16
#define P_NTX 8
struct pdesc { u32 addr; u32 flags; u32 mlen; u32 res; };
struct pinit { u16 mode; u8 rlen, tlen; u8 mac[6]; u16 res; u8 ladr[8]; u32 rdra, tdra; };
static struct pdesc p_rxd[P_NRX] __attribute__((aligned(16)));
static struct pdesc p_txd[P_NTX] __attribute__((aligned(16)));
static u8 p_rxb[P_NRX][2048] __attribute__((aligned(16)));
static u8 p_txb[P_NTX][2048] __attribute__((aligned(16)));
static struct pinit p_ib __attribute__((aligned(16)));
static u16 p_io; static int p_rx_cur, p_tx_cur;
static u32 p_csr(int n){ io_o32((u16)(p_io+0x14),(u32)n); return io_i32((u16)(p_io+0x10)); }
static void p_wcsr(int n,u32 v){ io_o32((u16)(p_io+0x14),(u32)n); io_o32((u16)(p_io+0x10),v); }
static u32 p_bcr(int n){ io_o32((u16)(p_io+0x14),(u32)n); return io_i32((u16)(p_io+0x1C)); }
static void p_wbcr(int n,u32 v){ io_o32((u16)(p_io+0x14),(u32)n); io_o32((u16)(p_io+0x1C),v); }
static int p_send(const u8 *f,int n){
    volatile struct pdesc *d=&p_txd[p_tx_cur];
    int t=200000; while((d->flags&0x80000000u) && --t) __asm__ volatile("" ::: "memory");
    if(!t || n>2000) return -1;
    memcpy(p_txb[p_tx_cur],f,(size_t)n); if(n<60){ memset(p_txb[p_tx_cur]+n,0,(size_t)(60-n)); n=60; }
    d->addr=(u32)(uintptr_t)p_txb[p_tx_cur]; d->mlen=0;
    __asm__ volatile("" ::: "memory");
    d->flags=0x80000000u|0x03000000u|0x0000F000u|((u32)(4096-n)&0xFFF);        /* OWN | STP | ENP | longitud negativa */
    p_tx_cur=(p_tx_cur+1)%P_NTX;
    p_wcsr(0,0x08);                                                              /* TDMD: transmitir ya */
    return 0;
}
static int p_recv(u8 *buf,int max){
    volatile struct pdesc *d=&p_rxd[p_rx_cur];
    if(d->flags&0x80000000u) return 0;
    u32 fl=d->flags; int n=(int)(d->mlen&0xFFF)-4, bad=(fl&0x40000000u)!=0 || n>max || n<14;
    if(!bad) memcpy(buf,p_rxb[p_rx_cur],(size_t)n);
    d->flags=0x80000000u|0x0000F000u|((u32)(4096-2048)&0xFFF);
    p_rx_cur=(p_rx_cur+1)%P_NRX;
    return bad?-1:n;
}
static int p_link(void){ return 1; }
static const struct nicdrv p_drv={"AMD PCnet",p_send,p_recv,p_link};
static int p_init(u16 io){
    p_io=io;
    (void)io_i16((u16)(io+0x14)); (void)io_i32((u16)(io+0x18)); msleep(10);        /* reinicio (acceso de 16 y de 32 bits) */
    io_o32((u16)(io+0x10),0);                                                     /* una escritura de 32 bits activa el modo DWIO */
    p_wcsr(58,(p_csr(58)&0xFF00u)|2u);                                            /* SWSTYLE = 2: estructuras de 32 bits */
    p_wbcr(2,p_bcr(2)|2u);                                                        /* ASEL: elige el medio solo */
    for(int i=0;i<6;i++) mac[i]=io_i8((u16)(io+i));
    for(int i=0;i<P_NRX;i++){ p_rxd[i].addr=(u32)(uintptr_t)p_rxb[i]; p_rxd[i].mlen=0; p_rxd[i].flags=0x80000000u|0x0000F000u|((u32)(4096-2048)&0xFFF); }
    for(int i=0;i<P_NTX;i++){ p_txd[i].flags=0; p_txd[i].addr=0; p_txd[i].mlen=0; }
    p_rx_cur=p_tx_cur=0;
    memset(&p_ib,0,sizeof p_ib); p_ib.rlen=(u8)(4<<4); p_ib.tlen=(u8)(3<<4);       /* 2^4 = 16 y 2^3 = 8 descriptores */
    for(int i=0;i<6;i++) p_ib.mac[i]=mac[i];
    for(int i=0;i<8;i++) p_ib.ladr[i]=0xFF;
    p_ib.rdra=(u32)(uintptr_t)p_rxd; p_ib.tdra=(u32)(uintptr_t)p_txd;
    p_wcsr(1,((u32)(uintptr_t)&p_ib)&0xFFFF); p_wcsr(2,((u32)(uintptr_t)&p_ib)>>16);
    p_wcsr(0,1);                                                                   /* INIT */
    int t=100000; while(t-- && !(p_csr(0)&0x100)) ;
    if(t<=0) return -1;
    p_wcsr(0,2);                                                                   /* STRT, sin interrupciones */
    return 0;
}

/* ===== virtio-net ===== */
#define V_NB 16
struct vq { volatile u8 *base; u16 n; };
static u16 v_io;
static u8 v_q0[12288] __attribute__((aligned(4096)));
static u8 v_q1[12288] __attribute__((aligned(4096)));
static u8 v_rxb[V_NB][2048] __attribute__((aligned(16)));
static u8 v_txb[V_NB][2048] __attribute__((aligned(16)));
static u16 v_rx_last, v_tx_count;
static struct vq v_rx, v_tx;
static volatile u16 *vq_availidx(struct vq *q){ return (volatile u16*)(q->base+16*q->n+2); }
static volatile u16 *vq_availring(struct vq *q){ return (volatile u16*)(q->base+16*q->n+4); }
static volatile u8 *vq_used(struct vq *q){ u32 off=16u*q->n+4u+2u*q->n; off=(off+4095u)&~4095u; return q->base+off; }
static void v_post_rx(int i){
    volatile u32 *d=(volatile u32*)(v_rx.base+16*i);
    uint64_t a=(uint64_t)(uintptr_t)v_rxb[i]; d[0]=(u32)a; d[1]=(u32)(a>>32); d[2]=2048; d[3]=2u;        /* 2 = el dispositivo escribe */
    u16 ai=*vq_availidx(&v_rx); vq_availring(&v_rx)[ai%v_rx.n]=(u16)i; __asm__ volatile("" ::: "memory"); *vq_availidx(&v_rx)=(u16)(ai+1);
}
static int v_send(const u8 *f,int n){
    if(n>1900) return -1;
    volatile u16 *ui=(volatile u16*)(vq_used(&v_tx)+2);
    int t=200000; while((u16)(v_tx_count-*ui)>=V_NB && --t) __asm__ volatile("" ::: "memory");
    if(!t) return -1;
    int i=v_tx_count%V_NB;
    memset(v_txb[i],0,10); memcpy(v_txb[i]+10,f,(size_t)n); if(n<60){ memset(v_txb[i]+10+n,0,(size_t)(60-n)); n=60; }
    volatile u32 *d=(volatile u32*)(v_tx.base+16*i); uint64_t a=(uint64_t)(uintptr_t)v_txb[i];
    d[0]=(u32)a; d[1]=(u32)(a>>32); d[2]=(u32)(n+10); d[3]=0;
    u16 ai=*vq_availidx(&v_tx); vq_availring(&v_tx)[ai%v_tx.n]=(u16)i; __asm__ volatile("" ::: "memory"); *vq_availidx(&v_tx)=(u16)(ai+1);
    v_tx_count++; io_o16((u16)(v_io+0x10),1);
    return 0;
}
static int v_recv(u8 *buf,int max){
    volatile u8 *used=vq_used(&v_rx); u16 idx=*(volatile u16*)(used+2);
    if(idx==v_rx_last) return 0;
    volatile u32 *e=(volatile u32*)(used+4+8*(v_rx_last%v_rx.n)); u32 id=e[0], len=e[1];
    int n=(int)len-10, ok=(id<V_NB && n>=14 && n<=max);
    if(ok) memcpy(buf,v_rxb[id]+10,(size_t)n);
    v_rx_last++; if(id<V_NB){ v_post_rx((int)id); io_o16((u16)(v_io+0x10),0); }
    return ok?n:-1;
}
static int v_link(void){ return 1; }
static const struct nicdrv v_drv={"virtio-net",v_send,v_recv,v_link};
static int v_setq(int q,u8 *mem,struct vq *out){
    io_o16((u16)(v_io+0x0E),(u16)q); u16 n=io_i16((u16)(v_io+0x0C));
    if(!n||n>256) return -1;
    memset(mem,0,12288); out->base=mem; out->n=n;
    io_o32((u16)(v_io+0x08),(u32)((uintptr_t)mem>>12));
    return 0;
}
static int v_init(u16 io){
    v_io=io; io_o8((u16)(io+0x12),0); io_o8((u16)(io+0x12),1); io_o8((u16)(io+0x12),3);   /* ACK | DRIVER */
    (void)io_i32(io); io_o32((u16)(io+4),0);                                             /* sin funciones opcionales */
    if(v_setq(0,v_q0,&v_rx) || v_setq(1,v_q1,&v_tx)) return -1;
    for(int i=0;i<6;i++) mac[i]=io_i8((u16)(io+0x14+i));
    v_rx_last=0; v_tx_count=0;
    for(int i=0;i<V_NB && i<v_rx.n;i++) v_post_rx(i);
    io_o8((u16)(io+0x12),7);                                                              /* DRIVER_OK */
    io_o16((u16)(io+0x10),0);
    return 0;
}

